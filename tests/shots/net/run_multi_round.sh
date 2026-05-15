#!/usr/bin/env bash
#
# tests/shots/net/run_multi_round.sh — verify round 2 actually runs
# after round 1 ends via score cap. Regression for the user-reported
# "round 2 starts and immediately ends" bug: pre-fix
# advance_to_next_round didn't clear per-round slot.score, so
# slot.score from round 1 (already at the cap) tripped the FFA
# score gate the same tick round 2 began.
#
# The fix lives in advance_to_next_round (main.c + the mirror in
# shotmode.c::shot_host_flow): walk all in-use slots and zero
# score / kills / deaths / team_kills / current_streak between
# rounds, preserving ready + longest_streak which are match-cumulative.

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
time_limit=120
score_limit=2
rounds_per_match=3
mode=ffa
friendly_fire=1
map_rotation=crossfire
mode_rotation=ffa
EOF

"$REPO/tests/shots/net/run.sh" 2p_multi_round
RC=$?

HOST_LOG="$REPO/build/shots/net/host_multi_round/2p_multi_round.host.log"
CLI_LOG="$REPO/build/shots/net/client_multi_round/2p_multi_round.client.log"

echo
echo "=== multi-round assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

# Round 1 ends via score cap.
asrt "host fires round-1 end (R0/B0 — FFA team_score unused)" \
     "grep -qE 'match: round end' '$HOST_LOG' 2>/dev/null"
# Match-flow LOG message marks round 2 began.
asrt "host advances to round 2 (continuing match)" \
     "grep -qE 'round 2/3 — continuing' '$HOST_LOG' 2>/dev/null"
# Round 2 begin log appears.
asrt "host runs match_begin_round twice (one per round)" \
     "[[ \$(grep -c 'match: round begin' '$HOST_LOG' 2>/dev/null) -ge 2 ]]"
# Critical: round 2 must NOT immediately end. If the bug were live
# we'd see TWO round-end log lines back to back (round 1 + round 2
# instantly). Round 2 should still be in ACTIVE phase until the
# script ends, so exactly ONE round-end happens.
asrt "host logs exactly one round-end (round 2 doesn't insta-end)" \
     "[[ \$(grep -c 'match: round end' '$HOST_LOG' 2>/dev/null) -eq 1 ]]"
# Client's last MATCH_STATE during round 2 shows phase=2 (ACTIVE)
# AND rounds_played=1 (in the second round of the match).
asrt "client sees match phase=ACTIVE with rounds_played=1 (round 2)" \
     "grep -qE 'rx_match_state .*phase=2.*rounds=1/3' '$CLI_LOG' 2>/dev/null"

echo
echo "== multi-round summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
