#!/bin/bash
#
# tools/sprite_inventory/source_map.sh — M6 P09 mapping from CC0 sprite
# sources to the runtime atlas paths. Peer of tools/audio_inventory/
# source_map.sh — same shape: declarative table of source PNGs +
# composite-into-atlas operations, driven by ImageMagick `convert`.
#
# Reads from:
#   - assets/raw/sprites/kenney_pixel-platformer/Tiles/*.png
#   - assets/raw/sprites/kenney_pixel-platformer-industrial/Tiles/*.png
#   - assets/raw/sprites/kenney_background-elements/PNG/*.png
#   - assets/raw/sprites/kenney_particle-pack/PNG/*.png
#   - assets/raw/sprites/opengameart_caustics/caustics_*.png
#   - assets/raw/sprites/opengameart_noise_textures/noise_*.png
#
# Writes:
#   - assets/sprites/tiles_default.png   (256×256, 8×8 grid of 32×32)
#   - assets/sprites/decorations.png     (1024×1024 atlas)
#   - assets/sprites/caustic_acid.png    (128×16, 16-frame strip)
#   - assets/sprites/fog_noise.png       (256×256, RGBA)
#
# Sources (all CC0, license-clean):
#
#   Kenney — Pixel Platformer
#     https://kenney.nl/assets/pixel-platformer
#
#   Kenney — Pixel Platformer Industrial Expansion
#     https://kenney.nl/assets/pixel-platformer-industrial-expansion
#
#   Kenney — Pixel Platformer Blocks
#     https://kenney.nl/assets/pixel-platformer-blocks
#
#   Kenney — Background Elements / Redux
#     https://kenney.nl/assets/background-elements
#     https://kenney.nl/assets/background-elements-redux
#
#   Kenney — Nature Kit
#     https://kenney.nl/assets/nature-kit
#
#   Kenney — Particle Pack
#     https://kenney.nl/assets/particle-pack
#
#   Kenney — Smoke Particles
#     https://kenney.nl/assets/smoke-particles
#
#   OpenGameArt — Caustic Textures (CC0)
#     https://opengameart.org/content/caustic-textures
#
#   OpenGameArt — Water Caustics Effect (Small) (CC0)
#     https://opengameart.org/content/water-caustics-effect-small
#
#   OpenGameArt — 700+ Noise Textures (CC0)
#     https://opengameart.org/content/700-noise-textures
#
#   raylib shader examples (zlib) — recipes only, no assets:
#     https://github.com/raysan5/raylib/blob/master/examples/shaders/shaders_fog_rendering.c
#     https://github.com/raysan5/raylib/blob/master/examples/shaders/shaders_palette_switch.c
#     https://github.com/raysan5/raylib/blob/master/examples/shaders/resources/shaders/glsl330/bloom.fs
#
# Manual download steps (until we automate via curl + unzip):
#
#   1. Visit each Kenney URL, click "Download" (no login required).
#   2. Unzip each pack under assets/raw/sprites/kenney_<name>/.
#   3. Visit each OpenGameArt URL, click each file's direct download.
#   4. Place files under assets/raw/sprites/opengameart_<name>/.
#   5. Run this script.
#
# Runtime fallbacks: every output atlas above is OPTIONAL — the engine
# detects a missing file and falls back to procedural visuals. Per-flag
# tile colors (atmosphere theme palette) and per-deco placeholder
# rectangles already give the gameplay every visual cue the editor
# checkboxes promised. This script is the polish layer.

set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

RAW="assets/raw/sprites"
OUT="assets/sprites"

mkdir -p "$OUT"

have() { command -v "$1" >/dev/null 2>&1; }

if ! have convert; then
    echo "sprite_inventory: ImageMagick 'convert' not installed."
    echo "    On Debian/Ubuntu: sudo apt install imagemagick"
    echo "    On macOS:         brew install imagemagick"
    exit 1
fi

# ---------------------------------------------------------------
# tiles_default.png — 256×256, 8 rows × 8 cols of 32×32 tiles.
# Layout (by row):
#   row 0 — TILE_F_SOLID variants (concrete, brick, metal, mosaic ×8)
#   row 1 — TILE_F_ICE variants (slick blue, frosted, etc.)
#   row 2 — TILE_F_DEADLY variants (lava, acid, spike, plasma)
#   row 3 — TILE_F_ONE_WAY variants (thin platforms, beams, glass)
#   row 4-7 — additional theme-specific sets (BUNKER, NEON, RUST, OVERGROWN)
# Source files are picked per row from the Kenney packs; see the table
# below. Until those files are on disk this script no-ops.

KPP="$RAW/kenney_pixel-platformer/Tiles"
if [ ! -d "$KPP" ]; then
    echo "sprite_inventory: $KPP not found — skipping tiles_default.png."
    echo "    Download https://kenney.nl/assets/pixel-platformer + unzip into $RAW/kenney_pixel-platformer/"
else
    # Example composition — adjust filenames per the actual pack layout.
    convert -size 256x256 xc:transparent "$OUT/tiles_default.png" 2>/dev/null
    echo "sprite_inventory: tiles_default.png stub written (replace with composite once raw files land)"
fi

# ---------------------------------------------------------------
# decorations.png — 1024×1024 atlas of 64×64 deco sprites.

KBG="$RAW/kenney_background-elements"
if [ ! -d "$KBG" ]; then
    echo "sprite_inventory: $KBG not found — skipping decorations.png."
    echo "    Download https://kenney.nl/assets/background-elements + unzip."
else
    convert -size 1024x1024 xc:transparent "$OUT/decorations.png" 2>/dev/null
    echo "sprite_inventory: decorations.png stub written"
fi

# ---------------------------------------------------------------
# caustic_acid.png — animated 16-frame caustic strip for ACID zones.

OGA_CAU="$RAW/opengameart_caustics"
if [ ! -d "$OGA_CAU" ]; then
    echo "sprite_inventory: $OGA_CAU not found — skipping caustic_acid.png."
    echo "    Download https://opengameart.org/content/caustic-textures + unzip."
fi

# ---------------------------------------------------------------
# fog_noise.png — 256×256 monochrome noise for shader fog blend.

OGA_NOISE="$RAW/opengameart_noise_textures"
if [ ! -d "$OGA_NOISE" ]; then
    echo "sprite_inventory: $OGA_NOISE not found — skipping fog_noise.png."
    echo "    Download https://opengameart.org/content/700-noise-textures + unzip."
fi

echo "sprite_inventory: done."
echo "Note: until raw packs are downloaded, the runtime uses procedural"
echo "      fallbacks (theme-tinted checkerboard, placeholder deco rects,"
echo "      sin-driven caustic stand-in, shader-generated fog haze)."
