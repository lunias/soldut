#include "mech.h"

#include "log.h"
#include "particle.h"
#include "physics.h"
#include "weapons.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- Chassis table -------------------------------------------------- */

static const Chassis g_chassis[CHASSIS_COUNT_M1] = {
    [CHASSIS_TROOPER] = {
        .name         = "Trooper",
        .run_mult     = 1.00f,
        .jump_mult    = 1.00f,
        .jet_mult     = 1.00f,
        .fuel_max     = 1.00f,
        .fuel_regen   = 0.20f,    /* per second when grounded */
        .mass_scale   = 1.00f,
        .health_max   = 150.0f,
        .bone_arm     = 14.0f,
        .bone_forearm = 16.0f,
        .bone_thigh   = 18.0f,
        .bone_shin    = 18.0f,
        .torso_h      = 30.0f,
        .neck_h       = 14.0f,
    },
};

const Chassis *mech_chassis(ChassisId id) {
    if ((unsigned)id >= CHASSIS_COUNT_M1) return &g_chassis[CHASSIS_TROOPER];
    return &g_chassis[id];
}

/* ---- Movement tunables (Soldat-derived; see reference/soldat-constants.md) */
#define RUN_SPEED_PXS      280.0f   /* per second */
#define JUMP_IMPULSE_PXS   320.0f   /* vertical impulse on press */
#define JET_THRUST_PXS2    2200.0f  /* upward acceleration while jetting */
#define JET_DRAIN_PER_SEC  0.60f
#define AIR_CONTROL        0.35f    /* run-force scale while airborne */

/* ---- Helpers: particle (de)init ------------------------------------ */

static int particles_reserve(World *w, int n) {
    ParticlePool *p = &w->particles;
    if (p->count + n > p->capacity) {
        LOG_E("mech_create: particle pool exhausted (need %d, have %d/%d)",
              n, p->count, p->capacity);
        return -1;
    }
    int base = p->count;
    p->count += n;
    return base;
}

static int constraints_reserve(World *w, int n) {
    ConstraintPool *c = &w->constraints;
    if (c->count + n > c->capacity) {
        LOG_E("mech_create: constraint pool exhausted (need %d, have %d/%d)",
              n, c->count, c->capacity);
        return -1;
    }
    int base = c->count;
    c->count += n;
    return base;
}

static void set_particle(World *w, int idx, Vec2 pos, float inv_mass) {
    ParticlePool *p = &w->particles;
    p->pos_x   [idx] = pos.x;
    p->pos_y   [idx] = pos.y;
    p->prev_x  [idx] = pos.x;
    p->prev_y  [idx] = pos.y;
    p->inv_mass[idx] = inv_mass;
    p->flags   [idx] = PARTICLE_FLAG_ACTIVE;
}

static void add_distance(World *w, int *next, int a, int b, float rest) {
    Constraint *c = &w->constraints.items[(*next)++];
    *c = (Constraint){
        .a = (uint16_t)a, .b = (uint16_t)b, .c = 0,
        .kind = CSTR_DISTANCE, .active = 1,
        .rest = rest, .min_len = 0, .max_len = 0, .min_ang = 0, .max_ang = 0,
    };
}

/* Angle constraint at joint `b` between particles `a` and `c`. The angle
 * is measured from b→a to b→c (radians); values outside [min_ang,
 * max_ang] are clamped. Without these, chains like hip-knee-foot can
 * fold in any direction under gravity. */
static void add_angle(World *w, int *next, int a, int b, int c, float min_ang, float max_ang) {
    Constraint *cs = &w->constraints.items[(*next)++];
    *cs = (Constraint){
        .a = (uint16_t)a, .b = (uint16_t)c, .c = (uint16_t)b,
        .kind = CSTR_ANGLE, .active = 1,
        .rest = 0, .min_len = 0, .max_len = 0,
        .min_ang = min_ang, .max_ang = max_ang,
    };
}

/* ---- mech_create --------------------------------------------------- */

