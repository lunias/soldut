#include "mech.h"

#include "log.h"
#include "particle.h"
#include "physics.h"
#include "projectile.h"
#include "weapons.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ---- Chassis table -------------------------------------------------- */
/*
 * One physics model per game; chassis variants differ only by data
 * (mass, speed, fuel, health, bone lengths, passive). Each chassis maps
 * to the same 16-particle skeleton — only the bone REST lengths shift
 * with hitbox_scale.
 *
 * Numbers track documents/02-game-design.md §Mechs.
 */

static const Chassis g_chassis[CHASSIS_COUNT] = {
    [CHASSIS_TROOPER] = {
        .name = "Trooper",
        .run_mult = 1.00f, .jump_mult = 1.00f, .jet_mult = 1.00f,
        .fuel_max = 1.00f, .fuel_regen = 0.20f, .mass_scale = 1.00f,
        .health_max = 150.0f,
        .bone_arm = 14.0f, .bone_forearm = 16.0f,
        .bone_thigh = 18.0f, .bone_shin = 18.0f,
        .torso_h = 30.0f, .neck_h = 14.0f,
        .hitbox_scale = 1.00f,
        .passive = PASSIVE_TROOPER_FAST_RELOAD,
    },
    [CHASSIS_SCOUT] = {
        .name = "Scout",
        .run_mult = 1.20f, .jump_mult = 1.10f, .jet_mult = 1.30f,
        .fuel_max = 1.20f, .fuel_regen = 0.25f, .mass_scale = 0.80f,
        .health_max = 100.0f,
        .bone_arm = 12.0f, .bone_forearm = 14.0f,
        .bone_thigh = 16.0f, .bone_shin = 16.0f,
        .torso_h = 26.0f, .neck_h = 12.0f,
        .hitbox_scale = 0.85f,
        .passive = PASSIVE_SCOUT_DASH,
    },
    [CHASSIS_HEAVY] = {
        .name = "Heavy",
        .run_mult = 0.85f, .jump_mult = 0.85f, .jet_mult = 0.80f,
        .fuel_max = 0.85f, .fuel_regen = 0.15f, .mass_scale = 1.40f,
        .health_max = 220.0f,
        .bone_arm = 16.0f, .bone_forearm = 18.0f,
        .bone_thigh = 20.0f, .bone_shin = 20.0f,
        .torso_h = 34.0f, .neck_h = 14.0f,
        .hitbox_scale = 1.20f,
        .passive = PASSIVE_HEAVY_AOE_RESIST,
    },
    [CHASSIS_SNIPER] = {
        .name = "Sniper",
        .run_mult = 0.95f, .jump_mult = 1.00f, .jet_mult = 1.00f,
        .fuel_max = 1.10f, .fuel_regen = 0.20f, .mass_scale = 0.95f,
        .health_max = 130.0f,
        .bone_arm = 14.0f, .bone_forearm = 17.0f,
        .bone_thigh = 18.0f, .bone_shin = 18.0f,
        .torso_h = 30.0f, .neck_h = 14.0f,
        .hitbox_scale = 1.00f,
        .passive = PASSIVE_SNIPER_STEADY,
    },
    [CHASSIS_ENGINEER] = {
        .name = "Engineer",
        .run_mult = 1.00f, .jump_mult = 1.00f, .jet_mult = 1.10f,
        .fuel_max = 1.00f, .fuel_regen = 0.25f, .mass_scale = 1.00f,
        .health_max = 140.0f,
        .bone_arm = 14.0f, .bone_forearm = 16.0f,
        .bone_thigh = 18.0f, .bone_shin = 18.0f,
        .torso_h = 30.0f, .neck_h = 14.0f,
        .hitbox_scale = 1.00f,
        .passive = PASSIVE_ENGINEER_REPAIR,
    },
};

const Chassis *mech_chassis(ChassisId id) {
    if ((unsigned)id >= CHASSIS_COUNT) return &g_chassis[CHASSIS_TROOPER];
    return &g_chassis[id];
}

ChassisId chassis_id_from_name(const char *name) {
    if (!name) return CHASSIS_TROOPER;
    for (int i = 0; i < CHASSIS_COUNT; ++i) {
        if (strcasecmp(name, g_chassis[i].name) == 0) return (ChassisId)i;
    }
    LOG_W("chassis_id_from_name: unknown '%s' — defaulting to Trooper", name);
    return CHASSIS_TROOPER;
}

/* ---- Armor table --------------------------------------------------- */

static const Armor g_armors[ARMOR_COUNT] = {
    [ARMOR_NONE]     = { .name = "None",     .hp = 0.0f,  .absorb_ratio = 0.0f, .run_mult = 1.00f, .jet_mult = 1.00f, .reactive_charges = 0 },
    [ARMOR_LIGHT]    = { .name = "Light",    .hp = 30.0f, .absorb_ratio = 0.40f, .run_mult = 1.00f, .jet_mult = 1.00f, .reactive_charges = 0 },
    [ARMOR_HEAVY]    = { .name = "Heavy",    .hp = 75.0f, .absorb_ratio = 0.60f, .run_mult = 0.90f, .jet_mult = 0.90f, .reactive_charges = 0 },
    [ARMOR_REACTIVE] = { .name = "Reactive", .hp = 30.0f, .absorb_ratio = 0.40f, .run_mult = 1.00f, .jet_mult = 1.00f, .reactive_charges = 1 },
};

const Armor *armor_def(int id) {
    if ((unsigned)id >= ARMOR_COUNT) return &g_armors[ARMOR_NONE];
    return &g_armors[id];
}

/* ---- Jetpack table ------------------------------------------------- */

