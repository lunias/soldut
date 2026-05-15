#!/usr/bin/env bash
#
# tests/shots/net/run_ctf_score_limit.sh — verifies the host's
# configured score_limit gates CTF round-end (regression for the
# user-reported "1 capture ends the round" bug).
#
# Cfg sets score_limit=3. Host arms 3 captures back-to-back. The
# round must stay ACTIVE after the first two captures, then end on
# the third when team_score hits the limit.

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
score_limit=3
mode=ctf
friendly_fire=0
map_rotation=crossfire
mode_rotation=ctf
EOF

"$REPO/tests/shots/net/run.sh" 2p_ctf_score_limit
RC=$?

HOST_LOG="$REPO/build/shots/net/host_ctf_score_limit/2p_ctf_score_limit.host.log"
CLI_LOG="$REPO/build/shots/net/client_ctf_score_limit/2p_ctf_score_limit.client.log"

echo
echo "=== CTF score-limit assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

# Three captures must fire on the host (server-authoritative).
asrt "host fires capture 1 (score 1-0)" \
     "grep -qE 'ctf: capture by mech=0 \\(team=1\\) score R1/B0' '$HOST_LOG' 2>/dev/null"
asrt "host fires capture 2 (score 2-0)" \
     "grep -qE 'ctf: capture by mech=0 \\(team=1\\) score R2/B0' '$HOST_LOG' 2>/dev/null"
asrt "host fires capture 3 (score 3-0)" \
     "grep -qE 'ctf: capture by mech=0 \\(team=1\\) score R3/B0' '$HOST_LOG' 2>/dev/null"

# Round end should fire ONLY at the third capture. The "round end"
# log line is from match_end_round, which is the canonical end-of-
# round side effect.
asrt "host logs exactly one round end (no premature end)" \
     "[[ \$(grep -c 'match: round end' '$HOST_LOG' 2>/dev/null) -eq 1 ]]"

# Sanity: the round-end winner should be RED (team_score 3, hit cap).
asrt "round end winner is RED (team_score 3-0 hits cap)" \
     "grep -qE 'match: round end.*winner_team=1' '$HOST_LOG' 2>/dev/null"

# Client mirrors team_score live — the regression for "score banner
# at top of screen doesn't update during the round". Each capture
# fires a MATCH_STATE broadcast and the client logs the decoded
# state via match_shot_log_phase's rx_match_state tag.
asrt "client mirrors team_score R1 (after capture 1)" \
     "grep -qE 'rx_match_state .*team_score=R1/B0' '$CLI_LOG' 2>/dev/null"
asrt "client mirrors team_score R2 (after capture 2)" \
     "grep -qE 'rx_match_state .*team_score=R2/B0' '$CLI_LOG' 2>/dev/null"
asrt "client mirrors team_score R3 (after capture 3, ROUND_END)" \
     "grep -qE 'team_score=R3/B0' '$CLI_LOG' 2>/dev/null"

# Client must receive ROUND_END broadcast.
asrt "client received ROUND_END" \
     "grep -q 'ROUND_END' '$CLI_LOG' 2>/dev/null"

echo
echo "== CTF score-limit summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