int mech_create(World *w, ChassisId chassis_id, Vec2 spawn, int team, bool is_dummy) {
    if (w->mech_count >= MAX_MECHS) {
        LOG_E("mech_create: MAX_MECHS reached");
        return -1;
    }

    int pbase = particles_reserve(w, PART_COUNT);
    if (pbase < 0) return -1;

    const Chassis *ch = mech_chassis(chassis_id);

    /* Skeleton positions, pelvis-relative. (See diagram in mech.h.) */
    Vec2 P[PART_COUNT];
    P[PART_PELVIS]     = (Vec2){ 0,                                    0 };
    P[PART_CHEST]      = (Vec2){ 0,                          -ch->torso_h };
    P[PART_NECK]       = (Vec2){ 0,                -ch->torso_h - ch->neck_h * 0.5f };
    P[PART_HEAD]       = (Vec2){ 0,                -ch->torso_h - ch->neck_h - 8.0f };
    P[PART_L_SHOULDER] = (Vec2){-10,                       -ch->torso_h + 4.0f };
    P[PART_R_SHOULDER] = (Vec2){ 10,                       -ch->torso_h + 4.0f };
    P[PART_L_ELBOW]    = (Vec2){-12,    -ch->torso_h + 4.0f + ch->bone_arm };
    P[PART_R_ELBOW]    = (Vec2){ 12,    -ch->torso_h + 4.0f + ch->bone_arm };
    P[PART_L_HAND]     = (Vec2){-12, -ch->torso_h + 4.0f + ch->bone_arm + ch->bone_forearm };
    P[PART_R_HAND]     = (Vec2){ 12, -ch->torso_h + 4.0f + ch->bone_arm + ch->bone_forearm };
    P[PART_L_HIP]      = (Vec2){-7,                                    0 };
    P[PART_R_HIP]      = (Vec2){ 7,                                    0 };
    P[PART_L_KNEE]     = (Vec2){-8,                          ch->bone_thigh };
    P[PART_R_KNEE]     = (Vec2){ 8,                          ch->bone_thigh };
    P[PART_L_FOOT]     = (Vec2){-9,             ch->bone_thigh + ch->bone_shin };
    P[PART_R_FOOT]     = (Vec2){ 9,             ch->bone_thigh + ch->bone_shin };

    for (int i = 0; i < PART_COUNT; ++i) {
        Vec2 wp = { spawn.x + P[i].x, spawn.y + P[i].y };
        set_particle(w, pbase + i, wp, 1.0f / ch->mass_scale);
    }

    /* Constraints — distance constraints for bones + spans + torso
     * triangulation, plus angle constraints on knees and elbows so the
     * chains don't fold under gravity. Reserve generously and trim. */
    int cbase = constraints_reserve(w, 32);
    if (cbase < 0) return -1;
    int next = cbase;

    #define DIST(an, bn) add_distance(w, &next, pbase + (an), pbase + (bn), \
        sqrtf((P[an].x - P[bn].x)*(P[an].x - P[bn].x) + \
              (P[an].y - P[bn].y)*(P[an].y - P[bn].y)))

    /* Spine. The HEAD↔CHEST constraint plus HEAD↔NECK and NECK↔CHEST
     * makes a rigid head/neck/chest triangle, so the head can't fold
     * forward under gravity when the pose drive isn't strong enough. */
    DIST(PART_HEAD,        PART_NECK);
    DIST(PART_NECK,        PART_CHEST);
    DIST(PART_HEAD,        PART_CHEST);
    DIST(PART_CHEST,       PART_PELVIS);
    /* Clavicles. */
    DIST(PART_CHEST,       PART_L_SHOULDER);
    DIST(PART_CHEST,       PART_R_SHOULDER);
    /* Hips. */
    DIST(PART_PELVIS,      PART_L_HIP);
    DIST(PART_PELVIS,      PART_R_HIP);
    /* Arms. */
    DIST(PART_L_SHOULDER,  PART_L_ELBOW);
    DIST(PART_L_ELBOW,     PART_L_HAND);
    DIST(PART_R_SHOULDER,  PART_R_ELBOW);
    DIST(PART_R_ELBOW,     PART_R_HAND);
    /* Legs. */
    DIST(PART_L_HIP,       PART_L_KNEE);
    DIST(PART_L_KNEE,      PART_L_FOOT);
    DIST(PART_R_HIP,       PART_R_KNEE);
    DIST(PART_R_KNEE,      PART_R_FOOT);
    /* Stabilizer spans (shoulder + hip widths). */
    DIST(PART_L_SHOULDER,  PART_R_SHOULDER);
    DIST(PART_L_HIP,       PART_R_HIP);
    /* Triangulate the torso so it doesn't shear left/right. We *don't*
     * also add l_sho↔r_hip cross-braces — that pair fights with the
     * shoulder/pelvis triangulation and oscillates. */
    DIST(PART_L_SHOULDER,  PART_PELVIS);
    DIST(PART_R_SHOULDER,  PART_PELVIS);

    #undef DIST

    /* (No angle constraints at M1 — they only restrict the *interior*
     * angle at the joint, which is π for any straight chain regardless
     * of orientation. So they don't prevent a leg from rotating into a
     * horizontal "lying down" pose, which is the failure mode we'd
     * want them to prevent. A real fix needs orientation constraints
     * relative to a "world-up" reference, which is out of scope for M1.) */

    /* Trim the pool back to what we actually used. */
    int constraint_count = next - cbase;
    w->constraints.count = next;

    /* Mech struct. */
    int mid = w->mech_count++;
    Mech *m = &w->mechs[mid];
    memset(m, 0, sizeof(*m));
    m->id              = mid;
    m->chassis_id      = (int)chassis_id;
    m->team            = team;
    m->alive           = true;
    m->is_dummy        = is_dummy;
    m->particle_base   = (uint16_t)pbase;
    m->constraint_base = (uint16_t)cbase;
    m->constraint_count= (uint16_t)constraint_count;
    m->aim_world       = (Vec2){ spawn.x + 100.0f, spawn.y - 20.0f };
    m->health          = ch->health_max;
    m->health_max      = ch->health_max;
    m->fuel            = ch->fuel_max;
    m->fuel_max        = ch->fuel_max;
    m->hp_arm_l        = 80.0f;
    m->hp_arm_r        = 80.0f;
    m->hp_leg_l        = 80.0f;
    m->hp_leg_r        = 80.0f;
    m->hp_head         = 50.0f;
    m->weapon_id       = WEAPON_PULSE_RIFLE;
    m->ammo_max        = 30;
    m->ammo            = 30;
    m->facing_left     = false;

    LOG_I("mech_create: id=%d chassis=%s%s (particles[%d..%d), cstr[%d..%d))",
          mid, ch->name, is_dummy ? " (dummy)" : "",
          pbase, pbase + PART_COUNT, cbase, cbase + constraint_count);
    return mid;
}

