#pragma once

#include "hash.h"
#include "input.h"
#include "math.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * World — the entire simulation state. One process owns one World.
 *
 * The shape is fixed at startup: pools are sized for the worst case we
 * design for (32 mechs in heavy combat) and never grow. Indices are
 * stable handles. simulate(World*, input, dt) is meant to be a pure
 * function of those arguments plus the World's seeded RNG; it doesn't
 * read globals or wall-clock time. (See [01-philosophy.md] Rule 2.)
 */

/* ---- Particle pool (SoA, used for mech bones, not for FX particles).
 * Capacity 4096 covers 32 mechs × 16 particles + headroom for gibs. */
#define PARTICLES_CAPACITY  4096

typedef enum {
    PARTICLE_FLAG_ACTIVE   = 1u << 0,
    PARTICLE_FLAG_GROUNDED = 1u << 1,
    PARTICLE_FLAG_PINNED   = 1u << 2,
} ParticleFlags;

typedef struct {
    float    *pos_x, *pos_y;
    float    *prev_x, *prev_y;
    float    *inv_mass;
    uint8_t  *flags;
    int       count;       /* high-water mark */
    int       capacity;
} ParticlePool;

/* ---- Constraint pool (AoS). The solver walks this linearly per
 * relaxation iteration, so layout cost is dominated by cache misses on
 * the particles, not the constraints themselves. Capacity 2048 fits
 * 32 mechs × ~21 constraints + slack. */
#define CONSTRAINTS_CAPACITY 2048

typedef enum {
    CSTR_DISTANCE = 0,
    CSTR_DISTANCE_LIMIT,    /* min/max bound — used for joint limit cones */
    CSTR_ANGLE,
} ConstraintKind;

typedef struct {
    uint16_t a, b;          /* particle indices */
    uint16_t c;             /* angle: middle joint */
    uint8_t  kind;          /* ConstraintKind */
    uint8_t  active;        /* false → dismembered/detached */
    float    rest;          /* DISTANCE */
    float    min_len, max_len;  /* DISTANCE_LIMIT */
    float    min_ang, max_ang;  /* ANGLE (radians) */
} Constraint;

typedef struct {
    Constraint *items;
    int count;
    int capacity;
} ConstraintPool;

/* ---- Skeleton particle indices, per [03-physics-and-mechs.md]. */
enum {
    PART_HEAD = 0,
    PART_NECK,
    PART_CHEST,
    PART_PELVIS,
    PART_L_SHOULDER, PART_L_ELBOW, PART_L_HAND,
    PART_R_SHOULDER, PART_R_ELBOW, PART_R_HAND,
    PART_L_HIP, PART_L_KNEE, PART_L_FOOT,
    PART_R_HIP, PART_R_KNEE, PART_R_FOOT,
    PART_COUNT
};

/* Limb bits, used for tracking which joints have been severed. The
 * dismember mask is broadcast in the snapshot so clients can render
 * the right set of bones. */
enum {
    LIMB_HEAD     = 1u << 0,
    LIMB_L_ARM    = 1u << 1,
    LIMB_R_ARM    = 1u << 2,
    LIMB_L_LEG    = 1u << 3,
    LIMB_R_LEG    = 1u << 4,
};

/* All-limbs mask — used by dismemberment counting for KILLFLAG_GIB. */
#define LIMB_ALL_MASK (LIMB_HEAD | LIMB_L_ARM | LIMB_R_ARM | LIMB_L_LEG | LIMB_R_LEG)
static inline int limb_count(uint8_t mask) {
    int n = 0;
    while (mask) { n += (int)(mask & 1u); mask >>= 1; }
    return n;
}

/* ---- Mech ----------------------------------------------------------- */
#define MAX_MECHS         32
#define MAX_BLOOD         3000

/* Lag-compensation history: per mech, ring buffer of bone particle
 * positions for the last LAG_HIST_TICKS ticks. At 60 Hz this covers
 * 200 ms — the cap on how far back we'll rewind a hitscan to a target's
 * past position. The hitscan path on the server scans this ring for the
 * frame the shooter "saw" and tests against those positions, not the
 * current ones. (See [05-networking.md] §5 — Lag compensation.) */
