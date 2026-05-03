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

/* Server-side: same but uses lag-compensated bone positions for the
 * targets. The client's perceived snapshot was rendered ~`interp_ms`
 * ms in the past plus the shooter's RTT/2; we look up that historical
 * tick in each potential target's lag_hist and ray-test against
 * those positions. Passing -1 for `shot_at_tick` falls back to
 * weapons_fire_hitscan. */
void weapons_fire_hitscan_lag_comp(World *w, int mid, uint64_t shot_at_tick);

/* Client-only: simulate the *visual* aspects of firing (recoil + tracer
 * + cooldown decrement) without applying damage or hits. The server
 * authoritative result will arrive in a snapshot ~RTT later. */
void weapons_predict_local_fire(World *w, int mid);
