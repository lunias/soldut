#!/usr/bin/env python3
"""
Generates tools/comfy/skeletons/trooper_tpose_skeleton.png — a 1024x1024
ControlNet input with proper mech anatomy (not stick figure).

The mech FILLS the canvas vertically: y=72..964 (87% of canvas height)
so ControlNet/T2I-Adapter conditioning gets a strong full-frame
silhouette instead of a tiny figure surrounded by empty space (which
the AI hallucinates into chaotic backgrounds).

Joint coordinates are scaled 2x from the original placeholder skeleton
geometry to match. trooper_crop.txt's src coords are the same scale.
"""
from PIL import Image, ImageDraw
import os, sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "trooper_tpose_skeleton.png")

W, H = 1024, 1024
BG    = (255, 255, 255, 255)
INK   = (0, 0, 0, 255)
GREY  = (200, 200, 200, 255)
PIVOT = (255, 0, 255, 255)

img  = Image.new("RGBA", (W, H), BG)
draw = ImageDraw.Draw(img)

CX = 512

# Joint anchors — 2x scale up from the original placeholder.
# Mech now fills y=72..964 (instead of y=64..458).
NECK    = (CX,         200)
HEAD    = (CX,         160)
CHEST   = (CX,         296)
PELVIS  = (CX,         440)
L_SH    = (CX -  88,   296)
R_SH    = (CX +  88,   296)
L_EL    = (CX -  88,   440)
R_EL    = (CX +  88,   440)
L_HAND  = (CX -  88,   588)
R_HAND  = (CX +  88,   588)
L_HIP   = (CX -  32,   492)
R_HIP   = (CX +  32,   492)
L_KNEE  = (CX -  32,   676)
R_KNEE  = (CX +  32,   676)
L_FOOT  = (CX -  32,   860)
R_FOOT  = (CX +  32,   860)


def fill_rounded(box, radius=4, outline=INK, width=3, fill=None):
    draw.rounded_rectangle(box, radius=radius, outline=outline,
                           width=width, fill=fill)


# ---- HEAD: helmet shape with visor (80x80) ----
helmet_box = (CX - 36, 132, CX + 36, 204)
fill_rounded(helmet_box, radius=16, outline=INK, width=4)
# Visor strip
draw.rectangle((CX - 28, 156, CX + 28, 172), fill=INK)
# Antenna stub
draw.line((CX, 128, CX, 140), fill=INK, width=4)

# Neck
draw.line((CX - 10, 204, CX - 10, 232), fill=INK, width=4)
draw.line((CX + 10, 204, CX + 10, 232), fill=INK, width=4)

# ---- TORSO: chest plate with panel lines (112x144) ----
torso_box = (CX - 56, 232, CX + 56, 376)
fill_rounded(torso_box, radius=8, outline=INK, width=4)
draw.line((CX, 240, CX, 368), fill=INK, width=2)
draw.line((CX - 52, 268, CX + 52, 268), fill=INK, width=2)
draw.line((CX - 52, 324, CX + 52, 324), fill=INK, width=2)
# Power core
draw.ellipse((CX - 12, 296, CX + 12, 320), outline=INK, width=3)
# Side vents
draw.rectangle((CX - 52, 284, CX - 36, 312), outline=INK, width=2)
draw.rectangle((CX + 36, 284, CX + 52, 312), outline=INK, width=2)

# ---- HIP PLATE: belt + side flares (128x72) ----
hip_box = (CX - 64, 408, CX + 64, 480)
fill_rounded(hip_box, radius=8, outline=INK, width=4)
draw.rectangle((CX - 16, 428, CX + 16, 452), outline=INK, width=3)
draw.rectangle((CX - 64, 448, CX - 44, 472), outline=INK, width=2)
draw.rectangle((CX + 44, 448, CX + 64, 472), outline=INK, width=2)