/* ---- Convenience accessors ---------------------------------------- */

static Vec2 part_pos(const World *w, const Mech *m, int part) {
    int idx = m->particle_base + part;
    return (Vec2){ w->particles.pos_x[idx], w->particles.pos_y[idx] };
}

Vec2 mech_chest_pos(const World *w, int mid) {
    return part_pos(w, &w->mechs[mid], PART_CHEST);
}

Vec2 mech_hand_pos(const World *w, int mid) {
    return part_pos(w, &w->mechs[mid], PART_R_HAND);
}

Vec2 mech_aim_dir(const World *w, int mid) {
    const Mech *m = &w->mechs[mid];
    Vec2 c = part_pos(w, m, PART_CHEST);
    Vec2 d = { m->aim_world.x - c.x, m->aim_world.y - c.y };
    float L = sqrtf(d.x * d.x + d.y * d.y);
    if (L < 1e-3f) return (Vec2){ m->facing_left ? -1.0f : 1.0f, 0.0f };
    return (Vec2){ d.x / L, d.y / L };
}

/* ---- Pose drive (animation) --------------------------------------- */

static void clear_pose(Mech *m) {
    for (int i = 0; i < PART_COUNT; ++i) m->pose_strength[i] = 0.0f;
}

static void pose_set(Mech *m, int part, Vec2 target, float strength) {
    m->pose_target  [part] = target;
    m->pose_strength[part] = strength;
}

/* Apply pose targets — runs *before* gravity/integrate. We don't lerp
 * the pelvis (that's where gravity and movement displacement land);
 * everything else gets pulled toward its target by strength.
 *
 * Crucially this is a *kinematic* translate (updates both pos AND prev
 * by the same delta). If we only moved pos, the next Verlet integrate
 * would read the lerp as injected velocity and the body would oscillate. */
static void apply_pose_to_particles(World *w, Mech *m) {
    ParticlePool *p = &w->particles;
    for (int i = 0; i < PART_COUNT; ++i) {
        float s = m->pose_strength[i];
        if (s <= 0.0f) continue;
        int idx = m->particle_base + i;
        float dx = (m->pose_target[i].x - p->pos_x[idx]) * s;
        float dy = (m->pose_target[i].y - p->pos_y[idx]) * s;
        /* Swept kinematic translate so a strong pose pull (e.g. 0.7 *
         * a 50-px gap to standing-height head target) can't teleport a
         * particle through a 1-tile-thick platform in one tick. */
        physics_translate_kinematic_swept(p, &w->level, idx, dx, dy);
    }
}

/* Build an animation pose into m->pose_*. Pelvis position is taken from
 * the live particle so the body remains attached to wherever physics
 * actually put it.
 *
 * KEY RULES:
 *   - Pose targets are *layout-consistent*. Each part's target is exactly
 *     its layout offset relative to the (possibly lifted) pelvis, so the
 *     distance between any two pose-driven particles equals the rest
 *     length of the constraint between them. That removes the
 *     pose-vs-constraint tug-of-war that otherwise pumps drift velocity
 *     into the body.
 *   - When grounded, the pelvis itself gets a pose target at the
 *     "standing height" derived from foot position. Without this,
 *     gravity slowly sags the pelvis below standing height; the
 *     pose-Y targets for legs follow it down; foot targets end up
 *     under the floor; collision pins them at the floor; the chain
 *     collapses to horizontal. Pose-driving the pelvis breaks the
 *     positive-feedback loop.
 *   - Aim only drives the right (rifle) arm chain. */
