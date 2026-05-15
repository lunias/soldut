#!/usr/bin/env bash
#
# tests/shots/net/run_ctf_respawn.sh — verifies mid-round CTF respawn
# over the wire. Host kills the client mech via kill_peer; the
# server-side mech_kill arms a per-mech respawn timer
# (RESPAWN_DELAY_TICKS = 180 = 3 s at 60 Hz); 180 ticks later the
# server fires mech_respawn and the client mirrors the alive→true
# transition through the existing snapshot path.
#
# Regression for the user-reported bug "I kill my opponent and the
# next round starts" — pre-fix the solo_warning rule ended the CTF
# round 3 s after the kill. Post-fix: respawn fires at the same 3 s
# mark, the round keeps running, the score gate (captures) is the
# only end condition.

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

# auto_start_seconds high enough that ROUND_START doesn't fire before
# both clients are connected, low enough that the test still finishes
# in <30 s. score_limit deliberately high so the round can't end by
# captures (we're not capturing). time_limit covers the test duration.
cat > "$CFG" <<EOF
auto_start_seconds=2
time_limit=30
score_limit=5
mode=ctf
friendly_fire=1
map_rotation=crossfire
mode_rotation=ctf
EOF

"$REPO/tests/shots/net/run.sh" 2p_ctf_respawn
RC=$?

HOST_LOG="$REPO/build/shots/net/host_ctf_respawn/2p_ctf_respawn.host.log"
CLI_LOG="$REPO/build/shots/net/client_ctf_respawn/2p_ctf_respawn.client.log"

echo
echo "=== CTF respawn assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host's mech_kill fired on client id=1" \
     "grep -q 'mech_kill: id=1' '$HOST_LOG' 2>/dev/null"
asrt "host arms CTF respawn timer for id=1" \
     "grep -qE 'mech_respawn: id=1' '$HOST_LOG' 2>/dev/null"
asrt "host does NOT end round during the test (no ROUND_END for 600+ ticks after kill)" \
     "! grep -q 'round end' '$HOST_LOG' 2>/dev/null"
asrt "host does NOT trigger solo_warning in CTF" \
     "! grep -q 'round ends in 3s' '$HOST_LOG' 2>/dev/null"
asrt "client renders past the would-be solo_warning end (still in ACTIVE phase at tick 600)" \
     "grep -q 'tick 600 ' '$CLI_LOG' 2>/dev/null"

echo
echo "== CTF respawn summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
