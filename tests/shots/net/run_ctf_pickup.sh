#!/usr/bin/env bash
#
# tests/shots/net/run_ctf_pickup.sh — verifies that in networked CTF,
# both the host AND the client can pick up the opposing team's flag
# by walking into it. Regression for the user-reported bug "neither
# the server nor the client can collect the opposing flags."
#
# The host walks RIGHT into the BLUE flag; the client walks LEFT into
# the RED flag. Both pickups must fire on the authoritative server
# AND the broadcasted NET_MSG_FLAG_STATE must transition the flag to
# CARRIED on the other peer's view.

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
time_limit=30
score_limit=5
mode=ctf
friendly_fire=0
map_rotation=crossfire
mode_rotation=ctf
EOF

"$REPO/tests/shots/net/run.sh" 2p_ctf_pickup
RC=$?

HOST_LOG="$REPO/build/shots/net/host_ctf_pickup/2p_ctf_pickup.host.log"
CLI_LOG="$REPO/build/shots/net/client_ctf_pickup/2p_ctf_pickup.client.log"

echo
echo "=== CTF pickup-by-walking assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

# Host walked into BLUE flag (idx 1, team=2). Pickup is server-side
# event so we check the HOST log.
asrt "host's mech_id=0 picks up the BLUE flag (idx 1)" \
     "grep -qE 'ctf: pickup flag=1 \\(team=2\\) by mech=0' '$HOST_LOG' 2>/dev/null"
# Client walked into RED flag (idx 0, team=1).
asrt "client's mech_id=1 picks up the RED flag (idx 0)" \
     "grep -qE 'ctf: pickup flag=0 \\(team=1\\) by mech=1' '$HOST_LOG' 2>/dev/null"
# Wire round-trip: client should see flag-state transitions for BOTH
# the BLUE flag (carrier=0) and the RED flag (carrier=1). The host's
# pickup of BLUE arrives over the wire; the client's pickup of RED is
# echoed back from the authoritative server.
asrt "client mirrors BLUE flag pickup (host as carrier=0)" \
     "grep -qE 'client_handle_flag_state flag=1 .*->1 carrier=0' '$CLI_LOG' 2>/dev/null"
asrt "client mirrors RED flag pickup (self as carrier=1)" \
     "grep -qE 'client_handle_flag_state flag=0 .*->1 carrier=1' '$CLI_LOG' 2>/dev/null"

echo
echo "== CTF pickup summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
