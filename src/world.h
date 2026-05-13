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
    PARTICLE_FLAG_CEILING  = 1u << 3,   /* set when contact normal points downward (ny > 0.5) */
} ParticleFlags;

typedef struct {
    float    *pos_x, *pos_y;
    float    *prev_x, *prev_y;
    float    *inv_mass;
    uint8_t  *flags;
    /* Per-particle most-recent contact data (P02). The contact resolver
     * writes these on every contact; the gravity pass zeros them at the
     * start of each tick. Used by slope-aware run velocity + jet thrust
     * + post-physics anchor to decide whether the foot is on a flat
     * floor or a slope, and which way the slope tangent goes.
     *
     * Q1.7: a value of 127 maps to +1.0; -128 maps to -1.0. */
    int8_t   *contact_nx_q;
    int8_t   *contact_ny_q;
    uint8_t  *contact_kind;     /* TILE_F_* bitmask of the touched surface */
    /* Render-side prev-frame snapshot (P03). pos_x/pos_y at the start
     * of the most-recent simulate_step. The renderer lerps between
     * render_prev and pos by `alpha = accum / TICK_DT` so motion stays
     * smooth when render rate exceeds sim rate (vsync-fast play on a
     * 144 Hz display). NOT to be confused with prev_x/prev_y, which is
     * Verlet's previous-position scratch (`pos - prev = velocity`).
     * Verlet `prev` updates inside the integrator; `render_prev` only
     * updates once per simulate tick. */
    float    *render_prev_x;
    float    *render_prev_y;
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
    CSTR_FIXED_ANCHOR,      /* P06: end b is a fixed Vec2 stored inline */
} ConstraintKind;

