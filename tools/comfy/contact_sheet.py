#!/usr/bin/env python3
"""Quick contact-sheet builder for ComfyUI candidate review.

  python tools/comfy/contact_sheet.py "tools/comfy/output/trooper_i4_*.png" \
      tools/comfy/output/trooper_i4_sheet.png [cell_px] [cols]

Helper only — not part of the asset pipeline proper. Run with a Python
that has Pillow (e.g. ComfyUI's venv on a dev box). Globs `pattern`,
shrinks each to a square cell, tiles them into one PNG. Cell index 1..N
is overlaid in the corner so you can name which candidate you want.
"""

import sys
import glob
from pathlib import Path
from PIL import Image, ImageDraw


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: contact_sheet.py '<glob>' <out.png> [cell=400] [cols=auto]")
    pat, out = sys.argv[1], sys.argv[2]
    cell = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    files = sorted(glob.glob(pat))
    if not files:
        sys.exit(f"no files match {pat!r}")
    cols = int(sys.argv[4]) if len(sys.argv) > 4 else min(4, len(files))
    rows = (len(files) + cols - 1) // cols
    sheet = Image.new("RGB", (cell * cols, cell * rows), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for i, f in enumerate(files):
        im = Image.open(f).convert("RGB").resize((cell, cell), Image.LANCZOS)
        ox, oy = (i % cols) * cell, (i // cols) * cell
        sheet.paste(im, (ox, oy))
        label = f"{i + 1}  {Path(f).name}"
        d.rectangle((ox, oy, ox + 8 + 6 * len(label), oy + 14), fill=(0, 0, 0))
        d.text((ox + 2, oy + 2), label, fill=(0, 255, 0))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    print(f"wrote {out} ({sheet.size[0]}x{sheet.size[1]}) — {len(files)} cells")
    for i, f in enumerate(files):
        print(f"  cell {i + 1}: {f}")


if __name__ == "__main__":
    main()
