#!/usr/bin/env python3
"""
Soldut — emit a C literal for g_chassis_sprites[CHASSIS_<N>].parts[]
from a crop table.

The default trooper_crop.txt's dst columns already match
src/mech_sprites.c::s_default_parts byte-for-byte, so transcription
is only needed when you tune part sizes/pivots away from the default
(e.g. authoring a Heavy with a 38-px torso).

Usage:
  ./transcribe_atlas.py trooper                   > /tmp/trooper_atlas.c
  ./transcribe_atlas.py heavy --table foo.txt
"""
import argparse, os, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Map crop-table id → MechSpriteId enum constant.
ID_MAP = {
    "torso":             "MSP_TORSO",
    "head":              "MSP_HEAD",
    "hip_plate":         "MSP_HIP_PLATE",
    "shoulder_l":        "MSP_SHOULDER_L",
    "shoulder_r":        "MSP_SHOULDER_R",
    "arm_upper_l":       "MSP_ARM_UPPER_L",
    "arm_upper_r":       "MSP_ARM_UPPER_R",
    "arm_lower_l":       "MSP_ARM_LOWER_L",
    "arm_lower_r":       "MSP_ARM_LOWER_R",
    "hand_l":            "MSP_HAND_L",
    "hand_r":            "MSP_HAND_R",
    "leg_upper_l":       "MSP_LEG_UPPER_L",
    "leg_upper_r":       "MSP_LEG_UPPER_R",
    "leg_lower_l":       "MSP_LEG_LOWER_L",
    "leg_lower_r":       "MSP_LEG_LOWER_R",
    "foot_l":            "MSP_FOOT_L",
    "foot_r":            "MSP_FOOT_R",
    "stump_shoulder_l":  "MSP_STUMP_SHOULDER_L",
    "stump_shoulder_r":  "MSP_STUMP_SHOULDER_R",
    "stump_hip_l":       "MSP_STUMP_HIP_L",
    "stump_hip_r":       "MSP_STUMP_HIP_R",
    "stump_neck":        "MSP_STUMP_NECK",
}

# Stable MSP_ order for the C literal (mirrors src/mech_sprites.h).
MSP_ORDER = [
    "MSP_TORSO", "MSP_HEAD", "MSP_HIP_PLATE",
    "MSP_SHOULDER_L", "MSP_SHOULDER_R",
    "MSP_ARM_UPPER_L", "MSP_ARM_UPPER_R",
    "MSP_ARM_LOWER_L", "MSP_ARM_LOWER_R",
    "MSP_HAND_L", "MSP_HAND_R",
    "MSP_LEG_UPPER_L", "MSP_LEG_UPPER_R",
    "MSP_LEG_LOWER_L", "MSP_LEG_LOWER_R",
    "MSP_FOOT_L", "MSP_FOOT_R",
    "MSP_STUMP_SHOULDER_L", "MSP_STUMP_SHOULDER_R",
    "MSP_STUMP_HIP_L", "MSP_STUMP_HIP_R",
    "MSP_STUMP_NECK",
]


def parse(path):
    rows = {}
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            parts = s.split()
            if len(parts) < 11:
                continue
            rows[parts[0]] = {
                "src_x":   int(parts[5]),  # we transcribe the DST coords
                "src_y":   int(parts[6]),  # (the atlas-relative position)
                "src_w":   int(parts[3]),
                "src_h":   int(parts[4]),
                "pivot_x": int(parts[7]),
                "pivot_y": int(parts[8]),
                "draw_w":  int(parts[9]),
                "draw_h":  int(parts[10]),
            }
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("chassis")
    ap.add_argument("--table", default=None,
                    help="Override (default: tools/comfy/crop_tables/<chassis>_crop.txt)")
    args = ap.parse_args()

    chassis = args.chassis.lower()
    path = Path(args.table) if args.table \
           else REPO / "tools" / "comfy" / "crop_tables" / f"{chassis}_crop.txt"
    if not path.is_file():
        sys.exit(f"crop table not found: {path}")

    rows = parse(path)

    upper = chassis.upper()
    print(f"/* Auto-generated from {path.relative_to(REPO)} by")
    print(f" * tools/comfy/transcribe_atlas.py.  Paste into")
    print(f" * src/mech_sprites.c at the per-chassis-art branch and")
    print(f" * delete the s_default_parts memcpy for CHASSIS_{upper}. */")
    print(f"static const MechSpritePart s_{chassis}_parts[MSP_COUNT] = {{")
    for msp in MSP_ORDER:
        # find the matching id
        id_for_msp = next((k for k, v in ID_MAP.items() if v == msp), None)
        r = rows.get(id_for_msp)
        if not r:
            print(f"    [{msp:24s}] = {{0}},   /* MISSING in {path.name} */")
            continue
        print(f"    [{msp:24s}] = {{ "
              f".src = {{ {r['src_x']:3d}, {r['src_y']:3d}, {r['src_w']:3d}, {r['src_h']:3d} }}, "
              f".pivot = {{ {r['pivot_x']:2d}, {r['pivot_y']:2d} }}, "
              f".draw_w = {r['draw_w']:3d}, .draw_h = {r['draw_h']:3d} }},")
    print("};")
    print(f"/* In mech_sprites_load_all, replace the s_default_parts memcpy")
    print(f" * for CHASSIS_{upper} with:")
    print(f" *   memcpy(g_chassis_sprites[CHASSIS_{upper}].parts, s_{chassis}_parts,")
    print(f" *          sizeof s_{chassis}_parts);")
    print(f" * Other chassis keep s_default_parts until their atlas ships. */")


if __name__ == "__main__":
    main()
