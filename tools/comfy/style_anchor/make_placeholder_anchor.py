#!/usr/bin/env python3
"""
Generates tools/comfy/style_anchor/trooper_anchor_v1.png — a placeholder
512x512 IP-Adapter style reference. The IP-Adapter conditions on style,
not geometry, so this image just needs to read as "Battletech-TRO ink-
on-paper": single-weight black lines on white, halftone-screen shading,
no glow, no gradient, no character.

THIS IS A PLACEHOLDER. The real anchor is one of two things:
  (a) a pencil sketch the user draws in Krita then scans (the "pure
      hand-drawn anchor" path), or
  (b) the best of the first 8 ComfyUI candidates after a manual review
      pass (the "anchor recursion" path — described in
      documents/m5/11-art-direction.md §"Pipeline 1").

Most users will start with (b): generate 8 with this placeholder anchor,
pick the best, save as trooper_anchor_v2.png, and re-run.
"""
from PIL import Image, ImageDraw
import os, sys, random

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "trooper_anchor_v1.png")

W, H = 512, 512
BG  = (244, 240, 230, 255)   # off-white "paper"
INK = (28, 24, 22, 255)

img  = Image.new("RGBA", (W, H), BG)
draw = ImageDraw.Draw(img)

# Crude rectangular silhouette — single-weight ink, no shading.
# IP-Adapter cares about TEXTURE/STYLE not anatomy; a clean ink figure
# anchored on a paper background is the conditioning signal we want.
CX, CY = 256, 280

# Body
draw.rectangle((CX - 60, CY - 100, CX + 60, CY + 30), outline=INK, width=3)
# Head
draw.rectangle((CX - 30, CY - 140, CX + 30, CY - 100), outline=INK, width=3)
# Hip
draw.rectangle((CX - 70, CY + 30, CX + 70, CY + 60), outline=INK, width=3)
# Arms (T-pose hanging)
draw.rectangle((CX - 100, CY - 80,  CX -  60, CY +  20), outline=INK, width=3)
draw.rectangle((CX +  60, CY - 80,  CX + 100, CY +  20), outline=INK, width=3)
# Forearms
draw.rectangle((CX - 100, CY +  20, CX -  60, CY + 100), outline=INK, width=3)
draw.rectangle((CX +  60, CY +  20, CX + 100, CY + 100), outline=INK, width=3)
# Legs
draw.rectangle((CX -  50, CY +  60, CX -  10, CY + 160), outline=INK, width=3)
draw.rectangle((CX +  10, CY +  60, CX +  50, CY + 160), outline=INK, width=3)
# Feet
draw.rectangle((CX -  60, CY + 160, CX -  10, CY + 180), outline=INK, width=3)
draw.rectangle((CX +  10, CY + 160, CX +  60, CY + 180), outline=INK, width=3)

# Panel-line detail on torso
draw.line((CX - 40, CY - 70, CX + 40, CY - 70), fill=INK, width=2)
draw.line((CX,      CY - 70, CX,      CY + 30), fill=INK, width=1)
draw.line((CX - 40, CY - 30, CX + 40, CY - 30), fill=INK, width=1)

# Halftone hatching on legs (manually drawn diagonal stripes)
random.seed(7)
for y in range(CY + 60, CY + 160, 4):
    for xstart in (CX - 50, CX + 10):
        if random.random() < 0.45:
            draw.line((xstart + 2, y, xstart + 36, y), fill=INK, width=1)

# Stamp/title
draw.text((24, 24),  "TROOPER · K-7", fill=INK)
draw.text((24, 40),  "MIL-STD 3025-A",   fill=INK)
draw.text((24, 472), "PLACEHOLDER ANCHOR — replace with pencil sketch", fill=INK)
draw.text((24, 488), "or with best-of-8 from first ComfyUI run.",       fill=INK)

img.convert("RGB").save(OUT, format="PNG", optimize=True)
print(f"wrote {OUT} ({W}x{H})", file=sys.stderr)
