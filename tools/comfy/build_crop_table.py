#!/usr/bin/env python3
"""
build_crop_table.py — generate a per-chassis crop table from a canonical
T-pose image + its detected joints.

The Soldut renderer authors limb sprites *vertically* (top of source =
parent end, bottom = child end; see src/render.c §draw_mech_sprites).
A T-pose readout has the arms *horizontal*, so the arm/hand part
rectangles must be sliced as horizontal strips and rotated 90° into
their vertical atlas slots. This script writes the crop table with that
rotation baked into a 12th `rotate` column (degrees CCW; crop_canonical.py
honours it). It also pre-flattens the canonical's background to pure
white so the downstream alpha-key is clean (handles a decorative border
frame + a cream paper tone), writing the flattened image alongside.

The dst_*, pivot_*, draw_w/h columns mirror s_default_parts in
src/mech_sprites.c so the atlas layout matches what the runtime expects
with no transcribe pass.

Usage:
  python build_crop_table.py CANONICAL.png CHASSIS [--joints J.json]
        [--out-table T.txt] [--out-canonical C.png] [--preview P.png]

Pairs with prepare_skeleton.py (which produces <chassis>_joints.json).
"""

from __future__ import annotations
import argparse
import json
import os
import sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "skeletons"))
from prepare_skeleton import (  # noqa: E402
    _to_mask,
    _largest_blob_bbox,
    _silhouette_row_extent,
    _column_extent,
    _leg_centres_at,
)

# --- s_default_parts mirror (src/mech_sprites.c) : the atlas layout ---
# id : (dst_x, dst_y, pivot_x, pivot_y, draw_w, draw_h)
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
ORDER = [
    "torso",
    "head",
    "hip_plate",
    "shoulder_l",
    "shoulder_r",
    "leg_upper_l",
    "leg_upper_r",
    "leg_lower_l",
    "leg_lower_r",
    "foot_l",
    "foot_r",
    "arm_upper_l",
    "arm_upper_r",
    "arm_lower_l",
    "arm_lower_r",
    "hand_l",
    "hand_r",
    "stump_shoulder_l",
    "stump_shoulder_r",
    "stump_hip_l",
    "stump_hip_r",
    "stump_neck",
]


def clamp_box(x0, y0, x1, y1, W, H):
    x0 = max(0, min(int(x0), W - 2))
    x1 = max(x0 + 1, min(int(x1), W - 1))
    y0 = max(0, min(int(y0), H - 2))
    y1 = max(y0 + 1, min(int(y1), H - 1))
    return x0, y0, x1, y1


