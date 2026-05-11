#!/usr/bin/env bash
#
# Generate per-map 2-colour palette PNGs (2x1 px each) for the
# ImageMagick post-process pipeline. Colours come from
# documents/m5/11-art-direction.md §"Two-color print, per map" — keep
# this script in sync with that table if the palettes ever change.
#
# Output: assets/raw/palettes/<short>.png  (2x1 dark+accent strip)
#
# Requires: ImageMagick (`magick` binary). Install with
#   sudo apt install imagemagick   (Linux)
#   brew install imagemagick       (macOS)
#

set -euo pipefail

# ImageMagick 7 ships `magick`; IM6 uses `convert`. Either works for the
# operations below; auto-detect rather than forcing one.
if command -v magick >/dev/null 2>&1; then
    MAGICK="magick"
elif command -v convert >/dev/null 2>&1; then
    MAGICK="convert"
else
    echo "build_palettes: neither 'magick' nor 'convert' on PATH — install ImageMagick" >&2
    exit 1
fi

PAL_DIR="assets/raw/palettes"
mkdir -p "$PAL_DIR"

gen() {
    # gen <dark> <accent> <short_name>
    "$MAGICK" -size 1x1 "xc:$1" -size 1x1 "xc:$2" +append "$PAL_DIR/$3.png"
}

gen "#1A1612" "#D8731A" foundry
gen "#0F1416" "#3FB6C2" slipstream
gen "#19131A" "#E0B838" concourse
gen "#0E1812" "#4FA67F" reactor
gen "#15121E" "#C2436F" catwalk
gen "#1A1A20" "#9FA8DC" aurora
gen "#1F1A14" "#B57A39" crossfire
gen "#0E1419" "#7BA0C2" citadel

echo "build_palettes: wrote 8 palette PNGs to $PAL_DIR/"
