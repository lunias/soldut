#!/usr/bin/env python3
"""
pack_weapons_atlas.py — pack a "weapon rack" PNG into the runtime
weapon atlas (assets/sprites/weapons.png), matching the sub-rect
layout that src/weapon_sprites.c::g_weapon_sprites[] expects.

The source PNG is what Perplexity (or any image generator) produced
from the P16 weapon-rack prompt — typically a 2816x1536 image with 14
weapons in a free-form 3-or-4-row layout against a checkered "fake-
transparent" background (light grey #C9C9C9 + white squares).

Pipeline:
  1. Detect non-background pixels — anything outside the grey/white
     checker range. The weapons are orange-on-black ink lines, well
     outside the background tone band.
  2. Run scipy connected-component label, area-filter to drop micro
     artifacts (anti-aliased halos, stray pixels).
  3. Reading-row sort (band-cluster by bbox-top, sort left-to-right
     within a band) so weapons land in caption order.
  4. Take the first 14 components. Each maps in order to the slots
     in g_weapon_sprites[]:
       0  PULSE_RIFLE         8  SIDEARM
       1  PLASMA_SMG          9  BURST_SMG
       2  RIOT_CANNON        10  FRAG_GRENADES
       3  RAIL_CANNON        11  MICRO_ROCKETS
       4  AUTO_CANNON        12  COMBAT_KNIFE
       5  MASS_DRIVER        13  GRAPPLING_HOOK
       6  PLASMA_CANNON
       7  MICROGUN
  5. For each weapon: crop tight to its bbox, key the checker
     background to transparent (alpha=0), resize to the slot's
     src.w x src.h, paste at the slot's src.x, src.y in a fresh
     1024x256 atlas.
  6. Write assets/sprites/weapons.png (overwriting). The source PNG
     should be backed up to tools/comfy/raw_atlases/weapons_source.png
     before running so re-runs are reproducible.

Usage:
  python pack_weapons_atlas.py SOURCE.png
        [--out assets/sprites/weapons.png] [--atlas-w 1024] [--atlas-h 256]
"""

from __future__ import annotations
import argparse
import os
import sys
from PIL import Image
import numpy as np

try:
    from scipy import ndimage as ndi
    _HAVE_SCIPY = True
except Exception:
    _HAVE_SCIPY = False

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# Mirror of src/weapon_sprites.c::g_weapon_sprites[]. Order matches the
# WEAPON_* enum so the Nth detected component lands in the Nth slot.
WEAPON_SLOTS = [
    ("PULSE_RIFLE",    0,   0,  56,  14),
    ("PLASMA_SMG",     60,  0,  48,  16),
    ("RIOT_CANNON",    110, 0,  60,  24),
    ("RAIL_CANNON",    175, 0,  88,  16),
    ("AUTO_CANNON",    270, 0,  60,  16),
    ("MASS_DRIVER",    0,   40, 96,  32),
    ("PLASMA_CANNON",  100, 40, 64,  22),
    ("MICROGUN",       170, 40, 80,  28),
    ("SIDEARM",        260, 40, 28,  10),
    ("BURST_SMG",      300, 40, 44,  14),
    ("FRAG_GRENADES",  0,   80, 16,  16),
    ("MICRO_ROCKETS",  24,  80, 52,  20),
    ("COMBAT_KNIFE",   84,  80, 32,   8),
    ("GRAPPLING_HOOK", 124, 80, 36,  14),
]


def is_background(rgb):
    """The Perplexity "fake-transparent" background is a #C9C9C9 + white
    checker. We treat any pixel where R, G, B are all in the
    [185, 255] band AND are within 25 of each other as background. The
    weapon ink (orange + black) is well outside this tone."""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    # All channels in the checker tone band
    in_band = (r >= 185) & (r <= 255) & (g >= 185) & (g <= 255) & (b >= 185) & (b <= 255)
    # All channels close together (greyscale, not chromatic)
    span = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    grey = span <= 25
    return in_band & grey