typedef struct {
    uint16_t a, b;          /* particle indices */
    uint16_t c;             /* angle: middle joint */
    uint8_t  kind;          /* ConstraintKind */
    uint8_t  active;        /* false → dismembered/detached */
    float    rest;          /* DISTANCE / FIXED_ANCHOR */
    float    min_len, max_len;  /* DISTANCE_LIMIT */
    float    min_ang, max_ang;  /* ANGLE (radians) */
    Vec2     fixed_pos;     /* FIXED_ANCHOR: world-space anchor for end b */
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

/* M6 P02 — Visual jet state flags. Source of truth for the
 * `mech_jet_fx_step` driver in src/mech_jet_fx.c. Set each tick by
 * mech_step_drive for owner mechs (server-side, or the local mech on a
 * client) and by snapshot_apply for remote mechs on the client. The FX
 * step reads these as a pure consumer — it never writes them. */
enum {
    MECH_JET_ACTIVE        = 1u << 0,   /* this tick is producing thrust (or jump impulse) */
    MECH_JET_IGNITION_TICK = 1u << 1,   /* first ACTIVE tick after grounded → airborne */
    MECH_JET_BOOSTING      = 1u << 2,   /* JET_BURST mid-boost (boost_timer > 0) */
};
static inline int limb_count(uint8_t mask) {
    int n = 0;
    while (mask) { n += (int)(mask & 1u); mask >>= 1; }
    return n;
}

/* ---- Grappling hook (M5 P06) ---------------------------------------
 *
 * Per-mech state for the secondary-slot grappling hook. Spec lives in
 * `documents/m5/05-grapple.md`. Fire path spawns a PROJ_GRAPPLE_HEAD;
 * on tile/bone hit the head sticks, anchor is stored, and a contracting
 * distance constraint pulls the firer toward the anchor. Released on
 * BTN_USE edge, anchor-mech death, or owner death.
 *
 * Constraint slot:
 *   - tile anchor → CSTR_FIXED_ANCHOR with fixed_pos = anchor_pos
 *   - bone anchor → CSTR_DISTANCE between firer pelvis and target bone
 *
 * Allocated lazily from the global ConstraintPool on attach; deactivated
 * (active=0) on release. Slots leak by ~40 per round per mech (bounded
 * vs. the 2048-slot pool); reset by round-end. */
typedef enum {
    GRAPPLE_IDLE     = 0,
    GRAPPLE_FLYING   = 1,    /* head in flight */
    GRAPPLE_ATTACHED = 2,    /* anchored, contracting */
} GrappleState;

typedef struct {
    uint8_t  state;          /* GrappleState */
    int8_t   anchor_mech;    /* -1 = tile-anchored; else mech_id of target */
    uint8_t  anchor_part;    /* PART_* if anchor_mech >= 0; ignored otherwise */
    uint8_t  reserved;
    Vec2     anchor_pos;     /* world-space anchor (tile case authoritative) */
    float    rest_length;    /* contracts each tick, clamped to >= 80 px */
    int      constraint_idx; /* index into world.constraints; -1 if none */
} Grapple;

/* ---- Mech ----------------------------------------------------------- */
#define MAX_MECHS         32
/* M6 P02 — bumped 3000 → 8000 to cover worst-case Burst-jet plume
 * spew: 16 mechs × 2 nozzles × 8 particles/tick × 60 Hz × ~0.5 s
 * average life = ~7680 live FX particles peak. ~384 KB permanent at
 * the new sizeof(FxParticle) (~48 B with the M6 P02 color_cool field).
 * Inside the 256 MB resident budget by a wide margin. */
#define MAX_BLOOD         8000

/* P12 — Persistent damage decals composited over each visible mech
 * part. Decals are stored in sprite-local space (i8 px relative to the
 * bone-segment midpoint, unrotated) so they migrate naturally with the
 * limb sprite each frame. The kind picks the placeholder color until
 * P13 ships authored decal sub-rects in the HUD atlas.
 *
 * MECH_LIMB_DECAL_COUNT must match `MSP_COUNT` in mech_sprites.h
 * (asserted there at compile time). World.h can't include
 * mech_sprites.h without an include cycle, so the constant is mirrored
 * here.
 *
 * Sprite-local i8 coords cap at ±127 px, comfortably inside our largest
 * sprite (96 px tall leg upper) — see TRADE_OFFS.md "Decal records use
 * sprite-local int8 coords" for the revisit trigger. */
#define MECH_LIMB_DECAL_COUNT   22
#define DAMAGE_DECALS_PER_LIMB  16

typedef enum {
    DAMAGE_DECAL_DENT   = 0,    /* light damage (< 30 dmg) */
    DAMAGE_DECAL_SCORCH = 1,    /* moderate / explosion damage */
    DAMAGE_DECAL_GOUGE  = 2,    /* heavy damage (>= 80 dmg) */
} DamageDecalKind;

typedef struct {
    int8_t   local_x, local_y;  /* sprite-local px, midpoint-relative, unrotated */
    uint8_t  kind;              /* DamageDecalKind */
    uint8_t  reserved;
} MechDamageDecal;

typedef struct {
    MechDamageDecal items[DAMAGE_DECALS_PER_LIMB];
    uint8_t         count;      /* >= DAMAGE_DECALS_PER_LIMB → ring overwrite via modulo */
    uint8_t         pad[3];
} MechLimbDecals;

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

/* P03 — client-side snapshot ring per remote mech. The client renders
 * remote mechs ~100 ms in the past, lerping between two snapshots that
 * bracket the render clock. A snapshot only carries pelvis pos + vel;
 * limb shape is maintained by the local constraint solver as it
 * applies the same translation to every particle.
 *
 * The ring is per-mech so that one peer's snapshot drop doesn't affect
 * another.
 *
 * Sizing: at 30 Hz snapshots (33 ms interval) the ring must span >
 * NET_INTERP_DELAY_MS (100 ms) — otherwise render_time always falls
 * before the oldest entry, clamps to oldest, and motion jumps once per
 * snapshot push instead of interpolating smoothly. 8 entries spans
 * ~231 ms which gives the 100 ms interp delay comfortable headroom
 * plus tolerance for a few packet drops. */
#define REMOTE_SNAP_RING 8
typedef struct {
    uint32_t server_time_ms;
    float    pelvis_x, pelvis_y;
    float    vel_x, vel_y;
    bool     valid;
} RemoteSnapBuf;

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

    /* wan-fixes-3 followup — air-time hysteresis for anim_id.
     * Increments each tick that !grounded, resets to 0 when grounded.
     * The walk cycle (ANIM_RUN) lifts each foot off the ground for
     * ~3 ticks per swing phase. Without hysteresis, anim_id flickers
     * RUN → FALL → STAND → RUN every snapshot — invisible on the
     * server (constraint solver smooths over it) but visibly jittery
     * on the client where wan-fixes-3 made remote mechs kinematic
     * (no constraint smoothing). We require `air_ticks > GAIT_LIFT_MAX`
     * before transitioning to FALL, so brief gait lifts stay
     * classified as RUN / STAND. Authoritative side only; clients
     * mirror by deriving from snapshot velocity instead of
     * grounded-bit flicker. */
    uint8_t   air_ticks;

    /* P14 — gait phase tracking for footstep audio. Updated by
     * build_pose's ANIM_RUN case each tick; the wrap from >0.5 → <0.5
     * is the swing→stance transition that fires SFX_FOOTSTEP_*. Both
     * fields stay at 0 outside ANIM_RUN, which suppresses spurious
     * footsteps on stand→run transitions (the comparison reads the
     * post-RUN value vs the new RUN value continuously). */
    float     gait_phase_l;
    float     gait_phase_r;
    /* P14 — rate-limit the jet pulse SFX. apply_jet_force fires a
     * pulse every JET_PULSE_INTERVAL_TICKS so a held-jet doesn't
     * machine-gun the cue. Compared against world.tick. */
    uint64_t  last_jet_pulse_tick;

    /* M6 P02 — visual jet state. `jet_state_bits` is MECH_JET_* flags
     * (set by mech_step_drive on the owner side, by snapshot_apply for
     * remote mechs on a client); `jet_prev_grounded` is last tick's
     * `grounded` value, used to detect the grounded→airborne edge that
     * fires the ignition burst + SFX_JET_IGNITION_* cue. */
    uint8_t   jet_state_bits;
    uint8_t   jet_prev_grounded;

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

    /* P11 — last-fired slot tracking (purely render-side). When the
     * inactive slot fires via BTN_FIRE_SECONDARY (RMB), the renderer
     * draws the OTHER slot's weapon at R_HAND for a short window so
     * the player sees the throw/shot the secondary just produced.
     * After the window expires the active slot's weapon is drawn
     * again. `last_fired_slot = -1` sentinel means "never fired" so
     * the very first ticks don't flicker on a fresh mech. */
    int8_t    last_fired_slot;
    uint64_t  last_fired_tick;

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

    /* P05 — Powerup timers (seconds remaining; > 0 = active). Server-side
     * truth; clients mirror via SNAP_STATE_BERSERK / INVIS / GODMODE bits
     * on EntitySnapshot, setting the timer to a sentinel value when the
     * bit is observed and zeroing when it clears. mech_step_drive ticks
     * down each tick on the authoritative side. */
    float     powerup_berserk_remaining;
    float     powerup_invis_remaining;
    float     powerup_godmode_remaining;

    /* P05 — Burst SMG cadence: WFIRE_BURST queues `burst_pending_rounds`
     * additional rounds that fire one-per-`burst_pending_timer` until
     * exhausted. The first round still lands on the trigger-press tick;
     * the trailing rounds land on subsequent ticks at `burst_interval_sec`
     * cadence (70 ms for the Burst SMG). Replaces M3's "all 3 rounds on
     * one tick" stopgap. */
    uint8_t   burst_pending_rounds;
    float     burst_pending_timer;

    /* P06 — Grappling hook state. See `documents/m5/05-grapple.md`.
     * Server-authoritative; clients mirror via SNAP_STATE_GRAPPLING +
     * the trailing 8-byte grapple wire suffix on the EntitySnapshot.
     * On the server, the constraint at `grapple.constraint_idx` is the
     * thing the solver uses to pull the firer's pelvis to the anchor;
     * on remote clients the field is rendered only (no constraint). */
    Grapple   grapple;

    /* Last time this mech took a hit (tracking for "OVERKILL", etc.). */
    float     last_damage_taken;
    int       last_killshot_weapon;

    /* P12 — Hit-flash timer. Set to ~0.10 s at every successful damage
     * application; decayed each tick in simulate. The renderer reads
     * `hit_flash_timer / 0.10f` as a 0..1 white-additive blend over the
     * body tint. Whole-mech granularity (per the M5 spec); per-limb is
     * a polish item. Distinct from `last_damage_taken` — that field
     * stays as the killing-blow amount for `KILLFLAG_OVERKILL`. */
    float     hit_flash_timer;

    /* P12 — Persistent damage decals, one ring per visible mech part
     * (indexed by `MechSpriteId`). The first MECH_RENDER_PART_COUNT
     * slots are the "real" parts; stump-cap slots are unused but kept
     * so the index space matches `MSP_COUNT`. Cleared by
     * `mech_clear_damage_decals` on respawn. */
    MechLimbDecals damage_decals[MECH_LIMB_DECAL_COUNT];

    /* Server-side: the most recent input we processed for this mech.
     * Echoed back in snapshots so the client can drop already-acked
     * inputs from its replay buffer. */
    uint16_t  last_processed_input_seq;

    /* Server-side, set by net.c::server_handle_input on every remote
     * peer input: the world tick the firing client was *seeing* when
     * the input was generated, computed as
     *   `w->tick - rtt_half_ticks - INTERP_DELAY_TICKS`.
     * mech_try_fire reads this on HITSCAN to route through
     * `weapons_fire_hitscan_lag_comp` instead of the current-time path,
     * so client shots at moving remote targets test against the bones
     * the shooter saw. Cleared (0) after each try_fire; stays 0 for the
     * host's own mech (local input never flows through server_handle_input)
     * and for AI / dummy mechs. See [05-networking.md] §5 — Lag comp. */
    uint64_t  input_view_tick;

    /* Bone-position history for lag compensation. Filled at the end of
     * each server tick by mech_record_lag_hist; only meaningful on the
     * authoritative side (server). Indexed modulo LAG_HIST_TICKS. */
    BoneHistFrame lag_hist[LAG_HIST_TICKS];
    int           lag_hist_head;  /* next slot to write */

    /* P03 — client-side snapshot interp ring for REMOTE mechs.
     * snapshot_apply pushes here instead of directly translating
     * particles; snapshot_interp_remotes lerps between bracketing
     * entries and writes the interpolated pelvis to the particle
     * pool each tick. Unused on the server / for the local mech. */
    RemoteSnapBuf remote_snap_ring[REMOTE_SNAP_RING];
    int           remote_snap_head;
    int           remote_snap_count;   /* up to REMOTE_SNAP_RING */
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
    PROJ_GRAPPLE_HEAD,        /* P06: hook head, no damage, sticks on hit */
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
    /* Render-side prev-frame snapshot (P03) — pos at the start of the
     * most-recent simulate_step, lerped against pos by render alpha. */
    float    render_prev_x[PROJECTILE_CAPACITY];
    float    render_prev_y[PROJECTILE_CAPACITY];
    int16_t  owner_mech[PROJECTILE_CAPACITY];
    int8_t   owner_team[PROJECTILE_CAPACITY];
    int8_t   weapon_id[PROJECTILE_CAPACITY];  /* WeaponId — for kill feed */
    uint8_t  kind  [PROJECTILE_CAPACITY];     /* ProjectileKind */
    uint8_t  alive [PROJECTILE_CAPACITY];
    uint8_t  bouncy[PROJECTILE_CAPACITY];     /* Frag grenade: bounces on tile hit */
    uint8_t  exploded[PROJECTILE_CAPACITY];   /* set when AOE has been spawned */
    int      count;
} ProjectilePool;