static const Jetpack g_jetpacks[JET_COUNT] = {
    [JET_NONE]       = { .name = "Baseline", .fuel_mult = 1.00f, .thrust_mult = 1.00f },
    [JET_STANDARD]   = { .name = "Standard", .fuel_mult = 1.20f, .thrust_mult = 1.10f },
    [JET_BURST]      = { .name = "Burst",    .fuel_mult = 1.00f, .thrust_mult = 1.00f,
                         .boost_fuel_cost = 0.30f, .boost_thrust_mult = 2.00f, .boost_duration = 0.40f },
    [JET_GLIDE_WING] = { .name = "Glide",    .fuel_mult = 0.70f, .thrust_mult = 0.85f,
                         .glide_thrust = 600.0f },
    [JET_JUMP_JET]   = { .name = "JumpJet",  .fuel_mult = 1.00f, .thrust_mult = 0.0f,
                         .jump_on_land = true },
};

const Jetpack *jetpack_def(int id) {
    if ((unsigned)id >= JET_COUNT) return &g_jetpacks[JET_NONE];
    return &g_jetpacks[id];
}

MechLoadout mech_default_loadout(void) {
    return (MechLoadout){
        .chassis_id   = CHASSIS_TROOPER,
        .primary_id   = WEAPON_PULSE_RIFLE,
        .secondary_id = WEAPON_SIDEARM,
        .armor_id     = ARMOR_LIGHT,
        .jetpack_id   = JET_STANDARD,
    };
}

/* ---- Movement tunables (Soldat-derived; see reference/soldat-constants.md) */
#define RUN_SPEED_PXS      280.0f
#define JUMP_IMPULSE_PXS   320.0f
#define JET_THRUST_PXS2    2200.0f
#define JET_DRAIN_PER_SEC  0.60f
#define AIR_CONTROL        0.35f
#define BINK_DECAY_PER_SEC 1.8f       /* exponential decay for aim_bink */
#define BINK_MAX           0.35f      /* clamp; ~20° */
#define SCOUT_DASH_PXS     720.0f
#define ENGINEER_HEAL      50.0f
#define ENGINEER_COOLDOWN  30.0f

/* ---- Helpers: particle/constraint reservation ---------------------- */

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

/* ---- mech_create --------------------------------------------------- */

int mech_create(World *w, ChassisId chassis_id, Vec2 spawn, int team, bool is_dummy) {
    MechLoadout lo = mech_default_loadout();
    lo.chassis_id = (int)chassis_id;
    return mech_create_loadout(w, lo, spawn, team, is_dummy);
}

int mech_create_loadout(World *w, MechLoadout lo, Vec2 spawn,
                        int team, bool is_dummy)
{
    if (w->mech_count >= MAX_MECHS) {
        LOG_E("mech_create: MAX_MECHS reached");
        return -1;
    }
    int pbase = particles_reserve(w, PART_COUNT);
    if (pbase < 0) return -1;

    const Chassis *ch = mech_chassis((ChassisId)lo.chassis_id);
    const Armor   *ar = armor_def(lo.armor_id);

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

    /* Constraints (same set as M1; per-chassis bone lengths flow from
     * the layout above). 21 distance constraints. */
    int cbase = constraints_reserve(w, 32);
    if (cbase < 0) return -1;
    int next = cbase;

    #define DIST(an, bn) add_distance(w, &next, pbase + (an), pbase + (bn), \
        sqrtf((P[an].x - P[bn].x)*(P[an].x - P[bn].x) + \
              (P[an].y - P[bn].y)*(P[an].y - P[bn].y)))

    DIST(PART_HEAD,        PART_NECK);
    DIST(PART_NECK,        PART_CHEST);
    DIST(PART_HEAD,        PART_CHEST);
    DIST(PART_CHEST,       PART_PELVIS);
    DIST(PART_CHEST,       PART_L_SHOULDER);
    DIST(PART_CHEST,       PART_R_SHOULDER);
    DIST(PART_PELVIS,      PART_L_HIP);
    DIST(PART_PELVIS,      PART_R_HIP);
    DIST(PART_L_SHOULDER,  PART_L_ELBOW);
    DIST(PART_L_ELBOW,     PART_L_HAND);
    DIST(PART_R_SHOULDER,  PART_R_ELBOW);
    DIST(PART_R_ELBOW,     PART_R_HAND);
    DIST(PART_L_HIP,       PART_L_KNEE);
    DIST(PART_L_KNEE,      PART_L_FOOT);
    DIST(PART_R_HIP,       PART_R_KNEE);
    DIST(PART_R_KNEE,      PART_R_FOOT);
    DIST(PART_L_SHOULDER,  PART_R_SHOULDER);
    DIST(PART_L_HIP,       PART_R_HIP);
    DIST(PART_L_SHOULDER,  PART_PELVIS);
    DIST(PART_R_SHOULDER,  PART_PELVIS);

    #undef DIST

    int constraint_count = next - cbase;
    w->constraints.count = next;

    /* Mech struct. */
    int mid = w->mech_count++;
    Mech *m = &w->mechs[mid];
    memset(m, 0, sizeof(*m));
    m->id              = mid;
    m->chassis_id      = (int)lo.chassis_id;
    m->team            = team;
    m->alive           = true;
    m->is_dummy        = is_dummy;
    m->particle_base   = (uint16_t)pbase;
    m->constraint_base = (uint16_t)cbase;
    m->constraint_count= (uint16_t)constraint_count;
    m->aim_world       = (Vec2){ spawn.x + 100.0f, spawn.y - 20.0f };
    m->health          = ch->health_max;
    m->health_max      = ch->health_max;

    /* Loadout. */
    m->primary_id      = lo.primary_id;
    m->secondary_id    = lo.secondary_id;
    m->active_slot     = 0;
    const Weapon *pw = weapon_def(m->primary_id);
    const Weapon *sw = weapon_def(m->secondary_id);
    m->ammo_primary    = pw ? pw->mag_size : 0;
    m->ammo_secondary  = sw ? sw->mag_size : 0;
    m->weapon_id       = m->primary_id;
    m->ammo            = m->ammo_primary;
    m->ammo_max        = pw ? pw->mag_size : 0;

    /* Armor. */
    m->armor_id        = lo.armor_id;
    m->armor_hp        = ar->hp;
    m->armor_hp_max    = ar->hp;
    m->armor_charges   = ar->reactive_charges;

    /* Jetpack module + adjusted fuel cap. */
    m->jetpack_id      = lo.jetpack_id;
    const Jetpack *jp  = jetpack_def(m->jetpack_id);
    m->fuel_max        = ch->fuel_max * jp->fuel_mult;
    m->fuel            = m->fuel_max;
    m->boost_timer     = 0.0f;
    m->jump_armed      = true;

    /* Per-limb HP. */
    m->hp_arm_l        = 80.0f;
    m->hp_arm_r        = 80.0f;
    m->hp_leg_l        = 80.0f;
    m->hp_leg_r        = 80.0f;
    m->hp_head         = 50.0f;
    m->facing_left     = false;
    m->aim_bink        = 0.0f;
    m->ability_cooldown = 0.0f;

    LOG_I("mech_create: id=%d chassis=%s%s armor=%s jet=%s primary=%s secondary=%s",
          mid, ch->name, is_dummy ? " (dummy)" : "",
          ar->name, jp->name,
          pw ? pw->name : "?", sw ? sw->name : "?");
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

