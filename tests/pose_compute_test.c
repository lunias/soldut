/*
 * tests/pose_compute_test.c — determinism + continuity tests for the
 * M6 procedural pose function.
 *
 * Covers §10.2 of documents/m6/01-ik-and-pose-sync.md:
 *   - Same inputs → bit-identical output over many ticks (no hidden
 *     state in `pose_compute`).
 *   - Advancing gait_phase produces smooth bone motion (no large
 *     frame-to-frame jumps).
 *   - Anim-ID switches (STAND / RUN / JET / FALL / FIRE) all produce
 *     sane pose shapes (chest above pelvis, feet below pelvis).
 *
 * The pose function is pure — it does NOT read the world, only the
 * PoseInputs struct. So we can test it standalone without spinning up
 * a World.
 */

#include "../src/log.h"
#include "../src/math.h"
#include "../src/mech.h"
#include "../src/mech_ik.h"
#include "../src/world.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int g_failed = 0;

static bool nearly_equal(float a, float b, float tol) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d <= tol;
}

#define ASSERT_TRUE(label, cond) do {                                    \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL: %s\n", label); ++g_failed;                \
    } else {                                                             \
        fprintf(stderr, "PASS: %s\n", label);                            \
    }                                                                    \
} while (0)

#define ASSERT_NEAR(label, got, want, tol) do {                          \
    if (!nearly_equal((got), (want), (tol))) {                           \
        fprintf(stderr, "FAIL: %s: got=%.4f want=%.4f tol=%.4f\n",       \
                label, (float)(got), (float)(want), (float)(tol));       \
        ++g_failed;                                                      \
    } else {                                                             \
        fprintf(stderr, "PASS: %s\n", label);                            \
    }                                                                    \
} while (0)

static PoseInputs make_default_inputs(void) {
    return (PoseInputs){
        .pelvis        = (Vec2){ 500.0f, 300.0f },
        .aim_dir       = (Vec2){ 1.0f, 0.0f },     /* aim right */
        .facing_left   = false,
        .anim_id       = ANIM_STAND,
        .gait_phase    = 0.0f,
        .grounded      = true,
        .chassis_id    = CHASSIS_TROOPER,
        .active_slot   = 0,
        .dismember_mask = 0,
        .foregrip_world = NULL,
        .grapple_state  = 0,
        .grapple_anchor = (Vec2){ 0, 0 },
    };
}

/* Determinism: identical inputs over 600 calls produce identical output
 * bit-for-bit (no hidden state in the function). */
static void test_determinism(void) {
    PoseInputs in = make_default_inputs();
    PoseBones first, current;
    pose_compute(&in, first);
    for (int t = 0; t < 600; ++t) {
        pose_compute(&in, current);
        for (int p = 0; p < PART_COUNT; ++p) {
            if (current[p].x != first[p].x || current[p].y != first[p].y) {
                fprintf(stderr,
                        "FAIL: determinism: part %d at iter %d "
                        "drifted (first=(%.6f,%.6f) cur=(%.6f,%.6f))\n",
                        p, t, first[p].x, first[p].y,
                        current[p].x, current[p].y);
                ++g_failed;
                return;
            }
        }
    }
    fprintf(stderr, "PASS: determinism (600 iters, all parts identical)\n");
}

/* Pose shape sanity: chest above pelvis; feet below pelvis; head above
 * chest; standing height matches torso + neck + head offset. */
static void test_rest_pose_shape(void) {
    PoseInputs in = make_default_inputs();
    PoseBones out;
    pose_compute(&in, out);

    const Chassis *ch = mech_chassis(CHASSIS_TROOPER);
    ASSERT_TRUE("rest: pelvis at input", nearly_equal(out[PART_PELVIS].x, 500.0f, 1e-3f)
                                       && nearly_equal(out[PART_PELVIS].y, 300.0f, 1e-3f));
    ASSERT_TRUE("rest: chest above pelvis", out[PART_CHEST].y < out[PART_PELVIS].y);
    ASSERT_TRUE("rest: head above chest", out[PART_HEAD].y < out[PART_CHEST].y);
    ASSERT_TRUE("rest: feet below pelvis", out[PART_L_FOOT].y > out[PART_PELVIS].y);
    ASSERT_TRUE("rest: feet below pelvis (R)", out[PART_R_FOOT].y > out[PART_PELVIS].y);
    ASSERT_NEAR("rest: chest y = pelvis - torso_h",
                out[PART_CHEST].y, 300.0f - ch->torso_h, 0.01f);
    ASSERT_NEAR("rest: foot y = pelvis + thigh + shin",
                out[PART_L_FOOT].y, 300.0f + ch->bone_thigh + ch->bone_shin, 0.01f);
}

/* Aim arm: aiming right places hand to the right of shoulder; aiming
 * left places hand to the left. Hand at full reach matches
 * shoulder + aim * (arm + forearm). */
