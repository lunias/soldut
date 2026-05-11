#!/usr/bin/env bash
#
# tests/shots/net/run_dedi.sh — paired-client dedicated-server shot test.
#
# Spawns three processes:
#   1. A `--dedicated PORT` Soldut server (no raylib, no audio).
#   2. Two `--shot` clients that connect to 127.0.0.1:PORT.
#
# Both clients are pure connect-as-client; neither is the listen-
# server. This matches the post-wan-fixes-5 production path where
# "Host Server" in the UI spawns the dedicated child and joins as a
# regular client. The point is identical rendering for both players —
# same prediction, same interp delay, same lag-comp.
#
# Naming convention (mirrors run.sh):
#   tests/shots/net/<name>.client_a.shot
#   tests/shots/net/<name>.client_b.shot
#
# Usage:
#   tests/shots/net/run_dedi.sh <name>            # default scenario
#   tests/shots/net/run_dedi.sh -k <name>         # keep tmpdir
#
# Outputs (one PNG per shot directive in the scripts) land at:
#   build/shots/net/dedi/<a-out>/...
#   build/shots/net/dedi/<b-out>/...

set -u

KEEP=0
NAME=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -k) KEEP=1; shift ;;
        -*) echo "unknown flag: $1" >&2; exit 1 ;;
        *)  NAME="$1"; shift ;;
    esac
done
[[ -n "$NAME" ]] || { echo "usage: $0 [-k] <scenario-name>"; exit 1; }

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

A_SHOT="$REPO/tests/shots/net/${NAME}.client_a.shot"
B_SHOT="$REPO/tests/shots/net/${NAME}.client_b.shot"
[[ -f "$A_SHOT" ]] || { echo "fail: $A_SHOT missing"; exit 1; }
[[ -f "$B_SHOT" ]] || { echo "fail: $B_SHOT missing"; exit 1; }

OUT="$REPO/build/shots/net"
mkdir -p "$OUT"

# Pick a port deterministically per test name so parallel runs don't
# collide.
PORT=$((24200 + $(printf '%s' "$NAME" | cksum | cut -d ' ' -f 1) % 800))

# Spawn the dedicated server in its own tmpdir so each test gets a
# fresh soldut-server.log and the cfg can be tuned per-scenario
# without leaking state.
TMP="$(mktemp -d -t soldut-dedi-XXXXXX)"
SRV_DIR="$TMP/server"
mkdir -p "$SRV_DIR"
cat > "$SRV_DIR/soldut.cfg" <<EOF
# wan-fixes-5 — shot-test dedicated server config. Crossfire is the
# only built-in with authored spawn points (LvlSpawn array) tight
# enough for slot 0 + slot 1 to land within ~200 px of each other;
# Slipstream / Reactor use the global FFA-lane fallback which puts
# them 64 tiles apart and well past one screen.
port=$PORT
auto_start_seconds=1
countdown_default=1
time_limit=8
score_limit=10
mode=ffa
map_rotation=crossfire
mode_rotation=ffa
snapshot_hz=60
EOF

SRV_PID=0
A_PID=0
B_PID=0
cleanup() {
    [[ $A_PID   -gt 0 ]] && kill     "$A_PID"   2>/dev/null
    [[ $B_PID   -gt 0 ]] && kill     "$B_PID"   2>/dev/null
    sleep 0.3
    [[ $A_PID   -gt 0 ]] && kill -9  "$A_PID"   2>/dev/null
    [[ $B_PID   -gt 0 ]] && kill -9  "$B_PID"   2>/dev/null
    [[ $SRV_PID -gt 0 ]] && kill     "$SRV_PID" 2>/dev/null
    sleep 0.3
    [[ $SRV_PID -gt 0 ]] && kill -9  "$SRV_PID" 2>/dev/null
    wait 2>/dev/null
    if [[ "$KEEP" -eq 0 ]]; then
        rm -rf "$TMP"
    else
        echo "kept: $TMP"
    fi
}
trap cleanup EXIT

cd "$REPO"

# 1. Launch dedicated server (cd into its own dir so it picks up the
#    per-test soldut.cfg). Backgrounded; we tail-poll for "listening"
#    before launching clients.
( cd "$SRV_DIR" && exec "$BIN" --dedicated "$PORT" ) >/dev/null 2>&1 &
SRV_PID=$!

# Wait for the server to bind. Poll the log for the listening line.
for ((i = 0; i < 50; ++i)); do
    sleep 0.05
    if grep -q "dedicated: listening on $PORT" "$SRV_DIR/soldut-server.log" 2>/dev/null; then
        break
    fi
done
if ! grep -q "dedicated: listening on $PORT" "$SRV_DIR/soldut-server.log" 2>/dev/null; then
    echo "fail: dedicated server didn't reach listening within 2.5 s"
    sleep 0.3
    cat "$SRV_DIR/soldut-server.log" 2>/dev/null | tail -10
    exit 1
fi

# 2. Launch both clients. Each picks up the port via its shot script's
#    `network connect 127.0.0.1:PORT` directive (so the script needs
#    `network connect 127.0.0.1:$PORT`-style port matching, OR a
#    placeholder we substitute). To keep test scripts as plain `.shot`
#    files (no preprocessor), each scenario's scripts hard-code the
#    base port number; we sed-substitute the cksum-derived port into
#    temporary copies at run time.
A_TMP="$TMP/$(basename "$A_SHOT")"
B_TMP="$TMP/$(basename "$B_SHOT")"
sed -e "s/{{PORT}}/$PORT/g" "$A_SHOT" > "$A_TMP"
sed -e "s/{{PORT}}/$PORT/g" "$B_SHOT" > "$B_TMP"

