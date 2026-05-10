#!/usr/bin/env python3
"""
Soldut — crop a ComfyUI canonical PNG into a per-chassis atlas.

Reads a crop table (tools/comfy/crop_tables/<chassis>_crop.txt), pulls
each part rectangle from the canonical 1024x1024 PNG, applies the
Pipeline 7 post (Bayer halftone + 2-color palette remap + nearest-
neighbor downsample to half-size then back), composites everything
into an atlas at the table's dst positions, and writes
assets/sprites/<chassis>.png.

Stump caps (stump_*) are not extracted from the canonical — they're
hand-drawn per documents/m5/11-art-direction.md §"Pipeline 5". Drop
32x32 hand-drawn PNGs into tools/comfy/stumps/<chassis>/<part>.png
(e.g. stump_shoulder_l.png); the script composites them in. If a
hand-drawn stump is missing, the script falls back to extracting the
labeled placeholder box from the canonical (good enough for
integration testing).

Usage:
  ./crop_canonical.py canonical.png trooper [--palette foundry] [--no-post]
                                             [--flip-head]

Output: assets/sprites/<chassis>.png  (1024x1024 atlas)
        tools/comfy/output/<chassis>_atlas_preview.png  (annotated preview)
"""

import argparse
import sys
from pathlib import Path
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parents[2]

# 8x8 Bayer matrix (matches assets/shaders/halftone_post.fs.glsl).
BAYER8 = [
    0,
    32,
    8,
    40,
    2,
    34,
    10,
    42,
    48,
    16,
    56,
    24,
    50,
    18,
    58,
    26,
    12,
    44,
    4,
    36,
    14,
    46,
    6,
    38,
    60,
    28,
    52,
    20,
    62,
    30,
    54,
    22,
    3,
    35,
    11,
    43,
    1,
    33,
    9,
    41,
    51,
    19,
    59,
    27,
    49,
    17,
    57,
    25,
    15,
    47,
    7,
    39,
    13,
    45,
    5,
    37,
    63,
    31,
    55,
    23,
    61,
    29,
    53,
    21,
]

# Map palettes — copy of documents/m5/11-art-direction.md §"Two-color print, per map".
PALETTES = {
    "foundry": ((0x1A, 0x16, 0x12), (0xD8, 0x73, 0x1A)),
    "slipstream": ((0x0F, 0x14, 0x16), (0x3F, 0xB6, 0xC2)),
    "concourse": ((0x19, 0x13, 0x1A), (0xE0, 0xB8, 0x38)),
    "reactor": ((0x0E, 0x18, 0x12), (0x4F, 0xA6, 0x7F)),
    "catwalk": ((0x15, 0x12, 0x1E), (0xC2, 0x43, 0x6F)),
    "aurora": ((0x1A, 0x1A, 0x20), (0x9F, 0xA8, 0xDC)),
    "crossfire": ((0x1F, 0x1A, 0x14), (0xB5, 0x7A, 0x39)),
    "citadel": ((0x0E, 0x14, 0x19), (0x7B, 0xA0, 0xC2)),
}


def parse_crop_table(path):
    rows = []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            parts = s.split()
            if len(parts) < 11:
                print(f"crop table: skipping malformed line: {s}", file=sys.stderr)
                continue
            rows.append(
                {
                    "id": parts[0],
                    "src_x": int(parts[1]),
                    "src_y": int(parts[2]),
                    "src_w": int(parts[3]),
                    "src_h": int(parts[4]),
                    "dst_x": int(parts[5]),
                    "dst_y": int(parts[6]),
                    "pivot_x": int(parts[7]),
                    "pivot_y": int(parts[8]),
                    "draw_w": int(parts[9]),
                    "draw_h": int(parts[10]),
                    # Optional 12th column: degrees CCW applied to the cropped
                    # tile before the resize. T-pose readouts have horizontal
                    # arms; the renderer's arm atlas slots are vertical.
                    "rotate": int(parts[11]) if len(parts) > 11 else 0,
                }
            )
    return rows


