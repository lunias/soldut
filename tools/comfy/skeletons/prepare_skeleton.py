#!/usr/bin/env python3
"""
prepare_skeleton.py — turn an arbitrary front T-pose mech illustration
into a ComfyUI ControlNet "skeleton" input that matches the Soldut
pipeline's expectations:

  1. If the image has no magenta (255,0,255) pivot dots, detect the
     mech silhouette and stamp 16 magenta joint dots at anatomically
     sensible locations (NECK HEAD CHEST PELVIS L/R_SHOULDER
     L/R_ELBOW L/R_HAND L/R_HIP L/R_KNEE L/R_FOOT). If dots are
     already present, they are left alone.
  2. Optionally "clean" the image for ControlNet use: erase floating
     background blobs (logos / title text in the corners) so the AI
     doesn't try to reproduce gibberish text.
  3. Emit <chassis>_joints.json (joint pixel coords, image size) — the
     crop-table builder consumes this so part rectangles + pivots stay
     in sync with the actual artwork.
  4. Write a human-review overlay (<out>_overlay.png) with big bright
     dots + labels so the joint placement can be eyeballed/approved.

This is the "build in the bone structure when the provided image lacks
it" step the workflow asks for. It is deterministic (no RNG) and uses
only PIL + numpy (+ scipy.ndimage if available; falls back otherwise).

Usage:
  python prepare_skeleton.py INPUT.png CHASSIS [--out OUT.png]
        [--clean] [--no-dots] [--size N] [--overlay OUT_overlay.png]

The joint *fractions* below are tuned for the Soldut "Trooper" front
T-pose readout (arms horizontal, legs vertical, helmeted head). Other
chassis reuse them; pass --profile later if a chassis needs different
ratios. Anything off gets fixed in the review loop, not guessed twice.
"""

from __future__ import annotations
import argparse
import json
import os
from PIL import Image, ImageDraw, ImageFont
import numpy as np

try:
    from scipy import ndimage as _ndi  # type: ignore

    _HAVE_SCIPY = True
except Exception:
    _HAVE_SCIPY = False

MAGENTA = (255, 0, 255)

# ---- joint layout, as fractions of the detected silhouette bbox ----
# x: 0..1 across the bbox width (0.5 = centre). y: 0..1 down the bbox.
# Arms are horizontal so L/R_SHOULDER..HAND share the "arm row" y; the
# x's are interpolated between the torso edge and the fingertip.
PROFILE_TROOPER = {
    # vertical anchors (y as a fraction of bbox height)
    "head_y": 0.075,  # HEAD particle ~ inside the helmet, near top
    "neck_y": 0.165,  # base of helmet / top of torso
    "arm_row_y": 0.245,  # shoulder line; arms run horizontally here
    "chest_y": 0.245,  # CHEST anchor sits on the shoulder line
    "pelvis_y": 0.460,  # just above the crotch split
    "hip_y": 0.475,  # L/R_HIP a touch below PELVIS
    "knee_y": 0.690,  # knee joints
    "foot_y": 0.945,  # ankle / top of boot
    # horizontal anchors along the arm row, as a fraction from centre
    # (0) to the fingertip (1) on that side:
    "shoulder_x": 0.300,  # proximal end of the upper-arm tube (under the pauldron)
    "elbow_x": 0.560,  # mid-arm joint
    "hand_x": 0.945,  # the fist
    # leg x fallback (only used if the legs can't be separated by mask)
    "hip_x": 0.430,
}


def _to_mask(img: Image.Image) -> np.ndarray:
    """Boolean 'ink/body present' mask: pixels noticeably darker than
    the paper background. Robust to cream paper + dark-brown ink."""
    g = np.asarray(img.convert("L"), dtype=np.int16)
    # background = the modal bright value (cream paper); ink is much darker.
    hist = np.bincount(g.ravel(), minlength=256)
    paper = int(np.argmax(hist[180:]) + 180) if hist[180:].any() else 245
    thr = max(60, paper - 45)
    return g < thr


def _bbox_of(m: np.ndarray):
    ys, xs = np.where(m)
    return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())


def _flood_light(light: np.ndarray, seed_xy):
    """Connected light region reachable from seed_xy. scipy if present,
    else PIL floodfill. Returns a boolean mask."""
    if _HAVE_SCIPY:
        lbl, _ = _ndi.label(light)
        sx = min(max(int(seed_xy[0]), 0), light.shape[1] - 1)
        sy = min(max(int(seed_xy[1]), 0), light.shape[0] - 1)
        target = lbl[sy, sx]
        return lbl == target if target != 0 else np.zeros_like(light)
    pil = Image.fromarray((light * 255).astype(np.uint8), "L")
    ImageDraw.floodfill(pil, (int(seed_xy[0]), int(seed_xy[1])), 128)
    return np.asarray(pil) == 128