"$BIN" --shot "$A_TMP" >/dev/null 2>&1 &
A_PID=$!
sleep 0.15
"$BIN" --shot "$B_TMP" >/dev/null 2>&1 &
B_PID=$!

# Wait for both clients to finish (they each have an `end` directive).
TIMEOUT_S="${TEST_TIMEOUT_S:-60}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    if [[ ! -d /proc/$A_PID && ! -d /proc/$B_PID ]]; then
        break
    fi
done

# Final reap.
[[ -d /proc/$A_PID ]] && kill "$A_PID" 2>/dev/null
[[ -d /proc/$B_PID ]] && kill "$B_PID" 2>/dev/null
sleep 0.3

# Stop the dedicated server explicitly so the next test can rebind.
[[ $SRV_PID -gt 0 ]] && kill "$SRV_PID" 2>/dev/null
sleep 0.3
[[ $SRV_PID -gt 0 ]] && kill -9 "$SRV_PID" 2>/dev/null

echo "=== shot output ==="
A_OUT=$(find "$OUT" -maxdepth 2 -mindepth 1 -type d -name '*client_a*' 2>/dev/null | head -1)
B_OUT=$(find "$OUT" -maxdepth 2 -mindepth 1 -type d -name '*client_b*' 2>/dev/null | head -1)
[[ -n "$A_OUT" ]] && ls -la "$A_OUT" 2>/dev/null | grep "\.png" || echo "(no a shots)"
[[ -n "$B_OUT" ]] && ls -la "$B_OUT" 2>/dev/null | grep "\.png" || echo "(no b shots)"

echo
echo "=== dedi assertions ==="

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

A_LOG=$(find "$A_OUT" -maxdepth 1 -name '*.log' 2>/dev/null | head -1)
B_LOG=$(find "$B_OUT" -maxdepth 1 -name '*.log' 2>/dev/null | head -1)
S_LOG="$SRV_DIR/soldut-server.log"

asrt "server reaches the listening state" \
    "grep -q 'dedicated: listening on $PORT' '$S_LOG'"
asrt "client_a connects and receives ACCEPT" \
    "grep -qE 'ACCEPT client_id=' '$A_LOG'"
asrt "client_b connects and receives ACCEPT" \
    "grep -qE 'ACCEPT client_id=' '$B_LOG'"
asrt "both clients reach MODE_MATCH" \
    "grep -q 'ROUND_START' '$A_LOG' && grep -q 'ROUND_START' '$B_LOG'"
asrt "both clients see snapshots" \
    "grep -q 'first snapshot' '$A_LOG' && grep -q 'first snapshot' '$B_LOG'"
asrt "both clients resolve their local mech" \
    "grep -q 'local_mech_id resolved' '$A_LOG' && grep -q 'local_mech_id resolved' '$B_LOG'"
asrt "neither client is authoritative (no listen-server)" \
    "! grep -q 'world.authoritative = true' '$A_LOG' && ! grep -q 'world.authoritative = true' '$B_LOG'"

# wan-fixes-5 — display consistency between the two windows. Both
# clients should see BOTH mechs (mech=0 and mech=1) in their per-tick
# pelvis dumps. Local mech is marked with '*' so:
#   client_a log: mech=0* (self) and mech=1 (remote)
#   client_b log: mech=1* (self) and mech=0 (remote)
asrt "client_a sees its own local mech (mech=0*)" \
    "grep -qE 'mech=0\* pelv' '$A_LOG'"
asrt "client_a sees the remote mech (mech=1, no asterisk)" \
    "grep -qE 'mech=1 pelv' '$A_LOG'"
asrt "client_b sees its own local mech (mech=1*)" \
    "grep -qE 'mech=1\* pelv' '$B_LOG'"
asrt "client_b sees the remote mech (mech=0, no asterisk)" \
    "grep -qE 'mech=0 pelv' '$B_LOG'"

# Server-side hit registration. Both clients fire at each other; the
# dedicated server should log fire events with shooter mech_id 0 AND
# mech_id 1 (proving server is processing both peers' inputs through
# the same simulate / weapons path).
asrt "server processes fires from client_a (shooter mech=0)" \
    "grep -qE 'fire mech=0 wpn=' '$S_LOG'"
asrt "server processes fires from client_b (shooter mech=1)" \
    "grep -qE 'fire mech=1 wpn=' '$S_LOG'"

# Phase 1 lag-comp wires through identically on both clients — neither
# is privileged. Each client's fires should carry the `lag_comp=N`
# suffix in the server's log (server-side hitscan was rewound to that
# tick).
asrt "server lag-comps client_a's fires (lag_comp= field present)" \
    "grep -qE 'fire mech=0 wpn=.*lag_comp=' '$S_LOG'"
asrt "server lag-comps client_b's fires (lag_comp= field present)" \
    "grep -qE 'fire mech=1 wpn=.*lag_comp=' '$S_LOG'"

# Snapshot rate confirmation — dedicated server uses cfg.snapshot_hz.
asrt "dedicated server runs at 60 Hz snapshot rate" \
    "grep -q 'snapshot=60 Hz' '$S_LOG'"

echo
echo "== dedi summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
