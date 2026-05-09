#pragma once

#include "math.h"
#include "weapons.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdbool.h>

/*
 * Weapon sprite atlas (M5 P11).
 *
 * One shared atlas covers all 14 weapons. Each `WeaponSpriteDef` carries
 * the sub-rect inside the atlas plus three sprite-local pivot points:
 *
 *   - `pivot_grip`     — anchor at the firer's R_HAND (alignment point
 *                        for `DrawTexturePro`).
 *   - `pivot_foregrip` — off-hand grip; (-1, -1) for one-handed weapons.
 *                        Drives the L_HAND pose target so two-handed
 *                        weapons get the off-hand on the foregrip.
 *   - `muzzle_offset`  — where bullets visibly emit. Replaces the old
 *                        `Weapon.muzzle_offset` (single float, distance
 *                        along aim from R_HAND); the sprite-driven
 *                        version aligns visible muzzle with physical.
 *
 * `weapons_atlas_load` walks `assets/sprites/weapons.png`. If the file
 * is missing the renderer falls back to a thin line scaled by
 * `draw_w * 0.7` (slightly per-weapon-distinct vs. the M4 fixed 22-px
 * line). The placeholder sub-rects below assume a 1024×256 atlas; real
 * art lands in P16.
 *
 * Spec: documents/m5/12-rigging-and-damage.md §"Per-weapon visible art"
 * + §"Foregrip positions per two-handed weapon".
 */

typedef struct {
    Rectangle src;              /* sub-rect in atlas (px)                 */
    Vector2   pivot_grip;       /* grip in src-relative px (R_HAND anchor)*/
    Vector2   pivot_foregrip;   /* foregrip in src-relative px            */
                                /* (-1, -1) → one-handed, no foregrip pose*/
    Vector2   muzzle_offset;    /* muzzle in src-relative px              */
    float     draw_w;           /* world-space draw width  (rest)         */
    float     draw_h;           /* world-space draw height (rest)         */
    int       weapon_id;        /* WeaponId — for sanity / table lookup   */
} WeaponSpriteDef;

extern Texture2D            g_weapons_atlas;
extern const WeaponSpriteDef g_weapon_sprites[WEAPON_COUNT];

/* Returns NULL for an out-of-range id; otherwise a pointer to the
 * static table entry (callers must not mutate). */
const WeaponSpriteDef *weapon_sprite_def(int weapon_id);

/* Compute the world-space muzzle position. Both fire and render paths
 * share this so projectile spawn origin matches the visible weapon
 * silhouette. `wp` may be NULL — falls back to the legacy
 * `hand + dir * fallback_offset` so charts that don't yet have a sprite
 * def still produce a sensible origin. */
Vec2 weapon_muzzle_world(Vec2 hand, Vec2 aim, const WeaponSpriteDef *wp,
                         float fallback_offset);

/* Compute the world-space foregrip position for the off-hand pose
 * target. Returns false (and leaves `out` unset) for one-handed weapons
 * or when `wp` is NULL. */
bool weapon_foregrip_world(Vec2 hand, Vec2 aim, const WeaponSpriteDef *wp,
                           Vec2 *out);

/* Walks `assets/sprites/weapons.png` and loads it as the shared weapon
 * atlas. Missing file leaves `g_weapons_atlas.id == 0` so the renderer
 * falls back to the per-weapon line draw. Returns true on successful
 * load (or false if the file isn't present / load failed).
 *
 * MUST be called after `platform_init` (raylib GL context required).
 * Idempotent: a second call no-ops if the atlas already loaded. */
bool weapons_atlas_load(void);

/* Releases the loaded atlas. Safe to call when nothing has been
 * loaded (no-op). */
void weapons_atlas_unload(void);