void mech_apply_bink(Mech *m, float bink_amount, float proximity_t,
                     pcg32_t *rng)
{
    if (!m->alive || bink_amount <= 0.0f) return;
    if (proximity_t < 0.0f) proximity_t = 0.0f;
    if (proximity_t > 1.0f) proximity_t = 1.0f;
    float sign = ((pcg32_next(rng) & 1u) ? 1.0f : -1.0f);
    m->aim_bink += sign * bink_amount * (0.5f + 0.5f * proximity_t);
    if (m->aim_bink >  BINK_MAX) m->aim_bink =  BINK_MAX;
    if (m->aim_bink < -BINK_MAX) m->aim_bink = -BINK_MAX;
}

/* ---- Pose drive (animation) --------------------------------------- */

static void clear_pose(Mech *m) {
    for (int i = 0; i < PART_COUNT; ++i) m->pose_strength[i] = 0.0f;
}

static void pose_set(Mech *m, int part, Vec2 target, float strength) {
    m->pose_target  [part] = target;
    m->pose_strength[part] = strength;
}

static void apply_pose_to_particles(World *w, Mech *m) {
    ParticlePool *p = &w->particles;
    for (int i = 0; i < PART_COUNT; ++i) {
        float s = m->pose_strength[i];
        if (s <= 0.0f) continue;
        int idx = m->particle_base + i;
        float dx = (m->pose_target[i].x - p->pos_x[idx]) * s;
        float dy = (m->pose_target[i].y - p->pos_y[idx]) * s;
        physics_translate_kinematic_swept(p, &w->level, idx, dx, dy);
    }
}