/* ---- Hit feed: server-side queue of damage events broadcast to
 * clients so they can spawn blood / sparks at the actual hit pos
 * with the actual hit direction (rather than the snapshot-apply
 * fallback that uses chest pos + facing-derived spray, which reads
 * visibly different from the server view).
 *
 * Capacity 64 covers worst-case bursts (32 mechs × 2 hits/tick); main.c
 * drains the queue every tick after simulate, so it should rarely
 * approach the cap. monotonic counter pattern matches killfeed. */
#define HITFEED_CAPACITY 64

typedef struct {
    int16_t  victim_mech_id;
    uint8_t  hit_part;          /* PART_* */
    uint8_t  damage;            /* clamped to 255 */
    float    pos_x, pos_y;      /* world-space hit point */
    float    dir_x, dir_y;      /* normalized hit direction */
} HitFeedEntry;

/* ---- Fire feed: server-side queue of fire events broadcast to
 * clients so they can spawn matching tracers (hitscan) and visual-
 * only projectiles (everything else). Without this, clients see
 * NOTHING when a remote player fires — only the local shooter's
 * predict path puts a tracer/projectile in front of them.
 *
 * One entry per shot (spread weapons fire one entry per pellet;
 * burst weapons fire one entry per shell). Capacity 128 covers
 * worst case (microgun spew + multiple shooters per tick).
 *
 * Client-side: events where `shooter_mech_id == local_mech_id` are
 * dropped because the local predict already spawned the visual. */
