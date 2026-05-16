#!/usr/bin/env bash
#
# tests/shots/net/run_frag_grenade.sh — wan-fixes-10 regression.
#
# Verifies that AOE explosions (frag grenades) render at the SERVER's
# authoritative position on both clients, NOT at each client's local
# detonate position (which would differ by 5–10 px because remote
# mechs render in rest pose per wan-fixes-3 while the server's mechs
# are in animated poses).
#
# Both clients connect to a dedicated server on Crossfire. Client A
# (slot 0) throws three frag grenades at client B (slot 1). Client B
# observes. We assert:
#   - Server logs at least 3 explosions.
#   - Both clients receive NET_MSG_EXPLOSION via client_handle_explosion.
#   - The server's explosion positions and the clients' handler
#     positions match (within 1/4 px wire quantization).
#   - Client-side projectile_step detonate did NOT emit its own
#     "explosion at" line — only the event-handler-driven path does.
#
# Pre-fix (before wan-fixes-10): clients ran explosion_spawn from
# their own projectile_step detonate against rest-pose remote bones,
# landing the visual ~10 px off from where damage was applied. With
# the fix, the visual + damage line up.

set -u

# Default to client_a throws. Run with `-b` to test client_b throws
# (symmetric — verifies both peers can trigger server-side detonation
# and broadcast EXPLOSION events to the other peer).
NAME="2p_frag_grenade_a"
while [[ $# -gt 0 ]]; do
    case "$1" in
        -b) NAME="2p_frag_grenade_b"; shift ;;
        -a) NAME="2p_frag_grenade_a"; shift ;;
        *)  echo "usage: $0 [-a|-b]"; exit 1 ;;
    esac
done
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

A_SHOT="$REPO/tests/shots/net/${NAME}.client_a.shot"
B_SHOT="$REPO/tests/shots/net/${NAME}.client_b.shot"
[[ -f "$A_SHOT" ]] || { echo "fail: $A_SHOT missing"; exit 1; }
[[ -f "$B_SHOT" ]] || { echo "fail: $B_SHOT missing"; exit 1; }

OUT="$REPO/build/shots/net"
mkdir -p "$OUT"

PORT=$((25100 + RANDOM % 200))
TMP="$(mktemp -d -t soldut-frag-XXXXXX)"
SRV_DIR="$TMP/server"
mkdir -p "$SRV_DIR"
cat > "$SRV_DIR/soldut.cfg" <<EOF
port=$PORT
auto_start_seconds=1
countdown_default=1
time_limit=10
score_limit=10
mode=ffa
map_rotation=crossfire
mode_rotation=ffa
snapshot_hz=60
EOF

