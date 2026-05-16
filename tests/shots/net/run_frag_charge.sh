#!/usr/bin/env bash
#
# tests/shots/net/run_frag_charge.sh — M6 ship-prep regression for
# the hold-to-charge frag-grenade throw + parabolic physics.
#
# Host/client pair on Aurora's flat central floor (cols 50–73). Host
# at x=1600, client at x=2200 — 600 px gap so both mechs are visible
# in each viewport at 1600×900 windowed. Both throw frag grenades at
# each other (staggered: host @ 210→240 medium, client @ 260→320 max,
# host @ 380→440 max, client @ 480→510 medium). Captures cover each
# throw's charging frame, launch, arc apex, descent, and boom.
#
# Assertions:
#   - Server logs ≥ 4 spawn_throw events (2 from each peer).
#   - Charge factors cover the intended range: at least one ≤ 0.60
#     and at least one ≥ 0.95 — confirms the accumulator scales.
#   - Velocities scale: at least one ≤ 1000 px/s, at least one
#     ≥ 1200 px/s.
#   - Server logs ≥ 4 explosion events for weapon=10.
#   - Both peers handle ≥ 4 client_handle_explosion events
#     (wire round-trip).

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

NAME="2p_frag_charge"
HOST_SHOT="$REPO/tests/shots/net/${NAME}.host.shot"
CLI_SHOT="$REPO/tests/shots/net/${NAME}.client.shot"
[[ -f "$HOST_SHOT" ]] || { echo "fail: $HOST_SHOT missing"; exit 1; }
[[ -f "$CLI_SHOT" ]]  || { echo "fail: $CLI_SHOT missing";  exit 1; }

OUT="$REPO/build/shots/net"
mkdir -p "$OUT"

