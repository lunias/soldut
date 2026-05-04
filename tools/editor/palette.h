#pragma once

/*
 * Editor palettes: tile id, polygon kind, slope/alcove presets, pickup
 * category × variant grid, ambient kind, decoration sprite list.
 *
 * The presets come from documents/m5/03-collision-polygons.md
 * §"Editor presets" and documents/m5/07-maps.md §"Editor presets".
 */

#include "doc.h"
#include "world.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- Polygon kind (the value stored in LvlPoly.kind). */
typedef struct PolyKindEntry {
    int         kind;          /* POLY_KIND_SOLID etc. */
    const char *name;
    /* Render color. Alpha is overlaid by the editor; 0xff for opaque. */
    uint32_t    rgba;
} PolyKindEntry;

const PolyKindEntry *palette_poly_kinds(int *out_count);

/* ---- Tile-flag toggles. The tile palette is a single id (0 by default
 * for the M5 industrial set) plus a bitmask. */
typedef struct TileFlagEntry {
    uint16_t    flag;          /* TILE_F_* */
    const char *name;
    char        hotkey;        /* e.g. 's' for SOLID */
} TileFlagEntry;

const TileFlagEntry *palette_tile_flags(int *out_count);

/* ---- Slope + alcove presets.
 *
 * A preset, when applied, drops one or more polygons into the doc at
 * the pivot point. The pivot is the "anchor" — typically bottom-left
 * for floor presets, bottom-right for mirrors, etc. The editor passes
 * the snapped pivot in world-space; presets emit triangles in the
 * surrounding ~3 tiles. */
typedef enum {
    PRESET_RAMP_UP_30, PRESET_RAMP_UP_45, PRESET_RAMP_UP_60,
    PRESET_RAMP_DN_30, PRESET_RAMP_DN_45, PRESET_RAMP_DN_60,
    PRESET_BOWL_30,    PRESET_BOWL_45,
    PRESET_OVERHANG_30, PRESET_OVERHANG_45, PRESET_OVERHANG_60,
    PRESET_ALCOVE_EDGE,        /* 2 tiles deep × 3 tiles tall */
    PRESET_ALCOVE_JETPACK,     /* 3 wide × 3 tall × 3 deep */
    PRESET_ALCOVE_SLOPE_ROOF,  /* 4 wide × 4 tall ↘ 1 tall */
    PRESET_CAVE_BLOCK,         /* one cave room (alias of EDGE for v1) */
    PRESET_COUNT,
} PresetKind;

typedef struct PresetEntry {
    PresetKind  kind;
    const char *name;
    const char *category;      /* "Slope" / "Bowl" / "Overhang" / "Alcove" */
} PresetEntry;

const PresetEntry *palette_presets(int *out_count);

/* Apply a preset at world-space pivot. Returns the number of polygons
 * appended to the doc (0 on failure). The polygons land as POLY_KIND_
 * SOLID by default (overhangs and alcove walls are all SOLID). */
int palette_apply_preset(EditorDoc *d, PresetKind k, int pivot_x, int pivot_y);

/* ---- Pickup palette. */
typedef enum {
    PICK_HEALTH_S = 0, PICK_HEALTH_M, PICK_HEALTH_L,
    PICK_AMMO_PRIMARY, PICK_AMMO_SECONDARY,
    PICK_ARMOR_LIGHT, PICK_ARMOR_HEAVY, PICK_ARMOR_REACTIVE,
    PICK_WEAPON_RAIL, PICK_WEAPON_MASSDRIVER, PICK_WEAPON_PLASMA,
    PICK_POWERUP_BERSERK, PICK_POWERUP_INVIS,
    PICK_JET_FUEL,
    PICK_PRACTICE_DUMMY,        /* per documents/m5/04-pickups.md §"Practice dummy" */
    PICK_VARIANT_COUNT,
} PickupVariant;

typedef struct PickupEntry {
    PickupVariant variant;
    const char   *name;
    const char   *category;     /* "Health" / "Ammo" / ... */
    uint8_t       category_id;  /* LvlPickup.category */
    uint8_t       variant_id;   /* LvlPickup.variant */
    uint16_t      respawn_ms;
} PickupEntry;

const PickupEntry *palette_pickups(int *out_count);

/* ---- Ambient kind dropdown. */
typedef struct AmbiKindEntry {
    int         kind;          /* AMBI_* */
    const char *name;
} AmbiKindEntry;

const AmbiKindEntry *palette_ambi_kinds(int *out_count);
