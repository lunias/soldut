#!/usr/bin/env python3
"""
Generates tools/comfy/skeletons/trooper_tpose_skeleton.png — a 1024x1024
placeholder ControlNet input. Pure black-on-white silhouette of a Trooper-
sized mech in T-pose, with magenta pivot dots at every joint.

THIS IS A PLACEHOLDER. Replace it with a hand-drawn skeleton (Krita,
2-px ink, ragged edges at parent-side overlap regions) before approving
any chassis. The placeholder exists so the workflow loads at all and so
crop_canonical.sh has a known-good geometry to test against.

Pivot dots:
  Magenta dot (#FF00FF) at every joint particle. crop_canonical.sh +
  transcribe_atlas.sh extract these post-AI to populate the
  MechSpritePart.pivot field.

Layout matches tools/comfy/crop_tables/trooper_crop.txt — keep them in
sync if you tune one.
"""
from PIL import Image, ImageDraw
import os, sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "trooper_tpose_skeleton.png")

W, H = 1024, 1024
BG       = (255, 255, 255, 255)
INK      = (0,   0,   0,   255)
GREY     = (200, 200, 200, 255)   # overlap zones
PIVOT    = (255, 0,   255, 255)   # magenta — extracted by tools

img  = Image.new("RGBA", (W, H), BG)
draw = ImageDraw.Draw(img)

# Body centerline. Mech occupies ~y=80..620 vertically.
CX = 512

# Anatomy: hand-drawn skeleton anchors (joint positions in the canonical
# 1024x1024 image). These match crop_tables/trooper_crop.txt's bbox
# centers + the existing s_default_parts pivots in src/mech_sprites.c.
NECK    = (CX,         100)
HEAD    = (CX,          80)   # top of head sprite
CHEST   = (CX,         148)   # torso top-half
PELVIS  = (CX,         220)   # hip plate center
L_SH    = (CX -  44,   148)
R_SH    = (CX +  44,   148)
L_EL    = (CX -  44,   220)
R_EL    = (CX +  44,   220)
L_HAND  = (CX -  44,   294)
R_HAND  = (CX +  44,   294)
L_HIP   = (CX -  16,   246)
R_HIP   = (CX +  16,   246)
L_KNEE  = (CX -  16,   338)
R_KNEE  = (CX +  16,   338)
L_FOOT  = (CX -  16,   430)
R_FOOT  = (CX +  16,   430)

LINE_W = 14

def line(a, b, w=LINE_W, fill=INK):
    draw.line([a, b], fill=fill, width=w)

# --- silhouette outlines (rounded rectangles for plates) ---
# Torso plate: 56 wide, 72 tall, centered at (CX, 148+36=184)
draw.rounded_rectangle((CX - 28, 116, CX + 28, 188), radius=6, outline=INK, width=3)
# Hip plate
draw.rounded_rectangle((CX - 32, 204, CX + 32, 240), radius=4, outline=INK, width=3)
# Head — small circle
draw.ellipse((CX - 20, 64, CX + 20, 104), outline=INK, width=3)
# Shoulder plates (small wedges)
draw.rounded_rectangle((L_SH[0] - 20, L_SH[1] - 16, L_SH[0] + 20, L_SH[1] + 16), radius=4, outline=INK, width=3)
draw.rounded_rectangle((R_SH[0] - 20, R_SH[1] - 16, R_SH[0] + 20, R_SH[1] + 16), radius=4, outline=INK, width=3)

# --- bone strokes (limb shafts) ---
# Arms hang straight down (A-pose interpretation of T-pose for sprite-
# vertical authoring; AI generates from this pose, parts crop vertically).
line(L_SH, L_EL, w=20)
line(L_EL, L_HAND, w=16)
line(R_SH, R_EL, w=20)
line(R_EL, R_HAND, w=16)
# Legs
line(L_HIP, L_KNEE, w=22)
line(L_KNEE, L_FOOT, w=20)
line(R_HIP, R_KNEE, w=22)
line(R_KNEE, R_FOOT, w=20)
# Feet stubs
draw.rounded_rectangle((L_FOOT[0] - 16, L_FOOT[1] - 4, L_FOOT[0] + 16, L_FOOT[1] + 20), radius=2, outline=INK, width=3)
draw.rounded_rectangle((R_FOOT[0] - 16, R_FOOT[1] - 4, R_FOOT[0] + 16, R_FOOT[1] + 20), radius=2, outline=INK, width=3)
# Hands
draw.ellipse((L_HAND[0] - 8, L_HAND[1] - 8, L_HAND[0] + 8, L_HAND[1] + 8), outline=INK, width=3)
draw.ellipse((R_HAND[0] - 8, R_HAND[1] - 8, R_HAND[0] + 8, R_HAND[1] + 8), outline=INK, width=3)

# Neck
line(NECK, (CX, 116), w=10)

# --- overlap-zone hints (light grey "exposed end" regions on parent
# side of each joint; AI fills with hydraulic-line cross-section). The
# real hand-drawn skeleton uses ragged edges here. Placeholder uses
# light-grey rectangles. ---
def overlap(c, w, h):
    draw.rectangle((c[0] - w//2, c[1] - h//2, c[0] + w//2, c[1] + h//2),
                   fill=GREY, outline=None)

overlap(L_SH, 20, 8)
overlap(R_SH, 20, 8)
overlap(L_EL, 18, 8)
overlap(R_EL, 18, 8)
overlap(L_HIP, 22, 6)
overlap(R_HIP, 22, 6)
overlap(L_KNEE, 20, 6)
overlap(R_KNEE, 20, 6)
overlap(NECK, 16, 6)

# --- magenta pivot dots (extracted post-AI for pivot transcription) ---
PIVOT_R = 4
def pivot(p):
    draw.ellipse((p[0] - PIVOT_R, p[1] - PIVOT_R,
                  p[0] + PIVOT_R, p[1] + PIVOT_R), fill=PIVOT)

pivot(NECK); pivot(HEAD); pivot(CHEST); pivot(PELVIS)
pivot(L_SH); pivot(R_SH); pivot(L_EL); pivot(R_EL)
pivot(L_HAND); pivot(R_HAND)
pivot(L_HIP); pivot(R_HIP); pivot(L_KNEE); pivot(R_KNEE)
pivot(L_FOOT); pivot(R_FOOT)

# Stump caps: small reserved boxes at y=900..960 (5 of them, 32×32 each
# laid out horizontally at x=16, 60, 104, 148, 192). Hand-drawn stumps
# go here per Pipeline 5; AI ignores this region.
for i, label in enumerate(["S_L", "S_R", "H_L", "H_R", "NK"]):
    x = 16 + i * 44
    draw.rounded_rectangle((x, 900, x + 32, 932), radius=2, outline=INK, width=2)
    draw.text((x + 4, 936), label, fill=INK)

# Watermark
draw.text((24, 16), "PLACEHOLDER — replace with hand-drawn skeleton", fill=INK)
draw.text((24, 32), "Pivots = magenta. Overlap zones = grey.",       fill=INK)
draw.text((24, 48), "T-pose interpretation: arms hang vertically",    fill=INK)
draw.text((24, 64), "(sprites authored vertically; renderer rotates).", fill=INK)

img.convert("RGB").save(OUT, format="PNG", optimize=True)
print(f"wrote {OUT} ({W}x{H})", file=sys.stderr)