HOST_PID=0
CLI_PID=0
cleanup() {
    [[ $CLI_PID  -gt 0 ]] && kill     "$CLI_PID"  2>/dev/null
    [[ $HOST_PID -gt 0 ]] && kill     "$HOST_PID" 2>/dev/null
    sleep 0.3
    [[ $CLI_PID  -gt 0 ]] && kill -9  "$CLI_PID"  2>/dev/null
    [[ $HOST_PID -gt 0 ]] && kill -9  "$HOST_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

cd "$REPO"

# Host first; brief delay so the listener is up before the client
# connects (same pattern as tests/shots/net/run.sh).
"$BIN" --shot "$HOST_SHOT" >/dev/null 2>&1 &
HOST_PID=$!
sleep 0.25
"$BIN" --shot "$CLI_SHOT" >/dev/null 2>&1 &
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
[[ -d /proc/$CLI_PID ]] && kill -9 "$CLI_PID" 2>/dev/null
[[ -d /proc/$HOST_PID ]] && kill -9 "$HOST_PID" 2>/dev/null

H_OUT="$REPO/build/shots/net/2p_frag_charge_host"
C_OUT="$REPO/build/shots/net/2p_frag_charge_client"
H_LOG="$H_OUT/${NAME}.host.log"
C_LOG="$C_OUT/${NAME}.client.log"

echo
echo "=== frag-charge assertions ==="

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

# Host log is authoritative (host runs the simulation).
asrt "host log exists" "[[ -f '$H_LOG' ]]"
asrt "client log exists" "[[ -f '$C_LOG' ]]"

# Four spawn_throw events (host throws 2, client throws 2 — both
# show up in the host's authoritative log because the host owns
# simulate for everyone).
THROW_COUNT=$(grep -cE '^\[SHOT \] t=[0-9]+ spawn_throw .* wpn=10' "$H_LOG" 2>/dev/null || echo 0)
asrt "host logs at least 4 spawn_throw events" \
    "[[ $THROW_COUNT -ge 4 ]]"

# Pull out the charge values + speeds.
mapfile -t CHARGE_LINES < <(
    grep -E '^\[SHOT \] t=[0-9]+ spawn_throw .* wpn=10 charge=' "$H_LOG" 2>/dev/null
)
echo "  spawn_throw events (host log):"
for L in "${CHARGE_LINES[@]}"; do
    echo "    $L" | sed -E 's/.*(t=[0-9]+ spawn_throw mech=[0-9]+ wpn=10 charge=[0-9.]+ v=[0-9.]+).*/    \1/'
done

# Scan for one ≤0.60 (medium) and one ≥0.95 (max).
HAS_LOW=0; HAS_HIGH=0
for L in "${CHARGE_LINES[@]}"; do
    C=$(echo "$L" | sed -E 's/.*charge=([0-9.]+).*/\1/')
    if awk -v c="$C" 'BEGIN{exit !(c <= 0.60 && c > 0.0)}'; then HAS_LOW=1; fi
    if awk -v c="$C" 'BEGIN{exit !(c >= 0.95)}';            then HAS_HIGH=1; fi
done
asrt "at least one throw is medium charge (≤ 0.60)" "[[ $HAS_LOW  -eq 1 ]]"
asrt "at least one throw is max charge (≥ 0.95)"    "[[ $HAS_HIGH -eq 1 ]]"

# Speed range covers both buckets. After the FRAG_THROW_SPEED_MAX_MUL
# bump (2.0 → 2.4) max-charge throws are now ~1680 px/s.
HAS_SLOW=0; HAS_FAST=0
for L in "${CHARGE_LINES[@]}"; do
    V=$(echo "$L" | sed -E 's/.*v=([0-9.]+).*/\1/')
    if awk -v v="$V" 'BEGIN{exit !(v <= 1200)}'; then HAS_SLOW=1; fi
    if awk -v v="$V" 'BEGIN{exit !(v >= 1500)}'; then HAS_FAST=1; fi
done
asrt "at least one throw is sub-max speed (≤ 1200 px/s)" "[[ $HAS_SLOW -eq 1 ]]"
asrt "at least one throw is full-charge speed (≥ 1500 px/s)" "[[ $HAS_FAST -eq 1 ]]"

# AOE detonations (host).
EXPL_COUNT=$(grep -cE '^\[SHOT \] t=[0-9]+ explosion at .* weapon=10' "$H_LOG" 2>/dev/null || echo 0)
asrt "host logs at least 4 frag explosions" \
    "[[ $EXPL_COUNT -ge 4 ]]"

# Client receives the explosion events via the wire. (The host IS the
# authoritative server — explosions are spawned there directly, no
# client_handle_explosion needed; only the remote peer dispatches it.)
C_HANDLE=$(grep -cE 'client_handle_explosion .* weapon=10' "$C_LOG" 2>/dev/null)
[[ -z "$C_HANDLE" ]] && C_HANDLE=0
asrt "client handles at least 4 explosion events (wire round-trip)" \
    "[[ $C_HANDLE -ge 4 ]]"

# ---- Side-by-side composites ----------------------------------------
# For every shot name present in BOTH dirs, stack host (left) +
# client (right) into a single PNG. Then build a contact sheet so a
# reviewer can spot mech-vs-mech action at a glance.
if command -v montage >/dev/null 2>&1; then
    COMBO_DIR="$REPO/build/shots/net/2p_frag_charge_combined"
    mkdir -p "$COMBO_DIR"
    for h in "$H_OUT"/*.png; do
        bn="$(basename "$h")"
        # Skip contact sheets and side-by-side outputs.
        [[ "$bn" == *_sheet.png ]] && continue
        c="$C_OUT/$bn"
        [[ ! -f "$c" ]] && continue
        montage "$h" "$c" -tile 2x1 -geometry +4+0 -background black \
                -title "${bn%.png}   host ↔ client" \
                "$COMBO_DIR/$bn" 2>/dev/null
    done
    # Roll a contact sheet of the combined frames.
    montage "$COMBO_DIR"/*.png -tile 3x -geometry 800x+2+2 \
            -background "#101418" "$COMBO_DIR/combined_sheet.png" \
            2>/dev/null
    echo "  side-by-side combos in: $COMBO_DIR/"
fi

echo
echo "== frag-charge summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
