#!/usr/bin/env bash
#
# tests/shots/net/run_snapshot_rate.sh — verifies Phase 2.
#
# Drives 2p_basic (the lightest scenario that goes through ACCEPT +
# INITIAL_STATE + a real broadcast round) and asserts:
#
#   - server logs `snapshot=60 Hz, interp=50 ms, range_coder=on`
#     (Phase 2 default — was 30 Hz / 100 ms / off pre-Phase-2).
#   - client logs `ACCEPT ... snapshot_hz=60 interp=50 ms`, meaning
#     the rate flowed through the ACCEPT wire field (Phase 2 added).
#   - client's render clock is initialized from the host rate
#     (snapshot decode log shows interp matching).
#
# Pre-Phase-2 build: server line lacks the snapshot/interp/range_coder
# suffix; client ACCEPT log doesn't include snapshot_hz. Either makes
# the asserts below fail.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
NAME="2p_basic"

"$REPO/tests/shots/net/run.sh" "$NAME"
RC=$?

echo
echo "=== snapshot-rate assertions ==="

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

asrt "host advertises 60 Hz snapshot rate" \
    "grep -q 'snapshot=60 Hz' '$HL'"
asrt "host advertises 50 ms interp delay" \
    "grep -q 'interp=50 ms' '$HL'"
asrt "host enables ENet range coder" \
    "grep -q 'range_coder=on' '$HL'"
asrt "client picks up 60 Hz from ACCEPT" \
    "grep -qE 'ACCEPT .*snapshot_hz=60' '$CL'"
asrt "client picks up 50 ms interp from ACCEPT" \
    "grep -qE 'ACCEPT .*interp=50 ms' '$CL'"

echo
echo "== snapshot-rate summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