SRV_PID=0
A_PID=0
B_PID=0
cleanup() {
    [[ $A_PID -gt 0 ]] && kill     "$A_PID"   2>/dev/null
    [[ $B_PID -gt 0 ]] && kill     "$B_PID"   2>/dev/null
    sleep 0.3
    [[ $A_PID -gt 0 ]] && kill -9  "$A_PID"   2>/dev/null
    [[ $B_PID -gt 0 ]] && kill -9  "$B_PID"   2>/dev/null
    [[ $SRV_PID -gt 0 ]] && kill     "$SRV_PID" 2>/dev/null
    sleep 0.3
    [[ $SRV_PID -gt 0 ]] && kill -9  "$SRV_PID" 2>/dev/null
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
    cat "$SRV_DIR/soldut-server.log" 2>/dev/null | tail -10
    exit 1
fi

A_TMP="$TMP/$(basename "$A_SHOT")"
B_TMP="$TMP/$(basename "$B_SHOT")"
sed -e "s/{{PORT}}/$PORT/g" "$A_SHOT" > "$A_TMP"
sed -e "s/{{PORT}}/$PORT/g" "$B_SHOT" > "$B_TMP"

"$BIN" --shot "$A_TMP" >/dev/null 2>&1 &
A_PID=$!
sleep 0.15
"$BIN" --shot "$B_TMP" >/dev/null 2>&1 &
B_PID=$!

TIMEOUT_S="${TEST_TIMEOUT_S:-30}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    if [[ ! -d /proc/$A_PID && ! -d /proc/$B_PID ]]; then
        break
    fi
done

[[ -d /proc/$A_PID ]] && kill "$A_PID" 2>/dev/null
[[ -d /proc/$B_PID ]] && kill "$B_PID" 2>/dev/null
sleep 0.3
[[ $SRV_PID -gt 0 ]] && kill "$SRV_PID" 2>/dev/null
sleep 0.3
[[ $SRV_PID -gt 0 ]] && kill -9 "$SRV_PID" 2>/dev/null

A_OUT="$REPO/build/shots/net/dedi/${NAME}.client_a"
B_OUT="$REPO/build/shots/net/dedi/${NAME}.client_b"
A_LOG="$A_OUT/${NAME}.client_a.log"
B_LOG="$B_OUT/${NAME}.client_b.log"
S_LOG="$SRV_DIR/soldut-server.log"

echo
echo "=== frag-grenade assertions ==="

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "server reaches listening" \
    "grep -q 'dedicated: listening on $PORT' '$S_LOG'"
asrt "both clients connect" \
    "grep -qE 'ACCEPT client_id=' '$A_LOG' && grep -qE 'ACCEPT client_id=' '$B_LOG'"

# Server-side detonation. Three frag grenades thrown by mech=0.
# (Radius is intentionally NOT pinned — the M6 ship-prep frag-buff
# bumped aoe_radius 140 → 180 and we don't want to chase the literal
# here when the assertion is "this many AOE detonations from weapon=10
# happened.")
SRV_EXPLOSION_COUNT=$(grep -cE '^\[SHOT \] t=[0-9]+ explosion at \([0-9.]+,[0-9.]+\) r=[0-9.]+ .* weapon=10' "$S_LOG" 2>/dev/null || echo 0)
asrt "server logs at least 3 frag explosions" \
    "[[ $SRV_EXPLOSION_COUNT -ge 3 ]]"

# Client-side EXPLOSION handling. wan-fixes-10's wire event drives the
# visual explosion on both clients via client_handle_explosion.
A_HANDLE_COUNT=$(grep -cE 'client_handle_explosion .* weapon=10' "$A_LOG" 2>/dev/null || echo 0)
B_HANDLE_COUNT=$(grep -cE 'client_handle_explosion .* weapon=10' "$B_LOG" 2>/dev/null || echo 0)
asrt "client_a handles at least 3 explosion events" \
    "[[ $A_HANDLE_COUNT -ge 3 ]]"
asrt "client_b handles at least 3 explosion events" \
    "[[ $B_HANDLE_COUNT -ge 3 ]]"

# Position parity — every "explosion at" log line on the client side
# should match a "client_handle_explosion" on the same tick (i.e. the
# explosion visual ONLY fires via the wire event, not via the local
# projectile-step detonate path which used to land at the wrong pos).
A_LOCAL_EXPLOSIONS=$(grep -cE '^\[SHOT \] t=[0-9]+ explosion at' "$A_LOG" 2>/dev/null || echo 0)
B_LOCAL_EXPLOSIONS=$(grep -cE '^\[SHOT \] t=[0-9]+ explosion at' "$B_LOG" 2>/dev/null || echo 0)
asrt "client_a's explosion-visual count matches its handler count (no double-explode)" \
    "[[ $A_LOCAL_EXPLOSIONS -eq $A_HANDLE_COUNT ]]"
asrt "client_b's explosion-visual count matches its handler count (no double-explode)" \
    "[[ $B_LOCAL_EXPLOSIONS -eq $B_HANDLE_COUNT ]]"

# Position parity — first explosion's server pos should match what the
# clients show. Extract first server-side position; both clients should
# report the same value (with 1/4 px wire-quantization rounding).
SRV_POS=$(grep -E '^\[SHOT \] t=[0-9]+ explosion at \([0-9.]+,[0-9.]+\) r=[0-9.]+' "$S_LOG" 2>/dev/null | head -1 | sed -E 's/.*at \(([0-9.]+),([0-9.]+)\).*/\1 \2/')
A_POS=$(grep -E 'client_handle_explosion .* at=\([0-9.]+,[0-9.]+\)' "$A_LOG" 2>/dev/null | head -1 | sed -E 's/.*at=\(([0-9.]+),([0-9.]+)\).*/\1 \2/')
B_POS=$(grep -E 'client_handle_explosion .* at=\([0-9.]+,[0-9.]+\)' "$B_LOG" 2>/dev/null | head -1 | sed -E 's/.*at=\(([0-9.]+),([0-9.]+)\).*/\1 \2/')

echo "  server first explosion at: $SRV_POS"
echo "  client_a first handler at: $A_POS"
echo "  client_b first handler at: $B_POS"

# Compare client_a vs client_b — they MUST agree exactly since both
# decode from the same wire quantization.
asrt "client_a and client_b agree on first explosion position" \
    "[[ -n '$A_POS' && '$A_POS' == '$B_POS' ]]"

# Server pos quantizes at 1/4 px; allow a small fuzz when comparing.
# Server: full-precision float. Client: quantized to nearest 0.25.
# Difference should be ≤ 0.25.
if [[ -n "$SRV_POS" && -n "$A_POS" ]]; then
    SRV_X=$(echo "$SRV_POS" | awk '{print $1}')
    A_X=$(echo "$A_POS" | awk '{print $1}')
    DIFF=$(awk -v a="$SRV_X" -v b="$A_X" 'BEGIN{d=a-b; if(d<0)d=-d; print d}')
    asrt "client position within wire quant of server (Δx ≤ 0.25 px)" \
        "awk -v d='$DIFF' 'BEGIN{exit !(d <= 0.25)}'"
fi

echo
echo "== frag-grenade summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