static void test_aim_arm(void) {
    const Chassis *ch = mech_chassis(CHASSIS_TROOPER);
    float arm_reach = ch->bone_arm + ch->bone_forearm;

    PoseInputs in = make_default_inputs();
    PoseBones out;

    /* Aim right. */
    in.aim_dir = (Vec2){ 1.0f, 0.0f };
    in.facing_left = false;
    pose_compute(&in, out);
    ASSERT_NEAR("aim-R hand x = R_SHO.x + reach",
                out[PART_R_HAND].x,
                out[PART_R_SHOULDER].x + arm_reach, 0.01f);
    ASSERT_NEAR("aim-R hand y = R_SHO.y",
                out[PART_R_HAND].y, out[PART_R_SHOULDER].y, 0.01f);

    /* Aim left. */
    in.aim_dir = (Vec2){ -1.0f, 0.0f };
    in.facing_left = true;
    pose_compute(&in, out);
    ASSERT_NEAR("aim-L hand x = R_SHO.x - reach",
                out[PART_R_HAND].x,
                out[PART_R_SHOULDER].x - arm_reach, 0.01f);
}

/* Engineer with active_slot=1 should NOT drive the aim arm — the
 * R_HAND target sits at the rest-dangle position regardless of aim. */
static void test_engineer_secondary_dangles(void) {
    PoseInputs in = make_default_inputs();
    in.chassis_id  = CHASSIS_ENGINEER;
    in.active_slot = 1;
    in.aim_dir     = (Vec2){ 1.0f, 0.0f };
    PoseBones out;
    pose_compute(&in, out);

    /* R_HAND should be near the rest-dangle position (offset 2 then 2
     * from the shoulder in face_dir, then bone_arm + bone_forearm down).
     * Just check that hand y is below shoulder y by ~bone_arm + bone_forearm. */
    const Chassis *ch = mech_chassis(CHASSIS_ENGINEER);
    float expected_dy = ch->bone_arm + ch->bone_forearm;
    float actual_dy   = out[PART_R_HAND].y - out[PART_R_SHOULDER].y;
    ASSERT_TRUE("engineer-secondary: R_HAND dangles below shoulder",
                actual_dy > expected_dy * 0.8f);
    /* And horizontally R_HAND should NOT be at aim-extended position
     * (which would be R_SHO.x + arm_reach). */
    float aim_x = out[PART_R_SHOULDER].x + (ch->bone_arm + ch->bone_forearm);
    ASSERT_TRUE("engineer-secondary: R_HAND not at aim-extended X",
                fabsf(out[PART_R_HAND].x - aim_x) > 5.0f);
}

/* Continuity: advancing gait_phase by small steps produces small
 * frame-to-frame deltas in foot positions. Catches cycle wrap bugs and
 * sin discontinuities. */
static void test_gait_continuity(void) {
    PoseInputs in = make_default_inputs();
    in.anim_id = ANIM_RUN;

    PoseBones prev, cur;
    in.gait_phase = 0.0f;
    pose_compute(&in, prev);

    float max_delta = 0.0f;
    int   max_delta_part = -1;
    const int steps = 240;       /* two full cycles */
    for (int i = 1; i <= steps; ++i) {
        in.gait_phase = (float)i / (float)steps;
        if (in.gait_phase >= 1.0f) in.gait_phase -= 1.0f;
        pose_compute(&in, cur);
        for (int p = 0; p < PART_COUNT; ++p) {
            float dx = cur[p].x - prev[p].x;
            float dy = cur[p].y - prev[p].y;
            float d  = sqrtf(dx*dx + dy*dy);
            if (d > max_delta) { max_delta = d; max_delta_part = p; }
            prev[p] = cur[p];
        }
    }
    /* Expected max delta for foot ≈ STRIDE/2 / steps_per_half_cycle.
     * With STRIDE=28, swing covers 28 px over 120 steps → ~0.23 px/step.
     * Plus lift_h * sin wobble — peak |d/du sin(u*PI)| = PI per unit u,
     * so over a 1/120 step ≈ PI/120 * LIFT_H ≈ 0.24 px.
     *
     * For the knee the IK introduces a sqrt-of-Δd boundary near d ==
     * chain_max (transition between reachable and unreachable
     * branches), so the knee can move ~3 px in a single step when
     * d crosses the boundary. Acceptable visually (well under 1
     * frame's worth of body motion); cap at 5 px to catch cycle-wrap
     * bugs without flagging the inherent IK boundary jump. */
    ASSERT_TRUE("gait continuity: max per-step delta < 5 px",
                max_delta < 5.0f);
    if (max_delta_part >= 0) {
        fprintf(stderr, "  (max delta=%.4f at part %d)\n", max_delta, max_delta_part);
    }
}

