#!/usr/bin/env bash
#
# tests/shots/net/run_lag_comp.sh — verifies Phase 1 lag-comp wiring.
#
# Drives the existing 2p_combat scenario (host and client both
# fire hitscan rifles at each other) and asserts the server-side
# SHOT_LOG records show:
#
#   - host's own shots (mech=0) take the no-lag-comp path
#     (`weapons_fire_hitscan` → log line has NO `lag_comp=` suffix).
#     The host has 0 RTT to itself; rewinding bone history would
#     be wrong.
#
#   - client's shots (mech=1) take the lag-comp path
#     (`weapons_fire_hitscan_lag_comp` → log line ends with
#     `lag_comp=N` where N is the rewound server tick). At
#     loopback latency N ≈ tick - 7 (rtt_half ~1 + interp 6).
#
# Pre-Phase-1 the wire path was hardcoded to `weapons_fire_hitscan`
# regardless of shooter — the `lag_comp=` suffix never appeared for
# anyone. This test fails on that build.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
NAME="2p_combat"

"$REPO/tests/shots/net/run.sh" "$NAME"
RC=$?

echo
echo "=== lag-comp assertions ==="

OUT="$REPO/build/shots/net"
HOST_OUT=$(find "$OUT" -maxdepth 1 -mindepth 1 -type d -name '*host*' | head -1)
HL=$(find "$HOST_OUT" -maxdepth 1 -name '*.log' | head -1)
[[ -n "$HL" ]] || { echo "fail: no host log under $OUT"; exit 1; }

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host fires (mech=0) without lag_comp= suffix" \
    "grep -E 'fire mech=0 wpn=' '$HL' | grep -qv 'lag_comp='"

asrt "client fires (mech=1) with lag_comp= suffix" \
    "grep -qE 'fire mech=1 wpn=.* lag_comp=[0-9]+' '$HL'"

# The rewound tick must be in [tick-LAG_HIST_TICKS, tick]. At LAN
# loopback rtt_half_ticks ≈ 1, interp_delay = 6 ticks → expected
# offset ≈ 7. We allow [3, 12] to cover ENet warmup variance.
asrt "client lag-comp rewind offset is in expected range (3..12 ticks)" \
    "awk -F'[ =,]' '/fire mech=1.*lag_comp=/ {
        for (i = 1; i <= NF; i++) { if (\$i == \"t\")        tick   = \$(i+1); if (\$i == \"lag_comp\") rewind = \$(i+1); }
        gsub(/[^0-9]/, \"\", tick);
        diff = tick - rewind;
        if (diff >= 3 && diff <= 12) ok = 1;
    } END { exit (ok ? 0 : 1) }' '$HL'"

# At least one hit should actually connect via the lag-comp path —
# proves the rewound bone positions are real, not just a code-path
# log entry sitting next to a miss.
asrt "client lag-comp shot connects (hit mech=0)" \
    "grep -qE 'fire mech=1 wpn=.* hit mech=0.* lag_comp=' '$HL'"

echo
echo "== lag-comp summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