def post_process_tile(tile, palette):
    """Apply Pipeline 7: Bayer halftone + 2-color palette remap + nearest-
    point half-size round-trip (the bicubic-killer step). Returns RGBA."""
    if tile.mode != "RGBA":
        tile = tile.convert("RGBA")
    px = tile.load()
    w, h = tile.size

    dark, accent = palette
    # Flat luminance threshold: bright surfaces -> accent, ink/shadow ->
    # dark. NO per-pixel Bayer dither baked in here — at 16-96 px sprite
    # sizes an 8x8 screen reads as noise once the runtime scales the
    # limb down further. The framebuffer halftone_post shader adds the
    # screen at the resolution it belongs (1 cell = 1 screen px).
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            lum = (r * 299 + g * 587 + b * 114) // 1000
            if lum > 110:
                px[x, y] = (accent[0], accent[1], accent[2], a)
            else:
                px[x, y] = (dark[0], dark[1], dark[2], a)

    # Nearest-point half + back-to-original — kills bicubic smoothness
    # left over from the diffusion/decoder pass without adding a true
    # downsample. The contract is identical to ImageMagick's
    # `-filter point -resize 50%` ... `-filter point -resize 200%` round
    # trip and is the core of the Pipeline 7 anti-AI-tell layer.
    half = (max(1, w // 2), max(1, h // 2))
    tile = tile.resize(half, Image.NEAREST).resize((w, h), Image.NEAREST)
    return tile


def alpha_key_white(tile, fuzz=12):
    """Cuts near-white pixels out of a tile (final cleanup after the
    canonical-level background flood-fill — catches any near-white that
    the flood missed, e.g. behind an isolated limb)."""
    px = tile.load()
    w, h = tile.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if r > 255 - fuzz and g > 255 - fuzz and b > 255 - fuzz:
                px[x, y] = (r, g, b, 0)
    return tile


def detect_bg_color(img):
    """Sample the canonical's corners + edge midpoints; return the most
    common color. AI 'white background' prompts usually produce a flat
    light-grey studio backdrop, not pure white, so we detect it rather
    than assume #ffffff."""
    w, h = img.size
    pts = [
        (0, 0),
        (w - 1, 0),
        (0, h - 1),
        (w - 1, h - 1),
        (w // 2, 0),
        (w // 2, h - 1),
        (0, h // 2),
        (w - 1, h // 2),
        (4, 4),
        (w - 5, 4),
        (4, h - 5),
        (w - 5, h - 5),
    ]
    px = img.convert("RGB").load()
    from collections import Counter

    c = Counter(px[x, y] for x, y in pts)
    return c.most_common(1)[0][0]


def strip_background(img, bg_rgb, tol=32):
    """Flood-fill from every border pixel toward transparency for any
    color within `tol` (per-channel max delta) of `bg_rgb`. Flood-fill
    (not a global color key) so mech panels that happen to match the
    backdrop grey survive — only background-connected regions go clear."""
    img = img.convert("RGBA")
    px = img.load()
    w, h = img.size
    br, bg, bb = bg_rgb

    def near_bg(p):
        r, g, b = p[0], p[1], p[2]
        return abs(r - br) <= tol and abs(g - bg) <= tol and abs(b - bb) <= tol

    # BFS from all border pixels.
    stack = []
    for x in range(w):
        stack.append((x, 0))
        stack.append((x, h - 1))
    for y in range(h):
        stack.append((0, y))
        stack.append((w - 1, y))
    seen = bytearray(w * h)
    while stack:
        x, y = stack.pop()
        if x < 0 or y < 0 or x >= w or y >= h:
            continue
        idx = y * w + x
        if seen[idx]:
            continue
        seen[idx] = 1
        p = px[x, y]
        if p[3] == 0 or near_bg(p):
            px[x, y] = (p[0], p[1], p[2], 0)
            stack.append((x + 1, y))
            stack.append((x - 1, y))
            stack.append((x, y + 1))
            stack.append((x, y - 1))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("canonical", type=str, help="Canonical 1024x1024 PNG")
    ap.add_argument(
        "chassis", type=str, help="trooper / scout / heavy / sniper / engineer"
    )
    ap.add_argument(
        "--crop-table",
        type=str,
        default=None,
        help="Override (default: tools/comfy/crop_tables/<chassis>_crop.txt)",
    )
    ap.add_argument(
        "--out",
        type=str,
        default=None,
        help="Override (default: assets/sprites/<chassis>.png)",
    )
    ap.add_argument(
        "--palette",
        type=str,
        default="foundry",
        help=f"Map palette key for the post-process color remap "
        f"({', '.join(PALETTES.keys())}). Default: foundry.",
    )
    ap.add_argument(
        "--no-post",
        action="store_true",
        help="Skip Pipeline 7 (debug only — never ship without).",
    )
    ap.add_argument(
        "--flip-head",
        action="store_true",
        help="Vertically flip the head sprite when extracting "
        "(use if your canonical has the head right-side-up).",
    )
    ap.add_argument(
        "--no-alpha-key",
        action="store_true",
        help="Skip the canonical background flood-fill + the per-tile "
        "near-white key. Use if your canonical already has a clean alpha "
        "channel.",
    )
    ap.add_argument(
        "--bg",
        type=str,
        default=None,
        help="Background color to strip as 'r,g,b'. Default: auto-detect "
        "from the canonical's corners (AI 'white background' prompts emit "
        "a flat light-grey backdrop, not pure white).",
    )
    ap.add_argument(
        "--bg-tol",
        type=int,
        default=34,
        help="Per-channel tolerance for the background flood-fill (default "
        "34). Flood-fill from the borders, so mech panels matching the "
        "backdrop grey survive — only background-connected pixels go clear.",
    )
    ap.add_argument(
        "--atlas-w", type=int, default=1024, help="Atlas width in px (default 1024)."
    )
    ap.add_argument(
        "--atlas-h", type=int, default=1024, help="Atlas height in px (default 1024)."
    )
    args = ap.parse_args()

    chassis = args.chassis.lower()
    canonical_path = Path(args.canonical)
    crop_path = (
        Path(args.crop_table)
        if args.crop_table
        else REPO / "tools" / "comfy" / "crop_tables" / f"{chassis}_crop.txt"
    )
    out_path = (
        Path(args.out) if args.out else REPO / "assets" / "sprites" / f"{chassis}.png"
    )
    stumps_dir = REPO / "tools" / "comfy" / "stumps" / chassis

    if not canonical_path.is_file():
        sys.exit(f"canonical not found: {canonical_path}")
    if not crop_path.is_file():
        sys.exit(f"crop table not found: {crop_path}")
    if args.palette not in PALETTES:
        sys.exit(
            f"unknown palette {args.palette!r}; pick one of {list(PALETTES.keys())}"
        )

    rows = parse_crop_table(crop_path)
    print(f"crop table: {len(rows)} entries from {crop_path}")

    canonical = Image.open(canonical_path).convert("RGBA")
    if canonical.size != (1024, 1024):
        print(
            f"warn: canonical is {canonical.size}, expected 1024x1024 — "
            f"crop coords assume 1024-canonical",
            file=sys.stderr,
        )

    # Strip the studio backdrop on the WHOLE canonical before cropping —
    # keying after the per-tile LANCZOS resize leaves blurred fringe.
    if not args.no_alpha_key:
        if args.bg:
            bg_rgb = tuple(int(v) for v in args.bg.split(","))
        else:
            bg_rgb = detect_bg_color(canonical)
        canonical = strip_background(canonical, bg_rgb, args.bg_tol)
        print(
            f"background: stripped {bg_rgb} ±{args.bg_tol} "
            f"({'override' if args.bg else 'auto-detected'})"
        )

    atlas = Image.new("RGBA", (args.atlas_w, args.atlas_h), (0, 0, 0, 0))
    palette = PALETTES[args.palette]

    for r in rows:
        is_stump = r["id"].startswith("stump_")
        # Stump caps come from a hand-drawn directory if present.
        stump_path = stumps_dir / f"{r['id']}.png"
        if is_stump and stump_path.is_file():
            tile = Image.open(stump_path).convert("RGBA")
            if tile.size != (r["src_w"], r["src_h"]):
                tile = tile.resize((r["src_w"], r["src_h"]), Image.NEAREST)
            print(f"  {r['id']:24s}  hand-drawn from {stump_path}")
        else:
            box = (
                r["src_x"],
                r["src_y"],
                r["src_x"] + r["src_w"],
                r["src_y"] + r["src_h"],
            )
            if box[2] > canonical.width or box[3] > canonical.height:
                print(
                    f"  {r['id']:24s}  SKIP — bbox {box} outside canonical",
                    file=sys.stderr,
                )
                continue
            tile = canonical.crop(box)
            if r["id"] == "head" and args.flip_head:
                tile = tile.transpose(Image.FLIP_TOP_BOTTOM)
            rot = r.get("rotate", 0) % 360
            if rot == 90:
                tile = tile.transpose(Image.ROTATE_90)
            elif rot == 270:
                tile = tile.transpose(Image.ROTATE_270)
            elif rot == 180:
                tile = tile.transpose(Image.ROTATE_180)
            elif rot != 0:
                tile = tile.rotate(
                    rot, expand=True, resample=Image.BICUBIC, fillcolor=(0, 0, 0, 0)
                )
            print(
                f"  {r['id']:24s}  src {r['src_x']:4d},{r['src_y']:4d} "
                f"{r['src_w']}x{r['src_h']}  rot {r.get('rotate', 0):>4d}  →  dst {r['dst_x']:4d},{r['dst_y']:4d}"
            )
            if is_stump:
                print(
                    f"    note: stump {r['id']} pulled from canonical "
                    f"placeholder; drop hand-drawn 32x32 at {stump_path} "
                    f"to override"
                )

        # Resize the cropped tile to the runtime sprite size (draw_w x
        # draw_h) before paste — otherwise larger src crops overflow into
        # adjacent atlas slots and the runtime reads garbage for those
        # slots (visual: feet rendered with leg pixels → mech sinking
        # into the floor). LANCZOS first to preserve detail; the
        # nearest-point round-trip in post_process_tile will re-introduce
        # the per-pixel hardness Pipeline 7 wants.
        target = (r["draw_w"], r["draw_h"])
        if tile.size != target:
            tile = tile.resize(target, Image.LANCZOS)

        if not args.no_post:
            # post_process_tile recolours every opaque pixel to dark/accent,
            # so nothing near-white survives — run it *instead of* the
            # per-tile white key, which would otherwise erase white armour
            # plates (the canonical-level flood-fill already took the bg).
            tile = post_process_tile(tile, palette)
        elif not args.no_alpha_key:
            tile = alpha_key_white(tile)

        atlas.paste(tile, (r["dst_x"], r["dst_y"]), tile)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(out_path, format="PNG", optimize=True)
    print(f"wrote {out_path} ({atlas.size[0]}x{atlas.size[1]})")

    # Annotated preview — atlas + per-part bounding-box overlay.
    preview = atlas.copy()
    pdraw = ImageDraw.Draw(preview)
    for r in rows:
        x0, y0 = r["dst_x"], r["dst_y"]
        x1, y1 = x0 + r["src_w"], y0 + r["src_h"]
        pdraw.rectangle((x0, y0, x1, y1), outline=(255, 0, 255, 255), width=1)
        pdraw.text((x0 + 1, y0 + 1), r["id"], fill=(255, 0, 255, 255))
        # pivot dot
        pdraw.ellipse(
            (
                x0 + r["pivot_x"] - 2,
                y0 + r["pivot_y"] - 2,
                x0 + r["pivot_x"] + 2,
                y0 + r["pivot_y"] + 2,
            ),
            fill=(0, 255, 0, 255),
        )
    preview_path = REPO / "tools" / "comfy" / "output" / f"{chassis}_atlas_preview.png"
    preview_path.parent.mkdir(parents=True, exist_ok=True)
    preview.save(preview_path, format="PNG", optimize=True)
    print(f"preview {preview_path}")


if __name__ == "__main__":
    main()