def detect_weapons(img: Image.Image, min_area_frac=0.0008):
    """Returns [(y0, x0, y1, x1, area)] inclusive bboxes for weapon
    shapes, sorted by reading order (top-to-bottom in bands, left-to-
    right within band)."""
    rgb = np.asarray(img.convert("RGB"))
    H, W, _ = rgb.shape
    bg = is_background(rgb)
    fg = ~bg
    # Drop very small isolated specks via morphological opening.
    fg = ndi.binary_opening(fg, structure=np.ones((3, 3)))
    labels, n = ndi.label(fg)
    boxes = []
    min_area = int(min_area_frac * H * W)
    for i, sl in enumerate(ndi.find_objects(labels), start=1):
        if sl is None:
            continue
        ys, xs = sl
        area = int((labels[sl] == i).sum())
        if area < min_area:
            continue
        h, w = ys.stop - ys.start, xs.stop - xs.start
        # Reject very thin slivers (stray rule lines, artefacts).
        if min(h, w) < 24:
            continue
        boxes.append((ys.start, xs.start, ys.stop - 1, xs.stop - 1, area))
    print(f"detect_weapons: {n} components → {len(boxes)} pass area/dim filter")
    # Band-cluster by bbox-top with a generous threshold (one band per
    # visible weapon row in the source).
    boxes.sort(key=lambda b: b[0])
    bands = []
    cur = [boxes[0]] if boxes else []
    band_gap = int(0.06 * H)
    for b in boxes[1:]:
        if b[0] - cur[-1][0] > band_gap and b[0] - cur[0][0] > band_gap:
            bands.append(cur)
            cur = [b]
        else:
            cur.append(b)
    if cur:
        bands.append(cur)
    out = []
    for band in bands:
        out.extend(sorted(band, key=lambda b: b[1]))
    print(f"  bands: {len(bands)} rows ({', '.join(str(len(b)) for b in bands)})")
    return out


def crop_alpha_key(img: Image.Image, box, pad=4):
    y0, x0, y1, x1 = box[:4]
    W, H = img.size
    x0 = max(0, x0 - pad)
    y0 = max(0, y0 - pad)
    x1 = min(W - 1, x1 + pad)
    y1 = min(H - 1, y1 + pad)
    tile = img.crop((x0, y0, x1 + 1, y1 + 1)).convert("RGBA")
    arr = np.asarray(tile).copy()
    bg = is_background(arr[:, :, :3])
    arr[bg, 3] = 0
    return Image.fromarray(arr, "RGBA")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="Path to the source weapon-rack PNG")
    ap.add_argument(
        "--out",
        default=os.path.join(REPO, "assets", "sprites", "weapons.png"),
        help="Output atlas path (default: assets/sprites/weapons.png)",
    )
    ap.add_argument("--atlas-w", type=int, default=1024)
    ap.add_argument("--atlas-h", type=int, default=256)
    args = ap.parse_args(argv)

    if not _HAVE_SCIPY:
        sys.exit("pack_weapons_atlas needs scipy (use the ComfyUI venv)")

    img = Image.open(args.source).convert("RGB")
    print(f"source: {args.source}  size: {img.size}")

    boxes = detect_weapons(img)
    if len(boxes) < len(WEAPON_SLOTS):
        print(
            f"WARNING: only {len(boxes)} components detected, expected "
            f"{len(WEAPON_SLOTS)} — slots {len(boxes)}..{len(WEAPON_SLOTS) - 1} "
            f"will be empty",
            file=sys.stderr,
        )

    atlas = Image.new("RGBA", (args.atlas_w, args.atlas_h), (0, 0, 0, 0))
    manifest_lines = [
        f"# weapons atlas — source {os.path.basename(args.source)} ({img.size[0]}x{img.size[1]})",
        f"# atlas {args.atlas_w}x{args.atlas_h}",
        "# slot_idx  weapon_name      src_bbox(x0,y0,x1,y1)  -> dst(x,y) (w,h)",
    ]
    for idx, slot in enumerate(WEAPON_SLOTS):
        name, dx, dy, dw, dh = slot
        if idx >= len(boxes):
            print(f"  #{idx:2d} {name:<16} (no component) — skipped")
            continue
        box = boxes[idx]
        y0, x0, y1, x1 = box[:4]
        tile = crop_alpha_key(img, box)
        tile = tile.resize((dw, dh), Image.LANCZOS)
        atlas.alpha_composite(tile, (dx, dy))
        print(
            f"  #{idx:2d} {name:<16} src ({x0},{y0},{x1},{y1}) "
            f"{x1 - x0 + 1}x{y1 - y0 + 1}  ->  dst ({dx},{dy}) ({dw}x{dh})"
        )
        manifest_lines.append(
            f"{idx:>2}  {name:<16} ({x0},{y0},{x1},{y1})  -> ({dx},{dy}) ({dw}x{dh})"
        )

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    atlas.save(args.out, format="PNG", optimize=True)
    print(f"wrote {args.out}  ({atlas.size[0]}x{atlas.size[1]})")

    manifest_path = os.path.join(HERE, "weapons_atlas_manifest.txt")
    open(manifest_path, "w").write("\n".join(manifest_lines) + "\n")
    print(f"manifest {manifest_path}")


if __name__ == "__main__":
    main()