static void build_pose(const Chassis *ch, World *w, Mech *m, float dt) {
    clear_pose(m);
    if (!m->alive) return;

    int b = m->particle_base;
    ParticlePool *p = &w->particles;
    Vec2 aim_dir = mech_aim_dir(w, m->id);
    m->facing_left = aim_dir.x < 0.0f;

    /* Compute the pelvis's pose-target Y. When grounded, we derive it
     * from the foot positions (so the body stands tall above its feet).
     * When in the air, just hold the live pelvis Y. */
    float pelvis_x = p->pos_x[b + PART_PELVIS];
    float pelvis_y = p->pos_y[b + PART_PELVIS];
    float chain_len = ch->bone_thigh + ch->bone_shin;
    /* When grounded, anchor the *target* pelvis Y at standing height
     * (foot−chain_len) so the body's pose is computed for an upright
     * stance regardless of where physics has put the live pelvis. The
     * actual lift of the live pelvis happens in the post-physics
     * pass via mech_post_physics_anchor (so we don't pump velocity
     * through the constraint solver). */
    if (m->grounded) {
        float foot_y_avg = (p->pos_y[b + PART_L_FOOT] +
                            p->pos_y[b + PART_R_FOOT]) * 0.5f;
        pelvis_y = foot_y_avg - chain_len;
    }
    /* The remaining pose targets are computed from this pelvis_y. */
    Vec2 pelvis = { pelvis_x, pelvis_y };

    /* Layout-consistent torso/head. */
    pose_set(m, PART_CHEST,
        (Vec2){ pelvis.x, pelvis.y - ch->torso_h }, 0.7f);
    pose_set(m, PART_NECK,
        (Vec2){ pelvis.x, pelvis.y - ch->torso_h - ch->neck_h * 0.5f }, 0.7f);
    pose_set(m, PART_HEAD,
        (Vec2){ pelvis.x, pelvis.y - ch->torso_h - ch->neck_h - 8.0f }, 0.7f);

    /* Hips. */
    pose_set(m, PART_L_HIP, (Vec2){ pelvis.x - 7, pelvis.y }, 0.7f);
    pose_set(m, PART_R_HIP, (Vec2){ pelvis.x + 7, pelvis.y }, 0.7f);

    /* Shoulders. */
    pose_set(m, PART_L_SHOULDER,
        (Vec2){ pelvis.x - 10, pelvis.y - ch->torso_h + 4 }, 0.7f);
    pose_set(m, PART_R_SHOULDER,
        (Vec2){ pelvis.x + 10, pelvis.y - ch->torso_h + 4 }, 0.7f);

    float arm_reach = ch->bone_arm + ch->bone_forearm;
    if (!m->is_dummy) {
        /* Right (rifle) arm — only the hand has a pose target; the
         * elbow follows from the constraint solver. Placing the hand
         * on the aim ray at full chain reach keeps the arm constraints
         * exactly at rest when the pose is fully met. */
        Vec2 r_sho = (Vec2){ pelvis.x + 10, pelvis.y - ch->torso_h + 4 };
        pose_set(m, PART_R_HAND,
            (Vec2){ r_sho.x + aim_dir.x * arm_reach,
                    r_sho.y + aim_dir.y * arm_reach }, 0.7f);
    }
    /* Left arm has no pose targets in either case. (A proper two-hand
     * rifle grip needs analytic IK; deferring past M1.) */
    (void)arm_reach;

    /* Legs — by anim. */
    Vec2 lhip = { pelvis.x - 7, pelvis.y };
    Vec2 rhip = { pelvis.x + 7, pelvis.y };
    float leg_strength = 0.5f;

    switch ((AnimId)m->anim_id) {
        case ANIM_RUN: {
            /* Procedural walk cycle. Each foot alternates between
             * STANCE (foot on the ground, moving backward in
             * body-frame at exactly the run velocity, so its world
             * position is stationary — the body pivots over it) and
             * SWING (foot lifts off and arcs forward to the next
             * plant point). The previous sinusoidal swing dragged
             * planted feet through the ground because no point on
             * the sin curve had body-frame velocity equal to
             * -RUN_SPEED, so feet skittered.
             *
             * cycle_freq is tuned so STANCE_DURATION × RUN_SPEED ==
             * STRIDE: the body covers exactly one stride length while
             * one foot is planted, which keeps that foot stationary
             * in world space. Stride and lift are static and
             * generous enough to look like a confident walk; on
             * varying-speed locomotion (air control, slowed by
             * obstacles) the planted foot will skitter slightly,
             * which is acceptable. */
            m->anim_time += dt;
            const float stride     = 28.0f;        /* peak-to-peak */
            const float lift_h     = 9.0f;
            const float run_v      = RUN_SPEED_PXS * ch->run_mult;
            float       cycle_freq = run_v / (2.0f * stride); /* 5 Hz @ 280 */

            float dir   = m->facing_left ? -1.0f : 1.0f;
            float front = stride * 0.5f * dir;     /* signed plant point ahead */
            float back  = -stride * 0.5f * dir;    /* signed plant point behind */
            float foot_y_ground = lhip.y + ch->bone_thigh + ch->bone_shin;

            /* phase ∈ [0,1): 0..0.5 is stance, 0.5..1 is swing.
             * Right foot is offset by 0.5 so the two are out of phase. */
            float p_l = m->anim_time * cycle_freq;
            p_l -= floorf(p_l);
            float p_r = p_l + 0.5f;
            if (p_r >= 1.0f) p_r -= 1.0f;

            float l_fx, l_fy, r_fx, r_fy;
            if (p_l < 0.5f) {
                float u = p_l * 2.0f;       /* stance: front → back */
                l_fx = lhip.x + front + (back - front) * u;
                l_fy = foot_y_ground;
            } else {
                float u = (p_l - 0.5f) * 2.0f;     /* swing: back → front + arc */
                l_fx = lhip.x + back + (front - back) * u;
                l_fy = foot_y_ground - lift_h * sinf(u * PI);
            }
            if (p_r < 0.5f) {
                float u = p_r * 2.0f;
                r_fx = rhip.x + front + (back - front) * u;
                r_fy = foot_y_ground;
            } else {
                float u = (p_r - 0.5f) * 2.0f;
                r_fx = rhip.x + back + (front - back) * u;
                r_fy = foot_y_ground - lift_h * sinf(u * PI);
            }

            /* Knee tracks toward the midpoint of hip and foot, with a
             * small forward bias on the swing leg so the leg actually
             * bends forward instead of buckling. */
            float l_knee_y = lhip.y + ch->bone_thigh - 2;
            float r_knee_y = rhip.y + ch->bone_thigh - 2;
            pose_set(m, PART_L_KNEE,
                     (Vec2){ (lhip.x + l_fx) * 0.5f, l_knee_y }, 0.4f);
            pose_set(m, PART_R_KNEE,
                     (Vec2){ (rhip.x + r_fx) * 0.5f, r_knee_y }, 0.4f);
            pose_set(m, PART_L_FOOT, (Vec2){ l_fx, l_fy }, leg_strength);
            pose_set(m, PART_R_FOOT, (Vec2){ r_fx, r_fy }, leg_strength);
            break;
        }
        case ANIM_JET: {
            /* Legs trail back/down a bit while jetting. */
            float dir = m->facing_left ? 1.0f : -1.0f;
            pose_set(m, PART_L_KNEE,
                (Vec2){ lhip.x + dir * 4, lhip.y + ch->bone_thigh - 2 }, 0.4f);
            pose_set(m, PART_R_KNEE,
                (Vec2){ rhip.x + dir * 4, rhip.y + ch->bone_thigh - 2 }, 0.4f);
            pose_set(m, PART_L_FOOT,
                (Vec2){ lhip.x + dir * 12, lhip.y + ch->bone_thigh + ch->bone_shin - 4 }, leg_strength);
            pose_set(m, PART_R_FOOT,
                (Vec2){ rhip.x + dir * 12, rhip.y + ch->bone_thigh + ch->bone_shin - 4 }, leg_strength);
            break;
        }
        case ANIM_FALL:
        case ANIM_FIRE:
        case ANIM_STAND:
        default: {
            /* Stand pose: legs straight down from the hips. */
            pose_set(m, PART_L_KNEE,
                (Vec2){ lhip.x - 1, lhip.y + ch->bone_thigh }, 0.4f);
            pose_set(m, PART_R_KNEE,
                (Vec2){ rhip.x + 1, rhip.y + ch->bone_thigh }, 0.4f);
            pose_set(m, PART_L_FOOT,
                (Vec2){ lhip.x - 1, lhip.y + ch->bone_thigh + ch->bone_shin },
                leg_strength);
            pose_set(m, PART_R_FOOT,
                (Vec2){ rhip.x + 1, rhip.y + ch->bone_thigh + ch->bone_shin },
                leg_strength);
            break;
        }
    }
}

