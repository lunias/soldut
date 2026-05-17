#!/usr/bin/env bash
#
# tests/shots/net/run_frag_sync.sh — M6 P12 acceptance test.
#
# Verifies the per-tick projectile snapshot replication described in
# documents/m6/12-projectile-snapshot-replication.md. The host throws
# three frag grenades; for every server "explosion at" SHOT_LOG we
# find the matching `client_handle_explosion` on the connecting
# client (chronological order, same owner_mech + weapon id) and
# assert the positions are within ≤ 4 px (one snapshot of motion at
# the worst-case grenade speed).
#
# Pre-M6-P12 the client ran its own bouncy physics off the FIRE_EVENT
# spawn; client/server divergence after a few bounces was routinely
# 50+ px. With snapshot replication the position is deterministic to
# wire quantization (≤ 0.25 px), and the explosion event uses the
# server's authoritative pos verbatim — so the client / server
# position pairs should be within wire quant plus snapshot interp
# jitter (well under 4 px).
#
# Structure mirrors run_frag_grenade.sh: a `--dedicated` host with a
# soldut.cfg setting time_limit + score_limit high enough that 3
# throws + detonations comfortably fit inside a single round.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

NAME="2p_frag_sync"
HOST_SHOT="$REPO/tests/shots/net/${NAME}.host.shot"
CLI_SHOT="$REPO/tests/shots/net/${NAME}.client.shot"
[[ -f "$HOST_SHOT" ]] || { echo "fail: $HOST_SHOT missing"; exit 1; }
[[ -f "$CLI_SHOT" ]]  || { echo "fail: $CLI_SHOT missing";  exit 1; }

mkdir -p "$REPO/build/shots/net"

PORT=$((25300 + RANDOM % 200))
TMP="$(mktemp -d -t soldut-frag-sync-XXXXXX)"
SRV_DIR="$TMP/server"
mkdir -p "$SRV_DIR"
cat > "$SRV_DIR/soldut.cfg" <<EOF
port=$PORT
auto_start_seconds=1
countdown_default=1
time_limit=30
score_limit=99
mode=ffa
map_rotation=aurora
mode_rotation=ffa
snapshot_hz=60
EOF