/* RUN gait: at phase=0.0, L foot is at "front", R foot is at "back"
 * (they're 180° out of phase). */
static void test_gait_phase_offset(void) {
    PoseInputs in = make_default_inputs();
    in.anim_id    = ANIM_RUN;
    in.gait_phase = 0.0f;
    PoseBones out;
    pose_compute(&in, out);

    /* Phase 0 stance start for L; phase 0.5 swing start for R (or
     * stance end → swing start). Front foot has higher x relative to
     * hip; back foot has lower x. Facing right, dir=+1, front = +stride/2. */
    float lhip_x = out[PART_L_HIP].x;
    float rhip_x = out[PART_R_HIP].x;
    float l_dx   = out[PART_L_FOOT].x - lhip_x;
    float r_dx   = out[PART_R_FOOT].x - rhip_x;
    ASSERT_TRUE("gait phase 0: L_FOOT in front of L_HIP (facing right)", l_dx > 0.0f);
    ASSERT_TRUE("gait phase 0: R_FOOT behind R_HIP (facing right)", r_dx < 0.0f);
}

/* JET pose: both feet are swept back (opposite to facing). */
static void test_jet_pose(void) {
    PoseInputs in = make_default_inputs();
    in.anim_id = ANIM_JET;
    PoseBones out;
    pose_compute(&in, out);
    /* Facing right (dir=-1 for sweep), feet to the LEFT of hips. */
    ASSERT_TRUE("jet (facing right): L_FOOT left of L_HIP",
                out[PART_L_FOOT].x < out[PART_L_HIP].x);
    ASSERT_TRUE("jet (facing right): R_FOOT left of R_HIP",
                out[PART_R_FOOT].x < out[PART_R_HIP].x);
}

/* Foregrip pose: when foregrip_world is supplied, L_HAND is driven
 * along the L_SHOULDER→foregrip ray (either at foregrip if reachable
 * or at chain max if not). */
static void test_foregrip_pose(void) {
    PoseInputs in = make_default_inputs();
    const Chassis *ch = mech_chassis(CHASSIS_TROOPER);
    PoseBones out;
    pose_compute(&in, out);
    Vec2 l_sho = out[PART_L_SHOULDER];

    /* Place foregrip ~24 px right-and-up of L_SHOULDER (within chain
     * reach for Trooper ≈ 30 px). */
    Vec2 fg = { l_sho.x + 20.0f, l_sho.y - 12.0f };
    in.foregrip_world = &fg;
    pose_compute(&in, out);
    float chain = ch->bone_arm + ch->bone_forearm;
    float d_fg  = sqrtf(20.0f*20.0f + 12.0f*12.0f);
    if (d_fg <= chain) {
        ASSERT_NEAR("foregrip reachable: L_HAND at foregrip x",
                    out[PART_L_HAND].x, fg.x, 0.05f);
        ASSERT_NEAR("foregrip reachable: L_HAND at foregrip y",
                    out[PART_L_HAND].y, fg.y, 0.05f);
    }

    /* Place foregrip ~80 px away (out of reach). L_HAND should land at
     * L_SHOULDER + chain * unit(fg - L_SHOULDER). */
    Vec2 fg_far = { l_sho.x + 80.0f, l_sho.y };
    in.foregrip_world = &fg_far;
    pose_compute(&in, out);
    ASSERT_NEAR("foregrip out-of-reach: L_HAND x = L_SHO.x + chain",
                out[PART_L_HAND].x, l_sho.x + chain, 0.05f);
    ASSERT_NEAR("foregrip out-of-reach: L_HAND y = L_SHO.y",
                out[PART_L_HAND].y, l_sho.y, 0.05f);

    /* Foregrip NULL → L_ARM dangles (hand below shoulder). */
    in.foregrip_world = NULL;
    pose_compute(&in, out);
    ASSERT_TRUE("foregrip none: L_HAND dangles below shoulder",
                out[PART_L_HAND].y > out[PART_L_SHOULDER].y + chain * 0.5f);
}

/* Grapple ATTACHED: R_HAND points TOWARD the anchor but is clamped to
 * chain reach. Placing the hand AT the anchor (regardless of distance)
 * stretches the arm bones past their rest length, producing a
 * giraffe-arm distortion. The rope renderer draws the line from
 * R_HAND to the anchor, so a clamped hand at chain max is still
 * visually correct. */
