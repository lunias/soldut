#pragma once

#include "mech.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdbool.h>

/*
 * Mech sprite atlases (M5 P10).
 *
 * One Texture2D per chassis. Each chassis carries a 22-entry sub-rect /
 * pivot / draw-size table that maps its `MechSpriteId`s to atlas regions.
 * The renderer (`src/render.c::draw_mech`) reads `g_chassis_sprites[m->
 * chassis_id]` per draw; if `atlas.id == 0` it falls back to the M4
 * capsule-line render path (`draw_mech_capsules`), so a fresh checkout
 * without `assets/sprites/<chassis>.png` files keeps rendering.
 *
 * Layout discipline + overlap zones + pivot rules are specified in
 * `documents/m5/12-rigging-and-damage.md`. Per-chassis art lands in
 * P15 / P16; the placeholder sub-rect/pivot table installed here at
 * load time matches a notional Trooper-sized atlas so a single dropped
 * PNG can drive the sprite path end-to-end during integration testing.
 */

typedef struct {
    Rectangle src;          /* sub-rect into the chassis atlas (px)        */
    Vector2   pivot;        /* pivot in src-relative px (DrawTexturePro origin) */
    float     draw_w;       /* world-space draw width  (rest)              */
    float     draw_h;       /* world-space draw height (rest)              */
} MechSpritePart;

/* Per-chassis sprite IDs. The indices below are stable across all chassis;
 * the per-chassis art each draws its part at the same `MechSpriteId`.
 * Stump caps are drawn at the parent particle when the relevant
 * `LIMB_*` bit is set in `m->dismember_mask` (P12). */
typedef enum {
    MSP_TORSO = 0,
    MSP_HEAD,
    MSP_HIP_PLATE,
    MSP_SHOULDER_L, MSP_SHOULDER_R,
    MSP_ARM_UPPER_L, MSP_ARM_UPPER_R,
    MSP_ARM_LOWER_L, MSP_ARM_LOWER_R,
    MSP_HAND_L,      MSP_HAND_R,
    MSP_LEG_UPPER_L, MSP_LEG_UPPER_R,
    MSP_LEG_LOWER_L, MSP_LEG_LOWER_R,
    MSP_FOOT_L,      MSP_FOOT_R,
    MSP_STUMP_SHOULDER_L, MSP_STUMP_SHOULDER_R,
    MSP_STUMP_HIP_L,      MSP_STUMP_HIP_R,
    MSP_STUMP_NECK,
    MSP_COUNT
} MechSpriteId;

typedef struct {
    Texture2D       atlas;                /* atlas.id == 0 → no sprite art, render falls back to capsules */
    MechSpritePart  parts[MSP_COUNT];     /* populated by mech_sprites_load_all */
} MechSpriteSet;

/* Indexed by ChassisId. Mutable: load_all writes `atlas` + `parts`,
 * unload_all zeroes `atlas`. Per-chassis art will eventually edit the
 * `parts` initializer when each chassis's atlas is hand-packed. */
extern MechSpriteSet g_chassis_sprites[CHASSIS_COUNT];

/* Walks `assets/sprites/<chassis>.png` for each chassis and loads any
 * present file as that chassis's atlas. Missing files log an INFO line
 * and leave that slot's `atlas.id == 0` so the renderer falls back to
 * the capsule path for that chassis only. Returns true if any atlas
 * loaded.
 *
 * MUST be called after `platform_init` (raylib GL context required).
 * Idempotent: a second call no-ops on already-loaded entries. */
bool mech_sprites_load_all(void);

/* Releases every loaded atlas. Called from `platform_shutdown` (or on
 * an early-error exit path). Safe to call when no atlases have been
 * loaded. */
void mech_sprites_unload_all(void);

/* P12 — Damage-decal accounting helpers.
 *
 * mech_part_to_sprite_id maps a hit on a particle (PART_*) to the
 * `MechSpriteId` whose decal ring should record the hit. Hits on a
 * limb's distal particle (e.g. PART_L_HAND) accumulate on the larger
 * parent sprite (e.g. MSP_ARM_LOWER_L) so the dot is visible.
 *
 * mech_sprite_part_endpoints returns the bone-segment endpoints that
 * anchor a sprite. `*out_a == -1` means single-particle anchor (the
 * sprite draws at `*out_b`). Used by the damage-decal world↔local
 * transform so the decal stays glued to its sprite as the body moves. */
MechSpriteId mech_part_to_sprite_id(int part);
void         mech_sprite_part_endpoints(MechSpriteId sp,
                                        int *out_a, int *out_b);

/* P12 — World decal-count slot count must equal MSP_COUNT so the
 * `damage_decals[MECH_LIMB_DECAL_COUNT]` array on Mech can be indexed
 * by `MechSpriteId` directly. world.h can't include this header
 * (cycle), so the constant is mirrored there and asserted here. */
_Static_assert(MSP_COUNT == MECH_LIMB_DECAL_COUNT,
               "MSP_COUNT must match MECH_LIMB_DECAL_COUNT in world.h");
