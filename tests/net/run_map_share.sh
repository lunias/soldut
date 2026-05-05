#!/usr/bin/env bash
#
# tests/net/run_map_share.sh — end-to-end map-sharing test (M5 P08).
#
# Setup:
#   - Host's TMP dir gets a synthesized custom .lvl at assets/maps/foundry.lvl.
#   - Client's TMP dir has NO assets/maps/ → resolve falls through to cache.
#   - Client's XDG_DATA_HOME points to a fresh dir so its cache starts empty.
#
# Expected wire flow:
#   INITIAL_STATE (with descriptor) → MAP_REQUEST → MAP_CHUNK* → MAP_READY ok
#   → ROUND_START → SNAPSHOT stream → ROUND_END.
#
# Asserts on log lines from both peers + the cache directory contents.

set -u

KEEP=0
if [[ "${1:-}" == "-k" ]]; then KEEP=1; fi

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/soldut"
SYNTH="$REPO/build/synth_map"
if [[ ! -x "$BIN" ]];   then echo "fail: $BIN not built — run 'make' first"          >&2; exit 1; fi
if [[ ! -x "$SYNTH" ]]; then echo "fail: $SYNTH not built — run 'make build/synth_map' first" >&2; exit 1; fi

PORT=$((24000 + RANDOM % 1000))
TMP="$(mktemp -d -t soldut-mapshare-XXXXXX)"
HOST_DIR="$TMP/host"
CLI_DIR="$TMP/client"
CACHE_DIR="$TMP/cache"
mkdir -p "$HOST_DIR" "$CLI_DIR" "$CACHE_DIR"
mkdir -p "$HOST_DIR/assets/maps"

# Synthesize a custom .lvl so the host's CRC differs from any
# checked-in map. Seed includes PORT so successive test runs don't
# share the same CRC (helps with cache eviction tests).
"$SYNTH" "$HOST_DIR/assets/maps/foundry.lvl" "$PORT" > /dev/null

# Same soldut.cfg in both cwds — single-map rotation on `foundry`.
for d in "$HOST_DIR" "$CLI_DIR"; do
    cat > "$d/soldut.cfg" <<EOF
port=$PORT
auto_start_seconds=4
time_limit=4
score_limit=5
mode=ffa
map_rotation=foundry
mode_rotation=ffa
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
( cd "$HOST_DIR" && exec "$BIN" --host "$PORT" --name HostA ) \
    > "$HOST_DIR/stdout.log" 2>&1 &
HOST_PID=$!

sleep 0.6

# Launch client with XDG_DATA_HOME pointed at a fresh dir so the cache
# is empty; client also has no assets/maps/foundry.lvl so it MUST
# download from the host.
( cd "$CLI_DIR" \
  && XDG_DATA_HOME="$CACHE_DIR" \
     exec "$BIN" --connect "127.0.0.1:$PORT" --name ClientB ) \
    > "$CLI_DIR/stdout.log" 2>&1 &
CLI_PID=$!

# Round timeline: connect (~0.4 s) + auto_start 4 s + countdown 5 s +
# round 4 s + summary 15 s ≈ 28 s. We just need round_end to fire.
WAIT_S="${TEST_WAIT_S:-25}"
for ((i = 0; i < WAIT_S; ++i)); do
    if [[ ! -d /proc/$HOST_PID || ! -d /proc/$CLI_PID ]]; then
        break
    fi
    sleep 1
done

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
assert_file() {
    local label="$1"; local path="$2"
    if [[ -f "$path" ]]; then
        echo "PASS: $label ($(stat -c%s "$path") bytes)"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $label (missing $path)"
        FAIL=$((FAIL + 1))
    fi
}

# ---- Host assertions ----
assert_log "host advertises non-zero map crc"  "$HL" "maps: serve assets/maps/foundry.lvl \\(crc=[0-9a-f]+, [0-9]+ bytes\\)"
assert_log "host receives MAP_REQUEST"         "$HL" "MAP_REQUEST crc="
assert_log "host streamed chunks"              "$HL" "server: MAP_REQUEST crc=[0-9a-f]+ → streamed [0-9]+ chunks"
assert_log "host sees client MAP_READY"        "$HL" "server: peer 0 MAP_READY crc=[0-9a-f]+"
assert_log "host begins round"                 "$HL" "match: round begin"

# ---- Client assertions ----
assert_log "client lobby applied"              "$CL" "INITIAL_STATE applied — in lobby"
assert_log "client sees pending map"           "$CL" "client: pending map crc=[0-9a-f]+ size=[0-9]+ short=foundry"
assert_log "client begins download"            "$CL" "downloading map crc=[0-9a-f]+"
assert_log "client sends MAP_REQUEST"          "$CL" "client: MAP_REQUEST crc="
assert_log "client cached + ready"             "$CL" "client: map crc=[0-9a-f]+ cached \\+ ready"
assert_log "client sends MAP_READY ok"         "$CL" "client: MAP_READY crc=[0-9a-f]+ status=0"
assert_log "client transitions to MATCH"       "$CL" "client: ROUND_START map=0"
assert_log "client receives snapshots"         "$CL" "client: first snapshot"
assert_log "client receives ROUND_END"         "$CL" "client: ROUND_END"

# ---- Cache assertion ----
# CRC is in the host's log line; pluck it and check the cache file.
CRC_HEX="$(grep -oE 'maps: serve [^ ]+ \(crc=[0-9a-f]+' "$HL" | head -n 1 \
           | sed -E 's/.*crc=([0-9a-f]+)/\1/')"
if [[ -n "$CRC_HEX" ]]; then
    assert_file "client cache contains <crc>.lvl" "$CACHE_DIR/soldut/maps/$CRC_HEX.lvl"
else
    echo "FAIL: could not extract CRC from host log"
    FAIL=$((FAIL + 1))
fi

echo
echo "== map-share summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
