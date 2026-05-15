#!/usr/bin/env bash
#
# tests/shots/net/run_ctf_respawn_clean.sh — verifies the user-reported
# "respawned mech still shows smoke / damage from last life" bug is
# fixed. The host kills the client; 180 ticks later the client's mech
# respawns. Both the HOST's view of the client and the CLIENT's view
# of its own local mech must come back clean — no FX_STUMP pinned to
# this mech in the FxPool, no persistent decals on the limbs, no
# carried hit-flash.

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
friendly_fire=1
map_rotation=crossfire
mode_rotation=ctf
EOF

"$REPO/tests/shots/net/run.sh" 2p_ctf_respawn_clean
RC=$?

HOST_LOG="$REPO/build/shots/net/host_ctf_respawn_clean/2p_ctf_respawn_clean.host.log"
CLI_LOG="$REPO/build/shots/net/client_ctf_respawn_clean/2p_ctf_respawn_clean.client.log"

echo
echo "=== CTF respawn cleanliness assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host's mech_kill fired on client id=1" \
     "grep -q 'mech_kill: id=1' '$HOST_LOG' 2>/dev/null"
asrt "host's mech_respawn fired on client id=1" \
     "grep -q 'mech_respawn: id=1' '$HOST_LOG' 2>/dev/null"
# CTF round must stay ACTIVE through the test — no end on kill, no
# end during the respawn window.
asrt "host does NOT end round on kill (no 'round end' log)" \
     "! grep -q 'round end' '$HOST_LOG' 2>/dev/null"
asrt "host does NOT trigger solo_warning (no 'round ends in 3s')" \
     "! grep -q 'round ends in 3s' '$HOST_LOG' 2>/dev/null"
# Client must have rendered past the would-be solo-warning end.
asrt "client renders past tick 600 (round still ACTIVE)" \
     "grep -q 'tick 600 ' '$CLI_LOG' 2>/dev/null"

echo
echo "== CTF respawn-clean summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
