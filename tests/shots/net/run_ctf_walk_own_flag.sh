#!/usr/bin/env bash
#
# tests/shots/net/run_ctf_walk_own_flag.sh — repro driver for the
# "client gets stuck on their own flag" bug. Wraps the generic
# tests/shots/net/run.sh with a temporary CTF cfg, runs the paired
# 2p_ctf_walk_own_flag shots, then dumps the per-tick pelvis trace
# for the local-mech on each side so we can compare host's view of
# mech=1 against the client's view of its own local mech.
#
# Usage:  tests/shots/net/run_ctf_walk_own_flag.sh

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
CFG="$REPO/soldut.cfg"
BACKUP=""

if [[ -f "$CFG" ]]; then
    BACKUP="$REPO/soldut.cfg.bak.$$"
    mv "$CFG" "$BACKUP"
fi
restore() {
    rm -f "$CFG"
    if [[ -n "$BACKUP" && -f "$BACKUP" ]]; then mv "$BACKUP" "$CFG"; fi
}
trap restore EXIT

cat > "$CFG" <<EOF
auto_start_seconds=2
time_limit=20
score_limit=5
mode=ctf
friendly_fire=1
map_rotation=crossfire
mode_rotation=ctf
EOF

"$REPO/tests/shots/net/run.sh" 2p_ctf_walk_own_flag
RC=$?

HOST_LOG="$REPO/build/shots/net/host_walk_own_flag/2p_ctf_walk_own_flag.host.log"
CLI_LOG="$REPO/build/shots/net/client_walk_own_flag/2p_ctf_walk_own_flag.client.log"

# Regression assertions for the snapshot-quant overflow bug. Before the
# fix, quant_pos's i16 + 8× factor capped pos at ~4096 px, so client
# snapshots for any mech east of x=4096 (the BLUE base on Crossfire,
# including the BLUE flag at x=4160) silently wrapped to x=4095. The
# client's local mech kept being snapped back to x=4095 every frame,
# looking exactly like "stuck on their own flag." After the fix
# (factor 4× → ~8190 px max) the client should sweep cleanly through
# its own flag at x=4160 just like the server does.
echo
echo "=== walk-own-flag (client-side) regression assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS+1));
    else echo "FAIL: $1"; FAIL=$((FAIL+1)); fi
}

# 1. The client's local mech pelvis must reach x>=4159 (the BLUE flag).
#    Pre-fix value was ~4111 (peak of the predict-snap-back oscillation).
asrt "client local mech crosses BLUE flag (x>=4159)" \
     "grep -oE 'mech=1\\* pelv=\\(4[12][0-9][0-9]\\.[0-9]+' '$CLI_LOG' \
        | grep -oE '[0-9]+\\.[0-9]+' \
        | awk 'BEGIN{f=0} {if (\$0 + 0 >= 4159) f=1} END{exit (f?0:1)}'"

# 2. After release at script tick 320, the local mech should have
#    travelled at least 100 px east of spawn (4096) — proving prediction
#    isn't being bounced back by reconcile.
asrt "client mech travels >=100 px from spawn (x>=4196 at some point)" \
     "grep -oE 'mech=1\\* pelv=\\(4[12][0-9][0-9]\\.[0-9]+' '$CLI_LOG' \
        | grep -oE '[0-9]+\\.[0-9]+' \
        | awk 'BEGIN{f=0} {if (\$0 + 0 >= 4196) f=1} END{exit (f?0:1)}'"

# 3. Host (authoritative) and client (predicted) should agree on
#    direction of motion — the snapshots now carry pos > 4095.
asrt "client received snapshot mech pos east of 4095" \
     "grep -E 'mech=1\\* pelv=\\(41[6-9][0-9]\\.|mech=1\\* pelv=\\(4[2-7][0-9][0-9]\\.' '$CLI_LOG' | head -1 | grep -q ."

echo
echo "== walk-own-flag summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