#define LAG_HIST_TICKS    12

typedef struct {
    float    pos_x[PART_COUNT];
    float    pos_y[PART_COUNT];
    uint64_t tick;                /* world tick this snapshot belongs to */
    bool     valid;
} BoneHistFrame;

typedef struct {
    int       id;                 /* index into world.mechs[] */
    int       chassis_id;
    int       team;
    bool      alive;
    bool      grounded;
    bool      facing_left;
    bool      is_dummy;
    uint8_t   dismember_mask;     /* LIMB_* bits set when severed */

    /* Particle/constraint range owned by this mech. */
    uint16_t  particle_base;
    uint16_t  constraint_base;
    uint16_t  constraint_count;

    /* Aim is the world-space target the cursor points at. The mech's
     * arms/torso are pulled toward it via the pose system.
     *
     * Set by mech_step_drive from latched_input.aim_x/y at the start of
     * each tick. The host's main loop converts cursor screen→world via
     * its camera and writes that into latched_input.aim_x/y; pure
     * clients do the same on their side before sending to the server. */
    Vec2      aim_world;

    /* The latest input we'll consume on the next sim tick. The server
     * fills this from received NET_MSG_INPUT packets (one per peer);
     * the local-input path (single-player or host) writes it from the
     * keyboard. simulate() reads each mech's latched_input regardless
     * of source. */
    ClientInput latched_input;
    uint16_t    prev_buttons;     /* edge-detect: which BTN_* were down last tick */

    /* Combat state. */
    float     health;
    float     health_max;
    float     fuel;
    float     fuel_max;

    /* Armor (worn body armor, separate from chassis HP). */
    int       armor_id;           /* ArmorId */
    float     armor_hp;           /* current armor capacity */
    float     armor_hp_max;
    int       armor_charges;      /* reactive armor: shots remaining */

    /* Jetpack module. */
    int       jetpack_id;         /* JetpackId */
    float     boost_timer;        /* JET_BURST: time remaining on a boost */
    bool      jump_armed;         /* JET_JUMP_JET: ready to fire another jump */

    /* Per-limb HP — when one drops to 0 the limb dismembers via
     * mech_dismember. (See documents/02-game-design.md and 03-physics.) */
    float     hp_arm_l, hp_arm_r, hp_leg_l, hp_leg_r, hp_head;

    /* Animation. We keep the index of a small built-in anim set; the
     * pose driver writes into `pose_target`/`pose_strength` each tick. */
    int       anim_id;
    float     anim_time;          /* seconds within the animation */
    Vec2      pose_target  [PART_COUNT];
    float     pose_strength[PART_COUNT];

    /* Weapon state. Each mech carries a primary + secondary; only the
     * active slot consumes BTN_FIRE. BTN_SWAP toggles between them. */
    int       primary_id;
    int       secondary_id;
    int       active_slot;        /* 0 = primary, 1 = secondary */
    int       ammo_primary;
    int       ammo_secondary;
    int       weapon_id;          /* alias for active slot's weapon (snapshot field) */
    int       ammo;               /* alias for active slot's ammo (snapshot field) */
    int       ammo_max;           /* alias for active slot's mag size */
    float     fire_cooldown;      /* seconds; counts down */
    float     reload_timer;       /* seconds; >0 = reloading */
    float     charge_timer;       /* Rail Cannon: charges before fire */
    float     spinup_timer;       /* Microgun: spin-up before sustained fire */

    /* Recoil decay (cosmetic — actual recoil is the hand impulse). */
    float     recoil_kick;

    /* Bink — angular wobble applied to the aim ray on fire. Both
     * incoming-fire bink (from `weapon.bink`) and self-bink (from
     * rapid-fire) accumulate here; decays each tick. See
     * documents/04-combat.md §"Recoil & bink". */
    float     aim_bink;           /* radians, signed */

    /* Sleep tracking for dead bodies (skips integrate when settled). */
    int       sleep_ticks;
    bool      sleeping;

    /* Engineer: cooldown on the BTN_USE repair pack. */
    float     ability_cooldown;

    /* Last time this mech took a hit (tracking for "OVERKILL", etc.). */
    float     last_damage_taken;
    int       last_killshot_weapon;

    /* Server-side: the most recent input we processed for this mech.
     * Echoed back in snapshots so the client can drop already-acked
     * inputs from its replay buffer. */
    uint16_t  last_processed_input_seq;

    /* Bone-position history for lag compensation. Filled at the end of
     * each server tick by mech_record_lag_hist; only meaningful on the
     * authoritative side (server). Indexed modulo LAG_HIST_TICKS. */
    BoneHistFrame lag_hist[LAG_HIST_TICKS];
    int           lag_hist_head;  /* next slot to write */
} Mech;

