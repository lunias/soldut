#pragma once

#include "math.h"
#include "world.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * mech_ik — analytic 2-bone IK + deterministic procedural pose.
 *
 * M6 — replaces `build_pose` + `apply_pose_to_particles` (Verlet
 * constraint-solver pose driver) with a pure function from synced
 * inputs to bone positions. Same code runs on every client, so every
 * mech renders the same skeleton without any per-bone wire data.
 *
 * Design canon: documents/m6/01-ik-and-pose-sync.md.
 *
 * The Verlet body still drives projectile collision against bones,
 * dismemberment ragdoll, grapple anchor chain. We keep it. Live mech
 * bone positions are pure-function output of this module; dead /
 * dismembered particles continue to evolve freely.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Per-tick inputs to the deterministic pose function. Every field must
 * be reproducible on every client given the snapshot stream and the
 * authoritative side's per-tick simulation.
 *
 * `aim_dir` must be a unit vector (or near-unit; pose_compute does NOT
 * re-normalize, so callers must do so first). Render coords: +X right,
 * +Y down.
 */
typedef struct PoseInputs {
    Vec2     pelvis;           /* world pelvis */
    Vec2     aim_dir;          /* unit aim direction */
    bool     facing_left;      /* aim.x < 0 */
    bool     is_dummy;         /* practice dummy — skips arm-aim drive */
    int      anim_id;          /* ANIM_STAND / ANIM_RUN / ANIM_JET / ANIM_FALL / ANIM_FIRE */
    float    gait_phase;       /* ∈ [0, 1), advances in ANIM_RUN */
    bool     grounded;
    int      chassis_id;       /* ChassisId */
    int      active_slot;      /* 0=primary, 1=secondary — drives Engineer quirk */
    uint16_t dismember_mask;   /* LIMB_* bits set when severed (kinematic skip per part) */

    /* Two-handed foregrip. When non-NULL the L_ARM IK target is the
     * foregrip world position; when NULL the L_ARM dangles. The pose
     * function does not call `weapon_foregrip_world` itself; the caller
     * passes either the precomputed foregrip or NULL. */
    const Vec2 *foregrip_world;

    /* Grapple — when state == GRAPPLE_ATTACHED, the R_HAND target is
     * the anchor instead of the aim line. Pose function reads
     * grapple_anchor when grapple_state == GRAPPLE_ATTACHED. */
    uint8_t  grapple_state;    /* GrappleState; GRAPPLE_IDLE = no effect */
    Vec2     grapple_anchor;   /* used when state == GRAPPLE_ATTACHED */
} PoseInputs;

/* Output: world-space position for every PART_*. 16 entries. */
typedef Vec2 PoseBones[PART_COUNT];

/* The canonical procedural pose function. Pure: same inputs → same
 * output bit-for-bit (modulo IEEE754 reorderings in optimized builds —
 * we test for tolerance, not bit-exactness). */
void pose_compute(const PoseInputs *in, PoseBones out);

/* 2-bone analytic IK. Given root `s`, target `t`, bone lengths
 * `L1`/`L2`, and a bend sign (+1 or -1), produce the middle joint
 * position `*out_joint`. Returns true when the target was reachable
 * (joint placed via law-of-cosines so end-effector lands on `t`);
 * false on out-of-reach or over-folded (joint placed straight along
 * s→t at L1, end clamped at the chain's max stable point).
 *
 * `bend_sign`:
 *   +1 — joint on the right-perpendicular side of the s→t line in
 *        screen coords (y-down: rotate forward by +90° → (-fy, fx))
 *   -1 — joint on the opposite side */
bool mech_ik_2bone(Vec2 s, Vec2 t, float L1, float L2,
                   float bend_sign, Vec2 *out_joint);

/* Write a PoseBones output into a mech's particle slots. Kinematic
 * translate (pos and prev both moved by the same delta) so the Verlet
 * integrator reads zero injected velocity. Skips PART_* slots whose
 * limb bit is set in `dismember_mask` — those particles remain
 * free-flying for the ragdoll path.
 *
 * Caller is responsible for having already called pose_compute(...) to
 * fill `bones`. */
void pose_write_to_particles(World *w, int mech_id, const PoseBones bones);

#ifdef __cplusplus
}
#endif