#define FIREFEED_CAPACITY 128

typedef struct {
    int16_t  shooter_mech_id;
    uint8_t  weapon_id;
    uint8_t  reserved;
    float    origin_x, origin_y;     /* muzzle, world-space px */
    float    dir_x, dir_y;           /* normalized fire direction */
} FireFeedEntry;

/* ---- Explosion feed: server-side queue of AOE detonations broadcast
 * to clients via NET_MSG_EXPLOSION so the visual explosion lands at
 * the SERVER's authoritative position rather than wherever the
 * client's visual-only projectile happened to detonate locally
 * (wan-fixes-10).
 *
 * Background: wan-fixes-3 makes remote mechs render in REST POSE on
 * the client (`inv_mass = 0`, snapshot_interp_remotes rigid-translates
 * the body but bones don't animate). The server runs animated poses
 * (idle wobble, walk, aim-arm), so the bone hit-volume positions
 * differ between server and client by up to ~10 px. For a bouncy
 * frag grenade rolling along the ground, this offset means the
 * server's grenade detonates against the target's animated R_KNEE
 * 5–10 px before the client's visual grenade detonates against the
 * same target's rest-pose R_KNEE. Result: damage applies at one
 * place, visual fires at another — the user sees the grenade
 * explode "near" them but takes damage as if it exploded "a bit to
 * the left."
 *
 * Fix: server records each detonation here; main.c broadcasts;
 * client kills its visual projectile and spawns the explosion visual
 * at the server's authoritative position. Damage stays
 * server-authoritative as before. Capacity 32 covers worst-case
 * tick bursts (multiple grenades + rockets airborne, several
 * detonate in the same tick). */
