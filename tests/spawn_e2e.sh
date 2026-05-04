#!/usr/bin/env bash
# tests/spawn_e2e.sh — end-to-end test of the M5 P04 F5 spawn-fix.
#
# Walks the full editor → game pipeline:
#   1. Drive the editor in shot mode to author a .lvl with a floor
#      across the bottom and a raised platform mid-map. Spawn point
#      sits on top of the platform (not floating in midair, not on
#      the bottom floor — must be the platform).
#   2. Run `./soldut --test-play <lvl>` for a few seconds. The game
#      enables SHOT_LOG when --test-play is on, so soldut.log gets:
#        - the lobby spawn line (proves the spawn coords were used)
#        - per-second pelvis-pos lines (proves the mech settled on
#          the platform under gravity, not on the bottom floor)
#   3. Grep both and assert they match the platform position.
#
# Pre-fix, map_spawn_point ignored level->spawns, so the mech landed
# in the hardcoded g_red_lanes[0] tile column of the bottom floor.

set -e

cd "$(dirname "$0")/.."
ROOT=$(pwd)

TMP=$(mktemp -d -t soldut-spawn-e2e-XXXXXX)
trap 'rm -rf "$TMP"' EXIT

LVL="$TMP/spawn_e2e.lvl"
BUILD_SHOT="$TMP/build.shot"
GAME_LOG="$TMP/soldut.log"

# ---- Scene layout (60×40 tile map = 1920×1280 px world) -----------
#
#   row  0..27   air
#   row 28..29   raised platform, columns 40..49 (256 px wide)
#   row 30..35   air below the platform
#   row 36..39   bottom floor across the whole map
#
# Spawn placement (pelvis position):
#   Standard standing pose puts feet at pelvis_y + 36 (px), and
#   spawn settles to floor_top - 4 (foot_clearance). Platform top is
#   at row 28 = y 896, so a mech standing on the platform has
#   pelvis_y = 896 - 36 - 4 = 856.  Platform x range is 40*32..50*32
#   = 1280..1600; spawn at column 44 mid (= 1424) puts the mech
#   solidly on top.
EXPECTED_X=1424
EXPECTED_Y=856
PLATFORM_TOP=896

cat > "$BUILD_SHOT" <<EOF
window 800 600
new 60 40
ticks 30
panels off

# bottom floor across the whole map
at 0 tool tile
at 0 tile_flags solid
at 1 tile_fill_rect    0 1152  1920 1280

# raised platform (10 tiles wide) at y = 896..960
at 2 tile_fill_rect 1280  896  1600  960

# spawn on top of the platform
at 5 spawn_add $EXPECTED_X $EXPECTED_Y 0

at 10 save $LVL
at 20 end
EOF

echo "[1/2] building .lvl via editor shot mode..."
"$ROOT/build/soldut_editor" --shot "$BUILD_SHOT" >/dev/null

if [ ! -f "$LVL" ]; then
    echo "FAIL: editor did not produce $LVL"
    exit 1
fi

echo "[2/2] running ./soldut --test-play $LVL ..."
# auto_start=1s + match countdown=1s + ~3 s settle window = ~5 s total.
cd "$ROOT"
timeout 6 ./soldut --test-play "$LVL" >/dev/null 2>&1 || true
cp soldut.log "$GAME_LOG"

# ---- Assertion 1: spawn coords match what the editor saved ---------
SPAWN_LINE=$(grep "lobby: slot 0 .* spawn" "$GAME_LOG" | tail -1 || true)
if [ -z "$SPAWN_LINE" ]; then
    echo "FAIL: no 'lobby: slot 0 ... spawn' SHOT_LOG line in $GAME_LOG"
    echo "--- last 30 log lines ---"
    tail -30 "$GAME_LOG"
    exit 1
fi
echo "spawn-line: $SPAWN_LINE"

LOGGED_X=$(echo "$SPAWN_LINE" | sed -E 's/.*spawn \(([0-9]+)\..*/\1/')
LOGGED_Y=$(echo "$SPAWN_LINE" | sed -E 's/.*\, ([0-9]+)\..*/\1/')

if [ "$LOGGED_X" != "$EXPECTED_X" ] || [ "$LOGGED_Y" != "$EXPECTED_Y" ]; then
    echo "FAIL: spawn ($LOGGED_X, $LOGGED_Y) != expected ($EXPECTED_X, $EXPECTED_Y)"
    exit 1
fi
echo "ok: spawn coords = ($LOGGED_X, $LOGGED_Y)"

# ---- Assertion 2: mech settled on the platform (not bottom floor) -
# Per-second pelvis log: "[SHOT ] test-play: tick N  pelvis (X, Y)  ..."
# Take the LAST one — by then physics has settled.
SETTLE_LINE=$(grep "test-play: tick.*pelvis" "$GAME_LOG" | tail -1 || true)
if [ -z "$SETTLE_LINE" ]; then
    echo "FAIL: no per-second 'test-play: ... pelvis' line in $GAME_LOG"
    echo "      (this means SHOT_LOG didn't fire — g_shot_mode flag?)"
    echo "--- last 30 log lines ---"
    tail -30 "$GAME_LOG"
    exit 1
fi
echo "settle-line: $SETTLE_LINE"

SETTLE_X=$(echo "$SETTLE_LINE" | sed -E 's/.*pelvis \(([0-9]+)\..*/\1/')
SETTLE_Y=$(echo "$SETTLE_LINE" | sed -E 's/.*\, ([0-9]+)\..*/\1/')

# Pelvis should be near (1424, 856). Allow a little drift in X (the
# mech rocks at rest by a pixel or two) and require Y to be within
# ~16 px of 856. Critically, Y must be FAR from the bottom floor
# (which would put pelvis at ~1112 — that's the bug we're catching).
DX=$(( SETTLE_X - EXPECTED_X )); [ $DX -lt 0 ] && DX=$(( -DX ))
DY=$(( SETTLE_Y - EXPECTED_Y )); [ $DY -lt 0 ] && DY=$(( -DY ))

if [ $DX -gt 32 ]; then
    echo "FAIL: post-settle X drift too large: |$SETTLE_X - $EXPECTED_X| = $DX > 32 px"
    exit 1
fi
if [ $DY -gt 32 ]; then
    echo "FAIL: mech did not land on the platform: settle y=$SETTLE_Y, expected ~$EXPECTED_Y, |delta|=$DY"
    echo "      This is the original bug — mech fell to the bottom floor instead."
    exit 1
fi

echo "ok: mech settled at ($SETTLE_X, $SETTLE_Y) — on the platform"
echo "spawn_e2e: all assertions passed"
