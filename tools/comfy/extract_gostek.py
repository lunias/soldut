#!/usr/bin/env python3
"""
extract_gostek.py — slice a Soldat-style "gostek part sheet" into a
per-chassis sprite atlas.

A gostek part sheet (tools/comfy/gostek_part_sheets/<chassis>_gostek_v*.png)
is a clean reference layout: 22 flat-shaded, black-outlined body parts
on a white field, each captioned "N. PART_NAME" in reading order
(TORSO, HIP_PLATE, HEAD, SHOULDER_L/R, UPPER_ARM_L/R, FOREARM_L/R,
HAND_L/R, UPPER_LEG_L/R, LOWER_LEG_L/R, FOOT_L/R, then 5 STUMP_CAP).
This is the style the renderer actually wants (Soldat's gostek model:
per-part rigid skinning, each sprite hooked to a skeleton point with an
anchor; see https://wiki.soldat.pl/index.php/Mod.ini and OpenSoldat's
client/GostekRendering.pas) — clean simple plates, not detailed
illustration crops.

Pipeline:
  1. detect the 22 part shapes (non-white connected components, area-
     filtered to drop the caption text)
  2. order them by reading rows (band-cluster on bbox top, sort by left
     within a band) -> caption sequence 1..22 -> part names
  3. for each part: crop tight (incl. the outline), key the white field
     to transparent, apply the per-part rotate/flip needed to land in
     its vertical/horizontal atlas slot, resize to the slot's draw_w x
     draw_h, paste at the slot's dst_x,dst_y
  4. optional 2-colour snap (Foundry palette by default; the runtime
     halftone_post shader adds the screen at framebuffer res)
  5. write assets/sprites/<chassis>.png + an annotated preview + a
     reproducibility manifest

The atlas layout (dst/pivot/draw_w/h per part) mirrors s_default_parts
in src/mech_sprites.c, so no transcribe pass is needed.

Usage:
  python extract_gostek.py SHEET.png CHASSIS [--palette foundry]
        [--no-post] [--out assets/sprites/<chassis>.png]
"""

from __future__ import annotations
import argparse
import os
import sys
from PIL import Image, ImageDraw, ImageFont
import numpy as np

try:
    from scipy import ndimage as ndi

    _HAVE_SCIPY = True
except Exception:
    _HAVE_SCIPY = False

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from crop_canonical import PALETTES, post_process_tile  # noqa: E402

# Caption order in the sheet (1..22), and the MechSpriteId each maps to.
SHEET_PARTS = [
    "TORSO",
    "HIP_PLATE",
    "HEAD",
    "SHOULDER_L",
    "SHOULDER_R",
    "UPPER_ARM_L",
    "UPPER_ARM_R",
    "FOREARM_L",
    "FOREARM_R",
    "HAND_L",
    "HAND_R",
    "UPPER_LEG_L",
    "UPPER_LEG_R",
    "LOWER_LEG_L",
    "LOWER_LEG_R",
    "FOOT_L",
    "FOOT_R",
    "STUMP_CAP",
    "STUMP_CAP",
    "STUMP_CAP",
    "STUMP_CAP",
    "STUMP_CAP",
]
# sheet caption name -> atlas-slot id (the keys of ATLAS below). The 5
# STUMP_CAPs fill the 5 stump slots in order.
NAME_TO_SLOT = {
    "TORSO": "torso",
    "HIP_PLATE": "hip_plate",
    "HEAD": "head",
    "SHOULDER_L": "shoulder_l",
    "SHOULDER_R": "shoulder_r",
    "UPPER_ARM_L": "arm_upper_l",
    "UPPER_ARM_R": "arm_upper_r",
    "FOREARM_L": "arm_lower_l",
    "FOREARM_R": "arm_lower_r",
    "HAND_L": "hand_l",
    "HAND_R": "hand_r",
    "UPPER_LEG_L": "leg_upper_l",
    "UPPER_LEG_R": "leg_upper_r",
    "LOWER_LEG_L": "leg_lower_l",
    "LOWER_LEG_R": "leg_lower_r",
    "FOOT_L": "foot_l",
    "FOOT_R": "foot_r",
}
STUMP_SLOTS = [
    "stump_shoulder_l",
    "stump_shoulder_r",
    "stump_hip_l",
    "stump_hip_r",
    "stump_neck",
]

