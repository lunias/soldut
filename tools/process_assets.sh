#!/usr/bin/env bash
#
# Pipeline 7 — per-asset ImageMagick post-process.
#
# Input:  assets/raw/<subpath>/<file>.png  (authored / Perplexity-generated)
# Output: assets/<subpath>/<file>.png      (raw/ stripped; runtime reads this)
#
# Per documents/m5/11-art-direction.md §"Pipeline 7", the post does:
#   1. ordered-dither halftone (h6x6a Bayer)
#   2. -remap <palette>.png   (hard quantize to the map's 2-colour palette)
#   3. optional paper-noise multiply at 25% alpha (if assets/raw/paper_noise.png exists)
#   4. nearest-point halve-then-double round trip
#      (kills bicubic smoothness without changing the final pixel dimensions)
#
# The palette is picked by the map subdirectory under assets/raw/maps/<short>/.
# Files outside a map directory (decorations, generic UI raw) fall back to
# the foundry palette unless --palette overrides.
#
# Chassis sprites at assets/sprites/<chassis>.png are pre-snapped by
# tools/comfy/extract_gostek.py and are NOT routed through this script;
# they live in assets/sprites/ directly, not assets/raw/.
#
# Requires: ImageMagick (`magick` binary).
#

set -euo pipefail

# ImageMagick 7 ships `magick`; IM6 uses `convert`. Either works for the
# operations below; auto-detect rather than forcing one.
if command -v magick >/dev/null 2>&1; then
    MAGICK="magick"
elif command -v convert >/dev/null 2>&1; then
    MAGICK="convert"
else
    echo "process_assets: neither 'magick' nor 'convert' on PATH — install ImageMagick" >&2
    exit 1
fi

PALETTES_DIR="assets/raw/palettes"
PAPER_NOISE="assets/raw/paper_noise.png"
DEFAULT_PALETTE_NAME="foundry"

# Build palette PNGs first if they aren't on disk yet.
if [ ! -d "$PALETTES_DIR" ] || [ -z "$(ls -A "$PALETTES_DIR" 2>/dev/null)" ]; then
    bash tools/build_palettes.sh
fi

DEFAULT_PALETTE="$PALETTES_DIR/$DEFAULT_PALETTE_NAME.png"
if [ ! -f "$DEFAULT_PALETTE" ]; then
    echo "process_assets: default palette $DEFAULT_PALETTE missing — run tools/build_palettes.sh" >&2
    exit 1
fi

if [ ! -d "assets/raw" ]; then
    echo "process_assets: nothing to do — assets/raw/ does not exist yet"
    exit 0
fi

count=0
shopt -s globstar nullglob
for src in assets/raw/**/*.png; do
    # Skip the palette inputs + paper_noise input itself.
    case "$src" in
        "$PALETTES_DIR"/*) continue ;;
        "$PAPER_NOISE")    continue ;;
    esac

    # Strip assets/raw/ from the path.
    rel="${src#assets/raw/}"
    dst="assets/$rel"
    mkdir -p "$(dirname "$dst")"

    # Pick palette by parent map dir.
    palette="$DEFAULT_PALETTE"
    if [[ "$rel" == maps/*/* ]]; then
        short=$(echo "$rel" | cut -d/ -f2)
        candidate="$PALETTES_DIR/$short.png"
        if [ -f "$candidate" ]; then
            palette="$candidate"
        fi
    fi

    if [ -f "$PAPER_NOISE" ]; then
        "$MAGICK" "$src" \
            -ordered-dither h6x6a \
            -remap "$palette" \
            \( "$PAPER_NOISE" -alpha set -channel A -evaluate set 25% +channel \) \
            -compose multiply -composite \
            -filter point -resize 50% -resize 200% \
            "$dst"
    else
        "$MAGICK" "$src" \
            -ordered-dither h6x6a \
            -remap "$palette" \
            -filter point -resize 50% -resize 200% \
            "$dst"
    fi
    pal_short=$(basename "$palette" .png)
    echo "  $src -> $dst  (palette: $pal_short)"
    count=$((count + 1))
done

if [ "$count" -eq 0 ]; then
    echo "process_assets: no PNGs found under assets/raw/ (excluding palettes + paper_noise)"
else
    echo "process_assets: processed $count asset(s)"
fi