static void test_grapple_attached_hand(void) {
    PoseInputs in = make_default_inputs();
    const Chassis *ch = mech_chassis(CHASSIS_TROOPER);
    float arm_reach = ch->bone_arm + ch->bone_forearm;

    /* Case 1: anchor within chain reach — hand sits AT the anchor. */
    {
        PoseBones out;
        pose_compute(&in, out);
        Vec2 sho = out[PART_R_SHOULDER];
        Vec2 near_anchor = { sho.x + 10.0f, sho.y - 6.0f };   /* d ≈ 11.7, within ~30 reach */
        in.grapple_state  = GRAPPLE_ATTACHED;
        in.grapple_anchor = near_anchor;
        pose_compute(&in, out);
        ASSERT_NEAR("grapple near anchor: R_HAND x at anchor",
                    out[PART_R_HAND].x, near_anchor.x, 0.05f);
        ASSERT_NEAR("grapple near anchor: R_HAND y at anchor",
                    out[PART_R_HAND].y, near_anchor.y, 0.05f);
    }

    /* Case 2: anchor far past reach — hand clamped to L1+L2 along
     * the s→anchor unit. R_ARM bones stay at rest length. */
    {
        Vec2 far_anchor = { 700.0f, 100.0f };
        in.grapple_state  = GRAPPLE_ATTACHED;
        in.grapple_anchor = far_anchor;
        in.aim_dir = (Vec2){ 1.0f, 0.0f };
        PoseBones out;
        pose_compute(&in, out);
        Vec2 sho = out[PART_R_SHOULDER];
        float dx = far_anchor.x - sho.x;
        float dy = far_anchor.y - sho.y;
        float d  = sqrtf(dx*dx + dy*dy);
        float ex = sho.x + dx * arm_reach / d;
        float ey = sho.y + dy * arm_reach / d;
        ASSERT_NEAR("grapple far anchor: R_HAND clamped to chain reach x",
                    out[PART_R_HAND].x, ex, 0.05f);
        ASSERT_NEAR("grapple far anchor: R_HAND clamped to chain reach y",
                    out[PART_R_HAND].y, ey, 0.05f);
        /* Distance from shoulder to hand is exactly arm_reach. */
        float hd = sqrtf((out[PART_R_HAND].x - sho.x)*(out[PART_R_HAND].x - sho.x) +
                         (out[PART_R_HAND].y - sho.y)*(out[PART_R_HAND].y - sho.y));
        ASSERT_NEAR("grapple far anchor: arm length = bone_arm + bone_forearm",
                    hd, arm_reach, 0.05f);
    }
}

/* Anim coverage: STAND, RUN, JET, FALL, FIRE all produce poses with
 * pelvis at input. */
static void test_anim_coverage(void) {
    PoseInputs in = make_default_inputs();
    const int anims[] = { ANIM_STAND, ANIM_RUN, ANIM_JET, ANIM_FALL, ANIM_FIRE };
    PoseBones out;
    for (size_t i = 0; i < sizeof(anims) / sizeof(anims[0]); ++i) {
        in.anim_id = anims[i];
        pose_compute(&in, out);
        if (!nearly_equal(out[PART_PELVIS].x, in.pelvis.x, 1e-3f) ||
            !nearly_equal(out[PART_PELVIS].y, in.pelvis.y, 1e-3f)) {
            fprintf(stderr, "FAIL: anim %d: pelvis drifted from input\n", anims[i]);
            ++g_failed;
        } else if (out[PART_HEAD].y >= out[PART_PELVIS].y) {
            fprintf(stderr, "FAIL: anim %d: HEAD not above pelvis\n", anims[i]);
            ++g_failed;
        }
    }
    fprintf(stderr, "PASS: anim coverage (5 anims)\n");
}

/* All five chassis produce sane rest poses (chest above pelvis, feet
 * below pelvis). Catches a Chassis* NULL bug or a bone-length zero. */
static void test_chassis_coverage(void) {
    PoseInputs in = make_default_inputs();
    PoseBones out;
    for (int c = 0; c < CHASSIS_COUNT; ++c) {
        in.chassis_id = c;
        pose_compute(&in, out);
        if (out[PART_CHEST].y >= out[PART_PELVIS].y) {
            fprintf(stderr, "FAIL: chassis %d: chest not above pelvis\n", c);
            ++g_failed;
        }
        if (out[PART_L_FOOT].y <= out[PART_PELVIS].y) {
            fprintf(stderr, "FAIL: chassis %d: L_FOOT not below pelvis\n", c);
            ++g_failed;
        }
    }
    fprintf(stderr, "PASS: chassis coverage (%d chassis)\n", CHASSIS_COUNT);
}

int main(void) {
    log_init(NULL);

    test_determinism();
    test_rest_pose_shape();
    test_aim_arm();
    test_engineer_secondary_dangles();
    test_gait_continuity();
    test_gait_phase_offset();
    test_jet_pose();
    test_foregrip_pose();
    test_grapple_attached_hand();
    test_anim_coverage();
    test_chassis_coverage();

    if (g_failed) {
        fprintf(stderr, "\npose_compute_test: %d FAILED\n", g_failed);
        return 1;
    }
    fprintf(stderr, "\npose_compute_test: ALL PASS\n");
    return 0;
}
