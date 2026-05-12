#!/usr/bin/env bash
#
# tests/shots/net/run_riot_cannon_sfx.sh — paired test for the
# Bug B fix in M6 (non-hitscan self-fire SFX suppressed on the
# firer's window).
#
# Setup: host on Pulse Rifle stays passive. Client on Riot Cannon
# (WFIRE_SPREAD, non-hitscan) LMB-fires its active slot.
#
# Assertions:
#   - client logs `client_fire_event sfx shooter=1 weapon=2 self=1
#     active=1 fire_kind=2` (fire_kind 2 = WFIRE_SPREAD, weapon 2
#     = WEAPON_RIOT_CANNON). Pre-fix this line never appears.
#   - host logs `spawn_proj mech=1 wpn=2` — confirms client's fire
#     reached the server.
#   - sanity: client does NOT log a "shooter=1 weapon=0" sfx line
#     (Pulse Rifle is wpn 0 and is HITSCAN, predict draws + plays
#     it — should never appear via the fire-event sfx branch for
#     self+active).

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
NAME="2p_riot_cannon_sfx"

HOST_SHOT="$REPO/tests/shots/net/$NAME.host.shot"
CLI_SHOT="$REPO/tests/shots/net/$NAME.client.shot"
[[ -f "$HOST_SHOT" && -f "$CLI_SHOT" ]] || { echo "fail: shots missing"; exit 1; }

"$REPO/tests/shots/net/run.sh" "$NAME"
RC=$?

echo
echo "=== riot-cannon-sfx assertions ==="

OUT="$REPO/build/shots/net"
HOST_OUT=$(find "$OUT" -maxdepth 1 -mindepth 1 -type d -name '*host*' | head -1)
CLI_OUT=$(find  "$OUT" -maxdepth 1 -mindepth 1 -type d -name '*client*' | head -1)
HL=$(find "$HOST_OUT" -maxdepth 1 -name '*.log' | head -1)
CL=$(find "$CLI_OUT"  -maxdepth 1 -name '*.log' | head -1)

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

# Client fired its Riot Cannon (server-side spawn_proj on the host
# proves the fire packet arrived + simulated).
asrt "host: client Riot Cannon fired (mech=1 wpn=2)" \
    "grep -qE 'spawn_proj mech=1 wpn=2' '$HL'"

# Client's local FIRE_EVENT path played the SFX. Pre-fix line absent.
# (self=1 active=1 means LMB-on-active; fire_kind=2 is WFIRE_SPREAD.)
asrt "client: own Riot Cannon SFX fired via FIRE_EVENT" \
    "grep -qE 'client_fire_event sfx shooter=1 weapon=2 self=1 active=1 fire_kind=2' '$CL'"

# Sanity: client should NEVER log a `client_fire_event sfx ...
# self=1 active=1 fire_kind=0` line — fire_kind 0 = WFIRE_HITSCAN,
# and HITSCAN active fire is drawn by predict (audio included).
# Anything matching that pattern would mean predict_drew_sfx is
# misclassifying.
asrt "client: never double-plays HITSCAN active SFX (no double-fire)" \
    "! grep -qE 'client_fire_event sfx shooter=1 weapon=0 self=1 active=1 fire_kind=0' '$CL'"

echo
echo "== riot-cannon-sfx summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
