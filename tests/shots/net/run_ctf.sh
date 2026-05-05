#!/usr/bin/env bash
#
# tests/shots/net/run_ctf.sh — networked CTF shot test driver.
#
# tests/shots/net/run.sh expects scripts to share the project root's
# soldut.cfg, but the existing project default is FFA. This wrapper
# briefly writes a CTF cfg in the project root, runs the shot pair,
# then restores the previous state.
#
# The shot pair (2p_ctf.host.shot + 2p_ctf.client.shot) uses
# `flag_carry` to bench a host-side capture — the wire round-trip
# verifies the client mirrors flag state via NET_MSG_FLAG_STATE.
#
# Usage:
#   tests/shots/net/run_ctf.sh        # default 2p_ctf
#   tests/shots/net/run_ctf.sh -k     # keep tmpdir / cfg

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
CFG="$REPO/soldut.cfg"
BACKUP=""
KEEP="${1:-}"

# Save any existing soldut.cfg.
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
time_limit=12
score_limit=5
mode=ctf
friendly_fire=1
map_rotation=crossfire
mode_rotation=ctf
EOF

# Delegate to the existing shot-test runner.
"$REPO/tests/shots/net/run.sh" $KEEP 2p_ctf
RC=$?

# CTF-specific assertions on top of the generic ones in run.sh. The
# inner script already verified host-begins-round + client-receives-
# ROUND_START + snapshots; here we layer on the CTF event milestones.
HOST_LOG="$REPO/build/shots/net/host_ctf/2p_ctf.host.log"
CLI_LOG="$REPO/build/shots/net/client_ctf/2p_ctf.client.log"

echo
echo "=== CTF-specific assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then
        echo "PASS: $1"; PASS=$((PASS + 1))
    else
        echo "FAIL: $1"; FAIL=$((FAIL + 1))
    fi
}

asrt "host built crossfire (mode_mask=0x7)" \
     "grep -q 'crossfire built.*mode_mask=0x7' '$HOST_LOG' 2>/dev/null"
asrt "host enters CTF round" \
     "grep -q 'mode=CTF' '$HOST_LOG' 2>/dev/null"
asrt "host populates flags via ctf_init_round" \
     "grep -q 'ctf_init_round: flags at RED' '$HOST_LOG' 2>/dev/null"
asrt "host auto-balance puts red=1 blue=1" \
     "grep -q 'team auto-balance.*red=1.*blue=1' '$HOST_LOG' 2>/dev/null"
asrt "host arms a flag carry" \
     "grep -q 'flag_carry flag=1 local_mech' '$HOST_LOG' 2>/dev/null"
asrt "host fires capture" \
     "grep -q 'ctf: capture by mech=' '$HOST_LOG' 2>/dev/null"
asrt "client populates flags via ctf_init_round" \
     "grep -q 'ctf_init_round: flags at RED' '$CLI_LOG' 2>/dev/null"

echo
echo "== CTF shot summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
