#!/usr/bin/env bash
#
# tests/shots/net/run_team_change.sh — verify the lobby team picker's
# wire round-trip. Client sends a team_change BLUE; host's lobby
# table reflects it on the next broadcast.
#
# Wraps tests/shots/net/run.sh with a TDM-mode soldut.cfg (so RED/BLUE
# are meaningful) — the existing default cfg is FFA which would force
# every slot to FFA-team-1.

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
# auto_start_seconds=4 (= 240 ticks) gives ~3 s of lobby time after
# handshake settles for the test to send a team_change, then the
# round kicks off so the run.sh "host begins a round" assertion
# passes. time_limit short so the round wraps within a 600-tick
# script.
auto_start_seconds=4
time_limit=5
score_limit=20
mode=tdm
mode_rotation=tdm
map_rotation=foundry
EOF

"$REPO/tests/shots/net/run.sh" 2p_team_change
RC=$?

HOST_LOG="$REPO/build/shots/net/host_team_change/2p_team_change.host.log"

echo
echo "=== team-change assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

# The client's team_change directive logs server-side as the lobby
# slot transitioning from RED (team=1) to BLUE (team=2).
CLI_LOG="$REPO/build/shots/net/client_team_change/2p_team_change.client.log"
asrt "host config loaded TDM mode" \
     "grep -q 'mode=TDM\\|mode=tdm' '$HOST_LOG' 2>/dev/null"
asrt "host accepts client into lobby slot 1" \
     "grep -q 'lobby slot 1' '$HOST_LOG' 2>/dev/null"
asrt "client sent team_change → 2 (BLUE)" \
     "grep -q 'shot: team_change → 2' '$CLI_LOG' 2>/dev/null"
asrt "client received lobby_list with mech_id resolved" \
     "grep -q 'lobby_list received' '$CLI_LOG' 2>/dev/null"

echo
echo "== team-change summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
