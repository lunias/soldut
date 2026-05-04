#include "palette.h"

#include "log.h"
#include "poly.h"

#include "stb_ds.h"

#include <string.h>

/* ---- Polygon kinds ------------------------------------------------ */

static const PolyKindEntry g_poly_kinds[] = {
    { POLY_KIND_SOLID,      "Solid",      0xa0a8b8ffu },
    { POLY_KIND_ICE,        "Ice",        0x80c8ffffu },
    { POLY_KIND_DEADLY,     "Deadly",     0xff5040ffu },
    { POLY_KIND_ONE_WAY,    "One-way",    0x80f080ffu },
    { POLY_KIND_BACKGROUND, "Background", 0x808080a0u },
};

const PolyKindEntry *palette_poly_kinds(int *n) {
    *n = (int)(sizeof g_poly_kinds / sizeof g_poly_kinds[0]);
    return g_poly_kinds;
}

/* ---- Tile flags --------------------------------------------------- */

static const TileFlagEntry g_tile_flags[] = {
    { TILE_F_SOLID,      "Solid",      's' },
    { TILE_F_ICE,        "Ice",        'i' },
    { TILE_F_DEADLY,     "Deadly",     'd' },
    { TILE_F_ONE_WAY,    "One-way",    'o' },
    { TILE_F_BACKGROUND, "Background", 'b' },
};

const TileFlagEntry *palette_tile_flags(int *n) {
    *n = (int)(sizeof g_tile_flags / sizeof g_tile_flags[0]);
    return g_tile_flags;
}

/* ---- Presets ------------------------------------------------------ */

static const PresetEntry g_presets[] = {
    { PRESET_RAMP_UP_30,        "Ramp ↗ 30°", "Slope" },
    { PRESET_RAMP_UP_45,        "Ramp ↗ 45°", "Slope" },
    { PRESET_RAMP_UP_60,        "Ramp ↗ 60°", "Slope" },
    { PRESET_RAMP_DN_30,        "Ramp ↘ 30°", "Slope" },
    { PRESET_RAMP_DN_45,        "Ramp ↘ 45°", "Slope" },
    { PRESET_RAMP_DN_60,        "Ramp ↘ 60°", "Slope" },
    { PRESET_BOWL_30,           "Bowl 30°",   "Bowl" },
    { PRESET_BOWL_45,           "Bowl 45°",   "Bowl" },
    { PRESET_OVERHANG_30,       "Overhang 30°", "Overhang" },
    { PRESET_OVERHANG_45,       "Overhang 45°", "Overhang" },
    { PRESET_OVERHANG_60,       "Overhang 60°", "Overhang" },
    { PRESET_ALCOVE_EDGE,       "Edge alcove", "Alcove" },
    { PRESET_ALCOVE_JETPACK,    "Jetpack alcove", "Alcove" },
    { PRESET_ALCOVE_SLOPE_ROOF, "Slope-roof alcove", "Alcove" },
    { PRESET_CAVE_BLOCK,        "Cave block", "Alcove" },
};

const PresetEntry *palette_presets(int *n) {
    *n = (int)(sizeof g_presets / sizeof g_presets[0]);
    return g_presets;
}

/* Helper: append `n` vertices forming a single polygon to the doc as
 * SOLID-kind triangles via poly_triangulate. Returns triangles emitted. */
static int emit_piece(EditorDoc *d, const EditorPolyVert *verts, int n,
                      uint16_t kind) {
    if (n < 3) return 0;
    LvlPoly tris[16];
    int t = poly_triangulate(verts, n, kind, tris,
                             (int)(sizeof tris / sizeof tris[0]));
    if (t <= 0) return 0;
    for (int i = 0; i < t; ++i) arrput(d->polys, tris[i]);
    return t;
}

/* Pivot is "bottom-left corner of the bounding box" by convention. The
 * Y-axis is screen-space (Y grows downward). Slope ramps "go up to the
 * right" by raising the Y-component of the right vertex. */
