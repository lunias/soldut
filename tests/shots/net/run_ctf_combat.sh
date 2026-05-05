#!/usr/bin/env bash
#
# tests/shots/net/run_ctf_combat.sh — verifies that shooting opposing
# mechs in CTF mode actually deals damage. Regression for the
# "auto-balance ran after spawn so everyone was on RED, friendly_fire
# off, no shots landed" bug.
#
# Wraps tests/shots/net/run.sh with a CTF-mode soldut.cfg so
# config_pick_mode returns CTF, the host's start_round runs the team
# auto-balance (now BEFORE spawn), and Pulse Rifle hits register on
# the host's mech_apply_damage path.

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

"$REPO/tests/shots/net/run.sh" 2p_ctf_combat
RC=$?

HOST_LOG="$REPO/build/shots/net/host_ctf_combat/2p_ctf_combat.host.log"

echo
echo "=== combat assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host built crossfire (CTF-capable)" \
     "grep -q 'crossfire built.*mode_mask=0x7' '$HOST_LOG' 2>/dev/null"
asrt "host begins CTF round" \
     "grep -q 'match: round begin (mode=CTF' '$HOST_LOG' 2>/dev/null"
asrt "team auto-balance ran (red=1 blue=1)" \
     "grep -q 'team auto-balance.*red=1.*blue=1' '$HOST_LOG' 2>/dev/null"
# The order matters — auto-balance must precede mech_create. If the
# bug regresses, mechs spawn first (with default RED team) then
# balance fires too late, mechs end up same-team, friendly_fire=off
# blocks all damage.
asrt "auto-balance precedes first mech_create" \
     "awk '/team auto-balance/ && !ab {ab=NR}
           /mech_create: id=0/ && !mc {mc=NR}
           END { exit (ab > 0 && mc > 0 && ab < mc ? 0 : 1) }' '$HOST_LOG'"
# The actual regression test: hits land on the opposing-team mech.
# mech_apply_damage emits a SHOT_LOG line "mech=N damage part=..."
# only when damage gets past the friendly-fire gate.
asrt "host hit opposing mech (damage logged on victim mech=1)" \
     "grep -q 'mech=1 damage part=' '$HOST_LOG' 2>/dev/null"
asrt "victim's HP dropped from 150" \
     "grep 'mech=1 damage part=' '$HOST_LOG' | head -1 |
      grep -q 'hp=14' 2>/dev/null"
# fire log line proves the hitscan path got past the FF gate.
asrt "fire-event log shows mech 0 hitting mech 1" \
     "grep -q 'fire mech=0 wpn=0 hit mech=1' '$HOST_LOG' 2>/dev/null"

echo
echo "== ctf-combat summary: $PASS passed, $FAIL failed (inner rc=$RC) =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