static void build_pose(const Chassis *ch, World *w, Mech *m, float dt) {
    clear_pose(m);
    if (!m->alive) return;

    int b = m->particle_base;
    ParticlePool *p = &w->particles;
    Vec2 aim_dir = mech_aim_dir(w, m->id);
    m->facing_left = aim_dir.x < 0.0f;

    float pelvis_x = p->pos_x[b + PART_PELVIS];
    float pelvis_y = p->pos_y[b + PART_PELVIS];
    float chain_len = ch->bone_thigh + ch->bone_shin;
    if (m->grounded) {
        float foot_y_avg = (p->pos_y[b + PART_L_FOOT] +
                            p->pos_y[b + PART_R_FOOT]) * 0.5f;
        pelvis_y = foot_y_avg - chain_len;
    }
    Vec2 pelvis = { pelvis_x, pelvis_y };

    pose_set(m, PART_CHEST,
        (Vec2){ pelvis.x, pelvis.y - ch->torso_h }, 0.7f);
    pose_set(m, PART_NECK,
        (Vec2){ pelvis.x, pelvis.y - ch->torso_h - ch->neck_h * 0.5f }, 0.7f);
    pose_set(m, PART_HEAD,
        (Vec2){ pelvis.x, pelvis.y - ch->torso_h - ch->neck_h - 8.0f }, 0.7f);

    pose_set(m, PART_L_HIP, (Vec2){ pelvis.x - 7, pelvis.y }, 0.7f);
    pose_set(m, PART_R_HIP, (Vec2){ pelvis.x + 7, pelvis.y }, 0.7f);

    pose_set(m, PART_L_SHOULDER,
        (Vec2){ pelvis.x - 10, pelvis.y - ch->torso_h + 4 }, 0.7f);
    pose_set(m, PART_R_SHOULDER,
        (Vec2){ pelvis.x + 10, pelvis.y - ch->torso_h + 4 }, 0.7f);

    float arm_reach = ch->bone_arm + ch->bone_forearm;
    if (!m->is_dummy) {
        Vec2 r_sho = (Vec2){ pelvis.x + 10, pelvis.y - ch->torso_h + 4 };
        pose_set(m, PART_R_HAND,
            (Vec2){ r_sho.x + aim_dir.x * arm_reach,
                    r_sho.y + aim_dir.y * arm_reach }, 0.7f);
    }
    (void)arm_reach;

    Vec2 lhip = { pelvis.x - 7, pelvis.y };
    Vec2 rhip = { pelvis.x + 7, pelvis.y };
    float leg_strength = 0.5f;

    switch ((AnimId)m->anim_id) {
        case ANIM_RUN: {
            m->anim_time += dt;
            const float stride     = 28.0f;
            const float lift_h     = 9.0f;
            const float run_v      = RUN_SPEED_PXS * ch->run_mult;
            float       cycle_freq = run_v / (2.0f * stride);

            float dir   = m->facing_left ? -1.0f : 1.0f;
            float front = stride * 0.5f * dir;
            float back  = -stride * 0.5f * dir;
            float foot_y_ground = lhip.y + ch->bone_thigh + ch->bone_shin;

            float p_l = m->anim_time * cycle_freq;
            p_l -= floorf(p_l);
            float p_r = p_l + 0.5f;
            if (p_r >= 1.0f) p_r -= 1.0f;

            float l_fx, l_fy, r_fx, r_fy;
            if (p_l < 0.5f) {
                float u = p_l * 2.0f;
                l_fx = lhip.x + front + (back - front) * u;
                l_fy = foot_y_ground;
            } else {
                float u = (p_l - 0.5f) * 2.0f;
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

static void apply_run_velocity(World *w, const Mech *m, float vx_pxs, float dt, bool grounded) {
    ParticlePool *p = &w->particles;
    float vx_per_tick = vx_pxs * dt;
    if (!grounded) vx_per_tick *= AIR_CONTROL;
    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        physics_set_velocity_x(p, idx, vx_per_tick);
    }
}

static void apply_jump(World *w, const Mech *m, float jump_pxs, float dt) {
    ParticlePool *p = &w->particles;
    float vy_per_tick = -jump_pxs * dt;
    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        physics_set_velocity_y(p, idx, vy_per_tick);
    }
    SHOT_LOG("t=%llu mech=%d jump impulse=%.0f vy/tick=%.3f",
             (unsigned long long)w->tick, m->id, jump_pxs, vy_per_tick);
}

#define JET_CEILING_TAPER_BEGIN  64.0f
#define JET_CEILING_TAPER_END    24.0f

static void apply_jet_force(World *w, const Mech *m, float thrust_pxs2, float dt) {
    ParticlePool *p = &w->particles;
    int b = m->particle_base;
    float head_y = p->pos_y[b + PART_HEAD];
    float scale  = 1.0f;
    if (head_y < JET_CEILING_TAPER_BEGIN) {
        if (head_y <= JET_CEILING_TAPER_END) scale = 0.0f;
        else scale = (head_y - JET_CEILING_TAPER_END) /
                     (JET_CEILING_TAPER_BEGIN - JET_CEILING_TAPER_END);
        SHOT_LOG("t=%llu mech=%d jet_taper head_y=%.1f scale=%.2f",
                 (unsigned long long)w->tick, m->id, head_y, scale);
    }
    if (scale <= 0.0f) return;
    float dy = -thrust_pxs2 * scale * dt * dt;
    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        p->pos_y[idx] += dy;
    }
}

/* Reload: kicks off if magazine is empty AND the weapon uses a mag. */
static void maybe_start_reload(Mech *m, const Weapon *wpn, const Chassis *ch) {
    if (!wpn) return;
    if (wpn->mag_size <= 0) return;     /* knife / grenades have no reload */
    if (m->reload_timer > 0.0f) return; /* already reloading */
    if (m->ammo > 0) return;            /* still ammo left */
    float mult = (ch->passive == PASSIVE_TROOPER_FAST_RELOAD) ? 0.75f : 1.0f;
    m->reload_timer = wpn->reload_sec * mult;
}

/* Flip the active weapon slot. Cancels any in-progress reload (per
 * documents/04-combat.md §"Reloading" — switching to secondary cancels
 * the reload and the magazine is dropped). */
static void swap_weapon(Mech *m) {
    /* Stash the current ammo back into its slot. */
    if (m->active_slot == 0) m->ammo_primary   = m->ammo;
    else                     m->ammo_secondary = m->ammo;

    m->active_slot ^= 1;
    int new_id = (m->active_slot == 0) ? m->primary_id : m->secondary_id;
    int new_ammo = (m->active_slot == 0) ? m->ammo_primary : m->ammo_secondary;
    const Weapon *wpn = weapon_def(new_id);
    m->weapon_id    = new_id;
    m->ammo         = new_ammo;
    m->ammo_max     = wpn ? wpn->mag_size : 0;
    m->reload_timer = 0.0f;
    m->charge_timer = 0.0f;
    m->spinup_timer = 0.0f;
}

void mech_step_drive(World *w, int mid, ClientInput in, float dt) {
    Mech *m = &w->mechs[mid];
    const Chassis *ch = mech_chassis((ChassisId)m->chassis_id);
    const Armor   *ar = armor_def(m->armor_id);
    const Jetpack *jp = jetpack_def(m->jetpack_id);

    if (in.aim_x != 0.0f || in.aim_y != 0.0f) {
        m->aim_world = (Vec2){ in.aim_x, in.aim_y };
    }

    /* Edge-detect for press events (swap, dash, use). */
    uint16_t pressed = (uint16_t)((~m->prev_buttons) & in.buttons);

    if (m->alive && !m->is_dummy) {
        bool grounded = any_foot_grounded(w, m);
        m->grounded = grounded;

        if (pressed & BTN_SWAP) swap_weapon(m);

        /* Engineer ability — drop a small repair pack on yourself.
         * A real "deploy on the ground" pickup wires up at M5; for now
         * the active ability heals the user. */
        if ((pressed & BTN_USE) && ch->passive == PASSIVE_ENGINEER_REPAIR
            && m->ability_cooldown <= 0.0f) {
            m->health = fminf(m->health_max, m->health + ENGINEER_HEAL);
            m->ability_cooldown = ENGINEER_COOLDOWN;
            SHOT_LOG("t=%llu mech=%d engineer_repair +%.0f hp",
                     (unsigned long long)w->tick, mid, ENGINEER_HEAL);
        }
        if (m->ability_cooldown > 0.0f) m->ability_cooldown -= dt;

        bool moving = false;
        float run_speed = RUN_SPEED_PXS * ch->run_mult * ar->run_mult;
        if (in.buttons & BTN_LEFT) {
            apply_run_velocity(w, m, -run_speed, dt, grounded);
            moving = true;
        }
        if (in.buttons & BTN_RIGHT) {
            apply_run_velocity(w, m,  run_speed, dt, grounded);
            moving = true;
        }
        if (grounded && !moving) {
            apply_run_velocity(w, m, 0.0f, dt, true);
        }

        /* Scout dash: BTN_DASH while grounded gives a one-shot horizontal
         * burst. (Air dash is a stretch — would need its own cooldown.) */
        if ((pressed & BTN_DASH) && grounded
            && ch->passive == PASSIVE_SCOUT_DASH) {
            float vx = m->facing_left ? -SCOUT_DASH_PXS : SCOUT_DASH_PXS;
            apply_run_velocity(w, m, vx, dt, true);
            moving = true;
            SHOT_LOG("t=%llu mech=%d scout_dash vx=%.0f",
                     (unsigned long long)w->tick, mid, vx);
        }

        /* Jet handling — varies by jetpack module. */
        bool jetting = false;
        float effective_thrust = JET_THRUST_PXS2 * ch->jet_mult * ar->jet_mult * jp->thrust_mult;

        if (jp->jump_on_land) {
            /* JET_JUMP_JET: BTN_JET acts as a re-jump (consumes a chunk
             * of fuel). Continuous thrust disabled. */
            if ((pressed & BTN_JET) && m->fuel > 0.05f) {
                apply_jump(w, m, JUMP_IMPULSE_PXS * ch->jump_mult * 1.05f, dt);
                m->fuel -= 0.10f;
                if (m->fuel < 0.0f) m->fuel = 0.0f;
            }
        } else {
            /* Standard / burst / glide. */
            jetting = (in.buttons & BTN_JET) && (m->fuel > 0.0f);
            if (jetting) {
                /* Burst: BTN_DASH triggers a temporary multiplier. */
                if (jp->boost_thrust_mult > 0.0f && m->boost_timer > 0.0f) {
                    apply_jet_force(w, m,
                        effective_thrust * jp->boost_thrust_mult, dt);
                } else {
                    apply_jet_force(w, m, effective_thrust, dt);
                }
                m->fuel -= JET_DRAIN_PER_SEC * dt;
                if (m->fuel < 0.0f) m->fuel = 0.0f;
            } else if (jp->glide_thrust > 0.0f && (in.buttons & BTN_JET)) {
                /* Glide wing: lift even at empty fuel. */
                ParticlePool *p = &w->particles;
                float dy = -jp->glide_thrust * dt * dt;
                for (int part = 0; part < PART_COUNT; ++part) {
                    p->pos_y[m->particle_base + part] += dy;
                }
            } else if (grounded) {
                m->fuel += ch->fuel_regen * dt;
                if (m->fuel > m->fuel_max) m->fuel = m->fuel_max;
            }

            /* Burst boost trigger. */
            if (jp->boost_thrust_mult > 0.0f && (pressed & BTN_DASH)
                && m->boost_timer <= 0.0f && m->fuel > jp->boost_fuel_cost * m->fuel_max) {
                m->boost_timer = jp->boost_duration;
                m->fuel -= jp->boost_fuel_cost * m->fuel_max;
                SHOT_LOG("t=%llu mech=%d jet_burst",
                         (unsigned long long)w->tick, mid);
            }
            if (m->boost_timer > 0.0f) m->boost_timer -= dt;
        }

        if ((in.buttons & BTN_JUMP) && grounded) {
            apply_jump(w, m, JUMP_IMPULSE_PXS * ch->jump_mult, dt);
        }

        if (jetting)        m->anim_id = ANIM_JET;
        else if (!grounded) m->anim_id = ANIM_FALL;
        else if (moving)    m->anim_id = ANIM_RUN;
        else                m->anim_id = ANIM_STAND;

    } else if (m->alive && m->is_dummy) {
        m->anim_id = ANIM_STAND;
        m->grounded = any_foot_grounded(w, m);
    }

    /* Cooldowns + bink decay. */
    if (m->fire_cooldown > 0.0f) m->fire_cooldown -= dt;
    if (m->recoil_kick   > 0.0f) m->recoil_kick   -= dt * 4.0f;
    if (m->aim_bink != 0.0f) {
        float decay = expf(-BINK_DECAY_PER_SEC * dt);
        m->aim_bink *= decay;
        if (fabsf(m->aim_bink) < 1e-4f) m->aim_bink = 0.0f;
    }

    /* Reload. The active weapon's reload kicks off when the mag is
     * empty (set during fire). It can also be commanded with BTN_RELOAD,
     * but the doc explicitly forbids reload-cancel-into-fire so we just
     * trigger a fresh reload and consume the press. */
    const Weapon *wpn_active = weapon_def(m->weapon_id);
    if (m->reload_timer > 0.0f) {
        m->reload_timer -= dt;
        if (m->reload_timer <= 0.0f) {
            m->reload_timer = 0.0f;
            if (wpn_active) m->ammo = wpn_active->mag_size;
            if (m->active_slot == 0) m->ammo_primary   = m->ammo;
            else                     m->ammo_secondary = m->ammo;
        }
    } else {
        if ((pressed & BTN_RELOAD) && wpn_active && wpn_active->mag_size > 0
            && m->ammo < wpn_active->mag_size) {
            float mult = (ch->passive == PASSIVE_TROOPER_FAST_RELOAD) ? 0.75f : 1.0f;
            m->reload_timer = wpn_active->reload_sec * mult;
        } else {
            maybe_start_reload(m, wpn_active, ch);
        }
    }

    /* prev_buttons gets updated AFTER mech_try_fire (in simulate.c)
     * so edge-detection inside the fire path sees the same edge that
     * mech_step_drive saw above. Otherwise BTN_FIRE on its first tick
     * would be eaten here and the fire path would never see it. */

    build_pose(ch, w, m, dt);
    apply_pose_to_particles(w, m);
}

/* Latch this tick's input into prev_buttons. Called by simulate AFTER
 * mech_try_fire so both passes share the same "is this an edge?" view. */
void mech_latch_prev_buttons(World *w, int mid) {
    Mech *m = &w->mechs[mid];
    m->prev_buttons = m->latched_input.buttons;
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
    if (m->anim_id != ANIM_STAND && m->anim_id != ANIM_RUN) return;

    const Chassis *ch = mech_chassis((ChassisId)m->chassis_id);
    float foot_y = (p->pos_y[b + PART_L_FOOT] + p->pos_y[b + PART_R_FOOT]) * 0.5f;

    float pelvis_y_target = foot_y - ch->bone_thigh - ch->bone_shin;
    float knee_y_target   = foot_y - ch->bone_shin;
    float dy_pelvis = pelvis_y_target - p->pos_y[b + PART_PELVIS];

    if (dy_pelvis >= -0.1f) return;

    if (dy_pelvis < -1.5f) {
        SHOT_LOG("t=%llu mech=%d anchor anim=%d dy_pelvis=%.2f",
                 (unsigned long long)w->tick, m->id, m->anim_id, dy_pelvis);
    }

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
        p->prev_y[idx] = p->pos_y[idx];
    }

    if (m->anim_id == ANIM_STAND) {
        float dy_knee_l = knee_y_target - p->pos_y[b + PART_L_KNEE];
        float dy_knee_r = knee_y_target - p->pos_y[b + PART_R_KNEE];
        physics_translate_kinematic(p, b + PART_L_KNEE, 0.0f, dy_knee_l);
        physics_translate_kinematic(p, b + PART_R_KNEE, 0.0f, dy_knee_r);
        p->prev_y[b + PART_L_KNEE] = p->pos_y[b + PART_L_KNEE];
        p->prev_y[b + PART_R_KNEE] = p->pos_y[b + PART_R_KNEE];
    }
}