int palette_apply_preset(EditorDoc *d, PresetKind k, int px, int py) {
    int total = 0;
    int T = d->tile_size;
    switch (k) {
        case PRESET_RAMP_UP_30: {
            /* 1.5 tiles wide × 1 tile tall. arctan(32 / 48) ≈ 33.7°
             * (close to 30°, matches the spec's "single triangle"
             * geometry). */
            EditorPolyVert v[3] = {
                { px,           py },
                { px + T*3/2,   py },
                { px + T*3/2,   py - T },
            };
            total = emit_piece(d, v, 3, POLY_KIND_SOLID);
        } break;
        case PRESET_RAMP_UP_45: {
            EditorPolyVert v[3] = {
                { px,     py     },
                { px + T, py     },
                { px + T, py - T },
            };
            total = emit_piece(d, v, 3, POLY_KIND_SOLID);
        } break;
        case PRESET_RAMP_UP_60: {
            /* 1 wide × 2 tall (steep). arctan(64 / 32) ≈ 63°. */
            EditorPolyVert v[3] = {
                { px,     py        },
                { px + T, py        },
                { px + T, py - 2*T  },
            };
            total = emit_piece(d, v, 3, POLY_KIND_SOLID);
        } break;
        case PRESET_RAMP_DN_30: {
            EditorPolyVert v[3] = {
                { px,           py - T },
                { px,           py     },
                { px + T*3/2,   py     },
            };
            total = emit_piece(d, v, 3, POLY_KIND_SOLID);
        } break;
        case PRESET_RAMP_DN_45: {
            EditorPolyVert v[3] = {
                { px,     py - T },
                { px,     py     },
                { px + T, py     },
            };
            total = emit_piece(d, v, 3, POLY_KIND_SOLID);
        } break;
        case PRESET_RAMP_DN_60: {
            EditorPolyVert v[3] = {
                { px,     py - 2*T },
                { px,     py       },
                { px + T, py       },
            };
            total = emit_piece(d, v, 3, POLY_KIND_SOLID);
        } break;
        case PRESET_BOWL_30:
        case PRESET_BOWL_45: {
            int slope_w = (k == PRESET_BOWL_45) ? T : T*3/2;
            int flat_w  = T;
            int height  = T;
            /* Single-piece concave outline going CW in screen space. */
            EditorPolyVert v[6] = {
                { px,                                py - height },
                { px + slope_w,                      py          },
                { px + slope_w + flat_w,             py          },
                { px + slope_w + flat_w + slope_w,   py - height },
                { px + slope_w + flat_w + slope_w,   py          },
                { px,                                py          },
            };
            /* Re-order to form a SOLID polygon below the dip — the
             * triangle below the bowl, fanning down. We model the
             * bowl as 2 ramps + a flat, all SOLID, beneath floor_y. */
            EditorPolyVert left[3]  = { v[0], v[1], { px, py } };
            EditorPolyVert mid[4]   = { v[1], v[2], { px + slope_w + flat_w, py },
                                        { px + slope_w, py } };
            EditorPolyVert right[3] = { v[2], v[3], v[4] };
            total += emit_piece(d, left,  3, POLY_KIND_SOLID);
            total += emit_piece(d, mid,   4, POLY_KIND_SOLID);
            total += emit_piece(d, right, 3, POLY_KIND_SOLID);
        } break;
        case PRESET_OVERHANG_30:
        case PRESET_OVERHANG_45:
        case PRESET_OVERHANG_60: {
            int wide   = (k == PRESET_OVERHANG_30) ? T*3/2 : T;
            int height = (k == PRESET_OVERHANG_60) ? 2*T   : T;
            /* Mirror of ramp_up around y = py: the slope goes up-right
             * starting from a flat top edge. */
            EditorPolyVert v[3] = {
                { px,         py + height },
                { px + wide,  py          },
                { px + wide,  py + height },
            };
            total = emit_piece(d, v, 3, POLY_KIND_SOLID);
        } break;
        case PRESET_ALCOVE_EDGE: {
            /* Edge alcove: 2 tiles deep × 3 tiles tall void, opening to
             * the left (away from a wall on the right). Pivot is the
             * front-bottom corner of the void (i.e. the floor-front
             * corner of the alcove). Emit four 1-tile-thick walls
             * around the void: top, back, bottom-floor-extension. */
            int dx = 2*T;     /* depth */
            int dy = 3*T;     /* height */
            int wt = T;       /* wall thickness */
            /* Bottom (floor lip — extends below the alcove pivot). */
            EditorPolyVert bot[4] = {
                { px,        py     },
                { px + dx,   py     },
                { px + dx,   py + wt},
                { px,        py + wt},
            };
            /* Back wall (right side of void). */
            EditorPolyVert back[4] = {
                { px + dx,        py - dy   },
                { px + dx + wt,   py - dy   },
                { px + dx + wt,   py        },
                { px + dx,        py        },
            };
            /* Top (ceiling). */
            EditorPolyVert top[4] = {
                { px,             py - dy - wt },
                { px + dx + wt,   py - dy - wt },
                { px + dx + wt,   py - dy      },
                { px,             py - dy      },
            };
            total += emit_piece(d, bot,  4, POLY_KIND_SOLID);
            total += emit_piece(d, back, 4, POLY_KIND_SOLID);
            total += emit_piece(d, top,  4, POLY_KIND_SOLID);
        } break;
        case PRESET_ALCOVE_JETPACK: {
            /* 3 wide × 3 tall × 3 deep, mouth facing down (entry is
             * vertical from below). Polygons: top + left + right + back. */
            int dx = 3*T, dy = 3*T, wt = T;
            EditorPolyVert top[4] = {
                { px,         py - dy - wt },
                { px + dx,    py - dy - wt },
                { px + dx,    py - dy      },
                { px,         py - dy      },
            };
            EditorPolyVert left[4] = {
                { px - wt,    py - dy },
                { px,         py - dy },
                { px,         py      },
                { px - wt,    py      },
            };
            EditorPolyVert right[4] = {
                { px + dx,         py - dy },
                { px + dx + wt,    py - dy },
                { px + dx + wt,    py      },
                { px + dx,         py      },
            };
            total += emit_piece(d, top,   4, POLY_KIND_SOLID);
            total += emit_piece(d, left,  4, POLY_KIND_SOLID);
            total += emit_piece(d, right, 4, POLY_KIND_SOLID);
        } break;
        case PRESET_ALCOVE_SLOPE_ROOF: {
            /* 4 wide × 4 tall at entrance, 1 tall at back. One floor +
             * one 45° slope ceiling. */
            int dx = 4*T;
            EditorPolyVert floor_q[4] = {
                { px,        py     },
                { px + dx,   py     },
                { px + dx,   py + T },
                { px,        py + T },
            };
            /* Ceiling: triangle that slopes up-back. */
            EditorPolyVert roof[3] = {
                { px,        py - 4*T },
                { px + dx,   py - T   },
                { px,        py - T   },
            };
            total += emit_piece(d, floor_q, 4, POLY_KIND_SOLID);
            total += emit_piece(d, roof,    3, POLY_KIND_SOLID);
        } break;
        case PRESET_CAVE_BLOCK:
        default:
            /* Alias of EDGE for v1 — designers chain these manually. */
            return palette_apply_preset(d, PRESET_ALCOVE_EDGE, px, py);
    }
    if (total > 0) d->dirty = true;
    return total;
}

