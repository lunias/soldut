#!/usr/bin/env bash
#
# tests/shots/net/run_ffa_respawn.sh — FFA respawn-until-score-cap.
# Host kills the client 3 times via kill_peer; client respawns
# after each of the first two kills; the third hits the FFA
# score_limit (3) and ends the round.
#
# Regression for the M6 round-shape redesign: pre-fix mech_kill
# arming was CTF-only, so a single kill in FFA left the victim
# dead and the solo_warning rule ended the round in 3 s. Now
# respawn fires universally and solo_warning skips when any mech
# has a pending respawn.

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
rounds_per_match=10
mode=ffa
friendly_fire=1
map_rotation=crossfire
mode_rotation=ffa
EOF

"$REPO/tests/shots/net/run.sh" 2p_ffa_respawn
RC=$?

HOST_LOG="$REPO/build/shots/net/host_ffa_respawn/2p_ffa_respawn.host.log"
CLI_LOG="$REPO/build/shots/net/client_ffa_respawn/2p_ffa_respawn.client.log"

echo
echo "=== FFA respawn assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

# Each kill fires on the host. We need three.
asrt "host fires three mech_kill on id=1" \
     "[[ \$(grep -c 'mech_kill: id=1' '$HOST_LOG' 2>/dev/null) -ge 3 ]]"
# Each of the first two kills MUST respawn the victim. Third kill
# hits the cap so no respawn fires after it (round ends).
asrt "host fires at least two mech_respawn for id=1" \
     "[[ \$(grep -c 'mech_respawn: id=1' '$HOST_LOG' 2>/dev/null) -ge 2 ]]"
# Round ends EXACTLY ONCE at the score cap.
asrt "host logs exactly one round end (no premature end)" \
     "[[ \$(grep -c 'match: round end' '$HOST_LOG' 2>/dev/null) -eq 1 ]]"
# Solo-warning safety must NOT fire — respawn always pending after
# the first two kills.
asrt "host does NOT trigger solo_warning" \
     "! grep -q 'round ends in 3s' '$HOST_LOG' 2>/dev/null"
# Client receives the ROUND_END broadcast.
asrt "client received ROUND_END" \
     "grep -q 'ROUND_END' '$CLI_LOG' 2>/dev/null"

echo
echo "== FFA respawn summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