/* ---- Try-fire: dispatched on weapon class -------------------------- */

bool mech_try_fire(World *w, int mid, ClientInput in) {
    Mech *m = &w->mechs[mid];
    if (!m->alive || m->is_dummy)            return false;
    const Weapon *wpn = weapon_def(m->weapon_id);
    if (!wpn) return false;
    if (m->fire_cooldown > 0.0f)             return false;
    if (m->reload_timer  > 0.0f)             return false;

    bool fire_held    = (in.buttons & BTN_FIRE) != 0;
    bool fire_pressed = ((~m->prev_buttons) & in.buttons & BTN_FIRE) != 0;

    /* Charged weapons (Rail Cannon, Microgun spin-up) need the trigger
     * to be HELD for `charge_sec` before they fire. */
    if (wpn->charge_sec > 0.0f) {
        if (!fire_held) {
            m->charge_timer = 0.0f;
            return false;
        }
        m->charge_timer += in.dt > 0 ? in.dt : 1.0f / 60.0f;
        if (m->charge_timer < wpn->charge_sec) return false;
        /* Microgun: keep charging "spun up" between shots so sustained
         * fire doesn't restart the spin every shot. We zero only on
         * release. */
    }

    if (wpn->fire == WFIRE_HITSCAN) {
        if (!fire_held) return false;
        if (wpn->mag_size > 0 && m->ammo <= 0) return false;
        weapons_fire_hitscan(w, mid);
        if (wpn->mag_size > 0) m->ammo--;
    }
    else if (wpn->fire == WFIRE_PROJECTILE) {
        if (!fire_held) return false;
        if (wpn->mag_size > 0 && m->ammo <= 0) return false;
        weapons_spawn_projectiles(w, mid, m->weapon_id);
        if (wpn->mag_size > 0) m->ammo--;
    }
    else if (wpn->fire == WFIRE_SPREAD) {
        if (!fire_pressed) return false;
        if (wpn->mag_size > 0 && m->ammo <= 0) return false;
        weapons_spawn_projectiles(w, mid, m->weapon_id);
        if (wpn->mag_size > 0) m->ammo--;
    }
    else if (wpn->fire == WFIRE_BURST) {
        /* Burst fire: spawn `burst_rounds` projectiles spaced by
         * burst_interval_sec on the same trigger pull. We fire all of
         * them on press; the visible cadence comes from the renderer
         * but for damage purposes they all hit on the same tick. (A
         * proper implementation would queue rounds for future ticks;
         * acceptable for M3 first pass.) */
        if (!fire_pressed) return false;
        if (wpn->mag_size > 0 && m->ammo < wpn->burst_rounds) return false;
        for (int b = 0; b < wpn->burst_rounds; ++b) {
            weapons_spawn_projectiles(w, mid, m->weapon_id);
        }
        if (wpn->mag_size > 0) m->ammo -= wpn->burst_rounds;
    }
    else if (wpn->fire == WFIRE_THROW) {
        /* Frag grenades: each press throws one. */
        if (!fire_pressed) return false;
        if (wpn->mag_size > 0 && m->ammo <= 0) return false;
        weapons_spawn_projectiles(w, mid, m->weapon_id);
        if (wpn->mag_size > 0) m->ammo--;
    }
    else if (wpn->fire == WFIRE_MELEE) {
        if (!fire_pressed) return false;
        weapons_fire_melee(w, mid, m->weapon_id);
        m->fire_cooldown = wpn->fire_rate_sec;
    }
    else if (wpn->fire == WFIRE_GRAPPLE) {
        /* Grappling hook: tagged in TRADE_OFFS as deferred. We still
         * play the cooldown so a player pressing fire isn't surprised
         * by silence. */
        if (!fire_pressed) return false;
        m->fire_cooldown = wpn->fire_rate_sec;
        SHOT_LOG("t=%llu mech=%d grapple_attempt (NOT YET IMPLEMENTED)",
                 (unsigned long long)w->tick, mid);
    }

    if (m->active_slot == 0) m->ammo_primary   = m->ammo;
    else                     m->ammo_secondary = m->ammo;
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

/* Map a body part to its parent limb (LIMB_*), or 0 for torso parts
 * that don't dismember. */
static int part_to_limb(int part) {
    switch (part) {
        case PART_HEAD:                                  return LIMB_HEAD;
        case PART_L_SHOULDER: case PART_L_ELBOW:
        case PART_L_HAND:                                return LIMB_L_ARM;
        case PART_R_SHOULDER: case PART_R_ELBOW:
        case PART_R_HAND:                                return LIMB_R_ARM;
        case PART_L_HIP: case PART_L_KNEE: case PART_L_FOOT: return LIMB_L_LEG;
        case PART_R_HIP: case PART_R_KNEE: case PART_R_FOOT: return LIMB_R_LEG;
        default:                                         return 0;
    }
}

/* Deactivate every constraint that touches `target_part` AND a particle
 * on the *parent* side of the dismembered limb. We pass in a list of
 * "kept-side" particles so the limb's internal sticks survive (the gib
 * stays connected) while the connection to the rest of the body is cut. */
static void cut_constraints_between(World *w, Mech *m,
                                    const int *limb_parts, int n_limb,
                                    const int *kept_parts, int n_kept)
{
    int base = m->particle_base;
    for (int i = 0; i < m->constraint_count; ++i) {
        Constraint *c = &w->constraints.items[m->constraint_base + i];
        if (!c->active) continue;
        bool a_in_limb = false, b_in_limb = false;
        bool a_in_kept = false, b_in_kept = false;
        for (int k = 0; k < n_limb; ++k) {
            if (c->a == base + limb_parts[k]) a_in_limb = true;
            if (c->b == base + limb_parts[k]) b_in_limb = true;
        }
        for (int k = 0; k < n_kept; ++k) {
            if (c->a == base + kept_parts[k]) a_in_kept = true;
            if (c->b == base + kept_parts[k]) b_in_kept = true;
        }
        /* Cut constraints that span limb↔kept. Constraints internal to
         * the limb (both endpoints in limb_parts) survive. */
        if ((a_in_limb && b_in_kept) || (a_in_kept && b_in_limb)) {
            c->active = 0;
        }
    }
}

/* Spawn an exuberant blood spew at the joint to sell the dismemberment. */
static void blood_spew_at(World *w, Vec2 at, Vec2 base_dir) {
    for (int k = 0; k < 32; ++k) fx_spawn_blood(&w->fx, at, base_dir, w->rng);
}

void mech_dismember(World *w, int mid, int limb) {
    Mech *m = &w->mechs[mid];
    if (m->dismember_mask & limb) return;     /* already gone */
    int base = m->particle_base;
    /* All particles in the mech (used to compute kept-side per limb). */
    static const int all_parts[] = {
        PART_HEAD, PART_NECK, PART_CHEST, PART_PELVIS,
        PART_L_SHOULDER, PART_L_ELBOW, PART_L_HAND,
        PART_R_SHOULDER, PART_R_ELBOW, PART_R_HAND,
        PART_L_HIP, PART_L_KNEE, PART_L_FOOT,
        PART_R_HIP, PART_R_KNEE, PART_R_FOOT,
    };
    const int N_ALL = (int)(sizeof(all_parts) / sizeof(all_parts[0]));

    int limb_parts[8]; int n_limb = 0;
    int joint_part = PART_CHEST;
    switch (limb) {
        case LIMB_HEAD:
            limb_parts[n_limb++] = PART_HEAD;
            joint_part = PART_NECK;
            break;
        case LIMB_L_ARM:
            limb_parts[n_limb++] = PART_L_SHOULDER;
            limb_parts[n_limb++] = PART_L_ELBOW;
            limb_parts[n_limb++] = PART_L_HAND;
            joint_part = PART_L_SHOULDER;
            break;
        case LIMB_R_ARM:
            limb_parts[n_limb++] = PART_R_SHOULDER;
            limb_parts[n_limb++] = PART_R_ELBOW;
            limb_parts[n_limb++] = PART_R_HAND;
            joint_part = PART_R_SHOULDER;
            break;
        case LIMB_L_LEG:
            limb_parts[n_limb++] = PART_L_HIP;
            limb_parts[n_limb++] = PART_L_KNEE;
            limb_parts[n_limb++] = PART_L_FOOT;
            joint_part = PART_L_HIP;
            break;
        case LIMB_R_LEG:
            limb_parts[n_limb++] = PART_R_HIP;
            limb_parts[n_limb++] = PART_R_KNEE;
            limb_parts[n_limb++] = PART_R_FOOT;
            joint_part = PART_R_HIP;
            break;
        default:
            return;
    }

    /* Kept = all parts that aren't in this limb. */
    int kept_parts[16]; int n_kept = 0;
    for (int i = 0; i < N_ALL; ++i) {
        bool in_limb = false;
        for (int j = 0; j < n_limb; ++j) {
            if (all_parts[i] == limb_parts[j]) { in_limb = true; break; }
        }
        if (!in_limb) kept_parts[n_kept++] = all_parts[i];
    }

    cut_constraints_between(w, m, limb_parts, n_limb, kept_parts, n_kept);
    m->dismember_mask |= (uint8_t)limb;

    Vec2 sp = (Vec2){ w->particles.pos_x[base + joint_part],
                      w->particles.pos_y[base + joint_part] };
    blood_spew_at(w, sp, (Vec2){ -120.0f, -60.0f });

    LOG_I("mech %d dismember limb=0x%02x at (%.1f,%.1f)",
          mid, limb, sp.x, sp.y);
    SHOT_LOG("t=%llu mech=%d dismember limb=0x%02x at (%.1f,%.1f) mask=0x%02x",
             (unsigned long long)w->tick, mid, limb, sp.x, sp.y,
             m->dismember_mask);
}

void mech_kill(World *w, int mid, int killshot_part, Vec2 dir,
               float impulse, int killer_mech_id, int weapon_id)
{
    Mech *m = &w->mechs[mid];
    if (!m->alive) return;
    m->alive = false;
    clear_pose(m);
    (void)killshot_part;

    /* Apply impulse to pelvis so the body ragdolls as a unit. */
    int idx = m->particle_base + PART_PELVIS;
    physics_apply_impulse(&w->particles, idx,
        (Vec2){ dir.x * impulse, dir.y * impulse });

    /* ---- Kill-feed entry. */
    uint32_t flags = 0;
    if (killshot_part == PART_HEAD) flags |= KILLFLAG_HEADSHOT;
    if (limb_count(m->dismember_mask) >= 2) flags |= KILLFLAG_GIB;
    if (m->last_damage_taken >= 200.0f)     flags |= KILLFLAG_OVERKILL;
    if (!m->grounded)                        flags |= KILLFLAG_RAGDOLL;
    if (killer_mech_id == mid)               flags |= KILLFLAG_SUICIDE;

    int slot = w->killfeed_count % KILLFEED_CAPACITY;
    w->killfeed[slot] = (KillFeedEntry){
        .killer_mech_id = killer_mech_id,
        .victim_mech_id = mid,
        .weapon_id      = weapon_id,
        .flags          = flags,
        .age            = 0.0f,
    };
    w->killfeed_count++;

    snprintf(w->last_event, sizeof(w->last_event),
             "[KILL] mech #%d down", mid);
    w->last_event_time = 0.0f;
    w->hit_pause_ticks = 5;
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.6f);
    LOG_I("mech_kill: id=%d (part=%d, impulse=%.1f, mask=0x%02x, weapon=%d, by=%d)",
          mid, killshot_part, impulse, m->dismember_mask, weapon_id, killer_mech_id);
    SHOT_LOG("t=%llu mech=%d kill killshot_part=%d dir=(%.2f,%.2f) impulse=%.1f mask=0x%02x weapon=%d by=%d flags=0x%x",
             (unsigned long long)w->tick, mid, killshot_part,
             dir.x, dir.y, impulse, m->dismember_mask,
             weapon_id, killer_mech_id, flags);
}

