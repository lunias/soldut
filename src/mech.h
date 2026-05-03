#pragma once

#include "world.h"

/*
 * Mech — a single chassis carrying the 16-particle skeleton, ~21
 * constraints, an animation timeline, and a weapon. M1 ships one
 * chassis (Trooper) and one weapon (Pulse Rifle); other chassis are
 * data added at M3.
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
    /* Scout/Heavy/Sniper/Engineer added at M3 — all share this skeleton */
    CHASSIS_COUNT_M1
} ChassisId;

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
} Chassis;

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

/* Allocate a mech: reserves PART_COUNT particles in `world.particles`
 * and ~21 constraints in `world.constraints`, all positioned around
 * `spawn`. Returns the slot in `world.mechs[]`, or -1 on failure. */
int  mech_create(World *w, ChassisId chassis, Vec2 spawn, int team, bool is_dummy);

/* Apply damage to a mech, routed by part index (PART_*). Returns true
 * if the hit killed the mech (transitioning it to ragdoll). */
bool mech_apply_damage(World *w, int mech_id, int part, float damage, Vec2 dir);

/* Force the mech into the dead/ragdoll state immediately. */
void mech_kill(World *w, int mech_id, int killshot_part, Vec2 dir, float impulse);

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

/* Trigger a single weapon fire (button edge). Schedules recoil, reads
 * weapon table, performs the hitscan, applies damage to whatever it
 * hits. Returns true if a shot left the barrel. */
bool mech_try_fire(World *w, int mech_id, ClientInput in);

/* World-space helpers used by render and HUD. */
Vec2 mech_chest_pos(const World *w, int mech_id);
Vec2 mech_hand_pos (const World *w, int mech_id);   /* right hand */
Vec2 mech_aim_dir  (const World *w, int mech_id);
