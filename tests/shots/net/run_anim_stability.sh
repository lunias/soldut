#!/usr/bin/env bash
#
# tests/shots/net/run_anim_stability.sh — verifies wan-fixes-3-followup.
#
# Drives `2p_anim_check` (host walks back and forth on Slipstream, no
# parallax) and asserts the client's anim_id for the remote mech
# (mech=0) is stable — i.e., it transitions only when the host
# actually starts / stops walking, not every 3 ticks because of the
# gait foot-lift flickering SNAP_STATE_GROUNDED.
#
# Pre-fix client log: ~50+ anim transitions over ~700 ticks
# (RUN ↔ FALL ↔ STAND once per gait swing).
# Post-fix client log: ~6 transitions matching scripted press/release.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
NAME="2p_anim_check"

"$REPO/tests/shots/net/run.sh" "$NAME"
RC=$?

echo
echo "=== anim-stability assertions ==="

OUT="$REPO/build/shots/net"
HOST_OUT=$(find "$OUT" -maxdepth 1 -mindepth 1 -type d -name '*host*' | head -1)
CLI_OUT=$(find  "$OUT" -maxdepth 1 -mindepth 1 -type d -name '*client*' | head -1)
HL=$(find "$HOST_OUT" -maxdepth 1 -name '*.log' | head -1)
CL=$(find "$CLI_OUT"  -maxdepth 1 -name '*.log' | head -1)
[[ -n "$HL" && -n "$CL" ]] || { echo "fail: logs missing"; exit 1; }

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

# Server's anim_id (host log mech=0). Should be stable — only changes
# at intentional run-stop / stop-run boundaries.
HOST_TRANSITIONS=$(grep -cE 'mech=0 anim' "$HL")
asrt "server-side anim_id transitions are bounded (<= 20 over the run)" \
    "[ \"$HOST_TRANSITIONS\" -le 20 ]"

# Client's anim_id for mech=0 (the remote host). Pre-fix this was
# 50+ flickers; post-fix should be the same number-ish as the server.
CLI_TRANSITIONS=$(grep -cE 'mech=0 anim' "$CL")
asrt "client-side anim_id transitions are bounded (<= 20 over the run)" \
    "[ \"$CLI_TRANSITIONS\" -le 20 ]"

# Specifically: no spurious FALL transitions during steady-state
# walking. The host script never jumps / falls (only runs left/right
# on flat ground), so post-fix there should be 0..1 FALL entries on
# the client side. Pre-fix this was the dominant transition.
CLI_FALL_FLIPS=$(grep -cE 'mech=0 anim .*->FALL' "$CL")
asrt "no spurious FALL transitions on client during walking (<= 2)" \
    "[ \"$CLI_FALL_FLIPS\" -le 2 ]"

# Confirm SNAP_STATE_RUNNING bit is in use (server is shipping it).
# We don't have a direct log line for the bit, but the existence of
# stable RUN periods on the client (without velocity-based jitter)
# implies it.
asrt "client logs at least one RUN entry for the remote mech" \
    "grep -qE 'mech=0 anim .*->RUN' '$CL'"

echo
echo "== anim-stability summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