def _largest_component(m: np.ndarray):
    if not m.any() or not _HAVE_SCIPY:
        return m
    lbl, n = _ndi.label(m)
    if n <= 1:
        return m
    sizes = np.bincount(lbl.ravel())
    sizes[0] = 0
    return lbl == int(np.argmax(sizes))


def _largest_blob_bbox(mask: np.ndarray):
    """Isolate the mech silhouette (the filled region enclosed by its
    outer outline), tolerating a decorative border frame around the
    artwork. Returns ((x0,y0,x1,y1) inclusive bbox, silhouette mask)."""
    light = ~mask
    h, w = mask.shape
    # 1) outer background = light reachable from the canvas corners.
    outer = _flood_light(light, (1, 1)) | _flood_light(light, (w - 2, 1))
    occupied = ~outer  # frame line + inner paper + mech, all together
    if not occupied.any():
        return _bbox_of(mask), mask
    ox0, oy0, ox1, oy1 = _bbox_of(occupied)
    # 2) inner paper = the light region that wraps *around* the mech but
    #    is still inside any decorative frame. Find a guaranteed-paper
    #    seed by walking in from the occupied bbox's top-left corner
    #    along the diagonal until we hit a pixel that is light *and* not
    #    part of the outer margin (robust to any frame thickness).
    inner_paper = None
    for d in range(2, max(3, min(ox1 - ox0, oy1 - oy0) // 2)):
        x, y = ox0 + d, oy0 + d
        if light[y, x] and not outer[y, x]:
            inner_paper = _flood_light(light, (x, y))
            break
    if inner_paper is None or not inner_paper.any():
        # no frame (or weird art): occupied *is* the body region
        body = _largest_component(occupied)
        return _bbox_of(body) if body.any() else _bbox_of(occupied), (
            body if body.any() else occupied
        )
    body_region = occupied & ~inner_paper  # frame line + mech (disjoint)
    # 3) the mech is the largest connected component of that.
    body = _largest_component(body_region)
    if not body.any():
        body = occupied
    return _bbox_of(body), body


def _silhouette_row_extent(body: np.ndarray, y: int):
    row = np.where(body[y])[0]
    if row.size == 0:
        return None
    return int(row.min()), int(row.max())


def _column_extent(body: np.ndarray, x: int):
    """(top, bottom) of the silhouette in column x, or None."""
    x = min(max(int(x), 0), body.shape[1] - 1)
    col = np.where(body[:, x])[0]
    if col.size == 0:
        return None
    return int(col.min()), int(col.max())


def _runs(row: np.ndarray, min_len: int = 3):
    """Start/end indices of contiguous True runs in a 1-D bool array."""
    idx = np.where(row)[0]
    if idx.size == 0:
        return []
    breaks = np.where(np.diff(idx) > 1)[0]
    starts = np.concatenate(([idx[0]], idx[breaks + 1]))
    ends = np.concatenate((idx[breaks], [idx[-1]]))
    return [(int(s), int(e)) for s, e in zip(starts, ends) if e - s + 1 >= min_len]


def _leg_centres_at(body: np.ndarray, y: int, cx: int, half_fallback: int):
    """Centre x of the left and right leg at row y. Below the crotch the
    silhouette is two blobs — take the leftmost and rightmost
    substantial runs. If they're still merged, split the run."""
    y = min(max(int(y), 0), body.shape[0] - 1)
    rs = _runs(body[y], min_len=max(4, body.shape[1] // 80))
    if len(rs) >= 2:
        left = min(rs, key=lambda r: (r[0] + r[1]) / 2)
        right = max(rs, key=lambda r: (r[0] + r[1]) / 2)
        return (left[0] + left[1]) // 2, (right[0] + right[1]) // 2
    if len(rs) == 1:
        s, e = rs[0]
        # one merged blob: leg centres sit ~quarter-in from each side
        return int(s + 0.27 * (e - s)), int(s + 0.73 * (e - s))
    return cx - half_fallback, cx + half_fallback


def detect_joints(img: Image.Image, profile=PROFILE_TROOPER) -> dict:
    mask = _to_mask(img)
    (x0, y0, x1, y1), body = _largest_blob_bbox(mask)
    W = x1 - x0 + 1
    H = y1 - y0 + 1
    cx = (x0 + x1) // 2
    # refine cx using the head row's centre of mass (helmet is symmetric)
    ext = _silhouette_row_extent(body, int(y0 + 0.05 * H))
    if ext:
        cx = (ext[0] + ext[1]) // 2

    def Y(f):
        return int(round(y0 + f * H))

    # --- arm row: detect the *vertical centre* of the horizontal arm ---
    # Pass 1: rough fingertip x from the silhouette extent at the guessed
    # arm row; provisional elbow x from there. Pass 2: the arm's column
    # extent at that elbow x gives the true arm centreline (the limb's
    # mid-height, not its lower edge).
    arm_y = Y(profile["arm_row_y"])
    ext = _silhouette_row_extent(body, arm_y) or (x0, x1)
    L_tip0, R_tip0 = ext[0] + int(0.02 * W), ext[1] - int(0.02 * W)
    eL0 = int(cx - profile["elbow_x"] * (cx - L_tip0))
    eR0 = int(cx + profile["elbow_x"] * (R_tip0 - cx))
    cys = []
    for ex in (eL0, eR0):
        ce = _column_extent(body, ex)
        if ce:
            cys.append((ce[0] + ce[1]) // 2)
    if cys:
        arm_y = int(round(sum(cys) / len(cys)))
    # re-measure fingertips at the corrected arm centreline
    ext = _silhouette_row_extent(body, arm_y) or (x0, x1)
    L_tip, R_tip = ext[0] + int(0.015 * W), ext[1] - int(0.015 * W)
    Lspan, Rspan = cx - L_tip, R_tip - cx

    def arm_x(side, frac):  # side: -1 left, +1 right; frac: centre→fingertip
        return int(round(cx + side * frac * (Lspan if side < 0 else Rspan)))

    # --- leg centres: detected at the knee and foot rows ---
    half_fb = max(int(0.10 * W), 1)
    Lk, Rk = _leg_centres_at(body, Y(profile["knee_y"]), cx, half_fb)
    Lf, Rf = _leg_centres_at(body, Y(profile["foot_y"]), cx, half_fb)
    # hip x: legs are near-vertical above the knee, so reuse the knee x
    Lh, Rh = Lk, Rk

    J = {
        "HEAD": (cx, Y(profile["head_y"])),
        "NECK": (cx, Y(profile["neck_y"])),
        "CHEST": (cx, arm_y),
        "PELVIS": (cx, Y(profile["pelvis_y"])),
        "L_SHOULDER": (arm_x(-1, profile["shoulder_x"]), arm_y),
        "R_SHOULDER": (arm_x(+1, profile["shoulder_x"]), arm_y),
        "L_ELBOW": (arm_x(-1, profile["elbow_x"]), arm_y),
        "R_ELBOW": (arm_x(+1, profile["elbow_x"]), arm_y),
        "L_HAND": (arm_x(-1, profile["hand_x"]), arm_y),
        "R_HAND": (arm_x(+1, profile["hand_x"]), arm_y),
        "L_HIP": (Lh, Y(profile["hip_y"])),
        "R_HIP": (Rh, Y(profile["hip_y"])),
        "L_KNEE": (Lk, Y(profile["knee_y"])),
        "R_KNEE": (Rk, Y(profile["knee_y"])),
        "L_FOOT": (Lf, Y(profile["foot_y"])),
        "R_FOOT": (Rf, Y(profile["foot_y"])),
    }
    return {
        "image_size": list(img.size),
        "bbox": [x0, y0, x1, y1],
        "center_x": cx,
        "joints": {k: [int(v[0]), int(v[1])] for k, v in J.items()},
    }


def has_magenta_dots(img: Image.Image, tol=24) -> bool:
    a = np.asarray(img.convert("RGB"), dtype=np.int16)
    hit = (
        (np.abs(a[..., 0] - 255) < tol)
        & (a[..., 1] < tol)
        & (np.abs(a[..., 2] - 255) < tol)
    )
    return int(hit.sum()) > 8


def erase_corner_text(
    img: Image.Image, mask_body: np.ndarray | None = None
) -> Image.Image:
    """Paint paper-white over the bottom-right ~24% × ~22% — that is
    where the SOLDUT title + emblem live in these readouts. Cheap and
    deterministic; the body never reaches that corner."""
    im = img.convert("RGB").copy()
    w, h = im.size
    # sample the paper colour from a clean top-left patch
    patch = np.asarray(im)[8:40, 8:40].reshape(-1, 3)
    paper = tuple(int(c) for c in np.median(patch, axis=0))
    d = ImageDraw.Draw(im)
    d.rectangle([int(w * 0.74), int(h * 0.76), w, h], fill=paper)
    return im


def stamp_dots(img: Image.Image, joints: dict, radius=None) -> Image.Image:
    im = img.convert("RGB").copy()
    d = ImageDraw.Draw(im)
    r = radius or max(5, im.size[0] // 220)
    for name, (x, y) in joints["joints"].items():
        d.ellipse([x - r, y - r, x + r, y + r], fill=MAGENTA)
    return im


def make_overlay(img: Image.Image, joints: dict) -> Image.Image:
    im = img.convert("RGB").copy()
    d = ImageDraw.Draw(im)
    r = max(8, im.size[0] // 120)
    try:
        font = ImageFont.truetype("arial.ttf", max(14, im.size[0] // 90))
    except Exception:
        font = ImageFont.load_default()
    # connective lines (visual sanity of the chain)
    J = joints["joints"]
    chain = [
        ("NECK", "HEAD"),
        ("CHEST", "NECK"),
        ("CHEST", "PELVIS"),
        ("CHEST", "L_SHOULDER"),
        ("L_SHOULDER", "L_ELBOW"),
        ("L_ELBOW", "L_HAND"),
        ("CHEST", "R_SHOULDER"),
        ("R_SHOULDER", "R_ELBOW"),
        ("R_ELBOW", "R_HAND"),
        ("PELVIS", "L_HIP"),
        ("L_HIP", "L_KNEE"),
        ("L_KNEE", "L_FOOT"),
        ("PELVIS", "R_HIP"),
        ("R_HIP", "R_KNEE"),
        ("R_KNEE", "R_FOOT"),
    ]
    for a, b in chain:
        d.line([tuple(J[a]), tuple(J[b])], fill=(0, 170, 255), width=max(2, r // 3))
    for name, (x, y) in J.items():
        d.ellipse(
            [x - r, y - r, x + r, y + r], fill=MAGENTA, outline=(0, 0, 0), width=2
        )
        d.text((x + r + 2, y - r), name, fill=(0, 130, 0), font=font)
    return im


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Prepare a ControlNet skeleton input (add pivot dots / clean / emit joints)."
    )
    ap.add_argument("input")
    ap.add_argument("chassis")
    ap.add_argument(
        "--out",
        default=None,
        help="output skeleton PNG (default: skeletons/<chassis>_tpose_skeleton_v2.png)",
    )
    ap.add_argument(
        "--overlay",
        default=None,
        help="review overlay PNG (default: <out>_overlay.png)",
    )
    ap.add_argument(
        "--joints-out",
        default=None,
        help="joints JSON (default: skeletons/<chassis>_joints.json)",
    )
    ap.add_argument(
        "--clean",
        action="store_true",
        help="erase corner title/emblem for ControlNet use",
    )
    ap.add_argument(
        "--no-dots",
        action="store_true",
        help="skip dot stamping (still emits joints + overlay)",
    )
    ap.add_argument(
        "--size",
        type=int,
        default=0,
        help="resize the longest side to N (0 = keep original)",
    )
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    out = args.out or os.path.join(here, f"{args.chassis}_tpose_skeleton_v2.png")
    overlay = args.overlay or (os.path.splitext(out)[0] + "_overlay.png")
    joints_out = args.joints_out or os.path.join(here, f"{args.chassis}_joints.json")

    img = Image.open(args.input).convert("RGB")
    if args.size and max(img.size) != args.size:
        s = args.size / max(img.size)
        img = img.resize(
            (round(img.size[0] * s), round(img.size[1] * s)), Image.LANCZOS
        )

    already = has_magenta_dots(img)
    joints = detect_joints(img)
    with open(joints_out, "w") as f:
        json.dump(joints, f, indent=2)

    work = img
    if args.clean:
        work = erase_corner_text(work)
    if not args.no_dots and not already:
        work = stamp_dots(work, joints)
    elif already:
        print("[prepare_skeleton] input already has magenta dots — leaving them as-is")
    work.save(out)
    make_overlay(img, joints).save(overlay)

    print(
        f"[prepare_skeleton] {args.chassis}: image {img.size}  bbox {joints['bbox']}  cx={joints['center_x']}"
    )
    for k, v in joints["joints"].items():
        print(f"    {k:<11} {v}")
    print(
        f"[prepare_skeleton] wrote:\n    skeleton  {out}\n    overlay   {overlay}\n    joints    {joints_out}"
    )


if __name__ == "__main__":
    main()