# s_default_parts mirror (src/mech_sprites.c): slot -> (dst_x,dst_y,pivot_x,pivot_y,draw_w,draw_h)
ATLAS = {
    "torso": (0, 0, 28, 36, 56, 72),
    "head": (60, 0, 20, 20, 40, 40),
    "hip_plate": (104, 0, 32, 18, 64, 36),
    "shoulder_l": (172, 0, 28, 20, 56, 40),
    "shoulder_r": (232, 0, 28, 20, 56, 40),
    "leg_upper_l": (0, 80, 18, 48, 36, 96),
    "leg_upper_r": (40, 80, 18, 48, 36, 96),
    "leg_lower_l": (80, 80, 16, 44, 32, 88),
    "leg_lower_r": (116, 80, 16, 44, 32, 88),
    "foot_l": (152, 80, 16, 24, 32, 24),
    "foot_r": (188, 80, 16, 24, 32, 24),
    "arm_upper_l": (0, 180, 16, 40, 32, 80),
    "arm_upper_r": (36, 180, 16, 40, 32, 80),
    "arm_lower_l": (72, 180, 14, 36, 28, 72),
    "arm_lower_r": (104, 180, 14, 36, 28, 72),
    "hand_l": (136, 180, 8, 8, 16, 16),
    "hand_r": (156, 180, 8, 8, 16, 16),
    "stump_shoulder_l": (0, 264, 16, 16, 32, 32),
    "stump_shoulder_r": (36, 264, 16, 16, 32, 32),
    "stump_hip_l": (72, 264, 16, 16, 32, 32),
    "stump_hip_r": (108, 264, 16, 16, 32, 32),
    "stump_neck": (144, 264, 16, 16, 32, 32),
}
# per-slot orientation fix: the sheet draws forearms horizontally (atlas
# slots are vertical) and the head crown-up (the NECK->HEAD bone gets a
# -180° render rotation, so the source must be authored upside-down).
#   "rot": degrees clockwise to apply;  "flipv": vertical flip after rot.
SLOT_FIX = {
    "arm_lower_l": {"rot": -90},
    "arm_lower_r": {"rot": -90},
    "head": {"flipv": True},
}


def detect_parts(rgb: np.ndarray, min_area_frac=0.0025):
    """Connected components of non-white pixels, area-filtered to drop
    caption text. Returns [(y0,x0,y1,x1)] inclusive bboxes."""
    H, W, _ = rgb.shape
    nonwhite = ~((rgb > 238).all(axis=2))
    if not _HAVE_SCIPY:
        sys.exit("extract_gostek needs scipy (pip install scipy in the ComfyUI venv)")
    lbl, n = ndi.label(nonwhite)
    boxes = []
    min_area = int(min_area_frac * H * W)
    objs = ndi.find_objects(lbl)
    for i, sl in enumerate(objs, start=1):
        if sl is None:
            continue
        ys, xs = sl
        area = int((lbl[sl] == i).sum())
        if area < min_area:
            continue
        # also reject very-thin strips (stray rule lines etc.)
        h, w = ys.stop - ys.start, xs.stop - xs.start
        if min(h, w) < 12:
            continue
        boxes.append((ys.start, xs.start, ys.stop - 1, xs.stop - 1, area))
    return boxes


def order_by_reading(boxes, band_frac=0.075, img_h=2048):
    """Cluster boxes into reading rows by bbox-top, then sort left->right
    within a row; concatenate rows top->bottom. Returns boxes in caption
    order (1..22)."""
    boxes = sorted(boxes, key=lambda b: b[0])  # by top edge
    bands, cur = [], [boxes[0]]
    band_px = band_frac * img_h
    for b in boxes[1:]:
        if b[0] - cur[-1][0] > band_px and b[0] - cur[0][0] > band_px:
            bands.append(cur)
            cur = [b]
        else:
            cur.append(b)
    bands.append(cur)
    out = []
    for band in bands:
        out.extend(sorted(band, key=lambda b: b[1]))  # by left edge
    return out, bands


