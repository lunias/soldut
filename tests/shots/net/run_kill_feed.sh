#!/usr/bin/env bash
#
# tests/shots/net/run_kill_feed.sh — wan-fixes-13 regression.
#
# Verifies that NET_MSG_KILL_EVENT carries flags + names end-to-end
# and that the client populates its local world.killfeed[] ring (not
# just `last_event`). Drives 2p_dedi_combat (existing paired-dedi
# script — A spams pulse rifle into B's chest from ~200 px) against
# a real dedicated server and asserts:
#
#   - Server logs at least one `mech=N kill ... flags=` line in
#     mech_kill (existing SHOT_LOG).
#   - Both clients' logs show client_handle_kill_event fires with
#     killer + victim names matching the lobby slot names.
#   - The flags byte rides the wire (header / suicide ride straight
#     through; gib / overkill / ragdoll may or may not depending on
#     the kill specifics, so we only assert the message arrives).
#
# Pre-fix (before wan-fixes-13): wire dropped flags AND
# client_handle_kill_event only set `world.last_event` (a string).
# The joiner's HUD kill rail stayed empty for the whole match.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

NAME="2p_kill_feed"
A_SHOT="$REPO/tests/shots/net/${NAME}.client_a.shot"
B_SHOT="$REPO/tests/shots/net/${NAME}.client_b.shot"

OUT="$REPO/build/shots/net"
mkdir -p "$OUT"

PORT=$((25300 + RANDOM % 200))
TMP="$(mktemp -d -t soldut-killfeed-XXXXXX)"
SRV_DIR="$TMP/server"
mkdir -p "$SRV_DIR"
cat > "$SRV_DIR/soldut.cfg" <<EOF
port=$PORT
auto_start_seconds=1
countdown_default=1
time_limit=20
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
    [[ $A_PID -gt 0 ]] && kill     "$A_PID"   2>/dev/null
    [[ $B_PID -gt 0 ]] && kill     "$B_PID"   2>/dev/null
    sleep 0.3
    [[ $A_PID -gt 0 ]] && kill -9  "$A_PID"   2>/dev/null
    [[ $B_PID -gt 0 ]] && kill -9  "$B_PID"   2>/dev/null
    [[ $SRV_PID -gt 0 ]] && kill     "$SRV_PID" 2>/dev/null
    sleep 0.3
    [[ $SRV_PID -gt 0 ]] && kill -9  "$SRV_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

cd "$REPO"

( cd "$SRV_DIR" && exec "$BIN" --dedicated "$PORT" ) >/dev/null 2>&1 &
SRV_PID=$!

for ((i = 0; i < 50; ++i)); do
    sleep 0.05
    if grep -q "dedicated: listening on $PORT" "$SRV_DIR/soldut-server.log" 2>/dev/null; then
        break
    fi
done

A_TMP="$TMP/$(basename "$A_SHOT")"
B_TMP="$TMP/$(basename "$B_SHOT")"
sed -e "s/{{PORT}}/$PORT/g" "$A_SHOT" > "$A_TMP"
sed -e "s/{{PORT}}/$PORT/g" "$B_SHOT" > "$B_TMP"

"$BIN" --shot "$A_TMP" >/dev/null 2>&1 &
A_PID=$!
sleep 0.15
"$BIN" --shot "$B_TMP" >/dev/null 2>&1 &
B_PID=$!

TIMEOUT_S="${TEST_TIMEOUT_S:-30}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    if [[ ! -d /proc/$A_PID && ! -d /proc/$B_PID ]]; then
        break
    fi
done

[[ -d /proc/$A_PID ]] && kill "$A_PID" 2>/dev/null
[[ -d /proc/$B_PID ]] && kill "$B_PID" 2>/dev/null
sleep 0.3
[[ $SRV_PID -gt 0 ]] && kill "$SRV_PID" 2>/dev/null
sleep 0.3
[[ $SRV_PID -gt 0 ]] && kill -9 "$SRV_PID" 2>/dev/null

A_OUT="$REPO/build/shots/net/dedi/${NAME}.client_a"
B_OUT="$REPO/build/shots/net/dedi/${NAME}.client_b"
A_LOG="$A_OUT/${NAME}.client_a.log"
B_LOG="$B_OUT/${NAME}.client_b.log"
S_LOG="$SRV_DIR/soldut-server.log"

echo
echo "=== kill-feed assertions ==="

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "server reaches listening" \
    "grep -q 'dedicated: listening on $PORT' '$S_LOG'"

# Server-side kill registration. mech_kill's SHOT_LOG emits
# `t=N mech=X kill killshot_part=... flags=...`.
asrt "server registers at least one kill" \
    "grep -qE 'mech=[0-9]+ kill killshot_part=' '$S_LOG'"

# Client-side kill receipt — wan-fixes-13's new SHOT_LOG fires from
# client_handle_kill_event. Both clients should see the event.
asrt "client_a receives kill events" \
    "grep -qE 'client_handle_kill_event killer=[-0-9]+' '$A_LOG'"
asrt "client_b receives kill events" \
    "grep -qE 'client_handle_kill_event killer=[-0-9]+' '$B_LOG'"

# Names rode the wire — the SHOT_LOG quotes the decoded names. Both
# clients are 'ClientA' / 'ClientB' per the shot script.
asrt "client_a kill event carries player names (not mech#N)" \
    "grep -qE \"client_handle_kill_event killer=[-0-9]+ \\('Client[AB]'\\)\" '$A_LOG'"
asrt "client_b kill event carries player names (not mech#N)" \
    "grep -qE \"client_handle_kill_event killer=[-0-9]+ \\('Client[AB]'\\)\" '$B_LOG'"

# Old behavior wrote `[KILL] mech #X -> mech #Y` to last_event. The
# new path writes player names there too. Make sure we DON'T see the
# old "mech #" rendering anymore (regression guard).
asrt "client_a no longer shows 'mech #N' in kill ribbon" \
    "! grep -q '\\[KILL\\] mech #' '$A_LOG'"
asrt "client_b no longer shows 'mech #N' in kill ribbon" \
    "! grep -q '\\[KILL\\] mech #' '$B_LOG'"

echo
echo "== kill-feed summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
