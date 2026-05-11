#!/usr/bin/env bash
#
# tests/net/run.sh — end-to-end network smoke test.
#
# Spawns ./soldut --host and ./soldut --connect as background processes,
# waits for the lobby/match flow to play through, captures both soldut.log
# files, asserts on key milestones, and ALWAYS reaps the processes before
# exiting.
#
# Usage:
#   tests/net/run.sh                    # default 2-player FFA, 1 round
#   tests/net/run.sh -k                 # keep tmp dirs for inspection
#
# Exit code 0 = all assertions pass.
#
# This is intentionally log-driven (no screenshots, no GUI interaction)
# so it can run in CI / over SSH where there's no display. The shotmode
# binary handles the screenshot side of things separately.

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
TMP="$(mktemp -d -t soldut-net-XXXXXX)"
HOST_DIR="$TMP/host"
CLI_DIR="$TMP/client"
mkdir -p "$HOST_DIR" "$CLI_DIR"

# Identical config in both cwds — small auto_start so the test
# completes in <30 s, single-map rotation so we don't need vote UI.
for d in "$HOST_DIR" "$CLI_DIR"; do
    cat > "$d/soldut.cfg" <<EOF
port=$PORT
auto_start_seconds=2
time_limit=6
score_limit=5
mode=ffa
map_rotation=foundry
mode_rotation=ffa
EOF
done

HOST_PID=0
CLI_PID=0
cleanup() {
    # Robust teardown — SIGTERM, give them a moment, then SIGKILL.
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
( cd "$HOST_DIR" && exec "$BIN" --listen-host "$PORT" --name HostA ) \
    > "$HOST_DIR/stdout.log" 2>&1 &
HOST_PID=$!

# Tiny wait for the listening socket to come up.
sleep 0.6

# Launch client.
( cd "$CLI_DIR" && exec "$BIN" --connect "127.0.0.1:$PORT" --name ClientB ) \
    > "$CLI_DIR/stdout.log" 2>&1 &
CLI_PID=$!

# Round timeline: connect (~0.3s) + auto_start 2s + countdown 5s +
# time_limit 6s + summary 15s ≈ 28-30s for a full cycle.
# We wait long enough to see ROUND_END but short enough to keep dev
# iteration fast.
WAIT_S="${TEST_WAIT_S:-22}"
for ((i = 0; i < WAIT_S; ++i)); do
    if [[ ! -d /proc/$HOST_PID || ! -d /proc/$CLI_PID ]]; then
        # One process died early — bail; assertions below will explain why.
        break
    fi
    sleep 1
done

# Stop them cleanly so logs flush.
kill "$CLI_PID"  2>/dev/null
kill "$HOST_PID" 2>/dev/null
sleep 0.5

HL="$HOST_DIR/soldut.log"
CL="$CLI_DIR/soldut.log"

PASS=0
FAIL=0
assert_log() {
    local label="$1"; local file="$2"; local pattern="$3"
    if grep -qE "$pattern" "$file"; then
        echo "PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $label"
        echo "  pattern: $pattern"
        echo "  file:    $file"
        FAIL=$((FAIL + 1))
    fi
}

# ---- Host assertions ----
assert_log "host listens on UDP $PORT"        "$HL" "net_server_start: listening on port $PORT"
assert_log "host accepts client into slot 1"  "$HL" "ACCEPT peer 0 → lobby slot 1"
assert_log "host enters countdown"            "$HL" "match: countdown"
assert_log "host spawns 2 mechs"              "$HL" "lobby_spawn_round_mechs: 2 mech\\(s\\) spawned"
assert_log "host begins round"                "$HL" "match: round begin"
assert_log "host ends round"                  "$HL" "match: round end"

# ---- Client assertions ----
assert_log "client connects + handshakes"     "$CL" "client: ACCEPT client_id=0 mech_id=1"
assert_log "client reaches lobby"             "$CL" "INITIAL_STATE applied — in lobby \\(local_slot=1"
assert_log "client receives lobby_list"       "$CL" "client: lobby_list received — slot 1 mech_id=1"
assert_log "client transitions to MATCH"      "$CL" "client: ROUND_START map=0"
assert_log "client receives snapshots"        "$CL" "client: first snapshot"
assert_log "client resolves local_mech_id"    "$CL" "client: local_mech_id resolved → 1 \\(slot 1\\)"
assert_log "client receives ROUND_END"        "$CL" "client: ROUND_END"

echo
echo "== summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