def flatten_bg(img: Image.Image) -> Image.Image:
    """Paint everything outside the mech silhouette pure white (#fff) so
    crop_canonical's `--bg 255,255,255` strips it cleanly — handles a
    decorative border frame + a cream paper tone in one shot."""
    rgb = img.convert("RGB")
    mask = _to_mask(rgb)
    (_, _, _, _), body = _largest_blob_bbox(mask)
    import numpy as np

    a = np.asarray(rgb).copy()
    a[~body] = (255, 255, 255)
    out = Image.fromarray(a, "RGB")
    return out, body


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("canonical")
    ap.add_argument("chassis")
    ap.add_argument(
        "--joints",
        default=None,
        help="joints JSON (default: skeletons/<chassis>_joints.json)",
    )
    ap.add_argument(
        "--out-table", default=None, help="default: crop_tables/<chassis>_crop.txt"
    )
    ap.add_argument(
        "--out-canonical",
        default=None,
        help="flattened-bg canonical (default: <canonical stem>_flat.png next to it)",
    )
    ap.add_argument(
        "--preview", default=None, help="default: output/<chassis>_crop_preview.png"
    )
    ap.add_argument(
        "--size",
        type=int,
        default=1024,
        help="resize the canonical's longest side to N before slicing (default 1024)",
    )
    args = ap.parse_args(argv)

    repo = os.path.dirname(HERE) if os.path.basename(HERE) == "comfy" else None
    repo = os.path.abspath(os.path.join(HERE, "..", ".."))
    joints_path = args.joints or os.path.join(
        HERE, "skeletons", f"{args.chassis}_joints.json"
    )
    out_table = args.out_table or os.path.join(
        HERE, "crop_tables", f"{args.chassis}_crop.txt"
    )
    out_canon = args.out_canonical or (
        os.path.splitext(args.canonical)[0] + "_flat.png"
    )
    preview_path = args.preview or os.path.join(
        HERE, "output", f"{args.chassis}_crop_preview.png"
    )
    os.makedirs(os.path.dirname(out_table), exist_ok=True)
    os.makedirs(os.path.dirname(preview_path), exist_ok=True)

    img = Image.open(args.canonical).convert("RGB")
    if args.size and max(img.size) != args.size:
        s = args.size / max(img.size)
        img = img.resize(
            (round(img.size[0] * s), round(img.size[1] * s)), Image.LANCZOS
        )
    W, H = img.size
    img_flat, body = flatten_bg(img)
    img_flat.save(out_canon)
    print(f"flattened canonical -> {out_canon}  ({W}x{H})")

    J = json.load(open(joints_path))["joints"]
    P = {k: tuple(v) for k, v in J.items()}
    cx = (P["L_SHOULDER"][0] + P["R_SHOULDER"][0]) // 2
    # silhouette bbox (on the flattened mask)
    sxs = [x for x in range(W) if body[:, x].any()]
    sys_ = [y for y in range(H) if body[y, :].any()]
    SX0, SX1 = (min(sxs), max(sxs)) if sxs else (0, W - 1)
    SY0, SY1 = (min(sys_), max(sys_)) if sys_ else (0, H - 1)

    def col_ext(x, default=(0, H - 1)):
        e = _column_extent(body, x)
        return e if e else default

    def row_ext(y, default=(0, W - 1)):
        e = _silhouette_row_extent(body, y)
        return e if e else default

    # --- limb thickness probes (perpendicular silhouette extent) ---
    arm_y = P["L_SHOULDER"][1]  # arms run along this row
    ua_l_mid = (P["L_SHOULDER"][0] + P["L_ELBOW"][0]) // 2
    ua_r_mid = (P["R_SHOULDER"][0] + P["R_ELBOW"][0]) // 2
    la_l_mid = (P["L_ELBOW"][0] + P["L_HAND"][0]) // 2
    la_r_mid = (P["R_ELBOW"][0] + P["R_HAND"][0]) // 2
    uaL = col_ext(ua_l_mid)
    uaR = col_ext(ua_r_mid)
    laL = col_ext(la_l_mid)
    laR = col_ext(la_r_mid)
    # leg blob centres + per-row extents
    thigh_y = (P["L_HIP"][1] + P["L_KNEE"][1]) // 2
    shin_y = (P["L_KNEE"][1] + P["L_FOOT"][1]) // 2
    Lleg_th, Rleg_th = _leg_centres_at(body, thigh_y, cx, int(0.1 * W))
    Lleg_sh, Rleg_sh = _leg_centres_at(body, shin_y, cx, int(0.1 * W))

    def leg_span_at(y, side):
        """[x0,x1] of one leg blob at row y; side='L' or 'R'."""
        from prepare_skeleton import _runs

        rs = _runs(body[y], min_len=max(4, W // 80))
        if not rs:
            c = Lleg_th if side == "L" else Rleg_th
            return c - int(0.05 * W), c + int(0.05 * W)
        if len(rs) >= 2:
            r = (
                min(rs, key=lambda r: r[0] + r[1])
                if side == "L"
                else max(rs, key=lambda r: r[0] + r[1])
            )
            return r
        s, e = rs[0]
        return (
            (s, int(s + 0.5 * (e - s))) if side == "L" else (int(s + 0.5 * (e - s)), e)
        )

    torso_w_half = max(
        40,
        (
            row_ext((P["CHEST"][1] + P["PELVIS"][1]) // 2)[1]
            - row_ext((P["CHEST"][1] + P["PELVIS"][1]) // 2)[0]
        )
        // 2,
    )
    hip_re = row_ext(P["PELVIS"][1])
    head_re = row_ext(P["HEAD"][1])
    head_w_half = max(30, (head_re[1] - head_re[0]) // 2)

    # generous overlap pads (canonical px) — captures the torn/exposed-end
    # region that the parent plate normally hides
    PAD = max(4, W // 130)

    boxes = {}  # id -> (x0,y0,x1,y1, rotate)
    # torso (chest->pelvis bone; already vertical, parent=chest at top)
    boxes["torso"] = (
        cx - torso_w_half - 4,
        P["NECK"][1] - 6,
        cx + torso_w_half + 4,
        P["PELVIS"][1] + 2 * PAD,
        0,
    )
    # head (neck->head bone; renderer wants parent=neck at TOP -> we crop
    # right-side-up and pass --flip-head to crop_canonical, rotate stays 0)
    boxes["head"] = (
        cx - head_w_half - 4,
        max(SY0, P["HEAD"][1] - (P["NECK"][1] - P["HEAD"][1]) - PAD),
        cx + head_w_half + 4,
        P["NECK"][1] + PAD,
        0,
    )
    # hip plate (anchored at pelvis particle, drawn upright)
    boxes["hip_plate"] = (
        hip_re[0] - 2,
        P["PELVIS"][1] - 2 * PAD,
        hip_re[1] + 2,
        P["PELVIS"][1] + 2 * PAD,
        0,
    )
    # shoulder pads (anchored at shoulder particle, drawn upright)
    boxes["shoulder_l"] = (
        P["L_SHOULDER"][0] - int(2.2 * PAD),
        arm_y - int(2.6 * PAD),
        P["L_SHOULDER"][0] + int(1.6 * PAD),
        arm_y + int(1.4 * PAD),
        0,
    )
    boxes["shoulder_r"] = (
        P["R_SHOULDER"][0] - int(1.6 * PAD),
        arm_y - int(2.6 * PAD),
        P["R_SHOULDER"][0] + int(2.2 * PAD),
        arm_y + int(1.4 * PAD),
        0,
    )
    # upper arms (shoulder->elbow): horizontal strip, parent end (shoulder)
    # extended by PAD into the pauldron-covered region; rotated so the
    # shoulder side becomes the TOP of the vertical sprite.
    #   left arm extends left  (shoulder x > elbow x) -> rotate +90 (CCW)
    #   right arm extends right (shoulder x < elbow x) -> rotate -90 (CW)
    boxes["arm_upper_l"] = (
        P["L_ELBOW"][0] - PAD,
        uaL[0] - 2,
        P["L_SHOULDER"][0] + 2 * PAD,
        uaL[1] + 2,
        90,
    )
    boxes["arm_upper_r"] = (
        P["R_SHOULDER"][0] - 2 * PAD,
        uaR[0] - 2,
        P["R_ELBOW"][0] + PAD,
        uaR[1] + 2,
        -90,
    )
    # forearms (elbow->hand): parent end (elbow) extended by PAD
    boxes["arm_lower_l"] = (
        P["L_HAND"][0] - PAD,
        laL[0] - 2,
        P["L_ELBOW"][0] + 2 * PAD,
        laL[1] + 2,
        90,
    )
    boxes["arm_lower_r"] = (
        P["R_ELBOW"][0] - 2 * PAD,
        laR[0] - 2,
        P["R_HAND"][0] + PAD,
        laR[1] + 2,
        -90,
    )
    # hands (single-particle, drawn upright): square-ish crop around the
    # fist; rotated to match the forearm so the wrist sits at the top.
    hh = int(2.4 * PAD)
    boxes["hand_l"] = (
        P["L_HAND"][0] - int(2.8 * PAD),
        arm_y - hh,
        P["L_HAND"][0] + int(1.4 * PAD),
        arm_y + hh,
        90,
    )
    boxes["hand_r"] = (
        P["R_HAND"][0] - int(1.4 * PAD),
        arm_y - hh,
        P["R_HAND"][0] + int(2.8 * PAD),
        arm_y + hh,
        -90,
    )
    # legs (hip->knee, knee->foot): already vertical, parent at top
    Lth = leg_span_at(thigh_y, "L")
    Rth = leg_span_at(thigh_y, "R")
    Lsh = leg_span_at(shin_y, "L")
    Rsh = leg_span_at(shin_y, "R")
    boxes["leg_upper_l"] = (
        Lth[0] - 2,
        P["L_HIP"][1] - 2 * PAD,
        Lth[1] + 2,
        P["L_KNEE"][1] + PAD,
        0,
    )
    boxes["leg_upper_r"] = (
        Rth[0] - 2,
        P["R_HIP"][1] - 2 * PAD,
        Rth[1] + 2,
        P["R_KNEE"][1] + PAD,
        0,
    )
    boxes["leg_lower_l"] = (
        Lsh[0] - 2,
        P["L_KNEE"][1] - PAD,
        Lsh[1] + 2,
        P["L_FOOT"][1] + PAD,
        0,
    )
    boxes["leg_lower_r"] = (
        Rsh[0] - 2,
        P["R_KNEE"][1] - PAD,
        Rsh[1] + 2,
        P["R_FOOT"][1] + PAD,
        0,
    )
    # feet (single-particle; pivot at bottom edge so the boot seats on the platform)
    fh = int(3.4 * PAD)
    boxes["foot_l"] = (
        P["L_FOOT"][0] - fh,
        P["L_FOOT"][1] - int(1.6 * PAD),
        P["L_FOOT"][0] + fh,
        min(SY1 + 1, P["L_FOOT"][1] + int(2.2 * PAD)),
        0,
    )
    boxes["foot_r"] = (
        P["R_FOOT"][0] - fh,
        P["R_FOOT"][1] - int(1.6 * PAD),
        P["R_FOOT"][0] + fh,
        min(SY1 + 1, P["R_FOOT"][1] + int(2.2 * PAD)),
        0,
    )
    # stump-cap placeholders — a chunk of chest plating (hand-drawn 32x32
    # PNGs in tools/comfy/stumps/<chassis>/ override these per the spec)
    sb = (cx - 18, P["CHEST"][1] + PAD, cx + 18, P["CHEST"][1] + PAD + 36, 0)
    for sid in (
        "stump_shoulder_l",
        "stump_shoulder_r",
        "stump_hip_l",
        "stump_hip_r",
        "stump_neck",
    ):
        boxes[sid] = sb

    # clamp + emit
    rows = []
    for sid in ORDER:
        x0, y0, x1, y1, rot = boxes[sid]
        x0, y0, x1, y1 = clamp_box(x0, y0, x1, y1, W, H)
        dx, dy, px, py, dw, dh = ATLAS[sid]
        rows.append((sid, x0, y0, x1 - x0, y1 - y0, dx, dy, px, py, dw, dh, rot))

    with open(out_table, "w") as f:
        f.write(
            f"# Soldut — {args.chassis} crop table (auto-generated by build_crop_table.py)\n"
        )
        f.write(
            f"# canonical: {os.path.basename(out_canon)}  ({W}x{H})  joints: {os.path.basename(joints_path)}\n"
        )
        f.write(
            "# src_* = bbox on the canonical; dst_*/pivot_*/draw_* mirror s_default_parts in src/mech_sprites.c.\n"
        )
        f.write(
            "# rotate = degrees CCW applied to the cropped tile before resize (arms are horizontal in a T-pose; atlas slots are vertical).\n"
        )
        f.write(
            "# part_id          src_x src_y src_w src_h   dst_x dst_y   pivot_x pivot_y   draw_w draw_h   rotate\n"
        )
        for r in rows:
            f.write(
                f"{r[0]:<17} {r[1]:5d} {r[2]:5d} {r[3]:5d} {r[4]:5d}   {r[5]:5d} {r[6]:5d}   {r[7]:7d} {r[8]:7d}   {r[9]:6d} {r[10]:6d}   {r[11]:6d}\n"
            )
    print(f"crop table -> {out_table}  ({len(rows)} rows)")

    # preview overlay
    prev = img_flat.convert("RGB").copy()
    d = ImageDraw.Draw(prev)
    try:
        font = ImageFont.truetype("arial.ttf", max(11, W // 90))
    except Exception:
        font = ImageFont.load_default()
    for r in rows:
        sid, x0, y0, w, h = r[0], r[1], r[2], r[3], r[4]
        col = (
            (255, 0, 255)
            if sid.startswith("stump")
            else (0, 150, 255)
            if "arm" in sid or "hand" in sid
            else (220, 30, 30)
        )
        d.rectangle((x0, y0, x0 + w, y0 + h), outline=col, width=3)
        d.text(
            (x0 + 2, y0 + 2),
            f"{sid}{' r' + str(r[11]) if r[11] else ''}",
            fill=col,
            font=font,
        )
    for name, (x, y) in P.items():
        d.ellipse((x - 5, y - 5, x + 5, y + 5), fill=(0, 200, 0), outline=(0, 0, 0))
    prev.save(preview_path)
    print(f"preview -> {preview_path}")


if __name__ == "__main__":
    main()