/* ---- Step: input → forces → pose ---------------------------------- */

static bool any_foot_grounded(const World *w, const Mech *m) {
    const ParticlePool *p = &w->particles;
    int lf = m->particle_base + PART_L_FOOT;
    int rf = m->particle_base + PART_R_FOOT;
    return (p->flags[lf] & PARTICLE_FLAG_GROUNDED) ||
           (p->flags[rf] & PARTICLE_FLAG_GROUNDED);
}

/* Set per-tick horizontal velocity on every particle in the mech so the
 * whole body translates as a unit. If we only set the lower body, the
 * upper body would have to be dragged along by constraints — a fight,
 * not a follow — and pose drive (which is kinematic) can't supply
 * velocity to the upper body either. Hence: all 16 particles. */
static void apply_run_velocity(World *w, const Mech *m, float vx_pxs, float dt, bool grounded) {
    ParticlePool *p = &w->particles;
    float vx_per_tick = vx_pxs * dt;
    if (!grounded) vx_per_tick *= AIR_CONTROL;     /* gentler in air */
    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        physics_set_velocity_x(p, idx, vx_per_tick);
    }
}

/* Jump: instantaneous upward velocity on every particle. The whole body
 * leaves the ground together. */
static void apply_jump(World *w, const Mech *m, float jump_pxs, float dt) {
    ParticlePool *p = &w->particles;
    float vy_per_tick = -jump_pxs * dt;        /* negative = upward */
    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        physics_set_velocity_y(p, idx, vy_per_tick);
    }
}

