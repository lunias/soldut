#pragma once

#include "world.h"

/*
 * Weapons. M1 has one (Pulse Rifle, hitscan); the rest follow at M3.
 * The table is data — we'll grow it instead of branching on weapon id.
 */

typedef enum {
    WEAPON_PULSE_RIFLE = 0,
    /* Plasma SMG, Riot Cannon, Rail Cannon, Auto-Cannon, Mass Driver,
     * Plasma Cannon, Microgun follow at M3. */
    WEAPON_COUNT_M1
} WeaponId;

typedef struct Weapon {
    const char *name;
    bool        hitscan;          /* true: ray on fire-tick. false: projectile. */
    float       damage;           /* base, before hit-location/armor */
    float       fire_rate_sec;    /* min seconds between shots */
    float       reload_sec;
    int         mag_size;
    float       range_px;         /* max hitscan range */
    float       recoil_impulse;   /* px-displacement on the firing hand */
    float       muzzle_offset;    /* push the tracer source out from the hand */
} Weapon;

const Weapon *weapon_def(int id);

/* Run a hitscan shot from `mid`. Walks the bone segments of every other
 * mech and the level tile grid; applies damage and recoil; spawns the
 * tracer FX. */
void weapons_fire_hitscan(World *w, int mid);