#define EXPLOSIONFEED_CAPACITY 32

typedef struct {
    int16_t  owner_mech_id;
    uint8_t  weapon_id;
    uint8_t  reserved;
    float    pos_x, pos_y;          /* authoritative explosion center */
} ExplosionFeedEntry;

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

/* wan-fixes-13 — name strings live IN the killfeed entry so the HUD
 * doesn't need to thread Lobby through hud_draw's signature, and so
 * the entry survives the slot getting freed (player leaves mid-
 * round). Server populates these in apply_new_kills via
 * lobby_find_slot_by_mech right before broadcast; client populates
 * them by decoding the wire payload in client_handle_kill_event. */
#define KILLFEED_NAME_BYTES 16

typedef struct {
    int      killer_mech_id;       /* -1 for environmental kill */
    int      victim_mech_id;
    int      weapon_id;
    uint32_t flags;                /* KillFlag bits */
    float    age;                  /* seconds since the kill */
    char     killer_name[KILLFEED_NAME_BYTES];
    char     victim_name[KILLFEED_NAME_BYTES];
} KillFeedEntry;

/* ---- Blood / sparks (FX particles, AoS for simplicity at M1 scale).
 * Keeping these distinct from mech particles: they don't have constraints
 * and the inner loop is simpler. */
typedef enum {
    FX_BLOOD = 0,
    FX_SPARK,
    FX_TRACER,
    FX_SMOKE,
    FX_STUMP,            /* P12: pinned dismemberment emitter — pin_mech_id / pin_limb track the parent particle each tick */
    FX_JET_EXHAUST,      /* M6 P02: additive, color lerps color_hot → color_cool over life */
    FX_GROUND_DUST,      /* M6 P02: alpha-blended, heavy drag, brief upward lift */
    FX_KIND_COUNT
} FxKind;

typedef struct {
    Vec2     pos, vel;
    /* Render-side prev-frame snapshot (P03) — pos at the start of the
     * most-recent fx_update, lerped against pos by render alpha. */
    Vec2     render_prev_pos;
    float    life;       /* seconds remaining */
    float    life_max;
    float    size;
    uint32_t color;      /* RGBA8 — "hot" / start color for kinds that lerp */
    /* M6 P02 — End-of-life color for kinds that lerp hot→cool over
     * `1 - life/life_max`. Only read by FX_JET_EXHAUST and
     * FX_GROUND_DUST today; other kinds leave this at zero (their
     * draw path doesn't consult it). Adds 4 bytes per particle. */
    uint32_t color_cool;
    uint8_t  kind;       /* FxKind */
    uint8_t  alive;
    /* P12 — FX_STUMP only: pinned-emitter parent. The emitter has no
     * own integrate path; each tick it spawns 1–2 blood particles at
     * the parent particle position so the trail tracks the still-moving
     * torso (not the tumbling severed limb). Self-deactivates when
     * pin_mech_id is invalid or the body's particle base disappears. */
    int16_t  pin_mech_id;
    uint8_t  pin_limb;   /* LIMB_* bit (HEAD/L_ARM/R_ARM/L_LEG/R_LEG) */
    uint8_t  pin_pad;
} FxParticle;

typedef struct {
    FxParticle *items;
    int count;
    int capacity;
} FxPool;

/* ---- Pickups (M5 P05) -----------------------------------------------
 *
 * Map-driven equipment-swap layer. Per `documents/m5/04-pickups.md`, the
 * spawner is the persistent record (lives in the level); the runtime
 * keeps one transient `PickupSpawner` entry per spawner that flips
 * between AVAILABLE and COOLDOWN.
 *
 * The level format's `LvlPickup.category` stores the kind directly
 * (PickupKind values are stable in `documents/m5/01-lvl-format.md`).
 * Engineer-deployed repair packs and dynamic spawns reuse the same
 * pool with the TRANSIENT flag set so they auto-remove on grab/expire
 * instead of cycling through COOLDOWN. */
typedef enum {
    PICKUP_HEALTH = 0,
    PICKUP_AMMO_PRIMARY,
    PICKUP_AMMO_SECONDARY,
    PICKUP_ARMOR,
    PICKUP_WEAPON,
    PICKUP_POWERUP,
    PICKUP_JET_FUEL,
    PICKUP_REPAIR_PACK,
    PICKUP_PRACTICE_DUMMY,
    PICKUP_KIND_COUNT
} PickupKind;