def crop_part(img: Image.Image, box, pad=3):
    y0, x0, y1, x1 = box[:4]
    W, H = img.size
    x0 = max(0, x0 - pad)
    y0 = max(0, y0 - pad)
    x1 = min(W - 1, x1 + pad)
    y1 = min(H - 1, y1 + pad)
    tile = img.crop((x0, y0, x1 + 1, y1 + 1)).convert("RGBA")
    # key the white field to transparent (interior orange/black survive —
    # they're not near-white). flood from the tile border so an enclosed
    # white panel inside a part stays opaque.
    a = np.asarray(tile)
    near_white = (a[:, :, :3] > 236).all(axis=2)
    seed = np.zeros_like(near_white)
    seed[0, :] = near_white[0, :]
    seed[-1, :] = near_white[-1, :]
    seed[:, 0] = near_white[:, 0]
    seed[:, -1] = near_white[:, -1]
    if _HAVE_SCIPY:
        bg = ndi.binary_propagation(seed, mask=near_white)
    else:
        bg = near_white
    out = a.copy()
    out[bg, 3] = 0
    return Image.fromarray(out, "RGBA")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet")
    ap.add_argument("chassis")
    ap.add_argument("--palette", default="foundry", choices=list(PALETTES))
    ap.add_argument(
        "--no-post",
        action="store_true",
        help="skip the 2-colour palette snap (keep sheet colours)",
    )
    ap.add_argument("--out", default=None)
    ap.add_argument("--preview", default=None)
    ap.add_argument("--atlas-w", type=int, default=1024)
    ap.add_argument("--atlas-h", type=int, default=1024)
    args = ap.parse_args(argv)

    out_path = args.out or os.path.join(
        REPO, "assets", "sprites", f"{args.chassis}.png"
    )
    prev_path = args.preview or os.path.join(
        HERE, "output", f"{args.chassis}_gostek_atlas_preview.png"
    )
    manifest_path = os.path.join(
        HERE, "gostek_part_sheets", f"{args.chassis}_gostek_manifest.txt"
    )
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    os.makedirs(os.path.dirname(prev_path), exist_ok=True)
    os.makedirs(os.path.dirname(manifest_path), exist_ok=True)

    img = Image.open(args.sheet).convert("RGB")
    W, H = img.size
    rgb = np.asarray(img)
    boxes = detect_parts(rgb)
    print(f"detected {len(boxes)} part shapes (expected 22)")
    if len(boxes) != 22:
        print(
            "  WARNING: not 22 — the area filter or band clustering may need tuning; "
            "proceeding with what was found",
            file=sys.stderr,
        )
    ordered, bands = order_by_reading(boxes, img_h=H)
    print(f"  {len(bands)} reading rows: " + ", ".join(str(len(b)) for b in bands))

    palette = PALETTES[args.palette]
    atlas = Image.new("RGBA", (args.atlas_w, args.atlas_h), (0, 0, 0, 0))
    manifest = [
        f"# {args.chassis} gostek manifest — source {os.path.basename(args.sheet)} ({W}x{H})",
        f"# palette={args.palette}  post={'no' if args.no_post else 'yes'}",
        "# caption_idx  part_name  slot  src_bbox(x0,y0,x1,y1)  fix  -> dst(x,y) draw(w,h)",
    ]
    stump_i = 0
    placed = []
    for idx, box in enumerate(ordered, start=1):
        name = SHEET_PARTS[idx - 1] if idx <= len(SHEET_PARTS) else f"EXTRA_{idx}"
        if name == "STUMP_CAP":
            slot = STUMP_SLOTS[stump_i] if stump_i < len(STUMP_SLOTS) else None
            stump_i += 1
        else:
            slot = NAME_TO_SLOT.get(name)
        if slot is None or slot not in ATLAS:
            print(f"  #{idx:2d} {name:<12} -> (no slot) — skipped")
            continue
        dx, dy, px, py, dw, dh = ATLAS[slot]
        y0, x0, y1, x1 = box[:4]
        tile = crop_part(img, box)
        fix = SLOT_FIX.get(slot, {})
        rot = fix.get("rot", 0)
        if rot == -90:
            tile = tile.transpose(Image.ROTATE_270)
        elif rot == 90:
            tile = tile.transpose(Image.ROTATE_90)
        elif rot == 180:
            tile = tile.transpose(Image.ROTATE_180)
        if fix.get("flipv"):
            tile = tile.transpose(Image.FLIP_TOP_BOTTOM)
        tile = tile.resize((dw, dh), Image.LANCZOS)
        if not args.no_post:
            tile = post_process_tile(tile, palette)
        atlas.alpha_composite(tile, (dx, dy))
        placed.append((idx, name, slot, (x0, y0, x1, y1), fix, dx, dy, dw, dh))
        print(
            f"  #{idx:2d} {name:<12} -> slot {slot:<14} src {x1 - x0 + 1}x{y1 - y0 + 1}"
            f"{('  ' + ('rot' + str(rot)) if rot else '') + ('  flipv' if fix.get('flipv') else '')}"
            f"  -> dst {dx},{dy} ({dw}x{dh})"
        )
        manifest.append(
            f"{idx:>2}  {name:<12} {slot:<14} ({x0},{y0},{x1},{y1})  "
            f"{('rot' + str(rot) if rot else '-') + ('+flipv' if fix.get('flipv') else '')}  "
            f"-> ({dx},{dy}) ({dw}x{dh})"
        )

    atlas.save(out_path, format="PNG", optimize=True)
    open(manifest_path, "w").write("\n".join(manifest) + "\n")
    print(f"wrote {out_path}  ({atlas.size[0]}x{atlas.size[1]})")
    print(f"manifest {manifest_path}")

    # annotated preview: the sheet with the detected/assigned boxes, plus
    # the atlas at 2x in the corner.
    prev = img.convert("RGB").copy()
    d = ImageDraw.Draw(prev)
    try:
        font = ImageFont.truetype("arial.ttf", max(20, W // 60))
    except Exception:
        font = ImageFont.load_default()
    for idx, name, slot, bb, fix, *_rest in placed:
        x0, y0, x1, y1 = bb
        d.rectangle((x0, y0, x1, y1), outline=(0, 120, 255), width=4)
        d.text((x0 + 4, y0 + 4), f"{idx}.{name}->{slot}", fill=(0, 120, 255), font=font)
    # paste the atlas (scaled up) into the bottom-right
    sc = 2
    av = atlas.resize((atlas.size[0] * sc, atlas.size[1] * sc), Image.NEAREST)
    prev.paste(
        Image.new("RGB", av.size, (235, 235, 235)), (W - av.size[0], H - av.size[1])
    )
    prev.paste(av, (W - av.size[0], H - av.size[1]), av)
    d.rectangle(
        (W - av.size[0], H - av.size[1], W - 1, H - 1), outline=(255, 0, 255), width=4
    )
    prev.save(prev_path)
    print(f"preview {prev_path}")


if __name__ == "__main__":
    main()
