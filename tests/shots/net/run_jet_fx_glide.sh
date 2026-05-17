#!/usr/bin/env bash
#
# tests/shots/net/run_jet_fx_glide.sh — M6 jet-pack FX coverage for
# the Glide-wing and JumpJet variants.
#
# Pairs with 2p_jet_fx_glide.{host,client}.shot. Host runs Glide-
# wing (intermittent pale-cyan wisps; sustain divisor = 4 ticks);
# client runs JumpJet (discrete kick on every press, green-cyan
# ignition flash, NO continuous plume).
#
# This is the sibling of run_jet_fx.sh — same shape, different
# asserts:
#   - Atlases load on both peers (same plume + dust assets).
#   - Glide host fires ignition at takeoff (1 nozzle continuous-plume
#     entry in the chassis table; the 2-nozzle mirror is a JET_BURST
#     thing).
#   - JumpJet client fires ignition on each BTN_JET tap. The shot
#     script taps three times; each tap is a discrete impulse with a
#     fresh ignition flash.
#   - Side-by-side composite emitted for visual review.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

NAME="2p_jet_fx_glide"
HOST_SHOT="$REPO/tests/shots/net/${NAME}.host.shot"
CLI_SHOT="$REPO/tests/shots/net/${NAME}.client.shot"
[[ -f "$HOST_SHOT" ]] || { echo "fail: $HOST_SHOT missing"; exit 1; }
[[ -f "$CLI_SHOT" ]]  || { echo "fail: $CLI_SHOT missing";  exit 1; }

rm -rf "$REPO/build/shots/net/${NAME}.host" \
       "$REPO/build/shots/net/${NAME}.client" \
       "$REPO/build/shots/net/${NAME}_combined"

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

"$BIN" --shot "$HOST_SHOT" >/dev/null 2>&1 &
HOST_PID=$!
sleep 0.25
"$BIN" --shot "$CLI_SHOT" >/dev/null 2>&1 &
CLI_PID=$!

TIMEOUT_S="${TEST_TIMEOUT_S:-30}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    if [[ ! -d /proc/$HOST_PID && ! -d /proc/$CLI_PID ]]; then
        break
    fi
done

[[ -d /proc/$CLI_PID  ]] && kill "$CLI_PID"  2>/dev/null
[[ -d /proc/$HOST_PID ]] && kill "$HOST_PID" 2>/dev/null
sleep 0.3
[[ -d /proc/$CLI_PID  ]] && kill -9 "$CLI_PID"  2>/dev/null
[[ -d /proc/$HOST_PID ]] && kill -9 "$HOST_PID" 2>/dev/null

H_OUT="$REPO/build/shots/net/${NAME}.host"
C_OUT="$REPO/build/shots/net/${NAME}.client"
H_LOG="$H_OUT/${NAME}.host.log"
C_LOG="$C_OUT/${NAME}.client.log"

echo
echo "=== jet-fx (glide / jump-jet) assertions ==="

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host log exists"   "[[ -f '$H_LOG' ]]"
asrt "client log exists" "[[ -f '$C_LOG' ]]"

asrt "host loaded plume atlas" \
    "grep -q 'jet_fx: loaded plume atlas' '$H_LOG'"
asrt "host loaded dust atlas" \
    "grep -q 'jet_fx: loaded dust atlas'  '$H_LOG'"
asrt "client loaded plume atlas" \
    "grep -q 'jet_fx: loaded plume atlas' '$C_LOG'"
asrt "client loaded dust atlas" \
    "grep -q 'jet_fx: loaded dust atlas'  '$C_LOG'"

# Glide-host takeoff produces at least one ignition impingement
# (continuous-plume jet with grounded → airborne edge).
H_IGN=$(grep -cE '^\[SHOT \] t=[0-9]+ mech=[01] jet_ignition surf=' "$H_LOG" 2>/dev/null)
[[ -z "$H_IGN" ]] && H_IGN=0
asrt "host logs at least 1 jet_ignition event (Glide takeoff)" \
    "[[ $H_IGN -ge 1 ]]"
asrt "host did NOT re-fire ignition across snapshot gap" "[[ $H_IGN -le 10 ]]"

# JumpJet on the client: every BTN_JET press is a discrete impulse
# that sets IGNITION_TICK on the press tick (see mech.c jet_jump_active
# branch). Three taps in the script → 3 ignitions on the local mech.
# Allow ≥2 to absorb the case where the third tap lands while the
# mech is still airborne and the takeoff edge doesn't fire.
C_IGN=$(grep -cE '^\[SHOT \] t=[0-9]+ mech=[01] jet_ignition surf=' "$C_LOG" 2>/dev/null)
[[ -z "$C_IGN" ]] && C_IGN=0
asrt "client logs at least 2 jet_ignition events (JumpJet taps)" \
    "[[ $C_IGN -ge 2 ]]"
asrt "client did NOT re-fire ignition across snapshot gap" "[[ $C_IGN -le 12 ]]"

# Each ignition emits a dust burst (n=8 per nozzle on takeoff per
# emit_ground_dust's boost=1 branch). At minimum match the ignition
# count one-for-one — the ground-query reach matches.
H_DUST=$(grep -cE '^\[SHOT \] t=[0-9]+ jet_dust hit=' "$H_LOG" 2>/dev/null)
[[ -z "$H_DUST" ]] && H_DUST=0
asrt "host logs at least 1 jet_dust spawn burst" "[[ $H_DUST -ge 1 ]]"

C_DUST=$(grep -cE '^\[SHOT \] t=[0-9]+ jet_dust hit=' "$C_LOG" 2>/dev/null)
[[ -z "$C_DUST" ]] && C_DUST=0
asrt "client logs at least 2 jet_dust spawn bursts" "[[ $C_DUST -ge 2 ]]"

if command -v montage >/dev/null 2>&1; then
    COMBO_DIR="$REPO/build/shots/net/${NAME}_combined"
    mkdir -p "$COMBO_DIR"
    for h in "$H_OUT"/*.png; do
        bn="$(basename "$h")"
        [[ "$bn" == sheet.png ]] && continue
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
echo "== jet-fx (glide / jump-jet) summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
