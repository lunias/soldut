#pragma once

#include "world.h"

/*
 * Mech — a single chassis carrying the 16-particle skeleton, ~21
 * constraints, an animation timeline, and a loadout. M3 ships all five
 * chassis variants and the full weapon/armor/jet table.
 *
 * The skeleton matches [03-physics-and-mechs.md]:
 *
 *      [HEAD]──neck──[CHEST]──spine──[PELVIS]
 *                    /     \         /     \
 *               [L_SHO]  [R_SHO]  [L_HIP] [R_HIP]
 *                  ↓       ↓        ↓       ↓
 *               [L_ELB]  [R_ELB]  [L_KNE] [R_KNE]
 *                  ↓       ↓        ↓       ↓
 *               [L_HND]  [R_HND]  [L_FOT] [R_FOT]
 */

typedef enum {
    CHASSIS_TROOPER = 0,
    CHASSIS_SCOUT,
    CHASSIS_HEAVY,
    CHASSIS_SNIPER,
    CHASSIS_ENGINEER,
    CHASSIS_COUNT
} ChassisId;

typedef enum {
    PASSIVE_NONE = 0,
    PASSIVE_TROOPER_FAST_RELOAD,    /* -25% reload time */
    PASSIVE_SCOUT_DASH,             /* BTN_DASH triggers a horizontal burst */
    PASSIVE_HEAVY_AOE_RESIST,       /* -10% incoming explosion damage */
    PASSIVE_SNIPER_STEADY,          /* lower bink/spread when crouched (M3: grounded+still) */
    PASSIVE_ENGINEER_REPAIR,        /* BTN_USE drops a 50-HP repair pack on cooldown */
} ChassisPassive;

typedef struct Chassis {
    const char *name;
    float run_mult;
    float jump_mult;
    float jet_mult;
    float fuel_max;
    float fuel_regen;
    float mass_scale;
    float health_max;
    float bone_arm;       /* upper-arm length in px */
    float bone_forearm;
    float bone_thigh;
    float bone_shin;
    float torso_h;        /* chest→pelvis */
    float neck_h;
    float hitbox_scale;   /* visual size scale (cosmetic; bones are authoritative) */
    int   passive;        /* ChassisPassive */
} Chassis;

/* ---- Armor ----------------------------------------------------------
 *
 * Armor sits in front of HP: incoming damage is split between armor and
 * mech HP per `absorb_ratio` until the armor's HP is drained, at which
 * point it falls off. Reactive armor is a one-shot: it absorbs 100% of
 * the first explosion that touches it and then breaks.
 * (See documents/02-game-design.md §"Body Armor" and 04-combat.md.) */
typedef enum {
    ARMOR_NONE = 0,
    ARMOR_LIGHT,
    ARMOR_HEAVY,
    ARMOR_REACTIVE,
    ARMOR_COUNT
} ArmorId;

typedef struct {
    const char *name;
    float hp;              /* armor capacity */
    float absorb_ratio;    /* fraction of incoming bullet damage absorbed */
    float run_mult;        /* movement penalty from weight */
    float jet_mult;
    int   reactive_charges;/* 1 for ARMOR_REACTIVE, 0 otherwise */
} Armor;

const Armor *armor_def(int id);

/* ---- Jetpack --------------------------------------------------------
 *
 * Modifies the chassis's baseline jet behavior. JET_NONE keeps the
 * baseline; JET_STANDARD adds capacity + thrust; JET_BURST adds a
 * BTN_DASH-triggered fuel-dumping boost; JET_GLIDE_WING gives lift even
 * at empty fuel; JET_JUMP_JET disables thrust but rearms a fresh jump
 * each time the foot touches ground. */
typedef enum {
    JET_NONE = 0,
    JET_STANDARD,
    JET_BURST,
    JET_GLIDE_WING,
    JET_JUMP_JET,
    JET_COUNT
} JetpackId;

typedef struct {
    const char *name;
    float fuel_mult;
    float thrust_mult;
    float boost_fuel_cost;     /* fraction of max fuel consumed by a burst */
    float boost_thrust_mult;   /* multiplier applied to JET_THRUST during burst */
    float boost_duration;      /* seconds */
    float glide_thrust;        /* px/s² lift while fuel == 0 (GLIDE_WING) */
    bool  jump_on_land;        /* JUMP_JET: every ground touch costs ε fuel and rearms a jump */
} Jetpack;

const Jetpack *jetpack_def(int id);

typedef enum {
    ANIM_STAND = 0,
    ANIM_RUN,
    ANIM_JET,
    ANIM_FALL,
    ANIM_FIRE,
    ANIM_DEATH,
    ANIM_COUNT_M1
} AnimId;

const Chassis *mech_chassis(ChassisId id);

/* Resolve a chassis name (case-insensitive) for CLI flag parsing. Returns
 * CHASSIS_TROOPER for unknown names (with a warning). */
ChassisId chassis_id_from_name(const char *name);

