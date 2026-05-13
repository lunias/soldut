#!/usr/bin/env bash
#
# tools/build_jet_atlases.sh — generate the M6 P02 jetpack FX atlases.
#
# Output:
#   assets/sprites/jet_plume.png  (32×96, RGBA8, white RGB + teardrop alpha)
#   assets/sprites/jet_dust.png   (16×16, RGBA8, white RGB + soft-disc alpha)
#
# Both atlases are Soldut-original procedural geometry — single
# authored gradient that the per-jetpack tint colors multiply at draw
# time. Re-run when you want to tweak the curves; output is
# deterministic given the same source.
#
# Requires: Python 3 with Pillow (PIL). Install with
#   pip install Pillow      (or via system package manager)
#
# Spec: documents/m6/02-jetpack-propulsion-fx.md §8 "The plume sprite quad"
#       — "vertical radial-gradient flame, bright white at the top-
#       center (nozzle end), fading to transparent at the bottom-tip;
#       sharper at the sides (narrow rim), softer at the bottom".

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT_DIR="assets/sprites"
mkdir -p "$OUT_DIR"

python3 - <<'PY'
import math
from PIL import Image

PLUME_W, PLUME_H = 32, 96
DUST_SIZE       = 16

# ---- Plume: white teardrop with sharp rim and soft tail ------------
#
# Authored with the nozzle end at the TOP of the image (y=0). The
# runtime rotates the quad so "top" pins to the nozzle position and
# the rest of the image stretches along the thrust direction.
plume = Image.new('RGBA', (PLUME_W, PLUME_H), (255, 255, 255, 0))
for y in range(PLUME_H):
    v = y / (PLUME_H - 1)                    # 0 at top (nozzle), 1 at bottom (tail)
    # Vertical brightness: held bright in the upper third, then
    # smooth fade to zero at the bottom. Slight pow-curve avoids a
    # hard-line cutoff at the top.
    vert = (1.0 - v) ** 0.55
    # Width factor: 0.30 (narrow nozzle) → 1.0 (full atlas width at tail).
    width = 0.30 + 0.70 * v
    for x in range(PLUME_W):
        u = (x - (PLUME_W - 1) / 2.0) / ((PLUME_W - 1) / 2.0)  # -1..1
        if width <= 0.0:
            a = 0
        else:
            r = abs(u) / width
            if r >= 1.0:
                a = 0
            else:
                # Sharp circular rim with smooth interior. (1-r^2)^p
                # with p ≈ 1.2 keeps a near-flat interior and a crisp
                # edge — "narrow rim" per spec §8.
                edge = (1.0 - r * r) ** 1.2
                a = int(round(255 * vert * edge))
                if a < 0:   a = 0
                if a > 255: a = 255
        plume.putpixel((x, y), (255, 255, 255, a))
plume.save('assets/sprites/jet_plume.png')

# ---- Dust: soft Gaussian disc -------------------------------------
#
# 16x16 grey-white puff. RGB = white; tint at draw time picks warm
# dust vs pale steam. Exponential falloff so the edge fades smoothly
# instead of hard-clipping.
dust = Image.new('RGBA', (DUST_SIZE, DUST_SIZE), (255, 255, 255, 0))
cx = cy = (DUST_SIZE - 1) / 2.0
R  = (DUST_SIZE - 1) / 2.0
for y in range(DUST_SIZE):
    for x in range(DUST_SIZE):
        dx = (x - cx) / R
        dy = (y - cy) / R
        r2 = dx * dx + dy * dy
        if r2 >= 1.0:
            a = 0
        else:
            # Tighter falloff than a 2D Gaussian — gives a small dense
            # core with a quick fade. Multiplied by 1.0 at center.
            a = int(round(255 * math.exp(-r2 * 2.6)))
            if a > 255: a = 255
        dust.putpixel((x, y), (255, 255, 255, a))
dust.save('assets/sprites/jet_dust.png')
print('jet_atlases: wrote assets/sprites/jet_plume.png (32x96) and jet_dust.png (16x16)')
PY
