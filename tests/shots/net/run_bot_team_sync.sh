#!/usr/bin/env bash
#
# tests/shots/net/run_bot_team_sync.sh — verifies that the host
# clicking a bot's team chip in the lobby UI flows over the wire to
# the in-process server thread, then back to every peer (host UI +
# remote client). Regression for the user-reported "changing a bot
# from red to blue does not update on both client and server lobby
# screens" sync bug.
#
# Architecture: spawns a `--dedicated PORT` server, then connects
# two clients. Client A is the FIRST to connect (the server marks
# it is_host=true on accept — matches the wan-fixes-16 production
# path where the host UI is the first peer of an in-process server
# thread). Client A clicks "Add Bot" then flips the bot's team
# twice. Client B is a pure observer that must see both transitions.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

NAME="2p_bot_team_sync"
A_SHOT="$REPO/tests/shots/net/${NAME}.client_a.shot"
B_SHOT="$REPO/tests/shots/net/${NAME}.client_b.shot"
[[ -f "$A_SHOT" ]] || { echo "fail: $A_SHOT missing"; exit 1; }
[[ -f "$B_SHOT" ]] || { echo "fail: $B_SHOT missing"; exit 1; }

OUT="$REPO/build/shots/net"
mkdir -p "$OUT"

PORT=$((24200 + $(printf '%s' "$NAME" | cksum | cut -d ' ' -f 1) % 800))

TMP="$(mktemp -d -t soldut-dedi-XXXXXX)"
SRV_DIR="$TMP/server"
mkdir -p "$SRV_DIR"
# Long auto_start so the round never begins during the test. TDM so
# the team chip is meaningful (FFA aliases RED). NO `bots=` line —
# the host UI adds bots via the wire after connecting (matches
# real-world flow where the player clicks "Add Bot" in the lobby).
cat > "$SRV_DIR/soldut.cfg" <<EOF
port=$PORT
auto_start_seconds=60
countdown_default=1
time_limit=30
score_limit=25
mode=tdm
friendly_fire=0
map_rotation=crossfire
mode_rotation=tdm
EOF