typedef enum {
    HEALTH_SMALL  = 0,    /* +25 */
    HEALTH_MEDIUM = 1,    /* +60 */
    HEALTH_LARGE  = 2,    /* +full */
} HealthVariant;

typedef enum {
    POWERUP_BERSERK      = 0,    /* 2× outgoing damage, 15 s */
    POWERUP_INVISIBILITY = 1,    /* alpha 0.2, 8 s */
    POWERUP_GODMODE      = 2,    /* incoming damage zeroed, 5 s */
} PowerupVariant;

typedef enum {
    PICKUP_STATE_AVAILABLE = 0,
    PICKUP_STATE_COOLDOWN  = 1,
} PickupState;

enum {
    /* Engineer pack / dynamic spawns. Auto-removed on grab or when
     * `tick >= available_at_tick` (used as a lifetime expiry rather
     * than a respawn timer). */
    PICKUP_FLAG_TRANSIENT = 1u << 8,
};

typedef struct PickupSpawner {
    Vec2     pos;
    uint8_t  kind;                  /* PickupKind */
    uint8_t  variant;               /* HealthVariant / PowerupVariant / WeaponId / ArmorId */
    uint16_t respawn_ms;            /* 0 = use kind default */
    uint8_t  state;                 /* PickupState */
    uint8_t  reserved;
    uint64_t available_at_tick;     /* COOLDOWN: when AVAILABLE returns. TRANSIENT: lifetime expiry. */
    uint16_t flags;                 /* CONTESTED / RARE / HOST_ONLY / TRANSIENT (bit 8) */
} PickupSpawner;

#define PICKUP_CAPACITY 64

typedef struct PickupPool {
    PickupSpawner items[PICKUP_CAPACITY];
    int           count;
} PickupPool;

/* ---- Flags (CTF, M5 P07) -------------------------------------------
 *
 * One per team in CTF mode. Populated at round start from `level.flags`
 * (LvlFlag records); zero entries when match.mode != CTF. The runtime
 * record is server-authoritative and replicated to clients via
 * NET_MSG_FLAG_STATE on every state transition (and inside
 * INITIAL_STATE for joining clients). See `documents/m5/06-ctf.md`. */
typedef enum {
    FLAG_HOME    = 0,
    FLAG_CARRIED = 1,
    FLAG_DROPPED = 2,
} FlagStatus;

typedef struct Flag {
    Vec2     home_pos;
    uint8_t  team;                  /* MATCH_TEAM_RED or _BLUE */
    uint8_t  status;                /* FlagStatus */
    int8_t   carrier_mech;          /* mech id when CARRIED; else -1 */
    uint8_t  reserved;
    Vec2     dropped_pos;           /* meaningful when DROPPED */
    uint64_t return_at_tick;        /* DROPPED: world.tick when auto-return fires */
} Flag;

#define FLAG_TOUCH_RADIUS_PX     36.0f
#define FLAG_AUTO_RETURN_TICKS   (30u * 60u)   /* 30 s @ 60 Hz */
#define FLAG_CAPTURE_DEFAULT     5             /* score_limit default for CTF */

/* ---- Tile / level data ---------------------------------------------
 *
 * The on-disk format lives in [documents/m5/01-lvl-format.md]. The Lvl*
 * record types below are the in-memory representation of those records;
 * level_io.{c,h} reads/writes them as packed little-endian byte
 * sequences. The byte layouts MUST match the spec — the static_asserts
 * at the bottom of this block are the wire-format guarantee.
 */

/* TileKind: legacy enum kept so existing callers (physics, render) can
 * keep doing `level_tile_at(...) == TILE_SOLID`. The authoritative bit
 * is `LvlTile.flags & TILE_F_SOLID` — `level_tile_at()` is now a thin
 * adapter over the flag check. */
typedef enum {
    TILE_EMPTY = 0,
    TILE_SOLID,
    TILE_KIND_COUNT
} TileKind;

/* TILE_F_*: the bit-flag vocabulary that lives in `LvlTile.flags`.
 * Matches the design canon in 01-lvl-format.md §TILE. The new
 * collision behaviors (ICE/DEADLY/ONE_WAY/BACKGROUND) are part of P02;
 * the format reserves them so v1 .lvl files round-trip them. */
enum {
    TILE_F_EMPTY      = 0,
    TILE_F_SOLID      = 1u << 0,
    TILE_F_ICE        = 1u << 1,
    TILE_F_DEADLY     = 1u << 2,
    TILE_F_ONE_WAY    = 1u << 3,
    TILE_F_BACKGROUND = 1u << 4,
};