/* ---- Projectiles (SoA, M3+).
 *
 * Live ballistic projectiles: grenades, rockets, plasma orbs, pellets.
 * Hitscan weapons don't go through this pool — they ray-test on the
 * fire tick and are done. Distinct from mech particles (no constraints)
 * and from FX particles (collide with mechs and apply damage).
 *
 * Capacity covers worst-case 32 mechs each with a microgun spew + a
 * couple of rockets in the air. */
#define PROJECTILE_CAPACITY 512

typedef enum {
    PROJ_NONE = 0,
    PROJ_PLASMA_BOLT,         /* Plasma SMG */
    PROJ_PELLET,              /* Riot Cannon (6 per shot) */
    PROJ_RIFLE_SLUG,          /* Auto-Cannon */
    PROJ_ROCKET,              /* Mass Driver */
    PROJ_PLASMA_ORB,          /* Plasma Cannon */
    PROJ_MICROGUN_BULLET,
    PROJ_FRAG_GRENADE,
    PROJ_MICRO_ROCKET,
    PROJ_THROWN_KNIFE,
    PROJ_KIND_COUNT
} ProjectileKind;

typedef struct {
    float    pos_x[PROJECTILE_CAPACITY];
    float    pos_y[PROJECTILE_CAPACITY];
    float    vel_x[PROJECTILE_CAPACITY];      /* px/sec */
    float    vel_y[PROJECTILE_CAPACITY];
    float    life [PROJECTILE_CAPACITY];      /* seconds remaining (also frag-fuse) */
    float    damage[PROJECTILE_CAPACITY];     /* base damage on direct hit */
    float    aoe_radius[PROJECTILE_CAPACITY]; /* explosion radius (0 = no AOE) */
    float    aoe_damage[PROJECTILE_CAPACITY]; /* explosion base damage */
    float    aoe_impulse[PROJECTILE_CAPACITY];
    float    gravity_scale[PROJECTILE_CAPACITY];
    float    drag      [PROJECTILE_CAPACITY]; /* per-second velocity damping */
    int16_t  owner_mech[PROJECTILE_CAPACITY];
    int8_t   owner_team[PROJECTILE_CAPACITY];
    int8_t   weapon_id[PROJECTILE_CAPACITY];  /* WeaponId — for kill feed */
    uint8_t  kind  [PROJECTILE_CAPACITY];     /* ProjectileKind */
    uint8_t  alive [PROJECTILE_CAPACITY];
    uint8_t  bouncy[PROJECTILE_CAPACITY];     /* Frag grenade: bounces on tile hit */
    uint8_t  exploded[PROJECTILE_CAPACITY];   /* set when AOE has been spawned */
    int      count;
} ProjectilePool;

/* ---- Kill feed ring buffer (HUD top-right). Holds the last few kill
 * events for display + fade. */
#define KILLFEED_CAPACITY 5

typedef enum {
    KILLFLAG_HEADSHOT = 1u << 0,
    KILLFLAG_GIB      = 1u << 1,   /* >=2 limbs lost in killing blow */
    KILLFLAG_OVERKILL = 1u << 2,   /* final blow exceeded 200 damage */
    KILLFLAG_RAGDOLL  = 1u << 3,   /* killed in midair, big tumble */
    KILLFLAG_SUICIDE  = 1u << 4,
} KillFlag;

