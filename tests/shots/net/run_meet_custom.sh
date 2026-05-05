#!/usr/bin/env bash
#
# tests/shots/net/run_meet_custom.sh — full P08 round-trip:
#
#   1. Run the editor shot tools/editor/shots/p08_custom_map.shot
#      from <host_dir> so it writes <host_dir>/assets/maps/foundry.lvl.
#   2. Launch the host shot with that .lvl in its assets dir.
#   3. Launch the client shot with NO assets/maps/foundry.lvl and an
#      empty XDG_DATA_HOME → must download the map before round start.
#   4. Both walk toward each other and screenshot the result.
#   5. Assert on:
#        - host streams MAP_CHUNK + sees client MAP_READY
#        - client begins download + caches + transitions to MATCH
#        - cached <crc>.lvl exists in the client's XDG_DATA_HOME
#        - both screenshots wrote out
#
# Exits non-zero on assertion failures. Cleans up host/client child
# processes on exit.

set -u

KEEP=0
if [[ "${1:-}" == "-k" ]]; then KEEP=1; fi

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
GAME="$REPO/soldut"
EDITOR="$REPO/build/soldut_editor"
EDIT_SHOT="$REPO/tools/editor/shots/p08_custom_map.shot"
HOST_SHOT="$REPO/tests/shots/net/p08_meet_custom.host.shot"
CLI_SHOT="$REPO/tests/shots/net/p08_meet_custom.client.shot"

[[ -x "$GAME"   ]] || { echo "fail: $GAME not built — run 'make' first";        exit 1; }
[[ -x "$EDITOR" ]] || { echo "fail: $EDITOR not built — run 'make editor' first"; exit 1; }
[[ -f "$EDIT_SHOT" ]] || { echo "fail: $EDIT_SHOT missing"; exit 1; }
[[ -f "$HOST_SHOT" ]] || { echo "fail: $HOST_SHOT missing"; exit 1; }
[[ -f "$CLI_SHOT"  ]] || { echo "fail: $CLI_SHOT  missing"; exit 1; }

TMP="$(mktemp -d -t soldut-meet-custom-XXXXXX)"
HOST_DIR="$TMP/host"
CLI_DIR="$TMP/client"
CACHE_DIR="$TMP/cache"
mkdir -p "$HOST_DIR" "$CLI_DIR" "$CACHE_DIR"

# Step 1 — editor authors the .lvl into <host_dir>/assets/maps/foundry.lvl.
( cd "$HOST_DIR" && "$EDITOR" --shot "$EDIT_SHOT" ) > "$TMP/editor.log" 2>&1
if [[ ! -f "$HOST_DIR/assets/maps/foundry.lvl" ]]; then
    echo "fail: editor did not write the map"
    cat "$TMP/editor.log"
    exit 1
fi
echo "INFO: editor wrote $(stat -c%s "$HOST_DIR/assets/maps/foundry.lvl") byte custom map"

# soldut.cfg in BOTH cwds — single-map rotation on `foundry`.
for d in "$HOST_DIR" "$CLI_DIR"; do
    cat > "$d/soldut.cfg" <<EOF
port=24600
auto_start_seconds=2
time_limit=8
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

# Step 2 — launch host shot. cd into HOST_DIR so the game's cwd is
# the dir with assets/maps/foundry.lvl + soldut.cfg.
( cd "$HOST_DIR" && exec "$GAME" --shot "$HOST_SHOT" ) \
    > "$HOST_DIR/stdout.log" 2>&1 &
HOST_PID=$!

sleep 0.7

# Step 3 — launch client shot. cwd has soldut.cfg but NO assets/maps/
# directory; XDG_DATA_HOME points at an empty cache dir.
( cd "$CLI_DIR" \
  && XDG_DATA_HOME="$CACHE_DIR" \
     exec "$GAME" --shot "$CLI_SHOT" ) \
    > "$CLI_DIR/stdout.log" 2>&1 &
CLI_PID=$!

# Wait for both to finish naturally.
TIMEOUT_S="${TEST_TIMEOUT_S:-30}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    if [[ ! -d /proc/$HOST_PID && ! -d /proc/$CLI_PID ]]; then
        break
    fi
done