/* Polygon kinds — small enum stored in LvlPoly.kind. Same vocabulary as
 * tiles but expressed as values, not bits, since a polygon has exactly
 * one kind. */
typedef enum {
    POLY_KIND_SOLID      = 0,
    POLY_KIND_ICE        = 1,
    POLY_KIND_DEADLY     = 2,
    POLY_KIND_ONE_WAY    = 3,
    POLY_KIND_BACKGROUND = 4,
} PolyKind;

/* Ambient zone kinds — small enum stored in LvlAmbi.kind. WIND nudges
 * particles per tick; ZERO_G zeros the gravity contribution inside the
 * rect; ACID applies 5 HP/s damage; FOG is render-only. */
typedef enum {
    AMBI_WIND   = 0,
    AMBI_ZERO_G = 1,
    AMBI_ACID   = 2,
    AMBI_FOG    = 3,
} AmbiKind;

typedef struct {
    uint16_t id;                   /* sprite atlas index */
    uint16_t flags;                /* TILE_F_* bitmask */
} LvlTile;

/* Free polygon — already triangulated by the editor at save time. The
 * runtime never re-triangulates. P02 fills the broadphase grid; the
 * record itself round-trips at P01. */
typedef struct {
    int16_t  v_x[3];               /* triangle vertices, world-space px */
    int16_t  v_y[3];
    int16_t  normal_x[3];          /* edge normals, fixed-point Q1.15 */
    int16_t  normal_y[3];
    uint16_t kind;                 /* SOLID/ICE/DEADLY/ONE_WAY/BACKGROUND */
    uint16_t group_id;             /* destructible group (reserved at v1) */
    int16_t  bounce_q;             /* restitution Q0.16 */
    uint16_t reserved;
} LvlPoly;

typedef struct {
    int16_t pos_x;
    int16_t pos_y;
    uint8_t team;                  /* 0=any, 1=red, 2=blue */
    uint8_t flags;                 /* PRIMARY=1, FALLBACK=2 */
    uint8_t lane_hint;             /* designer's lane order */
    uint8_t reserved;
} LvlSpawn;

typedef struct {
    int16_t  pos_x;
    int16_t  pos_y;
    uint8_t  category;             /* HEALTH/AMMO/ARMOR/WEAPON/POWERUP/JET_FUEL */
    uint8_t  variant;              /* small/med/large or weapon_id */
    uint16_t respawn_ms;           /* 0 = use category default */
    uint16_t flags;                /* CONTESTED/RARE/HOST_ONLY */
    uint16_t reserved;
} LvlPickup;

typedef struct {
    int16_t  pos_x;
    int16_t  pos_y;
    int16_t  scale_q;              /* Q1.15 */
    int16_t  rot_q;                /* 1/256 turns */
    uint16_t sprite_str_idx;       /* offset into STRT */
    uint8_t  layer;                /* 0..3 parallax depth */
    uint8_t  flags;                /* FLIPPED_X, ADDITIVE */
} LvlDeco;

typedef struct {
    int16_t  rect_x, rect_y;       /* top-left, world-space */
    int16_t  rect_w, rect_h;
    uint16_t kind;                 /* WIND/ZERO_G/ACID/FOG */
    int16_t  strength_q;           /* Q1.15, kind-dependent */
    int16_t  dir_x_q;              /* Q1.15 unit vector */
    int16_t  dir_y_q;
} LvlAmbi;

typedef struct {
    int16_t  pos_x;
    int16_t  pos_y;
    uint8_t  team;                 /* 1=red, 2=blue */
    uint8_t  reserved[3];
} LvlFlag;

typedef struct {
    uint16_t name_str_idx;
    uint16_t blurb_str_idx;
    uint16_t background_str_idx;
    uint16_t music_str_idx;
    uint16_t ambient_loop_str_idx;
    uint16_t reverb_amount_q;      /* Q0.16 */
    uint16_t mode_mask;            /* FFA=1, TDM=2, CTF=4 */
    uint16_t reserved[9];
} LvlMeta;

/* Wire-format guarantees. If any of these fail the .lvl format breaks. */
_Static_assert(sizeof(LvlTile)   ==  4, "LvlTile must be 4 bytes");
_Static_assert(sizeof(LvlPoly)   == 32, "LvlPoly must be 32 bytes");
_Static_assert(sizeof(LvlSpawn)  ==  8, "LvlSpawn must be 8 bytes");
_Static_assert(sizeof(LvlPickup) == 12, "LvlPickup must be 12 bytes");
_Static_assert(sizeof(LvlDeco)   == 12, "LvlDeco must be 12 bytes");
_Static_assert(sizeof(LvlAmbi)   == 16, "LvlAmbi must be 16 bytes");
_Static_assert(sizeof(LvlFlag)   ==  8, "LvlFlag must be 8 bytes");
_Static_assert(sizeof(LvlMeta)   == 32, "LvlMeta must be 32 bytes");

