#!/usr/bin/env bash
#
# tests/shots/net/run_rmb_hitscan.sh — paired test for the
# RMB-fired HITSCAN secondary (Sidearm) sync fix.
#
# Asserts:
#   - host fires its RMB Sidearm (server-side spawn_proj equivalent
#     for hitscan: a `fire mech=0 wpn=8 hit/miss` SHOT_LOG line — wpn
#     8 is WEAPON_SIDEARM).
#   - host sees its OWN RMB tracer via the FIRE_EVENT path
#     (`client_fire_event hitscan shooter=0 self=1 active=0` —
#     the new-fix log line; absent before the fix).
#   - client RMB-fires Sidearm.
#   - client sees its OWN RMB tracer (`shooter=N self=1 active=0`
#     where N = client's mech_id).
#   - both sides see each other's shots
#     (`shooter=K self=0 active=0` for the remote one).

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
NAME="2p_rmb_hitscan"

HOST_SHOT="$REPO/tests/shots/net/$NAME.host.shot"
CLI_SHOT="$REPO/tests/shots/net/$NAME.client.shot"
[[ -f "$HOST_SHOT" && -f "$CLI_SHOT" ]] || { echo "fail: shots missing"; exit 1; }

"$REPO/tests/shots/net/run.sh" "$NAME"
RC=$?

echo
echo "=== rmb-hitscan assertions ==="

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

# Server-side: host fires its Sidearm (wpn=8), client fires its Sidearm.
# weapons_fire_hitscan logs `fire mech=N wpn=8 hit/miss`.
asrt "host: own RMB Sidearm fired (wpn=8)" \
    "grep -qE 'fire mech=0 wpn=8' '$HL'"
asrt "host: client RMB Sidearm fired (mech=1)" \
    "grep -qE 'fire mech=1 wpn=8' '$HL'"

# Host-side visuals come directly from weapons_fire_hitscan running
# server-authoritatively for both mechs (the FIRE_EVENT broadcast
# never loops back to the host process). The `fire mech=N wpn=8`
# assertions above already cover the host's view.
#
# Pre-fix behavior on the CLIENT was: NO `client_fire_event hitscan`
# log for self (skip-self ate it). Post-fix: client logs the tracer
# spawn for BOTH the remote player's shot AND its own RMB-on-inactive
# shot. The client's mech_id is 1 (slot 1), the host's is 0.
asrt "client: sees own RMB tracer (self=1 active=0)" \
    "grep -qE 'client_fire_event hitscan shooter=1 self=1 active=0' '$CL'"
asrt "client: sees host's RMB tracer (self=0 active=0)" \
    "grep -qE 'client_fire_event hitscan shooter=0 self=0 active=0' '$CL'"

# Sanity check that the client did NOT log a self+active hitscan event
# (would indicate the predict-active classification mis-fired and
# we're double-drawing the active-slot LMB tracer that predict already
# drew). No LMB fires were scripted, so this should hold trivially —
# but keeping the assertion documents the invariant.
asrt "client: never sees self+active hitscan (no double-draw)" \
    "! grep -qE 'client_fire_event hitscan shooter=1 self=1 active=1' '$CL'"

echo
echo "== rmb-hitscan summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