/* ---- Pickup palette ----------------------------------------------- */
/* Per documents/m5/04-pickups.md — category enums (PICKUP_CAT_*) live
 * there at P05; until that ships we use the literal numeric values
 * specified by 01-lvl-format.md §PICK. respawn_ms = 0 means use the
 * category default at runtime. */

#define PCAT_HEALTH   0
#define PCAT_AMMO     1
#define PCAT_ARMOR    2
#define PCAT_WEAPON   3
#define PCAT_POWERUP  4
#define PCAT_JET_FUEL 5
#define PCAT_PRACTICE 6      /* per 04-pickups.md §"Practice dummy" */

static const PickupEntry g_pickups[] = {
    { PICK_HEALTH_S,         "Health S",     "Health",  PCAT_HEALTH,  0, 0 },
    { PICK_HEALTH_M,         "Health M",     "Health",  PCAT_HEALTH,  1, 0 },
    { PICK_HEALTH_L,         "Health L",     "Health",  PCAT_HEALTH,  2, 0 },
    { PICK_AMMO_PRIMARY,     "Ammo P",       "Ammo",    PCAT_AMMO,    0, 0 },
    { PICK_AMMO_SECONDARY,   "Ammo S",       "Ammo",    PCAT_AMMO,    1, 0 },
    { PICK_ARMOR_LIGHT,      "Armor L",      "Armor",   PCAT_ARMOR,   1, 0 },
    { PICK_ARMOR_HEAVY,      "Armor H",      "Armor",   PCAT_ARMOR,   2, 0 },
    { PICK_ARMOR_REACTIVE,   "Armor R",      "Armor",   PCAT_ARMOR,   3, 0 },
    { PICK_WEAPON_RAIL,      "W: Rail",      "Weapon",  PCAT_WEAPON,  3, 0 },
    { PICK_WEAPON_MASSDRIVER,"W: Mass Drv.", "Weapon",  PCAT_WEAPON,  5, 0 },
    { PICK_WEAPON_PLASMA,    "W: Plasma C.", "Weapon",  PCAT_WEAPON,  6, 0 },
    { PICK_POWERUP_BERSERK,  "P: Berserk",   "Powerup", PCAT_POWERUP, 0, 0 },
    { PICK_POWERUP_INVIS,    "P: Invis.",    "Powerup", PCAT_POWERUP, 1, 0 },
    { PICK_JET_FUEL,         "Jet Fuel",     "JetFuel", PCAT_JET_FUEL,0, 0 },
    { PICK_PRACTICE_DUMMY,   "Pract. Dummy", "Special", PCAT_PRACTICE,0, 0 },
};

const PickupEntry *palette_pickups(int *n) {
    *n = (int)(sizeof g_pickups / sizeof g_pickups[0]);
    return g_pickups;
}

/* ---- Ambient kinds ------------------------------------------------ */

static const AmbiKindEntry g_ambis[] = {
    { AMBI_WIND,   "Wind"   },
    { AMBI_ZERO_G, "Zero-G" },
    { AMBI_ACID,   "Acid"   },
    { AMBI_FOG,    "Fog"    },
};

const AmbiKindEntry *palette_ambi_kinds(int *n) {
    *n = (int)(sizeof g_ambis / sizeof g_ambis[0]);
    return g_ambis;
}
