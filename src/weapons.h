#pragma once

#include "world.h"

/*
 * Weapons. The full M3 set: 8 primaries + 6 secondaries, plus a small
 * shared table that the firing path branches on.
 *
 * Each weapon answers four questions per documents/04-combat.md:
 *   1. range (close / mid / long)
 *   2. tempo (single / burst / sustained)
 *   3. what does it punish
 *   4. what is its weakness
 *
 * The numbers below are starting targets — they will be tuned in
 * playtest. The *system* is fixed by 04-combat.md.
 */

typedef enum {
    /* --- Primaries --- */
    WEAPON_PULSE_RIFLE = 0,
    WEAPON_PLASMA_SMG,
    WEAPON_RIOT_CANNON,
    WEAPON_RAIL_CANNON,
    WEAPON_AUTO_CANNON,
    WEAPON_MASS_DRIVER,
    WEAPON_PLASMA_CANNON,
    WEAPON_MICROGUN,

    /* --- Secondaries --- */
    WEAPON_SIDEARM,
    WEAPON_BURST_SMG,
    WEAPON_FRAG_GRENADES,
    WEAPON_MICRO_ROCKETS,
    WEAPON_COMBAT_KNIFE,
    WEAPON_GRAPPLING_HOOK,

    WEAPON_COUNT
} WeaponId;

/* Weapon classes — used for sanity (loadout slot validation). */
typedef enum {
    WEAPON_CLASS_PRIMARY = 0,
    WEAPON_CLASS_SECONDARY,
} WeaponClass;

/* Firing model — drives which path in the fire pipeline runs. */
typedef enum {
    WFIRE_HITSCAN = 0,           /* ray on the fire tick (Pulse, Sidearm, Rail) */
    WFIRE_PROJECTILE,            /* spawns a single projectile */
    WFIRE_SPREAD,                /* spawns N pellets in a cone (Riot Cannon) */
    WFIRE_BURST,                 /* fires N rounds with short interval (Burst SMG) */
    WFIRE_MELEE,                 /* short-range hitscan (Combat Knife) */
    WFIRE_THROW,                 /* projectile but consumes ammo / charges (knife throw, grenades) */
    WFIRE_GRAPPLE,               /* utility — anchors / pulls */
} WeaponFireKind;

typedef struct Weapon {
    const char    *name;
    WeaponClass    klass;
    WeaponFireKind fire;

    /* Common stats. */
    float damage;                /* base, before hit-location/armor */
    float fire_rate_sec;         /* min seconds between shots (or between bursts) */
    float reload_sec;
    int   mag_size;              /* 0 = no mag (charges or melee) */
    float range_px;              /* hitscan max; melee reach for WFIRE_MELEE */
    float recoil_impulse;        /* px-displacement on the firing hand */
    float bink;                  /* rad applied to nearby targets on fire */
    float self_bink;             /* rad of jitter to own aim per rapid shot */
    float muzzle_offset;
    float charge_sec;            /* >0 → must be held this long before firing (Rail Cannon, Microgun spin-up) */

    /* Spread / burst. */
    int   spread_pellets;        /* WFIRE_SPREAD: pellets per fire */
    float spread_cone_rad;       /* half-angle of pellet cone */
    int   burst_rounds;          /* WFIRE_BURST: rounds per trigger pull */
    float burst_interval_sec;    /* seconds between rounds inside a burst */

    /* Projectile params (WFIRE_PROJECTILE / WFIRE_SPREAD / WFIRE_THROW). */
    int   projectile_kind;       /* ProjectileKind */
    float projectile_speed_pxs;  /* initial speed */
    float projectile_life_sec;
    float projectile_drag;       /* per-second damping */
    float projectile_grav_scale; /* 0 = ignore gravity */
    float aoe_radius;            /* 0 = no AOE */
    float aoe_damage;
    float aoe_impulse;
    bool  bouncy;                /* tile hit → bounce (frag grenades) */
} Weapon;

const Weapon *weapon_def(int id);

/* Run a hitscan shot from the firing mech's right hand. Walks the bone
 * segments of every other mech and the level tile grid; applies damage
 * and recoil; spawns the tracer FX. The aim direction is rotated by
 * the shooter's accumulated bink before the ray test. */
void weapons_fire_hitscan(World *w, int mid);

/* Lag-compensated server hitscan. Same as above but uses the lag-
 * history bone positions for the target tick. Passing -1 / 0 falls
 * back to weapons_fire_hitscan. */
void weapons_fire_hitscan_lag_comp(World *w, int mid, uint64_t shot_at_tick);

/* Client-only: simulate the *visual* aspects of firing (recoil + tracer
 * + cooldown decrement) without applying damage or hits. The server
 * authoritative result will arrive in a snapshot ~RTT later. */
void weapons_predict_local_fire(World *w, int mid);

/* Used by the projectile pool: spawn the bullet(s) for a projectile
 * weapon. (Caller has already validated cooldown / ammo / etc.) */
void weapons_spawn_projectiles(World *w, int mid, int weapon_id);

/* Melee hit on the closest mech inside `range_px` of the firer's chest
 * along the aim direction. Backstab does ×2.5 damage. */
void weapons_fire_melee(World *w, int mid, int weapon_id);

/* Helper used by the HUD and the kill feed. */
const char *weapon_short_name(int id);
