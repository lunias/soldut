#!/usr/bin/env bash
#
# tests/net/run_ctf.sh — CTF mode end-to-end network smoke test.
#
# Spawns ./soldut --host and ./soldut --connect with mode=ctf +
# map_rotation=crossfire in their respective cwds, lets the lobby/match
# flow run for ~12 s, captures both soldut.log files, asserts on
# CTF-specific milestones (flag init, team auto-balance, both sides
# see the same flag state).
#
# This is log-driven (no GUI, no scripted input) so it runs in CI.
# Functional capture flow is exercised by tests/shots/net/2p_ctf.*
# (paired shot scripts) — that adds visual verification.
#
# Usage:
#   tests/net/run_ctf.sh        # 2-player CTF, 1 round
#   tests/net/run_ctf.sh -k     # keep tmp dirs for inspection
#
# Exit code 0 = all assertions pass.

set -u

KEEP=0
if [[ "${1:-}" == "-k" ]]; then KEEP=1; fi

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/soldut"
if [[ ! -x "$BIN" ]]; then
    echo "fail: $BIN not built — run 'make' first" >&2
    exit 1
fi

PORT=$((23000 + RANDOM % 1000))
TMP="$(mktemp -d -t soldut-ctf-XXXXXX)"
HOST_DIR="$TMP/host"
CLI_DIR="$TMP/client"
mkdir -p "$HOST_DIR" "$CLI_DIR"

# Identical cfg in both cwds. Score limit kept tiny so the test won't
# accidentally hit it within the 12 s window — we want the round to
# stay ACTIVE for the full duration so we can capture state. Friendly-
# fire on so any incidental damage actually lands (otherwise CTF's no-
# FF default would silence things).
for d in "$HOST_DIR" "$CLI_DIR"; do
    cat > "$d/soldut.cfg" <<EOF
port=$PORT
auto_start_seconds=2
time_limit=10
score_limit=5
mode=ctf
friendly_fire=1
map_rotation=crossfire
mode_rotation=ctf
EOF
done

HOST_PID=0
CLI_PID=0
cleanup() {
    [[ $CLI_PID  -gt 0 ]] && kill     "$CLI_PID"  2>/dev/null
    [[ $HOST_PID -gt 0 ]] && kill     "$HOST_PID" 2>/dev/null
    sleep 0.3
    [[ $CLI_PID  -gt 0 ]] && kill -9  "$CLI_PID"  2>/dev/null
    [[ $HOST_PID -gt 0 ]] && kill -9  "$HOST_PID" 2>/dev/null
    wait 2>/dev/null
    if [[ "$KEEP" == "0" ]]; then
        rm -rf "$TMP"
    else
        echo "kept: $TMP"
    fi
}
trap cleanup EXIT

# Launch host first.
( cd "$HOST_DIR" && exec "$BIN" --host "$PORT" --name HostRed ) \
    > "$HOST_DIR/stdout.log" 2>&1 &
HOST_PID=$!

sleep 0.6

# Launch client.
( cd "$CLI_DIR" && exec "$BIN" --connect "127.0.0.1:$PORT" --name ClientBlue ) \
    > "$CLI_DIR/stdout.log" 2>&1 &
CLI_PID=$!

# Wait for the round to play out. With auto_start_seconds=2 + 5 s
# countdown + 10 s round, total is ~17 s. We give a safety margin
# but bail early if both processes have exited.
TIMEOUT_S="${TEST_TIMEOUT_S:-25}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    if [[ ! -d /proc/$HOST_PID && ! -d /proc/$CLI_PID ]]; then
        break
    fi
done

# Belt-and-braces stop in case either is still alive.
[[ -d /proc/$CLI_PID  ]] && kill "$CLI_PID"  2>/dev/null
[[ -d /proc/$HOST_PID ]] && kill "$HOST_PID" 2>/dev/null
sleep 0.5

HL="$HOST_DIR/soldut.log"
CL="$CLI_DIR/soldut.log"

echo "=== assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then
        echo "PASS: $1"; PASS=$((PASS + 1))
    else
        echo "FAIL: $1"; FAIL=$((FAIL + 1))
    fi
}

asrt "host log exists"          "[[ -f '$HL' ]]"
asrt "client log exists"        "[[ -f '$CL' ]]"

# Plumbing — both sides reach the lobby + match phase.
asrt "host accepts client into slot 1" \
     "grep -q 'ACCEPT peer' '$HL' 2>/dev/null"
asrt "host enters countdown" \
     "grep -q 'match: countdown' '$HL' 2>/dev/null"

# CTF setup on the host.
asrt "host built crossfire" \
     "grep -q 'crossfire built.*mode_mask=0x7' '$HL' 2>/dev/null"
asrt "host begins CTF round" \
     "grep -q 'match: round begin (mode=CTF' '$HL' 2>/dev/null"
asrt "host ctf_init_round populates flags" \
     "grep -q 'ctf_init_round: flags at RED' '$HL' 2>/dev/null"
asrt "host auto-balanced teams (CTF, red=1 blue=1)" \
     "grep -q 'team auto-balance.*red=1.*blue=1' '$HL' 2>/dev/null"
asrt "host CTF score limit clamped to 5" \
     "grep -q 'mode=CTF.*limit=5' '$HL' 2>/dev/null"

# CTF setup on the client.
asrt "client receives ROUND_START with mode=CTF" \
     "grep -q 'client: ROUND_START.*mode=CTF' '$CL' 2>/dev/null"
asrt "client ctf_init_round populates flags" \
     "grep -q 'ctf_init_round: flags at RED' '$CL' 2>/dev/null"
asrt "client resolves local_mech_id (slot 1)" \
     "grep -q 'local_mech_id resolved' '$CL' 2>/dev/null"

# Both sides survive snapshot streaming (no crashes / unhandled tags).
asrt "client receives snapshots" \
     "grep -q 'first snapshot' '$CL' 2>/dev/null"

# Round completes (time_limit=10).
asrt "host ends round" \
     "grep -q 'match_flow: round.*end\\|match: round end' '$HL' 2>/dev/null"
asrt "client receives ROUND_END" \
     "grep -q 'client: ROUND_END' '$CL' 2>/dev/null"

echo
echo "== ctf summary: $PASS passed, $FAIL failed =="
echo "host: $HL"
echo "cli:  $CL"
[[ $FAIL -eq 0 ]] || exit 1
exit 0