/* Jet: continuous upward acceleration applied uniformly to every body
 * particle. Gravity and jet are both whole-body accelerations; applying
 * jet only to the torso would tilt the body forward as the legs lag.
 *
 * We cut the thrust as the body approaches the world's y=0 boundary.
 * Without this guard, sustained thrust crams every particle against
 * the out-of-bounds-as-solid ceiling and the constraint solver tangles
 * the skeleton trying to keep bones rigid against the wedge. The taper
 * starts one chain-length below the ceiling (so the head doesn't quite
 * reach it) and goes to zero at half that distance. */
#define JET_CEILING_TAPER_BEGIN  64.0f   /* px below y=0 where thrust starts to fade */
#define JET_CEILING_TAPER_END    24.0f   /* px below y=0 where thrust hits zero */

static void apply_jet_force(World *w, const Mech *m, float thrust_pxs2, float dt) {
    ParticlePool *p = &w->particles;
    int b = m->particle_base;
    float head_y = p->pos_y[b + PART_HEAD];
    float scale  = 1.0f;
    if (head_y < JET_CEILING_TAPER_BEGIN) {
        if (head_y <= JET_CEILING_TAPER_END) scale = 0.0f;
        else scale = (head_y - JET_CEILING_TAPER_END) /
                     (JET_CEILING_TAPER_BEGIN - JET_CEILING_TAPER_END);
    }
    if (scale <= 0.0f) return;
    float dy = -thrust_pxs2 * scale * dt * dt;
    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        p->pos_y[idx] += dy;
    }
}

void mech_step_drive(World *w, int mid, ClientInput in, float dt) {
    Mech *m = &w->mechs[mid];
    const Chassis *ch = mech_chassis((ChassisId)m->chassis_id);

    if (m->alive && !m->is_dummy) {
        bool grounded = any_foot_grounded(w, m);
        m->grounded = grounded;

        bool moving = false;
        if (in.buttons & BTN_LEFT) {
            apply_run_velocity(w, m, -RUN_SPEED_PXS * ch->run_mult, dt, grounded);
            moving = true;
        }
        if (in.buttons & BTN_RIGHT) {
            apply_run_velocity(w, m,  RUN_SPEED_PXS * ch->run_mult, dt, grounded);
            moving = true;
        }
        /* Active braking — when no horizontal input and feet are on the
         * floor, zero horizontal velocity directly so stop is instant.
         * Verlet's tangential friction alone takes ~half a second to
         * decelerate, which feels mushy. */
        if (grounded && !moving) {
            apply_run_velocity(w, m, 0.0f, dt, true);
        }

        bool jetting = (in.buttons & BTN_JET) && m->fuel > 0.0f;
        if (jetting) {
            apply_jet_force(w, m, JET_THRUST_PXS2 * ch->jet_mult, dt);
            m->fuel -= JET_DRAIN_PER_SEC * dt;
            if (m->fuel < 0.0f) m->fuel = 0.0f;
        } else if (grounded) {
            m->fuel += ch->fuel_regen * dt;
            if (m->fuel > ch->fuel_max) m->fuel = ch->fuel_max;
        }

        if ((in.buttons & BTN_JUMP) && grounded) {
            apply_jump(w, m, JUMP_IMPULSE_PXS * ch->jump_mult, dt);
        }

        /* Anim selection — purely visual. */
        if (jetting)        m->anim_id = ANIM_JET;
        else if (!grounded) m->anim_id = ANIM_FALL;
        else if (moving)    m->anim_id = ANIM_RUN;
        else                m->anim_id = ANIM_STAND;

        /* Aim — pose driver reads this; the platform layer fills aim
         * world-space *before* this step (see simulate.c). */
    } else if (m->alive && m->is_dummy) {
        /* Dummies stand still. */
        m->anim_id = ANIM_STAND;
        m->grounded = any_foot_grounded(w, m);
    }

    /* Cooldowns. */
    if (m->fire_cooldown > 0.0f) m->fire_cooldown -= dt;
    if (m->recoil_kick   > 0.0f) m->recoil_kick   -= dt * 4.0f;

    /* Reload state machine. Either we're mid-reload (count down, then
     * refill on completion) or we're empty and need to kick one off. */
    if (m->reload_timer > 0.0f) {
        m->reload_timer -= dt;
        if (m->reload_timer <= 0.0f) {
            m->reload_timer = 0.0f;
            m->ammo = m->ammo_max;
        }
    } else if (m->ammo <= 0) {
        const Weapon *wpn = weapon_def(m->weapon_id);
        m->reload_timer = wpn ? wpn->reload_sec : 1.5f;
    }

    /* Build the pose targets and apply them. The constraint pass that
     * follows will redistribute the displacement through the rest of
     * the skeleton. */
    build_pose(ch, w, m, dt);
    apply_pose_to_particles(w, m);
}

