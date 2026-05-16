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

/* M6 ship-prep — hold-to-charge throw (frag grenade). `charge_factor`
 * ∈ [0, 1] is the fraction of `FRAG_CHARGE_MAX_SEC` the player held the
 * fire button before releasing. Internally maps to a velocity multiplier
 * over `wpn->projectile_speed_pxs` (see `FRAG_THROW_SPEED_MIN_MUL` /
 * `_MAX_MUL`) — short tap lobs short, full charge throws far. Damage,
 * AOE, drag, gravity, fuse are unchanged. Caller validates cooldown /
 * ammo / CTF-carrier gates. */
void weapons_spawn_throw_charged(World *w, int mid, int weapon_id,
                                 float charge_factor);

/* Hold-to-charge timings for WFIRE_THROW (frag grenade). */
#define FRAG_CHARGE_MAX_SEC      1.0f   /* seconds to reach full power */
#define FRAG_THROW_SPEED_MIN_MUL 0.5f   /* multiplier at 0% charge (quick tap) */
#define FRAG_THROW_SPEED_MAX_MUL 4.0f   /* multiplier at 100% charge (2800 px/s max — bullets are ~1900) */

/* Upward "lob" bias applied to the launch direction in
 * weapons_spawn_throw_charged. Bias = MIN + (MAX-MIN) × charge_factor;
 * 0.20..0.70 yields ~11°..35° of upward rotation at horizontal aim
 * (renormalized into a unit vector, so launch speed is unchanged).
 * 35° at max charge is close to the no-drag optimal range angle (45°)
 * but stays low enough to clear most concourse / reactor ceilings —
 * max-charge throws reach ~3200 px laterally before the first ground
 * hit, vs ~1800 px at the prior 0.55 max. */
#define FRAG_LOB_MIN_BIAS        0.20f
#define FRAG_LOB_MAX_BIAS        0.70f

/* Post-bounce velocity magnitude (px/s) below which a bouncy grenade
 * detonates on the spot instead of sitting and waiting for the fuse.
 * Without this, max-energy throws bounced 3-4 times and the final
 * "grenade rests on the ground" frame ran for ~0.5 s before the
 * fuse fired — read as a confusing "the grenade disappeared, then
 * exploded" pause. Empirical: 80 px/s ≈ 1.3 px/tick, slow enough to
 * be a settled grenade, fast enough that a still-rolling one keeps
 * going. */
#define FRAG_SETTLED_VMAG_PXS    80.0f

/* Camera-linger window (ticks @ 60 Hz) after a grenade owned by the
 * local mech detonates. Keeps `update_camera`'s focus biased toward
 * `world.last_explosion_pos` for ~0.67 s so the player sees the sparks
 * and impact AND the smooth-follow pan back home reads as a single
 * continuous motion, not a snap. */
#define FRAG_EXPLOSION_LINGER_TICKS 40

/* Melee hit on the closest mech inside `range_px` of the firer's chest
 * along the aim direction. Backstab does ×2.5 damage. */
void weapons_fire_melee(World *w, int mid, int weapon_id);

/* Server-side: queue a fire event for broadcast as NET_MSG_FIRE_EVENT.
 * The standard fire paths (hitscan / spawn_projectiles / fire_melee)
 * call this internally — only mech.c's bespoke WFIRE_GRAPPLE branch
 * needs to call it directly (the grapple path doesn't go through
 * weapons_spawn_projectiles since the head is non-damaging). */
void weapons_record_fire(World *w, int mid, int weapon_id, Vec2 origin, Vec2 dir);

/* Helper used by the HUD and the kill feed. */
const char *weapon_short_name(int id);
