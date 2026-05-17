#!/usr/bin/env bash
#
# tests/shots/net/run_jet_fx.sh — M6 jet-pack FX regression.
#
# Drives the existing 2p_jet_fx.{host,client}.shot pair: host on
# Burst, client on Standard, both jetting from the Reactor floor.
# Asserts the SHOT_LOG signals that prove the jet FX pipeline is
# alive end-to-end:
#   - Plume + dust atlases load on both peers.
#   - `jet_burst` boost SFX fires on the host (Burst is the only
#     loadout with a discrete boost trigger).
#   - `jet_ignition` impingement fires on both peers for the local +
#     remote takeoff edges (without re-firing across the snapshot
#     gap — pre-fix, REMOTE-mech ignition fired every tick between
#     applies because MECH_JET_IGNITION_TICK persisted; the fix
#     consumes the bit at the end of mech_jet_fx_step).
#   - `jet_dust` ground-impingement bursts spawn at least once on
#     each peer at takeoff (n=8 per burst with boost=1 — boost path
#     gates the wider fan).
#
# Side-by-side host↔client composite of every captured tick lands
# in build/shots/net/2p_jet_fx_combined/ for visual review.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

NAME="2p_jet_fx"
HOST_SHOT="$REPO/tests/shots/net/${NAME}.host.shot"
CLI_SHOT="$REPO/tests/shots/net/${NAME}.client.shot"
[[ -f "$HOST_SHOT" ]] || { echo "fail: $HOST_SHOT missing"; exit 1; }
[[ -f "$CLI_SHOT" ]]  || { echo "fail: $CLI_SHOT missing";  exit 1; }

# Clear prior captures so a stale PNG from a hung previous run can't
# masquerade as a pass.
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
echo "=== jet-fx assertions ==="

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host log exists"   "[[ -f '$H_LOG' ]]"
asrt "client log exists" "[[ -f '$C_LOG' ]]"

# Atlases must load on both peers — the M6 P02 spec ships both as
# real authored gradients; a missing-file failure shows up as the
# absence of these INFO lines and a regression to the octagon
# fallback path.
asrt "host loaded plume atlas" \
    "grep -q 'jet_fx: loaded plume atlas' '$H_LOG'"
asrt "host loaded dust atlas" \
    "grep -q 'jet_fx: loaded dust atlas'  '$H_LOG'"
asrt "client loaded plume atlas" \
    "grep -q 'jet_fx: loaded plume atlas' '$C_LOG'"
asrt "client loaded dust atlas" \
    "grep -q 'jet_fx: loaded dust atlas'  '$C_LOG'"

# Burst boost SFX fires once on the host's discrete dash-press trigger.
H_BURST=$(grep -cE '^\[SHOT \] t=[0-9]+ mech=0 jet_burst sfx=jet_boost' "$H_LOG" 2>/dev/null)
[[ -z "$H_BURST" ]] && H_BURST=0
asrt "host fires SFX_JET_BOOST at least once" "[[ $H_BURST -ge 1 ]]"

# Ignition impingement fires on the takeoff edge for each mech with
# `mech_jet_fx_ground_query` reaching the floor (the M6 jet-fx-polish
# bump from 48px → 80px reach is what unblocks this for Trooper).
# Host should log 2 events from its own Burst mech (2 nozzles) + 1
# from the remote Standard (1 nozzle). Allow ≥2 here so the test
# tolerates the remote-mech tick where the snapshot arrives slightly
# late and the local takeoff edge fires first.
H_IGN=$(grep -cE '^\[SHOT \] t=[0-9]+ mech=[01] jet_ignition surf=' "$H_LOG" 2>/dev/null)
[[ -z "$H_IGN" ]] && H_IGN=0
asrt "host logs at least 2 jet_ignition events" "[[ $H_IGN -ge 2 ]]"

# The IGNITION-edge-consume guard prevents the bit from re-firing
# across the snapshot gap. Pre-fix the same takeoff would log 30+
# events per second per nozzle while the bit lingered. Cap at 10 to
# catch any future regression that drops the consume.
asrt "host did NOT re-fire ignition across snapshot gap" "[[ $H_IGN -le 10 ]]"

C_IGN=$(grep -cE '^\[SHOT \] t=[0-9]+ mech=[01] jet_ignition surf=' "$C_LOG" 2>/dev/null)
[[ -z "$C_IGN" ]] && C_IGN=0
asrt "client logs at least 2 jet_ignition events" "[[ $C_IGN -ge 2 ]]"
asrt "client did NOT re-fire ignition across snapshot gap" "[[ $C_IGN -le 10 ]]"

# Ground-impingement dust spawned at least once on each peer. The
# bursts are per-nozzle so a 2-nozzle Burst produces 2 spawn events
# in the same tick (each n=8 particles into the FX pool).
H_DUST=$(grep -cE '^\[SHOT \] t=[0-9]+ jet_dust hit=' "$H_LOG" 2>/dev/null)
[[ -z "$H_DUST" ]] && H_DUST=0
asrt "host logs at least 2 jet_dust spawn bursts" "[[ $H_DUST -ge 2 ]]"

C_DUST=$(grep -cE '^\[SHOT \] t=[0-9]+ jet_dust hit=' "$C_LOG" 2>/dev/null)
[[ -z "$C_DUST" ]] && C_DUST=0
asrt "client logs at least 2 jet_dust spawn bursts" "[[ $C_DUST -ge 2 ]]"

# Side-by-side composites. Same pattern as run_frag_concourse.sh
# (the user already reviews diffs this way for the grenade test).
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
echo "== jet-fx summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
