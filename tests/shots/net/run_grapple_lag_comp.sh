#!/usr/bin/env bash
#
# tests/shots/net/run_grapple_lag_comp.sh — verifies Phase 4.
#
# Drives 2p_grapple_bone where the CLIENT fires the Grappling Hook at
# the stationary host. Phase 4 wires the bone-collision branch of
# `projectile_step` for PROJ_GRAPPLE_HEAD to use `snapshot_lag_lookup`
# at the firer's `input_view_tick`. Without it, the swept-segment test
# runs current-time and the hook regularly misses a target the firer
# clearly saw on-screen at WAN ping.
#
# Assertions on the SERVER's SHOT_LOG:
#
#   - mech=1 (client) fires Grappling Hook         (`grapple_fire ph=`)
#   - mech=1 lands on a bone (tgt_mech=0, not -1)  (`grapple_attach`)
#   - the attach log has `lag_comp=N` with N > 0   — proves Phase 4's
#     bone rewind actually engaged on this shot.
#   - the rewound tick is in the expected band relative to `t=` —
#     [3, 12] tick offset covers loopback ENet RTT + 50 ms interp.
#
# Pre-Phase-4 build: `lag_comp=` field absent OR always 0 even when
# `mech=1` is the firer. Either makes the assertions below fail.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
NAME="2p_grapple_bone"

"$REPO/tests/shots/net/run.sh" "$NAME"
RC=$?

echo
echo "=== grapple-lag-comp assertions ==="

OUT="$REPO/build/shots/net"
HOST_OUT=$(find "$OUT" -maxdepth 1 -mindepth 1 -type d -name '*host*' | head -1)
HL=$(find "$HOST_OUT" -maxdepth 1 -name '*.log' | head -1)
[[ -n "$HL" ]] || { echo "fail: host log missing"; exit 1; }

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "client (mech=1) fires Grappling Hook" \
    "grep -qE 'mech=1 grapple_fire ph=' '$HL'"

asrt "grapple attached to host's body (tgt_mech=0)" \
    "grep -qE 'mech=1 grapple_attach .*tgt_mech=0' '$HL'"

asrt "attach log carries Phase 4's lag_comp= field" \
    "grep -qE 'mech=1 grapple_attach .*lag_comp=[0-9]+' '$HL'"

asrt "lag_comp value is non-zero (rewind engaged)" \
    "awk -F'lag_comp=' '/mech=1 grapple_attach/ { if (\$2+0 > 0) found = 1 } END { exit (found ? 0 : 1) }' '$HL'"

# Rewind tick = t - (rtt_half + interp) ≈ t - 4 at LAN loopback with
# Phase 2's 50ms interp. Same envelope as run_lag_comp.sh's hitscan
# test: [3, 12] covers warmup variance.
asrt "rewind offset in expected range (3..12 ticks)" \
    "awk -F'[ =,]' '/mech=1 grapple_attach .*lag_comp=/ {
        for (i = 1; i <= NF; i++) { if (\$i == \"t\")        tick   = \$(i+1); if (\$i == \"lag_comp\") rewind = \$(i+1); }
        gsub(/[^0-9]/, \"\", tick);
        diff = tick - rewind;
        if (diff >= 3 && diff <= 12) ok = 1;
    } END { exit (ok ? 0 : 1) }' '$HL'"

echo
echo "== grapple-lag-comp summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