typedef struct Level {
    int       width, height;       /* in tiles */
    int       tile_size;           /* px per tile */
    LvlTile  *tiles;               /* width * height records */

    /* Free polygons + section arrays loaded from .lvl. The pointers
     * are owned by the level arena; on level reload the arena resets
     * and these zero out together. P02 populates poly_grid; P05/P06/P07
     * populate the rest. */
    LvlPoly   *polys;
    int        poly_count;
    LvlSpawn  *spawns;
    int        spawn_count;
    LvlPickup *pickups;
    int        pickup_count;
    LvlDeco   *decos;
    int        deco_count;
    LvlAmbi   *ambis;
    int        ambi_count;
    LvlFlag   *flags;
    int        flag_count;
    LvlMeta    meta;

    /* String table — a blob of zero-terminated UTF-8 owned by the level
     * arena. LvlMeta and LvlDeco sprite_str_idx index into this by
     * byte offset (offset 0 is reserved as "empty string"). */
    const char *string_table;
    int         string_table_size;

    /* Polygon broadphase index — populated in P02. `poly_grid` is a
     * flat list of polygon ids per cell; `poly_grid_off[cell + 1]
     * - poly_grid_off[cell]` is the count for cell #cell. Both are
     * NULL until P02 lands. */
    int       *poly_grid;
    int       *poly_grid_off;

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
    PickupPool     pickups;

    /* Kill feed — small ring buffer; HUD draws the most recent entries
     * with a fade-out. */
    KillFeedEntry killfeed[KILLFEED_CAPACITY];
    int           killfeed_count;       /* total kills observed (head index = count % CAP) */

    /* Hit feed — server-side queue of per-hit info that main.c drains
     * each tick and broadcasts via NET_MSG_HIT_EVENT so clients can
     * mirror the server's blood/spark FX. Monotonic counter pattern. */
    HitFeedEntry hitfeed[HITFEED_CAPACITY];
    int          hitfeed_count;

    /* Fire feed — server-side queue of per-shot info that main.c
     * drains and broadcasts via NET_MSG_FIRE_EVENT so clients can
     * mirror the muzzle flash + tracer + projectile spawn for every
     * remote player's fire. */
    FireFeedEntry firefeed[FIREFEED_CAPACITY];
    int           firefeed_count;

    /* Explosion feed — server-side queue of AOE detonations (frag
     * grenade, rocket, plasma orb, mass driver dud) broadcast to
     * clients so the explosion visual lands at the SERVER's
     * authoritative pos rather than the client's locally-detonated
     * pos (wan-fixes-10). Same drain-each-tick pattern. */
    ExplosionFeedEntry explosionfeed[EXPLOSIONFEED_CAPACITY];
    int                explosionfeed_count;

    /* Pickup-event feed (P05). Server-side queue of spawner indices
     * that changed state this tick (touched / respawned / transient
     * created). main.c drains and ships NET_MSG_PICKUP_STATE for each
     * entry so clients update their local pool. Same monotonic-counter
     * pattern as HitFeed / FireFeed; capacity sized for worst case
     * (everyone scrambles for a respawn). */
#define PICKUPFEED_CAPACITY 64
    int          pickupfeed[PICKUPFEED_CAPACITY];
    int          pickupfeed_count;

    /* CTF flag entities (P07). Populated at round start when
     * match.mode == CTF, zeroed out otherwise. flags[0]=RED, flags[1]=BLUE.
     *
     * `flag_state_dirty` is set by ctf operations (pickup/capture/return/
     * drop-on-death/auto-return) and cleared by main.c after broadcasting
     * NET_MSG_FLAG_STATE. Same pattern as `lobby.dirty` — keeps mutation
     * sites decoupled from the network layer. */
    Flag         flags[2];
    int          flag_count;        /* 0 outside CTF; 2 in a valid CTF round */
    bool         flag_state_dirty;  /* set by any ctf transition; main.c clears on broadcast */

    /* Server config: friendly-fire toggle. False by default — same-team
     * damage is dropped. Tournament servers can flip this. */
    bool     friendly_fire;

    /* Cache of match.mode (P07). World-side mirror so mech.c (which
     * doesn't see Game) can branch on mode without an extra parameter
     * threading through every kill / damage / fire path. Mirrored from
     * MatchState by the host's start_round and the client's round-start
     * handler. 0 (MATCH_MODE_FFA) until set. */
    int      match_mode_cached;

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
