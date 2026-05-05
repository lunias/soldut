#!/usr/bin/env bash
#
# tests/shots/net/run_ctf_drop_on_kill.sh — verifies that killing a
# flag carrier drops the flag at the death position with a 30-s
# auto-return timer. Regression for "carrier death should drop the
# flag, not vanish it" (which was already wired in P07 — this is the
# end-to-end wire-level proof).

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
time_limit=15
score_limit=5
mode=ctf
friendly_fire=0
map_rotation=crossfire
mode_rotation=ctf
EOF

"$REPO/tests/shots/net/run.sh" 2p_ctf_drop_on_kill
RC=$?

HOST_LOG="$REPO/build/shots/net/host_ctf_drop/2p_ctf_drop_on_kill.host.log"
CLI_LOG="$REPO/build/shots/net/client_ctf_drop/2p_ctf_drop_on_kill.client.log"

echo
echo "=== drop-on-kill assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host armed flag carrier (server-side)" \
     "grep -q 'arm_carry flag=0 mech=1' '$HOST_LOG' 2>/dev/null"
asrt "host's mech_kill fired on carrier id=1" \
     "grep -q 'mech_kill: id=1' '$HOST_LOG' 2>/dev/null"
asrt "carrier death drops flag (ctf: drop flag=0)" \
     "grep -q 'ctf: drop flag=0 carrier mech=1' '$HOST_LOG' 2>/dev/null"
asrt "drop position matches the carrier's pelvis (near spawn x=2400)" \
     "grep -qE 'drop flag=0 carrier mech=1 at \\(2[34]' '$HOST_LOG' 2>/dev/null"

echo
echo "== drop-on-kill summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