typedef struct {
    int      killer_mech_id;       /* -1 for environmental kill */
    int      victim_mech_id;
    int      weapon_id;
    uint32_t flags;                /* KillFlag bits */
    float    age;                  /* seconds since the kill */
} KillFeedEntry;

/* ---- Blood / sparks (FX particles, AoS for simplicity at M1 scale).
 * Keeping these distinct from mech particles: they don't have constraints
 * and the inner loop is simpler. */
typedef enum {
    FX_BLOOD = 0,
    FX_SPARK,
    FX_TRACER,
    FX_SMOKE,
    FX_KIND_COUNT
} FxKind;

typedef struct {
    Vec2     pos, vel;
    float    life;       /* seconds remaining */
    float    life_max;
    float    size;
    uint32_t color;      /* RGBA8 */
    uint8_t  kind;       /* FxKind */
    uint8_t  alive;
} FxParticle;

typedef struct {
    FxParticle *items;
    int count;
    int capacity;
} FxPool;

/* ---- Tile-grid level (free-poly support deferred past M1). */
typedef enum {
    TILE_EMPTY = 0,
    TILE_SOLID,
    TILE_KIND_COUNT
} TileKind;

typedef struct {
    int       width, height;       /* in tiles */
    int       tile_size;           /* px per tile */
    uint8_t  *tiles;               /* width * height kinds */
    Vec2      ambient_light;       /* unused at M1; placeholder */
    Vec2      gravity;             /* per-tick acceleration */
} Level;

/* ---- World --------------------------------------------------------- */
typedef struct World {
    ParticlePool   particles;
    ConstraintPool constraints;

    Mech     mechs[MAX_MECHS];
    int      mech_count;

    Level    level;

    FxPool         fx;
    ProjectilePool projectiles;

    /* Kill feed — small ring buffer; HUD draws the most recent entries
     * with a fade-out. */
    KillFeedEntry killfeed[KILLFEED_CAPACITY];
    int           killfeed_count;       /* total kills observed (head index = count % CAP) */

    /* Server config: friendly-fire toggle. False by default — same-team
     * damage is dropped. Tournament servers can flip this. */
    bool     friendly_fire;

    /* Camera state — written by the renderer, read by simulate for
     * effects like screen-shake decay. Storing on World means the
     * camera survives interp alpha cleanly. */
    Vec2     camera_target;
    Vec2     camera_smooth;
    float    camera_zoom;
    float    shake_intensity;

    /* Hit-pause: when > 0, the simulation skips advancing for this many
     * ticks. Used to punctuate kills. (Render keeps drawing; only the
     * world clock pauses.) */
    int      hit_pause_ticks;

    /* Most recent hit feed line (kill-feed at M1 is a single string). */
    char     last_event[64];
    float    last_event_time;     /* seconds since this event */

    /* World-local RNG, seeded by Game. */
    pcg32_t *rng;

    /* Local player handle. The mech this client predicts and renders
     * with input latency hidden. On the host, this is the host's own
     * mech; on a remote client, it's whichever mech the server
     * assigned us at handshake. -1 outside a match. */
    int      local_mech_id;
    int      dummy_mech_id;

    /* Authoritative? True on the server (simulation owns kills,
     * damage, hit detection). False on a pure client — the client
     * still runs simulate() for prediction, but its damage/death
     * decisions are overwritten by snapshot apply.
     *
     * We DON'T fire weapons or apply damage on the client even during
     * prediction: instead, the client renders a tracer locally so the
     * shot feels instant, but the actual hit is decided server-side. */
    bool     authoritative;

    /* Monotonic simulation tick. */
    uint64_t tick;
} World;

/* Convenience accessors. */
static inline Vec2 particle_pos(const ParticlePool *p, int i) {
    return (Vec2){ p->pos_x[i], p->pos_y[i] };
}

static inline void particle_set_pos(ParticlePool *p, int i, Vec2 v) {
    p->pos_x[i] = v.x; p->pos_y[i] = v.y;
}

static inline void particle_set_prev(ParticlePool *p, int i, Vec2 v) {
    p->prev_x[i] = v.x; p->prev_y[i] = v.y;
}