void mech_post_physics_anchor(World *w, int mid) {
    Mech *m = &w->mechs[mid];
    if (!m->alive) return;

    ParticlePool *p = &w->particles;
    int b = m->particle_base;
    bool grounded =
        (p->flags[b + PART_L_FOOT] & PARTICLE_FLAG_GROUNDED) ||
        (p->flags[b + PART_R_FOOT] & PARTICLE_FLAG_GROUNDED);
    m->grounded = grounded;
    if (!grounded) return;
    /* Anchor for any grounded, alive bipedal pose: the upper body wants
     * to sit at standing height regardless of whether the legs are
     * static (STAND) or striding (RUN). Skipping the anchor in RUN
     * caused a "crumpled landing" bug where the body landed mid-stride,
     * pelvis sagged below standing height, and the run pose alone
     * couldn't pull it back up. JET/FALL/DEATH don't enter here:
     * grounded is false during them (or m->alive is false for death). */
    if (m->anim_id != ANIM_STAND && m->anim_id != ANIM_RUN) return;

    const Chassis *ch = mech_chassis((ChassisId)m->chassis_id);
    float foot_y = (p->pos_y[b + PART_L_FOOT] + p->pos_y[b + PART_R_FOOT]) * 0.5f;

    /* For a straight, vertical leg: foot at floor, knee mid-chain,
     * pelvis at the top. Translate each part to its proper height
     * kinematically (preserving velocity). */
    float pelvis_y_target = foot_y - ch->bone_thigh - ch->bone_shin;
    float knee_y_target   = foot_y - ch->bone_shin;
    float dy_pelvis = pelvis_y_target - p->pos_y[b + PART_PELVIS];

    /* Only lift, never push down (push-down would prevent jumps). */
    if (dy_pelvis >= -0.1f) return;

    /* Lift pelvis, hips, knees, and the entire upper body together, then
     * zero Y-velocity by collapsing prev_y onto pos_y. The kinematic
     * lift alone would preserve the (gravity-accumulated) Y-velocity,
     * which grows toward terminal velocity over many idle ticks; that
     * stored velocity then erupts the moment the mech loses ground
     * contact. We keep X velocity intact so run/jump motion works.
     *
     * Knees are translated by the same dy_pelvis as the hips so the
     * thigh constraint stays at rest length. The shin (knee→foot) gets
     * stretched by dy_pelvis instead — the constraint solver resolves
     * it on the next tick. This is preferable to fighting the run
     * pose's lateral knee swing by snapping knees to mid-chain. */
    int up_with_pelvis[] = {
        PART_PELVIS, PART_L_HIP, PART_R_HIP,
        PART_L_KNEE, PART_R_KNEE,
        PART_CHEST, PART_NECK, PART_HEAD,
        PART_L_SHOULDER, PART_R_SHOULDER,
        PART_L_ELBOW, PART_R_ELBOW,
        PART_L_HAND, PART_R_HAND,
    };
    int n_up = (int)(sizeof(up_with_pelvis) / sizeof(up_with_pelvis[0]));
    for (int i = 0; i < n_up; ++i) {
        int idx = b + up_with_pelvis[i];
        physics_translate_kinematic(p, idx, 0.0f, dy_pelvis);
        p->prev_y[idx] = p->pos_y[idx];   /* zero Y velocity */
    }

    /* In STAND, the legs are static, so finish straightening them by
     * snapping each knee to mid-chain Y. (Same X is left to layout from
     * the pose pass next tick.) Skipping this in RUN lets the stride
     * cycle drive knee swing naturally. */
    if (m->anim_id == ANIM_STAND) {
        float dy_knee_l = knee_y_target - p->pos_y[b + PART_L_KNEE];
        float dy_knee_r = knee_y_target - p->pos_y[b + PART_R_KNEE];
        physics_translate_kinematic(p, b + PART_L_KNEE, 0.0f, dy_knee_l);
        physics_translate_kinematic(p, b + PART_R_KNEE, 0.0f, dy_knee_r);
        p->prev_y[b + PART_L_KNEE] = p->pos_y[b + PART_L_KNEE];
        p->prev_y[b + PART_R_KNEE] = p->pos_y[b + PART_R_KNEE];
    }
}

bool mech_try_fire(World *w, int mid, ClientInput in) {
    Mech *m = &w->mechs[mid];
    if (!m->alive || m->is_dummy)            return false;
    if (m->fire_cooldown > 0.0f)              return false;
    if (m->reload_timer  > 0.0f)              return false;
    if (m->ammo <= 0)                         return false;
    if (!(in.buttons & BTN_FIRE))             return false;

    weapons_fire_hitscan(w, mid);
    m->ammo--;
    return true;
}

/* ---- Damage + death + dismemberment ------------------------------- */

static float hit_location_mult(int part) {
    switch (part) {
        case PART_HEAD:                              return 1.6f;
        case PART_CHEST: case PART_NECK:
        case PART_PELVIS:                            return 1.0f;
        case PART_L_SHOULDER: case PART_R_SHOULDER:
        case PART_L_ELBOW:    case PART_R_ELBOW:     return 0.7f;
        case PART_L_HAND:     case PART_R_HAND:      return 0.5f;
        case PART_L_HIP:      case PART_R_HIP:
        case PART_L_KNEE:     case PART_R_KNEE:      return 0.7f;
        case PART_L_FOOT:     case PART_R_FOOT:      return 0.5f;
        default:                                     return 1.0f;
    }
}