/* Loadout — set at spawn time, applied throughout the mech's life. */
typedef struct MechLoadout {
    int chassis_id;     /* ChassisId */
    int primary_id;     /* WeaponId */
    int secondary_id;   /* WeaponId */
    int armor_id;       /* ArmorId */
    int jetpack_id;     /* JetpackId */
} MechLoadout;

/* The default loadout: a Trooper with Pulse Rifle + Sidearm + Light
 * armor + Standard jet. Used when no loadout is specified. */
MechLoadout mech_default_loadout(void);

/* Allocate a mech: reserves PART_COUNT particles in `world.particles`
 * and ~21 constraints in `world.constraints`, all positioned around
 * `spawn`. Returns the slot in `world.mechs[]`, or -1 on failure. */
int  mech_create(World *w, ChassisId chassis, Vec2 spawn, int team, bool is_dummy);

/* Same, but with a full loadout. mech_create() above is now equivalent
 * to mech_create_loadout with the chassis substituted into the default
 * loadout. */
int  mech_create_loadout(World *w, MechLoadout lo, Vec2 spawn,
                         int team, bool is_dummy);

/* Apply damage to a mech, routed by part index (PART_*). Returns true
 * if the hit killed the mech (transitioning it to ragdoll). The shooter
 * id is used for friendly-fire gating; pass -1 for ambient damage
 * (acid, fall) where there is no shooter. */
bool mech_apply_damage(World *w, int mech_id, int part, float damage,
                       Vec2 dir, int shooter_mech_id);

/* Force the mech into the dead/ragdoll state immediately. */
void mech_kill(World *w, int mech_id, int killshot_part, Vec2 dir,
               float impulse, int killer_mech_id, int weapon_id);

/* Detach a limb. Safe to call multiple times — only the first
 * invocation does any work. `limb` is a LIMB_* bit from world.h. */
void mech_dismember(World *w, int mech_id, int limb);

/* The pose/anim driver — called every simulation tick *before* the
 * physics step. Reads input, sets pose targets and strengths, kicks off
 * anim transitions. Local control happens here too: run force, jet
 * thrust, jump impulse. */
void mech_step_drive(World *w, int mech_id, ClientInput in, float dt);

/* Called *after* the physics pass each tick. When a mech is grounded,
 * lifts the body kinematically so the pelvis sits at standing height
 * above the feet. Without this, gravity sag accumulates faster than the
 * constraint solver can correct it; the mech slowly sinks until its
 * pelvis is at floor level. Doing the lift kinematically (pos and prev
 * shifted by the same amount) preserves velocity, so motion still
 * works. */
void mech_post_physics_anchor(World *w, int mech_id);

/* Per-tick environmental damage check: DEADLY tiles, DEADLY polygons,
 * ACID ambient zones. Applies 5 HP/s × dt to PART_PELVIS when any of
 * the mech's particles overlap a hazard. Called from simulate_step
 * after the physics pass. */
void mech_apply_environmental_damage(World *w, int mech_id, float dt);

/* Trigger a single weapon fire (button edge). Schedules recoil, reads
 * weapon table, performs the hitscan, applies damage to whatever it
 * hits. Returns true if a shot left the barrel. */
bool mech_try_fire(World *w, int mech_id, ClientInput in);

/* Called by simulate at the end of a tick — copies the just-consumed
 * input bitmask into m->prev_buttons so next tick's mech_step_drive
 * and mech_try_fire can do edge detection. Doing this at end-of-tick
 * (after mech_try_fire) is the only way both the drive pass and the
 * fire pass see the same edge for any given press. */
void mech_latch_prev_buttons(World *w, int mech_id);

/* Apply incoming-fire bink to a target mech. Called from the weapon
 * hitscan / projectile-collision paths whenever a shot passes near or
 * hits a mech. `proximity_t` is 0..1 — 1 for direct hit, less for
 * near-miss. */
void mech_apply_bink(Mech *m, float bink_amount, float proximity_t,
                     pcg32_t *rng);

/* World-space helpers used by render and HUD. */
Vec2 mech_chest_pos(const World *w, int mech_id);
Vec2 mech_hand_pos (const World *w, int mech_id);   /* right hand */
Vec2 mech_aim_dir  (const World *w, int mech_id);

/* P06 — Grapple constraint lifecycle. Both server-side calls.
 *
 * mech_grapple_attach assumes m->grapple already has anchor_pos /
 * anchor_mech / anchor_part / rest_length filled in (set by the
 * projectile collision path); it allocates a constraint slot from
 * the global ConstraintPool and stores its index in
 * m->grapple.constraint_idx. Tile anchors get a CSTR_FIXED_ANCHOR;
 * mech anchors get a CSTR_DISTANCE between the firer's pelvis and
 * the target's bone particle.
 *
 * mech_grapple_release deactivates the constraint (active=0; slot
 * leak is bounded per spec) and resets m->grapple.state to IDLE.
 * Safe to call at any state — if state is already IDLE it's a no-op. */
void mech_grapple_attach (World *w, int mech_id);
void mech_grapple_release(World *w, int mech_id);
