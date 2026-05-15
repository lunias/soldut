#!/usr/bin/env bash
#
# tests/shots/net/run_tdm_respawn.sh — TDM respawn-until-team-cap.
# Host kills the client 3 times via kill_peer + shooter; client
# respawns after each of the first two; the third hits the TDM
# team_score cap (3) and ends the round. Regression for the M6
# round-shape redesign: pre-fix only CTF respawned mid-round, so
# TDM rounds ended on the first kill via solo_warning.

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
mode=tdm
friendly_fire=0
map_rotation=crossfire
mode_rotation=tdm
EOF

"$REPO/tests/shots/net/run.sh" 2p_tdm_respawn
RC=$?

HOST_LOG="$REPO/build/shots/net/host_tdm_respawn/2p_tdm_respawn.host.log"
CLI_LOG="$REPO/build/shots/net/client_tdm_respawn/2p_tdm_respawn.client.log"

echo
echo "=== TDM respawn assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host fires three mech_kill on id=1" \
     "[[ \$(grep -c 'mech_kill: id=1' '$HOST_LOG' 2>/dev/null) -ge 3 ]]"
asrt "host fires at least two mech_respawn for id=1" \
     "[[ \$(grep -c 'mech_respawn: id=1' '$HOST_LOG' 2>/dev/null) -ge 2 ]]"
asrt "host logs exactly one round end (no premature end)" \
     "[[ \$(grep -c 'match: round end' '$HOST_LOG' 2>/dev/null) -eq 1 ]]"
asrt "host does NOT trigger solo_warning" \
     "! grep -q 'round ends in 3s' '$HOST_LOG' 2>/dev/null"
# Client mirrors team_score live (regression for the HUD banner sync
# bug from the previous patch — applies to TDM kills too).
asrt "client mirrors team_score R3 (after last kill)" \
     "grep -qE 'rx_match_state .*team_score=R3' '$CLI_LOG' 2>/dev/null"
asrt "client received ROUND_END" \
     "grep -q 'ROUND_END' '$CLI_LOG' 2>/dev/null"

echo
echo "== TDM respawn summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