/* Find the constraint that anchors the L_SHOULDER to the L_ELBOW (the
 * upper-arm bone) and deactivate it. Walk the constraint range — the
 * count is small (~22 per mech). */
static void dismember_left_arm(World *w, Mech *m) {
    if (m->dismember_mask & LIMB_L_ARM) return;
    int p_shoulder = m->particle_base + PART_L_SHOULDER;
    int p_elbow    = m->particle_base + PART_L_ELBOW;
    for (int i = 0; i < m->constraint_count; ++i) {
        Constraint *c = &w->constraints.items[m->constraint_base + i];
        if ((c->a == p_shoulder && c->b == p_elbow) ||
            (c->b == p_shoulder && c->a == p_elbow)) {
            c->active = 0;
        }
    }
    /* Also cut the chest↔shoulder so the whole arm flops free. */
    int p_chest = m->particle_base + PART_CHEST;
    for (int i = 0; i < m->constraint_count; ++i) {
        Constraint *c = &w->constraints.items[m->constraint_base + i];
        if ((c->a == p_shoulder && c->b == p_chest) ||
            (c->b == p_shoulder && c->a == p_chest)) {
            c->active = 0;
        }
        /* Cut the shoulder-span and shoulder→pelvis brace too so the
         * remaining torso doesn't drag the floating arm around. */
        int p_r_shoulder = m->particle_base + PART_R_SHOULDER;
        int p_pelvis     = m->particle_base + PART_PELVIS;
        int p_r_hip      = m->particle_base + PART_R_HIP;
        if ((c->a == p_shoulder && c->b == p_r_shoulder) ||
            (c->b == p_shoulder && c->a == p_r_shoulder)) c->active = 0;
        if ((c->a == p_shoulder && c->b == p_pelvis)     ||
            (c->b == p_shoulder && c->a == p_pelvis))    c->active = 0;
        if ((c->a == p_shoulder && c->b == p_r_hip)      ||
            (c->b == p_shoulder && c->a == p_r_hip))     c->active = 0;
    }
    m->dismember_mask |= LIMB_L_ARM;

    /* Spew at the shoulder. */
    Vec2 sp = part_pos(w, m, PART_L_SHOULDER);
    Vec2 v0 = (Vec2){ -160.0f, -80.0f };
    for (int k = 0; k < 24; ++k) fx_spawn_blood(&w->fx, sp, v0, w->rng);
}

void mech_kill(World *w, int mid, int killshot_part, Vec2 dir, float impulse) {
    Mech *m = &w->mechs[mid];
    if (!m->alive) return;
    m->alive = false;
    /* Drop pose drive. The constraint solver still keeps the body
     * coherent; gravity now owns the trajectory. */
    clear_pose(m);

    /* Killshot impulse to the pelvis, not the hit part. The pelvis is
     * always connected to the rest of the body via active constraints,
     * so the impulse propagates through the skeleton and the body
     * ragdolls as a unit. Putting it on the hit part would let a
     * dismembered limb (e.g. L_ELBOW after a left-arm tear) take the
     * full kick by itself; with no body mass to absorb it the limb
     * flies clear across the level and pins against the world wall —
     * visually a long bone-shaped line from the corpse to the wall. */
    (void)killshot_part;
    int idx = m->particle_base + PART_PELVIS;
    physics_apply_impulse(&w->particles, idx,
        (Vec2){ dir.x * impulse, dir.y * impulse });

    snprintf(w->last_event, sizeof(w->last_event),
             "[KILL] mech #%d down", mid);
    w->last_event_time = 0.0f;
    w->hit_pause_ticks = 5;       /* ~83 ms at 60 Hz */
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.6f);
    LOG_I("mech_kill: id=%d (part=%d, impulse=%.1f, mask=0x%02x)",
          mid, killshot_part, impulse, m->dismember_mask);
}

bool mech_apply_damage(World *w, int mid, int part, float dmg, Vec2 dir) {
    Mech *m = &w->mechs[mid];
    if (!m->alive) return false;

    float final_dmg = dmg * hit_location_mult(part);
    m->health -= final_dmg;

    /* Limb HP — only L arm at M1 (per the roadmap). */
    if (part == PART_L_SHOULDER || part == PART_L_ELBOW || part == PART_L_HAND) {
        m->hp_arm_l -= final_dmg;
        if (m->hp_arm_l <= 0.0f && !(m->dismember_mask & LIMB_L_ARM)) {
            dismember_left_arm(w, m);
        }
    }

    /* Hit FX. */
    Vec2 hp = part_pos(w, m, part);
    for (int k = 0; k < 8; ++k) fx_spawn_blood(&w->fx, hp, dir, w->rng);
    for (int k = 0; k < 4; ++k) fx_spawn_spark(&w->fx, hp, dir, w->rng);

    if (m->health <= 0.0f) {
        mech_kill(w, mid, part, dir, 90.0f);
        return true;
    }

    /* Light hit-pause + shake on every solid hit. */
    w->hit_pause_ticks = 1;
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.12f);
    return false;
}