bool mech_apply_damage(World *w, int mid, int part, float dmg, Vec2 dir,
                       int shooter_mech_id)
{
    Mech *m = &w->mechs[mid];
    if (!m->alive) return false;

    /* Friendly-fire gating. Self-damage (e.g. own rocket splash) always
     * goes through; friendly damage is dropped when FF is off. */
    if (shooter_mech_id >= 0 && shooter_mech_id != mid) {
        const Mech *shooter = &w->mechs[shooter_mech_id];
        if (shooter->team == m->team && !w->friendly_fire) {
            return false;
        }
    }

    float final_dmg = dmg * hit_location_mult(part);

    /* Armor absorption. Non-reactive armor splits damage by absorb_ratio
     * until its HP drains, then falls off. (Reactive is handled by the
     * explosion path, which eats one charge for full negation; bullets
     * still go through reactive at standard absorb.) */
    if (m->armor_id != ARMOR_NONE && m->armor_hp > 0.0f) {
        const Armor *ar = armor_def(m->armor_id);
        float absorbed = final_dmg * ar->absorb_ratio;
        if (absorbed > m->armor_hp) absorbed = m->armor_hp;
        m->armor_hp -= absorbed;
        final_dmg   -= absorbed;
        if (m->armor_hp <= 0.0f) {
            m->armor_hp = 0.0f;
            m->armor_id = ARMOR_NONE;
            m->armor_hp_max = 0.0f;
            SHOT_LOG("t=%llu mech=%d armor_break", (unsigned long long)w->tick, mid);
        }
    }

    if (final_dmg <= 0.0f) {
        /* Light hit feedback even when armor ate it. */
        Vec2 hp = part_pos(w, m, part);
        for (int k = 0; k < 4; ++k) fx_spawn_spark(&w->fx, hp, dir, w->rng);
        return false;
    }

    m->health -= final_dmg;
    m->last_damage_taken = final_dmg;
    SHOT_LOG("t=%llu mech=%d damage part=%d dmg=%.1f hp=%.1f/%.1f mask=0x%02x",
             (unsigned long long)w->tick, mid, part, final_dmg,
             m->health, m->health_max, m->dismember_mask);

    /* Per-limb HP tracking. The limb takes the same damage that hit
     * the chassis. When it drops to zero it dismembers. (See
     * documents/02-game-design.md §"Health, damage, and death".) */
    int limb = part_to_limb(part);
    if (limb) {
        float *limb_hp = NULL;
        switch (limb) {
            case LIMB_HEAD:  limb_hp = &m->hp_head;  break;
            case LIMB_L_ARM: limb_hp = &m->hp_arm_l; break;
            case LIMB_R_ARM: limb_hp = &m->hp_arm_r; break;
            case LIMB_L_LEG: limb_hp = &m->hp_leg_l; break;
            case LIMB_R_LEG: limb_hp = &m->hp_leg_r; break;
        }
        if (limb_hp) {
            *limb_hp -= final_dmg;
            if (*limb_hp <= 0.0f && !(m->dismember_mask & limb)) {
                mech_dismember(w, mid, limb);
                /* HEAD detach is also lethal — losing your head ends the
                 * mech regardless of remaining HP. */
                if (limb == LIMB_HEAD && m->health > 0.0f) {
                    m->health = 0.0f;
                }
            }
        }
    }

    /* Hit FX. */
    Vec2 hp = part_pos(w, m, part);
    for (int k = 0; k < 8; ++k) fx_spawn_blood(&w->fx, hp, dir, w->rng);
    for (int k = 0; k < 4; ++k) fx_spawn_spark(&w->fx, hp, dir, w->rng);

    if (m->health <= 0.0f) {
        mech_kill(w, mid, part, dir, 90.0f, shooter_mech_id,
                  m->last_killshot_weapon);
        return true;
    }

    w->hit_pause_ticks = 1;
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.12f);
    return false;
}