# ---- SHOULDER PADS (112x80) ----
for sh in (L_SH, R_SH):
    sx, sy = sh
    pad_box = (sx - 56, sy - 40, sx + 56, sy + 40)
    fill_rounded(pad_box, radius=16, outline=INK, width=4)
    draw.arc((sx - 44, sy - 28, sx + 44, sy + 28),
             start=0, end=180, fill=INK, width=3)
    draw.ellipse((sx - 8, sy - 8, sx + 8, sy + 8), outline=INK, width=3)

# ---- ARMS (upper + lower + gauntlet) ----
for sh, el, ha in [(L_SH, L_EL, L_HAND), (R_SH, R_EL, R_HAND)]:
    sx, sy = sh; ex, ey = el; hx, hy = ha
    # Upper arm shaft
    upper_box = (sx - 28, sy + 28, sx + 28, ey - 8)
    fill_rounded(upper_box, radius=8, outline=INK, width=4)
    draw.line((sx, sy + 36, sx, ey - 16), fill=INK, width=2)
    # Hatching for shading
    for y in range(int(sy + 44), int(ey - 16), 10):
        draw.line((sx - 24, y, sx - 8, y), fill=INK, width=2)
    # Elbow joint
    draw.ellipse((ex - 14, ey - 14, ex + 14, ey + 14), outline=INK, width=3, fill=GREY)
    # Forearm
    lower_box = (ex - 24, ey + 12, ex + 24, hy - 12)
    fill_rounded(lower_box, radius=6, outline=INK, width=4)
    draw.line((ex - 12, ey + 20, ex - 12, hy - 20), fill=INK, width=2)
    draw.line((ex + 12, ey + 20, ex + 12, hy - 20), fill=INK, width=2)
    # Wrist
    draw.rectangle((hx - 20, hy - 16, hx + 20, hy - 8), outline=INK, width=3, fill=GREY)
    # Gauntlet
    fill_rounded((hx - 16, hy - 12, hx + 16, hy + 16), radius=4, outline=INK, width=3)

# ---- LEGS (upper + lower + boot) ----
for hp, kn, ft in [(L_HIP, L_KNEE, L_FOOT), (R_HIP, R_KNEE, R_FOOT)]:
    hx, hy = hp; kx, ky = kn; fx, fy = ft
    # Thigh
    thigh_box = (kx - 32, hy + 12, kx + 32, ky - 12)
    fill_rounded(thigh_box, radius=8, outline=INK, width=4)
    draw.line((kx, hy + 24, kx, ky - 24), fill=INK, width=2)
    for y in range(int(hy + 24), int(ky - 24), 10):
        draw.line((kx + 16, y, kx + 28, y), fill=INK, width=2)
    # Knee
    draw.ellipse((kx - 18, ky - 18, kx + 18, ky + 18), outline=INK, width=3, fill=GREY)
    # Shin
    shin_box = (fx - 26, ky + 16, fx + 26, fy - 8)
    fill_rounded(shin_box, radius=6, outline=INK, width=4)
    draw.line((fx, ky + 28, fx, fy - 16), fill=INK, width=2)
    # Boot
    boot_box = (fx - 32, fy - 8, fx + 32, fy + 36)
    fill_rounded(boot_box, radius=4, outline=INK, width=4)
    draw.rectangle((fx + 8, fy + 16, fx + 32, fy + 32), outline=INK, width=3, fill=GREY)

# ---- Magenta pivot dots (extracted post-AI) ----
PIVOT_R = 8
for p in [NECK, HEAD, CHEST, PELVIS,
          L_SH, R_SH, L_EL, R_EL, L_HAND, R_HAND,
          L_HIP, R_HIP, L_KNEE, R_KNEE, L_FOOT, R_FOOT]:
    draw.ellipse((p[0] - PIVOT_R, p[1] - PIVOT_R,
                  p[0] + PIVOT_R, p[1] + PIVOT_R), fill=PIVOT)

img.convert("RGB").save(OUT, format="PNG", optimize=True)
print(f"wrote {OUT} ({W}x{H})", file=sys.stderr)