SRV_PID=0
HOST_PID=0
CLI_PID=0
cleanup() {
    [[ $CLI_PID  -gt 0 ]] && kill     "$CLI_PID"  2>/dev/null
    [[ $HOST_PID -gt 0 ]] && kill     "$HOST_PID" 2>/dev/null
    sleep 0.3
    [[ $CLI_PID  -gt 0 ]] && kill -9  "$CLI_PID"  2>/dev/null
    [[ $HOST_PID -gt 0 ]] && kill -9  "$HOST_PID" 2>/dev/null
    [[ $SRV_PID  -gt 0 ]] && kill     "$SRV_PID"  2>/dev/null
    sleep 0.3
    [[ $SRV_PID  -gt 0 ]] && kill -9  "$SRV_PID"  2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

cd "$REPO"

( cd "$SRV_DIR" && exec "$BIN" --dedicated "$PORT" ) >/dev/null 2>&1 &
SRV_PID=$!

for ((i = 0; i < 50; ++i)); do
    sleep 0.05
    if grep -q "dedicated: listening on $PORT" "$SRV_DIR/soldut-server.log" 2>/dev/null; then
        break
    fi
done
if ! grep -q "dedicated: listening on $PORT" "$SRV_DIR/soldut-server.log" 2>/dev/null; then
    echo "fail: dedicated server didn't reach listening within 2.5 s"
    tail -10 "$SRV_DIR/soldut-server.log" 2>/dev/null
    exit 1
fi

HOST_TMP="$TMP/$(basename "$HOST_SHOT")"
CLI_TMP="$TMP/$(basename "$CLI_SHOT")"
sed -e "s/{{PORT}}/$PORT/g" "$HOST_SHOT" > "$HOST_TMP"
sed -e "s/{{PORT}}/$PORT/g" "$CLI_SHOT" > "$CLI_TMP"

"$BIN" --shot "$HOST_TMP" >/dev/null 2>&1 &
HOST_PID=$!
sleep 0.15
"$BIN" --shot "$CLI_TMP" >/dev/null 2>&1 &
CLI_PID=$!

TIMEOUT_S="${TEST_TIMEOUT_S:-40}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    if [[ ! -d /proc/$HOST_PID && ! -d /proc/$CLI_PID ]]; then
        break
    fi
done

[[ -d /proc/$CLI_PID ]] && kill "$CLI_PID" 2>/dev/null
[[ -d /proc/$HOST_PID ]] && kill "$HOST_PID" 2>/dev/null
sleep 0.3
[[ $SRV_PID -gt 0 ]] && kill "$SRV_PID" 2>/dev/null
sleep 0.3
[[ $SRV_PID -gt 0 ]] && kill -9 "$SRV_PID" 2>/dev/null

H_OUT="$REPO/build/shots/net/dedi/2p_frag_sync.host"
C_OUT="$REPO/build/shots/net/dedi/2p_frag_sync.client"
H_LOG="$H_OUT/${NAME}.host.log"
C_LOG="$C_OUT/${NAME}.client.log"
S_LOG="$SRV_DIR/soldut-server.log"

echo
echo "=== frag-sync assertions ==="

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host log exists"   "[[ -f '$H_LOG' ]]"
asrt "client log exists" "[[ -f '$C_LOG' ]]"
asrt "server log exists" "[[ -f '$S_LOG' ]]"

# Server explosion positions (authoritative).
mapfile -t S_LINES < <(
    grep -E '^\[SHOT \] t=[0-9]+ explosion at \([0-9.-]+,[0-9.-]+\) .* weapon=10' \
         "$S_LOG" 2>/dev/null
)
mapfile -t S_POSX < <(
    printf '%s\n' "${S_LINES[@]}" \
        | sed -E 's/.*at \(([0-9.-]+),[0-9.-]+\).*/\1/'
)
mapfile -t S_POSY < <(
    printf '%s\n' "${S_LINES[@]}" \
        | sed -E 's/.*at \([0-9.-]+,([0-9.-]+)\).*/\1/'
)

# Client handler positions.
mapfile -t C_LINES < <(
    grep -E 'client_handle_explosion .* weapon=10 at=\([0-9.-]+,[0-9.-]+\)' \
         "$C_LOG" 2>/dev/null
)
mapfile -t C_POSX < <(
    printf '%s\n' "${C_LINES[@]}" \
        | sed -E 's/.*at=\(([0-9.-]+),[0-9.-]+\).*/\1/'
)
mapfile -t C_POSY < <(
    printf '%s\n' "${C_LINES[@]}" \
        | sed -E 's/.*at=\([0-9.-]+,([0-9.-]+)\).*/\1/'
)

SCOUNT=${#S_LINES[@]}
CCOUNT=${#C_LINES[@]}

echo "  server explosions: $SCOUNT"
echo "  client handler events: $CCOUNT"
for i in "${!S_LINES[@]}"; do
    echo "    server[$i]: (${S_POSX[$i]}, ${S_POSY[$i]})"
done
for i in "${!C_LINES[@]}"; do
    echo "    client[$i]: (${C_POSX[$i]}, ${C_POSY[$i]})"
done

asrt "server logs at least 3 frag explosions" "[[ $SCOUNT -ge 3 ]]"
asrt "client receives at least 3 explosion events" "[[ $CCOUNT -ge 3 ]]"

# Per-event position parity: |client_pos - server_pos| ≤ 4 px on each
# axis. With M6 P12 snapshot replication this should be at wire quant
# (0.25 px). The 4 px envelope leaves room for any future rounding
# drift without making the test brittle.
MAX_PAIRS=$SCOUNT
[[ $CCOUNT -lt $MAX_PAIRS ]] && MAX_PAIRS=$CCOUNT
PARITY_FAILS=0
WORST_DX="0"; WORST_DY="0"
for ((i = 0; i < MAX_PAIRS; ++i)); do
    SX="${S_POSX[$i]}"; SY="${S_POSY[$i]}"
    CX="${C_POSX[$i]}"; CY="${C_POSY[$i]}"
    DX=$(awk -v a="$SX" -v b="$CX" 'BEGIN{d=a-b; if(d<0)d=-d; print d}')
    DY=$(awk -v a="$SY" -v b="$CY" 'BEGIN{d=a-b; if(d<0)d=-d; print d}')
    WORST_DX=$(awk -v a="$WORST_DX" -v b="$DX" 'BEGIN{print (a>b)?a:b}')
    WORST_DY=$(awk -v a="$WORST_DY" -v b="$DY" 'BEGIN{print (a>b)?a:b}')
    if ! awk -v d="$DX" 'BEGIN{exit !(d <= 4.0)}'; then PARITY_FAILS=$((PARITY_FAILS + 1)); fi
    if ! awk -v d="$DY" 'BEGIN{exit !(d <= 4.0)}'; then PARITY_FAILS=$((PARITY_FAILS + 1)); fi
done
echo "  worst Δx across $MAX_PAIRS pairs: $WORST_DX px"
echo "  worst Δy across $MAX_PAIRS pairs: $WORST_DY px"
asrt "every paired explosion position is within 4 px on both axes" \
    "[[ $PARITY_FAILS -eq 0 && $MAX_PAIRS -ge 3 ]]"

# Side-by-side composites for visual review.
if command -v montage >/dev/null 2>&1; then
    COMBO_DIR="$REPO/build/shots/net/dedi/2p_frag_sync_combined"
    mkdir -p "$COMBO_DIR"
    for h in "$H_OUT"/*.png; do
        bn="$(basename "$h")"
        [[ "$bn" == *_sheet.png ]] && continue
        c="$C_OUT/$bn"
        [[ ! -f "$c" ]] && continue
        montage "$h" "$c" -tile 2x1 -geometry +4+0 -background black \
                -title "${bn%.png}   host ↔ client" \
                "$COMBO_DIR/$bn" 2>/dev/null
    done
    montage "$COMBO_DIR"/*.png -tile 3x -geometry 800x+2+2 \
            -background "#101418" "$COMBO_DIR/combined_sheet.png" \
            2>/dev/null
    echo "  side-by-side combos in: $COMBO_DIR/"
fi

echo
echo "== frag-sync summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