[[ -d /proc/$CLI_PID  ]] && kill "$CLI_PID"  2>/dev/null
[[ -d /proc/$HOST_PID ]] && kill "$HOST_PID" 2>/dev/null
sleep 0.5

# ---- Assertions ----
# The game's cwd was each side's tmp dir, so the `out` directive in
# the .shot file resolves there. We mirror those into REPO's build
# tree at the end so a reviewer can find the screenshots in a
# predictable place — useful when the test passes but a human still
# wants to look at the meeting frames.
HOST_OUT="$HOST_DIR/build/shots/net/p08_meet_custom_host"
CLI_OUT="$CLI_DIR/build/shots/net/p08_meet_custom_client"
MIRROR_OUT="$REPO/build/shots/net"
mkdir -p "$MIRROR_OUT"
[[ -d "$HOST_OUT" ]] && cp -r "$HOST_OUT" "$MIRROR_OUT/" 2>/dev/null
[[ -d "$CLI_OUT"  ]] && cp -r "$CLI_OUT"  "$MIRROR_OUT/" 2>/dev/null
HL=$(find "$HOST_OUT" -maxdepth 1 -name '*.log' 2>/dev/null | head -1)
CL=$(find "$CLI_OUT"  -maxdepth 1 -name '*.log' 2>/dev/null | head -1)

PASS=0
FAIL=0
asrt() {
    local label="$1"; local cond="$2"
    if eval "$cond"; then echo "PASS: $label"; PASS=$((PASS+1));
    else                  echo "FAIL: $label"; FAIL=$((FAIL+1)); fi
}

# Output dirs + screenshots.
asrt "host shot wrote PNGs"        "[[ \$(ls '$HOST_OUT'/*.png 2>/dev/null | wc -l) -ge 3 ]]"
asrt "client shot wrote PNGs"      "[[ \$(ls '$CLI_OUT'/*.png 2>/dev/null | wc -l) -ge 3 ]]"
asrt "host log exists"             "[[ -f '$HL' ]]"
asrt "client log exists"           "[[ -f '$CL' ]]"

# Map-share wire flow on each side.
asrt "host advertises non-zero CRC" "grep -qE 'maps: serve assets/maps/foundry.lvl \\(crc=[0-9a-f]+, [0-9]+ bytes\\)' '$HL' 2>/dev/null"
asrt "host streams chunks"          "grep -qE 'server: MAP_REQUEST crc=[0-9a-f]+ → streamed [0-9]+ chunks' '$HL' 2>/dev/null"
asrt "host sees client MAP_READY"   "grep -qE 'server: peer 0 MAP_READY' '$HL' 2>/dev/null"

asrt "client begins download"       "grep -qE 'downloading map crc=[0-9a-f]+' '$CL' 2>/dev/null"
asrt "client cached + ready"        "grep -qE 'client: map crc=[0-9a-f]+ cached \\+ ready' '$CL' 2>/dev/null"
asrt "client sends MAP_READY ok"    "grep -qE 'client: MAP_READY crc=[0-9a-f]+ status=0' '$CL' 2>/dev/null"
asrt "client transitions to MATCH"  "grep -qE 'client: ROUND_START map=' '$CL' 2>/dev/null"
asrt "client receives snapshots"    "grep -q  'first snapshot' '$CL' 2>/dev/null"

# Cache file exists.
CRC_HEX="$(grep -oE 'maps: serve [^ ]+ \(crc=[0-9a-f]+' "$HL" 2>/dev/null | head -n 1 \
           | sed -E 's/.*crc=([0-9a-f]+)/\1/')"
if [[ -n "$CRC_HEX" ]]; then
    asrt "client cached <crc>.lvl"  "[[ -f '$CACHE_DIR/soldut/maps/$CRC_HEX.lvl' ]]"
else
    echo "FAIL: could not extract CRC from host log"; FAIL=$((FAIL+1))
fi

# Both runs reached `end` — proves no crash mid-shot.
asrt "host shot ran to end"   "grep -q 'shotmode: networked done' '$HL' 2>/dev/null"
asrt "client shot ran to end" "grep -q 'shotmode: networked done' '$CL' 2>/dev/null"

echo
echo "== meet-on-custom summary: $PASS passed, $FAIL failed =="
echo "outputs: $HOST_OUT and $CLI_OUT"
[[ $FAIL -eq 0 ]] || exit 1
exit 0