SRV_PID=0
A_PID=0
B_PID=0
cleanup() {
    [[ $A_PID -gt 0 ]] && kill    "$A_PID"   2>/dev/null
    [[ $B_PID -gt 0 ]] && kill    "$B_PID"   2>/dev/null
    sleep 0.3
    [[ $A_PID -gt 0 ]] && kill -9 "$A_PID"   2>/dev/null
    [[ $B_PID -gt 0 ]] && kill -9 "$B_PID"   2>/dev/null
    [[ $SRV_PID -gt 0 ]] && kill    "$SRV_PID" 2>/dev/null
    sleep 0.3
    [[ $SRV_PID -gt 0 ]] && kill -9 "$SRV_PID" 2>/dev/null
    wait 2>/dev/null
    # Preserve the server log next to the client outputs so assertions
    # below (and post-mortem debugging) can read it without racing
    # the tmpdir cleanup.
    if [[ -f "$SRV_DIR/soldut-server.log" ]]; then
        mkdir -p "$OUT/dedi"
        cp "$SRV_DIR/soldut-server.log" \
           "$OUT/dedi/${NAME}.server.log" 2>/dev/null
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

cd "$REPO"

# Launch dedicated server.
( cd "$SRV_DIR" && exec "$BIN" --dedicated "$PORT" ) >/dev/null 2>&1 &
SRV_PID=$!

for ((i = 0; i < 50; ++i)); do
    sleep 0.05
    if grep -q "dedicated: listening on $PORT" "$SRV_DIR/soldut-server.log" 2>/dev/null; then
        break
    fi
done
if ! grep -q "dedicated: listening on $PORT" "$SRV_DIR/soldut-server.log" 2>/dev/null; then
    echo "fail: dedicated server didn't reach listening within 2.5 s"
    cat "$SRV_DIR/soldut-server.log" 2>/dev/null | tail -10
    exit 1
fi

# Sed-substitute the per-test port into temporary script copies (same
# pattern as run_dedi.sh).
A_TMP="$TMP/$(basename "$A_SHOT")"
B_TMP="$TMP/$(basename "$B_SHOT")"
sed -e "s/{{PORT}}/$PORT/g" "$A_SHOT" > "$A_TMP"
sed -e "s/{{PORT}}/$PORT/g" "$B_SHOT" > "$B_TMP"

# Launch client A first — it connects first and becomes is_host=true.
"$BIN" --shot "$A_TMP" >/dev/null 2>&1 &
A_PID=$!
sleep 0.3   # give A a moment to ACCEPT before B joins
"$BIN" --shot "$B_TMP" >/dev/null 2>&1 &
B_PID=$!

TIMEOUT_S="${TEST_TIMEOUT_S:-60}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    if [[ ! -d /proc/$A_PID && ! -d /proc/$B_PID ]]; then break; fi
done
[[ -d /proc/$A_PID ]] && kill "$A_PID" 2>/dev/null
[[ -d /proc/$B_PID ]] && kill "$B_PID" 2>/dev/null
sleep 0.3
[[ $SRV_PID -gt 0 ]] && kill "$SRV_PID" 2>/dev/null
sleep 0.3

A_OUT=$(find "$OUT/dedi" -maxdepth 1 -mindepth 1 -type d -name '*client_a*' 2>/dev/null | head -1)
B_OUT=$(find "$OUT/dedi" -maxdepth 1 -mindepth 1 -type d -name '*client_b*' 2>/dev/null | head -1)

A_LOG=$(find "$A_OUT" -maxdepth 1 -name '*.log' 2>/dev/null | head -1)
B_LOG=$(find "$B_OUT" -maxdepth 1 -name '*.log' 2>/dev/null | head -1)
# Save the server log to a stable location BEFORE cleanup deletes
# the tmpdir, so assertions + post-mortem can both read it.
mkdir -p "$OUT/dedi"
S_LOG="$OUT/dedi/${NAME}.server.log"
cp "$SRV_DIR/soldut-server.log" "$S_LOG" 2>/dev/null

echo
echo "=== bot-team-sync assertions ==="
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else                echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

# Both clients must connect cleanly.
asrt "server reaches the listening state" \
    "grep -q 'dedicated: listening on $PORT' '$S_LOG'"
asrt "client A connects and receives ACCEPT" \
    "grep -q 'ACCEPT client_id=' '$A_LOG'"
asrt "client B connects and receives ACCEPT" \
    "grep -q 'ACCEPT client_id=' '$B_LOG'"

# Client A is is_host=true (first to connect). Server logs ADD_BOT
# on receipt; bot lands at slot 2 because client B joined first.
asrt "client A sent ADD_BOT via the wire" \
    "grep -qE 'shot: add_bot tier=1 \\(sent to host\\)' '$A_LOG'"
asrt "server accepted ADD_BOT from host (slot 2)" \
    "grep -qE 'server: ADD_BOT slot=2' '$S_LOG'"

# Bot team flip wire round-trip (slot 2 = the just-added bot).
asrt "client A sent BOT_TEAM (slot=2 team=2 BLUE)" \
    "grep -qE 'shot: bot_team slot=2 team=2 \\(sent to host\\)' '$A_LOG'"
asrt "server applied BOT_TEAM slot=2 team=2 (BLUE)" \
    "grep -qE 'server: BOT_TEAM slot=2 team=2' '$S_LOG'"
asrt "client A sent BOT_TEAM (slot=2 team=1 RED)" \
    "grep -qE 'shot: bot_team slot=2 team=1 \\(sent to host\\)' '$A_LOG'"
asrt "server applied BOT_TEAM slot=2 team=1 (RED)" \
    "grep -qE 'server: BOT_TEAM slot=2 team=1' '$S_LOG'"

# Client B must see LOBBY_LIST broadcasts — proves the table reached
# the remote peer.
asrt "client B received LOBBY_LIST broadcasts" \
    "grep -q 'lobby_list' '$B_LOG'"

echo
echo "== bot-team-sync summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
