#!/usr/bin/env bash
#
# tests/shots/net/run_rounds_match.sh — verify rounds_per_match gates
# the SUMMARY → next-round transition. With cfg rounds_per_match=1
# the first round-end triggers end_match (back to LOBBY, no map
# vote winner applied for round 2). M6 round-shape redesign.

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
time_limit=60
score_limit=3
rounds_per_match=1
mode=ffa
friendly_fire=1
map_rotation=crossfire
mode_rotation=ffa
EOF

"$REPO/tests/shots/net/run.sh" 2p_rounds_match
RC=$?

HOST_LOG="$REPO/build/shots/net/host_rounds_match/2p_rounds_match.host.log"
CLI_LOG="$REPO/build/shots/net/client_rounds_match/2p_rounds_match.client.log"

echo
echo "=== rounds_per_match assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

# Round-end fires once.
asrt "host logs exactly one round end" \
     "[[ \$(grep -c 'match: round end' '$HOST_LOG' 2>/dev/null) -eq 1 ]]"
# end_match runs (not advance_to_next_round). The "match over" log
# line is the canonical end-of-match marker — no map vote, return
# to lobby.
asrt "host logs match over (rounds_per_match=1 → end_match)" \
     "grep -q 'match_flow: match over' '$HOST_LOG' 2>/dev/null"
# We should NOT see advance_to_next_round fire ("round 2/N — continuing match").
asrt "host does NOT advance to next round" \
     "! grep -q 'continuing match' '$HOST_LOG' 2>/dev/null"
# Client's last MATCH_STATE should land in MATCH_PHASE_LOBBY (=0).
asrt "client sees match phase return to LOBBY" \
     "grep -qE 'rx_match_state .*phase=0' '$CLI_LOG' 2>/dev/null"

echo
echo "== rounds_match summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
