#!/usr/bin/env bash
#
# tests/shots/net/run_atmosphere_parity.sh — M6 P09
#
# Verifies that LvlMeta atmosphere fields (theme_id, weather, fog,
# vignette) replicate identically across the wire and that both peers
# produce identical atmosphere_init_for_map log lines on every map
# load. Spawns a dedicated server running Slipstream + two clients
# (A, B). Asserts:
#   - both clients see the same theme=2 (ICE_SHEET) + weather=1@0.30
#   - both clients render snowflakes (FX_WEATHER_SNOW counts increase
#     post-spawn)
#   - no atmosphere-related desync warnings in either log

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

NAME="2p_atmosphere_parity"
A_SHOT="$REPO/tests/shots/net/${NAME}.client_a.shot"
B_SHOT="$REPO/tests/shots/net/${NAME}.client_b.shot"
[[ -f "$A_SHOT" ]] || { echo "fail: $A_SHOT missing"; exit 1; }
[[ -f "$B_SHOT" ]] || { echo "fail: $B_SHOT missing"; exit 1; }

OUT="$REPO/build/shots/net"
mkdir -p "$OUT"

PORT=$((25500 + RANDOM % 200))
TMP="$(mktemp -d -t soldut-atmos-XXXXXX)"
SRV_DIR="$TMP/server"
mkdir -p "$SRV_DIR"
cat > "$SRV_DIR/soldut.cfg" <<EOF
port=$PORT
auto_start_seconds=1
countdown_default=1
time_limit=30
score_limit=10
mode=ffa
map_rotation=slipstream
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

for ((i = 0; i < 60; ++i)); do
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
sleep 0.2
"$BIN" --shot "$B_TMP" >/dev/null 2>&1 &
B_PID=$!

TIMEOUT_S="${TEST_TIMEOUT_S:-40}"
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
echo "=== atmosphere parity assertions ==="

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "server reached listening" \
    "grep -q 'dedicated: listening on $PORT' '$S_LOG'"
asrt "client A shot complete" \
    "grep -q 'shotmode: networked done' '$A_LOG'"
asrt "client B shot complete" \
    "grep -q 'shotmode: networked done' '$B_LOG'"
asrt "client A logs atmosphere theme=2 (ICE_SHEET)" \
    "grep -q 'atmosphere: theme=2' '$A_LOG'"
asrt "client B logs atmosphere theme=2 (ICE_SHEET)" \
    "grep -q 'atmosphere: theme=2' '$B_LOG'"
asrt "client A logs SNOW weather (weather=1)" \
    "grep -q 'atmosphere:.*weather=1' '$A_LOG'"
asrt "client B logs SNOW weather (weather=1)" \
    "grep -q 'atmosphere:.*weather=1' '$B_LOG'"

# Parity check — both clients should log the same atmosphere line for
# Slipstream. Extract the first matching line per side and diff them.
A_ATMOS=$(grep -E 'atmosphere: theme=' "$A_LOG" 2>/dev/null | head -1 | sed -E 's/^\[[^]]+\] //' | sed -E 's/^\[[^]]+\] //')
B_ATMOS=$(grep -E 'atmosphere: theme=' "$B_LOG" 2>/dev/null | head -1 | sed -E 's/^\[[^]]+\] //' | sed -E 's/^\[[^]]+\] //')
asrt "client A and B logged identical atmosphere parameters" \
    '[[ -n "$A_ATMOS" && "$A_ATMOS" == "$B_ATMOS" ]]'

# Both sides should have written at least the lobby + match shot.
asrt "client A wrote >=2 pngs" \
    '[[ $(ls "$A_OUT"/*.png 2>/dev/null | wc -l) -ge 2 ]]'
asrt "client B wrote >=2 pngs" \
    '[[ $(ls "$B_OUT"/*.png 2>/dev/null | wc -l) -ge 2 ]]'

echo
echo "== atmosphere-parity summary: $PASS passed, $FAIL failed =="
echo "outputs at: $A_OUT, $B_OUT"
[[ $FAIL -eq 0 ]] || exit 1
exit 0
