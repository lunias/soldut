#!/usr/bin/env python3
"""keyout_halftone_bg.py — make ordered-dither halftone "canvas" transparent.

P16 parallax PNGs come back from Perplexity (or any 2-color halftone
pipeline) with a fully-opaque dithered checker canvas. For parallax_near
layers — which are drawn AFTER the world inside src/render.c — that
opaque canvas hides the chassis underneath. This tool keys out the
dither pattern by detecting "solid silhouette" regions (high local dark
fraction) as opaque foreground and zeroing the alpha for everything
else (the canvas dither and the surrounding orange).

Method:
  1. Treat any pixel whose RGB is near the foreground dark color as a
     "dark" pixel.
  2. Compute the local dark fraction inside a small window. The
     ordered-dither canvas hits ~25%-50% darkness depending on tone;
     a solid silhouette is >85% dark.
  3. Anything above `--threshold` (default 0.78) keeps alpha=255.
     Everything else becomes alpha=0.
  4. Optionally dilate the foreground mask by `--dilate` pixels to
     capture anti-aliased silhouette edges so they don't get
     accidentally clipped.

This is destructive (in-place by default) — back up the source first
or pass `--out` to write to a new path.
"""
from __future__ import annotations
import argparse
import sys
import numpy as np
from PIL import Image

try:
    from scipy.ndimage import uniform_filter, binary_dilation
except Exception as e:
    sys.exit(f"keyout_halftone_bg needs scipy: {e}\n"
             f"  use the ComfyUI venv: ~/ComfyUI/venv/bin/python")


def keyout(in_path: str,
           out_path: str,
           dark_color: tuple,
           tol: int,
           win: int,
           threshold: float,
           dilate: int) -> None:
    img = Image.open(in_path).convert("RGBA")
    arr = np.array(img)
    rgb = arr[:, :, :3].astype(np.int16)

    target = np.array(dark_color, dtype=np.int16)
    is_dark = np.all(np.abs(rgb - target) <= tol, axis=2)

    dark_frac = uniform_filter(is_dark.astype(np.float32), size=win)
    fg = dark_frac >= threshold

    if dilate > 0:
        fg = binary_dilation(fg, iterations=dilate)

    arr[:, :, 3] = np.where(fg, 255, 0).astype(np.uint8)

    out = Image.fromarray(arr, "RGBA")
    out.save(out_path, format="PNG", optimize=True)

    total = arr.shape[0] * arr.shape[1]
    kept = int(fg.sum())
    print(f"{in_path}: kept {kept}/{total} px ({100 * kept / total:.1f}%) "
          f"-> {out_path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("input", help="Source PNG (dithered halftone)")
    ap.add_argument("--out", default=None,
                    help="Output PNG (default: overwrite input)")
    ap.add_argument("--dark", default="26,22,18",
                    help="Foreground dark color RGB (default 26,22,18)")
    ap.add_argument("--tol", type=int, default=60,
                    help="Per-channel color tolerance (default 60)")
    ap.add_argument("--win", type=int, default=5,
                    help="Local-window size for dark-fraction (default 5)")
    ap.add_argument("--threshold", type=float, default=0.78,
                    help="Min local dark fraction to count as fg (default 0.78)")
    ap.add_argument("--dilate", type=int, default=2,
                    help="Dilation iters on fg mask (default 2)")
    args = ap.parse_args()

    out = args.out or args.input
    dark = tuple(int(c) for c in args.dark.split(","))
    keyout(args.input, out, dark, args.tol, args.win, args.threshold, args.dilate)


if __name__ == "__main__":
    main()
