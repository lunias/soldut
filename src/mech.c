#include "mech.h"

#include "ctf.h"
#include "level.h"
#include "log.h"
#include "particle.h"
#include "physics.h"
#include "pickup.h"
#include "projectile.h"
#include "weapon_sprites.h"
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

/* Per-chassis bone lengths drive both the constraint rest lengths
 * (set in mech_create_loadout via add_distance) and the sprite atlas
 * sub-rect sizes (mech_sprites.c). Heavy is visibly bigger, Scout
 * smaller, Sniper has long forearms + tall stance. Numbers per
 * documents/m5/12-rigging-and-damage.md §"Per-chassis bone structures". */
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
        .bone_arm = 11.0f, .bone_forearm = 13.0f,
        .bone_thigh = 14.0f, .bone_shin = 14.0f,
        .torso_h = 24.0f, .neck_h = 12.0f,
        .hitbox_scale = 0.85f,
        .passive = PASSIVE_SCOUT_DASH,
    },
    [CHASSIS_HEAVY] = {
        .name = "Heavy",
        .run_mult = 0.85f, .jump_mult = 0.85f, .jet_mult = 0.80f,
        .fuel_max = 0.85f, .fuel_regen = 0.15f, .mass_scale = 1.40f,
        .health_max = 220.0f,
        .bone_arm = 17.0f, .bone_forearm = 18.0f,
        .bone_thigh = 20.0f, .bone_shin = 20.0f,
        .torso_h = 38.0f, .neck_h = 16.0f,
        .hitbox_scale = 1.20f,
        .passive = PASSIVE_HEAVY_AOE_RESIST,
    },
    [CHASSIS_SNIPER] = {
        .name = "Sniper",
        .run_mult = 0.95f, .jump_mult = 1.00f, .jet_mult = 1.00f,
        .fuel_max = 1.10f, .fuel_regen = 0.20f, .mass_scale = 0.95f,
        .health_max = 130.0f,
        .bone_arm = 13.0f, .bone_forearm = 19.0f,
        .bone_thigh = 17.0f, .bone_shin = 21.0f,
        .torso_h = 30.0f, .neck_h = 16.0f,
        .hitbox_scale = 1.00f,
        .passive = PASSIVE_SNIPER_STEADY,
    },
    [CHASSIS_ENGINEER] = {
        .name = "Engineer",
        .run_mult = 1.00f, .jump_mult = 1.00f, .jet_mult = 1.10f,
        .fuel_max = 1.00f, .fuel_regen = 0.25f, .mass_scale = 1.00f,
        .health_max = 140.0f,
        .bone_arm = 14.0f, .bone_forearm = 14.0f,
        .bone_thigh = 16.0f, .bone_shin = 18.0f,
        .torso_h = 32.0f, .neck_h = 13.0f,
        .hitbox_scale = 0.95f,
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
    p->pos_x        [idx] = pos.x;
    p->pos_y        [idx] = pos.y;
    p->prev_x       [idx] = pos.x;
    p->prev_y       [idx] = pos.y;
    p->render_prev_x[idx] = pos.x;
    p->render_prev_y[idx] = pos.y;
    p->inv_mass     [idx] = inv_mass;
    p->flags        [idx] = PARTICLE_FLAG_ACTIVE;
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

    /* P06 — grapple starts IDLE. The memset above zeroed the struct,
     * which gives state=GRAPPLE_IDLE and anchor_mech=0; constraint_idx
     * needs to be -1 explicitly so a stale 0 isn't misread as "the
     * first constraint slot is mine". */
    m->grapple.constraint_idx = -1;
    m->grapple.anchor_mech    = -1;

    /* P11 — render-side RMB-flicker tracking. -1 = "never fired", so
     * the renderer doesn't flicker between slots in the early ticks of
     * a freshly spawned mech. */
    m->last_fired_slot = -1;
    m->last_fired_tick = 0;

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

/* ---- P06: Grapple constraint lifecycle ----------------------------- */

void mech_grapple_attach(World *w, int mid) {
    if (mid < 0 || mid >= w->mech_count) return;
    Mech *m = &w->mechs[mid];

    /* Allocate from the global ConstraintPool — append at count. The
     * per-mech constraint range covers only the skeleton; the grapple
     * is a transient extra slot. Slot leaks on release (active=0) but
     * is bounded per spec. */
    ConstraintPool *cp = &w->constraints;
    if (cp->count >= cp->capacity) {
        LOG_W("mech_grapple_attach: constraint pool full (%d/%d)",
              cp->count, cp->capacity);
        m->grapple.state = GRAPPLE_IDLE;
        m->grapple.constraint_idx = -1;
        return;
    }
    int ci = cp->count++;
    Constraint *c = &cp->items[ci];
    *c = (Constraint){ 0 };
    c->active = 1;
    /* Rope acts as a one-sided length limit: slack (no force) when the
     * pelvis is closer than the rope length, taut (pulls in) when
     * stretched beyond it. solve_fixed_anchor (tile case) and
     * CSTR_DISTANCE_LIMIT (mech case) both implement this when min=0
     * and the ceiling lives in `rest` / `max_len`. */
    c->a = (uint16_t)(m->particle_base + PART_PELVIS);
    if (m->grapple.anchor_mech < 0) {
        /* Tile anchor — fixed Vec2 + one-sided distance. */
        c->kind      = CSTR_FIXED_ANCHOR;
        c->b         = c->a;             /* unused, but matches solver assumption */
        c->rest      = m->grapple.rest_length;
        c->fixed_pos = m->grapple.anchor_pos;
    } else {
        /* Bone anchor — distance-limit constraint with min=0 / max=rope
         * length so the rope is one-sided (slack when shorter, taut
         * when stretched). The constraint solver's inv_mass split
         * pulls both ends toward each other proportional to mass when
         * stretched: a Heavy yanking a Scout pulls the Scout much more. */
        const Mech *t = &w->mechs[m->grapple.anchor_mech];
        c->kind     = CSTR_DISTANCE_LIMIT;
        c->b        = (uint16_t)(t->particle_base + m->grapple.anchor_part);
        c->min_len  = 0.0f;
        c->max_len  = m->grapple.rest_length;
    }
    m->grapple.constraint_idx = ci;
}

void mech_grapple_release(World *w, int mid) {
    if (mid < 0 || mid >= w->mech_count) return;
    Mech *m = &w->mechs[mid];
    if (m->grapple.state == GRAPPLE_IDLE && m->grapple.constraint_idx < 0) return;
    if (m->grapple.constraint_idx >= 0
        && m->grapple.constraint_idx < w->constraints.count) {
        w->constraints.items[m->grapple.constraint_idx].active = 0;
    }
    m->grapple.constraint_idx = -1;
    m->grapple.state          = GRAPPLE_IDLE;
    m->grapple.anchor_mech    = -1;
    SHOT_LOG("t=%llu mech=%d grapple_release",
             (unsigned long long)w->tick, mid);
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

    /* Chassis-specific posture quirks (P10) — small per-chassis biases
     * applied to chest/head/r-hand pose targets. Spec:
     * documents/m5/12-rigging-and-damage.md §"Posture differences per
     * chassis". Scout leans forward, Heavy locks chest upright (higher
     * pose strength), Sniper hunches the head, Engineer skips the
     * right-arm aim drive when holding the secondary slot (tool, not
     * rifle). */
    float chest_strength    = 0.7f;
    bool  skip_right_arm_aim = m->is_dummy;
    float face_dir          = m->facing_left ? -1.0f : 1.0f;
    Vec2  chest_target = { pelvis.x, pelvis.y - ch->torso_h };
    Vec2  head_target  = { pelvis.x, pelvis.y - ch->torso_h - ch->neck_h - 8.0f };
    switch ((ChassisId)m->chassis_id) {
        case CHASSIS_SCOUT:
            chest_target.x += face_dir * 2.0f;
            break;
        case CHASSIS_HEAVY:
            chest_strength = 0.85f;
            break;
        case CHASSIS_SNIPER:
            head_target.x += face_dir * 2.0f;
            head_target.y += 3.0f;
            break;
        case CHASSIS_ENGINEER:
            if (m->active_slot == 1) skip_right_arm_aim = true;
            break;
        case CHASSIS_TROOPER:
        default: break;
    }

    pose_set(m, PART_CHEST, chest_target, chest_strength);
    pose_set(m, PART_NECK,
        (Vec2){ pelvis.x, pelvis.y - ch->torso_h - ch->neck_h * 0.5f }, 0.7f);
    pose_set(m, PART_HEAD, head_target, 0.7f);

    pose_set(m, PART_L_HIP, (Vec2){ pelvis.x - 7, pelvis.y }, 0.7f);
    pose_set(m, PART_R_HIP, (Vec2){ pelvis.x + 7, pelvis.y }, 0.7f);

    pose_set(m, PART_L_SHOULDER,
        (Vec2){ pelvis.x - 10, pelvis.y - ch->torso_h + 4 }, 0.7f);
    pose_set(m, PART_R_SHOULDER,
        (Vec2){ pelvis.x + 10, pelvis.y - ch->torso_h + 4 }, 0.7f);

    float arm_reach = ch->bone_arm + ch->bone_forearm;
    if (!skip_right_arm_aim) {
        Vec2 r_sho = (Vec2){ pelvis.x + 10, pelvis.y - ch->torso_h + 4 };
        Vec2 r_hand_target = (Vec2){ r_sho.x + aim_dir.x * arm_reach,
                                     r_sho.y + aim_dir.y * arm_reach };
        pose_set(m, PART_R_HAND, r_hand_target, 0.7f);

        /* P11 — two-handed foregrip pose. Drive both L_ELBOW and
         * L_HAND so the L_ARM chain sits at rest length pointing from
         * L_SHOULDER toward the weapon's foregrip pixel. One-handed
         * weapons return false from `weapon_foregrip_world` and the
         * L_ARM keeps no pose target — the off-hand dangles from the
         * constraint chain (the M1 "no IK" default). Spec:
         * documents/m5/12-rigging-and-damage.md
         * §"Two-handed weapons and the off-hand foregrip".
         *
         * Why pose BOTH elbow and hand instead of just the hand:
         * the raw foregrip is typically UNREACHABLE for the L_ARM
         * chain (Pulse Rifle on Trooper: foregrip is ~74 px from
         * L_SHOULDER, but the chain `bone_arm + bone_forearm` only
         * reaches ~30 px). Posing only L_HAND yanks the hand toward
         * an out-of-reach target each tick; the constraint solver
         * corrects the over-stretched chain by pulling L_ELBOW and
         * L_SHOULDER forward; that propagates through CHEST →
         * PELVIS and drags the whole body in the aim direction —
         * the "mech moves on its own" bug. Posing BOTH elbow and
         * hand at chain-rest-length positions on the L_SHOULDER →
         * foregrip line forces the chain straight from the start;
         * the constraint solver has nothing to correct, no force
         * propagates back to the body, no drift. Visually the L_ARM
         * extends straight toward the foregrip — a "reaching for the
         * rifle" pose; once held-weapon art lands at P16 the L_HAND
         * sits roughly on the rifle's foregrip pixel. */
        /* P11 — two-handed foregrip pose: DEFERRED.
         *
         * The original implementation drove L_HAND toward the weapon
         * sprite's foregrip pixel (with strength 0.6 at first, then a
         * snap-pose IK variant that posed both L_ELBOW and L_HAND at
         * chain-rest positions). Both versions visibly drifted the
         * mech body in the aim direction — the "mech moves on its
         * own" bug.
         *
         * Root cause: the rifle's grip-to-foregrip span (~24 px on a
         * Pulse Rifle) plus the body's shoulder-to-shoulder span
         * (~20 px) plus the aim-extended R_ARM (~30 px) puts the
         * rifle's foregrip ~70+ px from L_SHOULDER, far past the
         * L_ARM chain's max reach (`bone_arm + bone_forearm` ≈
         * 30 px). Even a strength-1.0 snap-IK that places L_ELBOW +
         * L_HAND on rest-length positions decouples from L_SHOULDER
         * during the constraint-solve iterations: as PELVIS shifts
         * (driven by the R_ARM aim drive's chain-pull each tick),
         * L_SHOULDER follows via cross-brace; the L_ARM chain is no
         * longer at rest relative to the moved L_SHOULDER; constraint
         * corrections fire, propagating back to PELVIS — net
         * additive drift in the aim direction. Pose strength <1 just
         * moves the chain into a compressed state per-tick which
         * also generates corrections.
         *
         * Real fix: an IK constraint that runs INSIDE the constraint-
         * solve loop (instead of before it as a one-shot pose), so
         * the L_ARM tracks L_SHOULDER per iteration. That's a real
         * 2-bone analytic IK addition to the constraint pool — out
         * of scope for P11. Deferred to M6 polish; tracked under
         * TRADE_OFFS.md "Left hand has no pose target". One-handed
         * weapons (which were never going to drive the foregrip)
         * dangle as before; two-handed weapons also dangle now —
         * resolved when the IK lands. */
        (void)r_hand_target;
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

    if (!grounded) {
        /* Air control: keep the existing horizontal-only behavior. */
        float vx_per_tick = vx_pxs * dt * AIR_CONTROL;
        for (int part = 0; part < PART_COUNT; ++part) {
            int idx = m->particle_base + part;
            physics_set_velocity_x(p, idx, vx_per_tick);
        }
        return;
    }

    /* Read the average foot contact normal from the previous tick's
     * contact resolver. We use it for two things: (a) deciding
     * whether the foot is on a slope, so we know whether to brake
     * (flat) or let gravity-along-tangent run wild (sloped); (b)
     * projecting the run input onto the slope tangent so a held run
     * input climbs/descends along the surface instead of dragging
     * the body horizontally through it. */
    int lf = m->particle_base + PART_L_FOOT;
    int rf = m->particle_base + PART_R_FOOT;
    float nx = (p->contact_nx_q[lf] + p->contact_nx_q[rf]) / 254.0f;
    float ny = (p->contact_ny_q[lf] + p->contact_ny_q[rf]) / 254.0f;
    float nlen = sqrtf(nx * nx + ny * ny);
    bool flat;
    if (nlen < 0.5f) {
        /* No fresh contact data — treat as flat floor. */
        nx = 0.0f; ny = -1.0f;
        flat = true;
    } else {
        nx /= nlen; ny /= nlen;
        flat = (ny < -0.92f);
    }

    /* No-input + flat ground: brake horizontal velocity. On a slope,
     * skip the braking entirely so gravity-along-tangent + slope-aware
     * friction can drive passive slide. */
    if (vx_pxs == 0.0f) {
        if (!flat) return;
        for (int part = 0; part < PART_COUNT; ++part) {
            physics_set_velocity_x(p, m->particle_base + part, 0.0f);
        }
        return;
    }

    /* Tangent is normal rotated 90°. Sign chosen to match run direction. */
    float dir = (vx_pxs > 0.0f) ? 1.0f : -1.0f;
    float tx  = -ny * dir;
    float ty  =  nx * dir;

    float speed = fabsf(vx_pxs);
    float vt_per_tick_x = tx * speed * dt;
    float vt_per_tick_y = ty * speed * dt;

    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        physics_set_velocity_x(p, idx, vt_per_tick_x);
        /* Y is set on the lower chain only — upper body keeps its
         * gravity component so the body leans naturally into a slope
         * instead of being dragged tilted. */
        if (part == PART_L_FOOT || part == PART_R_FOOT ||
            part == PART_L_KNEE || part == PART_R_KNEE) {
            physics_set_velocity_y(p, idx, vt_per_tick_y);
        }
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
    /* P07 — CTF carrier penalty: half jet thrust. Soldat tradition;
     * keeps the carrier from sky-dancing the flag back to base. The
     * fuel-drain rate is unchanged (fuel runs out twice as fast for
     * the same vertical gain — same effective penalty). */
    if (ctf_is_carrier(w, m->id)) {
        thrust_pxs2 *= 0.5f;
    }
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
    float fy = -thrust_pxs2 * scale * dt * dt;     /* base impulse, straight up */

    /* Run-input sign for ceiling-tangent direction selection. The
     * latched_input tells us which way the player is leaning; when
     * jetting against an angled overhang we redirect the upward thrust
     * sideways along the ceiling tangent in that direction. */
    float run_sign = 0.0f;
    if (m->latched_input.buttons & BTN_LEFT)  run_sign = -1.0f;
    if (m->latched_input.buttons & BTN_RIGHT) run_sign = +1.0f;

    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        if ((p->flags[idx] & PARTICLE_FLAG_CEILING) && run_sign != 0.0f) {
            /* Project the (0, fy) impulse onto the ceiling tangent.
             * For a flat ceiling (n = (0, +1)) the tangent is (1, 0)
             * and dot is 0 — upward thrust dies, no slide. For an
             * angled ceiling (n = (-0.7, +0.7)) the tangent is
             * (-0.7, -0.7); dot picks up a sideways component the
             * run input chooses the sign of. */
            float nx = p->contact_nx_q[idx] / 127.0f;
            float ny = p->contact_ny_q[idx] / 127.0f;
            float tx = -ny;
            float ty =  nx;
            /* Pick the tangent direction matching the run input. */
            if (tx * run_sign < 0.0f) { tx = -tx; ty = -ty; }
            float mag = fabsf(fy);     /* magnitude of original upward thrust */
            p->pos_x[idx] += tx * mag;
            p->pos_y[idx] += ty * mag;
            /* Vertical component eaten by the ceiling — no upward push. */
        } else {
            p->pos_y[idx] += fy;
        }
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

    /* Powerup timers (P05). Server-side truth — clients mirror via the
     * SNAP_STATE_BERSERK / INVIS / GODMODE bits, setting their local
     * timer to a sentinel value while the bit is observed. */
    if (m->powerup_berserk_remaining > 0.0f) {
        m->powerup_berserk_remaining -= dt;
        if (m->powerup_berserk_remaining < 0.0f) m->powerup_berserk_remaining = 0.0f;
    }
    if (m->powerup_invis_remaining > 0.0f) {
        m->powerup_invis_remaining -= dt;
        if (m->powerup_invis_remaining < 0.0f) m->powerup_invis_remaining = 0.0f;
    }
    if (m->powerup_godmode_remaining > 0.0f) {
        m->powerup_godmode_remaining -= dt;
        if (m->powerup_godmode_remaining < 0.0f) m->powerup_godmode_remaining = 0.0f;
    }

    /* Burst SMG cadence (P05). When mech_try_fire kicked off a burst,
     * we owe `burst_pending_rounds` more shots one per
     * `burst_interval_sec`. Authoritative side only — the client's local
     * predict path doesn't need to schedule the trailing rounds because
     * the server's NET_MSG_FIRE_EVENT replicates them as they actually
     * fire. (Trying to spawn from the client predict ahead of the
     * server's events would double-up the visible rounds.) */
    if (w->authoritative && m->burst_pending_rounds > 0) {
        m->burst_pending_timer -= dt;
        if (m->burst_pending_timer <= 0.0f) {
            const Weapon *bwpn = weapon_def(m->weapon_id);
            if (bwpn && bwpn->fire == WFIRE_BURST &&
                (bwpn->mag_size <= 0 || m->ammo > 0))
            {
                weapons_spawn_projectiles(w, mid, m->weapon_id);
                if (bwpn->mag_size > 0) m->ammo--;
                if (m->active_slot == 0) m->ammo_primary   = m->ammo;
                else                     m->ammo_secondary = m->ammo;
                m->burst_pending_rounds--;
                m->burst_pending_timer = bwpn->burst_interval_sec;
                SHOT_LOG("t=%llu mech=%d burst_pending fired (left=%u)",
                         (unsigned long long)w->tick, mid,
                         (unsigned)m->burst_pending_rounds);
            } else {
                /* Weapon swapped or out of ammo — abort the burst. */
                m->burst_pending_rounds = 0;
                m->burst_pending_timer = 0.0f;
            }
        }
    }

    if (in.aim_x != 0.0f || in.aim_y != 0.0f) {
        m->aim_world = (Vec2){ in.aim_x, in.aim_y };
    }

    /* Edge-detect for press events (swap, dash, use). */
    uint16_t pressed = (uint16_t)((~m->prev_buttons) & in.buttons);

    if (m->alive && !m->is_dummy) {
        bool grounded = any_foot_grounded(w, m);
        m->grounded = grounded;

        if (pressed & BTN_SWAP) swap_weapon(m);

        /* Engineer ability (P05): drop a deployable repair pack at the
         * engineer's feet. Lasts 10 s; allies (and the engineer) walking
         * onto it consume it for +50 HP. Replaces M3's instant self-heal.
         * Server-only — the client never spawns transient pickups; the
         * NET_MSG_PICKUP_STATE broadcast replicates the spawn. */
        if (w->authoritative
            && (pressed & BTN_USE) && ch->passive == PASSIVE_ENGINEER_REPAIR
            && m->ability_cooldown <= 0.0f) {
            Vec2 pelv = part_pos(w, m, PART_PELVIS);
            PickupSpawner s = (PickupSpawner){
                .pos                = (Vec2){ pelv.x, pelv.y + 24.0f },
                .kind               = PICKUP_REPAIR_PACK,
                .variant            = 0,
                .respawn_ms         = 0,
                .state              = PICKUP_STATE_AVAILABLE,
                .reserved           = 0,
                .available_at_tick  = w->tick + 600,    /* 10 s @ 60 Hz */
                .flags              = 0,
            };
            int idx = pickup_spawn_transient(w, s);
            if (idx >= 0) {
                m->ability_cooldown = ENGINEER_COOLDOWN;
                SHOT_LOG("t=%llu mech=%d engineer_deploy idx=%d at (%.0f,%.0f)",
                         (unsigned long long)w->tick, mid, idx,
                         s.pos.x, s.pos.y);
            }
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

        /* For REMOTE mechs on a non-authoritative side (= client),
         * the snapshot already set anim_id (snapshot.c derives RUN
         * from velocity since the input bitmask doesn't ride the
         * wire). Overriding here with input-based logic would
         * always pick STAND (in.buttons=0 for remote mechs on the
         * client), which freezes the leg-swing cycle and makes the
         * mech appear to slide across the ground instead of walk.
         *
         * On the server, every mech has authoritative input — the
         * host's own keyboard for slot 0, the per-peer NET_MSG_INPUT
         * latched on each remote slot — so we always run the
         * input-based path there. */
        bool input_drives_anim = w->authoritative || mid == w->local_mech_id;
        if (input_drives_anim) {
            if (jetting)        m->anim_id = ANIM_JET;
            else if (!grounded) m->anim_id = ANIM_FALL;
            else if (moving)    m->anim_id = ANIM_RUN;
            else                m->anim_id = ANIM_STAND;
        }

    } else if (m->alive && m->is_dummy) {
        m->anim_id = ANIM_STAND;
        m->grounded = any_foot_grounded(w, m);
    }

    /* P06 — Grapple step. Server-side only. The rope is a one-sided
     * distance constraint (slack when shorter than rest, taut when
     * stretched), so the firer swings as a pendulum at a fixed radius
     * — the "Tarzan" feel. BTN_USE edge-release; anchor-mech death
     * also releases. (Firer death is handled in mech_kill.)
     *
     * Re-firing BTN_FIRE while attached is handled in mech_try_fire —
     * it releases the current grapple and fires a new head, so the
     * player can chain grapples to swing across a level.
     *
     * Holding BTN_JET (W) while attached retracts the rope at
     * GRAPPLE_RETRACT_PXS — the player zip-lines toward the anchor.
     * Release W and the rope stops shortening. The retract bottoms
     * out at GRAPPLE_MIN_REST_LEN so a fully-retracted firer doesn't
     * end up inside the anchor tile / bone. (Jet thrust still applies
     * normally while W is held — the two effects reinforce: jet
     * pushes up, rope shortens toward the anchor.) */
    if (w->authoritative && m->alive && m->grapple.state == GRAPPLE_ATTACHED) {
        bool jet_held     = (in.buttons      & BTN_JET) != 0;
        bool jet_was_held = (m->prev_buttons & BTN_JET) != 0;
        if (jet_held && !jet_was_held) {
            SHOT_LOG("t=%llu mech=%d grapple_retract_start rest=%.1f",
                     (unsigned long long)w->tick, mid,
                     m->grapple.rest_length);
        } else if (!jet_held && jet_was_held) {
            SHOT_LOG("t=%llu mech=%d grapple_retract_stop  rest=%.1f",
                     (unsigned long long)w->tick, mid,
                     m->grapple.rest_length);
        }
        if (jet_held) {
            const float GRAPPLE_RETRACT_PXS  = 800.0f;
            /* MIN must clear the body height: head sits ~48 px above
             * pelvis with a ~8 px head radius, so a min of 60 px puts
             * the head crown right at the anchor's tile surface and
             * the constraint solver vs. tile collision fight produces
             * the "stuck in the ceiling" report. 100 px gives ~40 px
             * of head-clearance below a ceiling-tile anchor with the
             * default Trooper proportions, plus headroom for the
             * other chassis. */
            const float GRAPPLE_MIN_REST_LEN = 100.0f;
            m->grapple.rest_length -= GRAPPLE_RETRACT_PXS * dt;
            if (m->grapple.rest_length < GRAPPLE_MIN_REST_LEN) {
                m->grapple.rest_length = GRAPPLE_MIN_REST_LEN;
            }
        }
        if (m->grapple.constraint_idx >= 0
            && m->grapple.constraint_idx < w->constraints.count) {
            Constraint *c = &w->constraints.items[m->grapple.constraint_idx];
            /* Mirror rest length onto whichever solver field this
             * constraint kind reads from. Tile anchor: c->rest. Mech
             * anchor: c->max_len (CSTR_DISTANCE_LIMIT). */
            c->rest    = m->grapple.rest_length;
            c->max_len = m->grapple.rest_length;
            /* Tile anchor — fixed_pos is authoritative every tick (the
             * grapple struct's anchor_pos is the source of truth; the
             * constraint mirrors it). */
            if (m->grapple.anchor_mech < 0) c->fixed_pos = m->grapple.anchor_pos;
        }
        /* Release on anchor-mech death. */
        if (m->grapple.anchor_mech >= 0
            && m->grapple.anchor_mech < (int8_t)w->mech_count
            && !w->mechs[m->grapple.anchor_mech].alive) {
            mech_grapple_release(w, mid);
        }
        /* Release on BTN_USE edge. */
        if (pressed & BTN_USE) mech_grapple_release(w, mid);

        /* Per-second swing-distance trace for shot tests. SHOT_LOG is a
         * one-branch no-op outside shot mode; cheap to keep on. */
        if (m->grapple.state == GRAPPLE_ATTACHED && (w->tick % 60) == 0) {
            int pelv = m->particle_base + PART_PELVIS;
            float dx, dy;
            if (m->grapple.anchor_mech < 0) {
                dx = w->particles.pos_x[pelv] - m->grapple.anchor_pos.x;
                dy = w->particles.pos_y[pelv] - m->grapple.anchor_pos.y;
            } else {
                const Mech *t = &w->mechs[m->grapple.anchor_mech];
                int tp = t->particle_base + m->grapple.anchor_part;
                dx = w->particles.pos_x[pelv] - w->particles.pos_x[tp];
                dy = w->particles.pos_y[pelv] - w->particles.pos_y[tp];
            }
            float d = sqrtf(dx*dx + dy*dy);
            SHOT_LOG("t=%llu mech=%d grapple_swing dist=%.1f rest=%.1f%s",
                     (unsigned long long)w->tick, mid, d,
                     m->grapple.rest_length,
                     (in.buttons & BTN_JET) ? " retracting" : "");
        }
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

    /* P02: slope-aware gating. If either foot's contact normal is
     * tilted more than ~22° off straight-up, the mech is on a slope —
     * skip the standing anchor and let slope physics (slope-tangent
     * run velocity + slope-aware friction) drive pose. Without this,
     * the anchor zeros Y-velocity every tick and kills passive
     * downhill slide. */
    int lf = b + PART_L_FOOT;
    int rf = b + PART_R_FOOT;
    float ny_l = p->contact_ny_q[lf] / 127.0f;
    float ny_r = p->contact_ny_q[rf] / 127.0f;
    float ny_avg = (ny_l + ny_r) * 0.5f;
    if (ny_avg > -0.92f) return;       /* sloped or no contact — skip anchor */

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

/* ---- Environmental damage (P02) ----------------------------------- */
/*
 * 5 HP/s damage tick when a mech is touching a DEADLY tile, a DEADLY
 * polygon, or inside an ACID ambient zone. Per the design canon
 * (documents/m5/03-collision-polygons.md §"DEADLY tiles + ACID ambient
 * zones"). Called from simulate_step after the physics pass.
 */

static bool point_in_tri(float px, float py,
                         float ax, float ay,
                         float bx, float by,
                         float cx, float cy)
{
    float d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (fabsf(d) < 1e-6f) return false;
    float u = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d;
    float v = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d;
    return u >= 0.0f && v >= 0.0f && (u + v) <= 1.0f;
}

void mech_apply_environmental_damage(World *w, int mid, float dt) {
    Mech *m = &w->mechs[mid];
    if (!m->alive || m->is_dummy) return;

    const ParticlePool *p = &w->particles;
    const Level        *L = &w->level;
    int   ts = L->tile_size;
    bool  in_hazard = false;

    for (int part = 0; part < PART_COUNT && !in_hazard; ++part) {
        int idx = m->particle_base + part;
        float px = p->pos_x[idx];
        float py = p->pos_y[idx];

        /* Tile DEADLY check. */
        int tx = (int)(px / (float)ts);
        int ty = (int)(py / (float)ts);
        if (level_flags_at(L, tx, ty) & TILE_F_DEADLY) {
            in_hazard = true;
            break;
        }

        /* Polygon DEADLY check via broadphase. */
        if (L->poly_grid_off && L->poly_count > 0 &&
            tx >= 0 && tx < L->width && ty >= 0 && ty < L->height) {
            int cell = ty * L->width + tx;
            int s = L->poly_grid_off[cell];
            int e = L->poly_grid_off[cell + 1];
            for (int k = s; k < e; ++k) {
                const LvlPoly *poly = &L->polys[L->poly_grid[k]];
                if ((PolyKind)poly->kind != POLY_KIND_DEADLY) continue;
                if (point_in_tri(px, py,
                                 (float)poly->v_x[0], (float)poly->v_y[0],
                                 (float)poly->v_x[1], (float)poly->v_y[1],
                                 (float)poly->v_x[2], (float)poly->v_y[2])) {
                    in_hazard = true;
                    break;
                }
            }
        }

        /* ACID ambient zone check. */
        if (!in_hazard && L->ambi_count > 0) {
            for (int z = 0; z < L->ambi_count; ++z) {
                const LvlAmbi *a = &L->ambis[z];
                if (a->kind != AMBI_ACID) continue;
                if (px < (float)a->rect_x ||
                    px > (float)(a->rect_x + a->rect_w)) continue;
                if (py < (float)a->rect_y ||
                    py > (float)(a->rect_y + a->rect_h)) continue;
                in_hazard = true;
                break;
            }
        }
    }

    if (in_hazard) {
        mech_apply_damage(w, mid, PART_PELVIS, 5.0f * dt,
                          (Vec2){0.0f, -1.0f}, /*shooter*/-1);
    }
}

/* ---- BTN_FIRE_SECONDARY (RMB) one-shot dispatch (P09) -----------------
 *
 * Fires the inactive slot for one shot without flipping active_slot.
 * Per documents/m5/13-controls-and-residuals.md §"BTN_FIRE_SECONDARY".
 *
 * Pattern: temporarily swap the active-slot aliases (m->weapon_id /
 * m->ammo / m->ammo_max) to the other slot, dispatch by fire kind, then
 * restore. The weapons_fire_* helpers read m->weapon_id and m->ammo as
 * authoritative, so the temporary swap covers them transparently —
 * recoil, bink, cooldown, and FX all flow through the same paths.
 *
 * Gating mirrors mech_try_fire's outer guard:
 *   - shared fire_cooldown / reload_timer (no LMB+RMB double DPS)
 *   - charge weapons (charge_sec > 0) rejected — RMB is press-only
 *   - flag carrier can't fire from the secondary slot via either path
 */
static void fire_other_slot_one_shot(World *w, int mid) {
    Mech *m = &w->mechs[mid];
    int   other_slot = m->active_slot ^ 1;
    int   weapon_id  = (other_slot == 0) ? m->primary_id : m->secondary_id;
    int  *ammo_ptr   = (other_slot == 0) ? &m->ammo_primary : &m->ammo_secondary;

    const Weapon *wpn = weapon_def(weapon_id);
    if (!wpn) return;

    /* Charge weapons (Rail Cannon, Microgun spin-up) need the trigger
     * held over `charge_sec` to fire — they can't be RMB one-shotted. */
    if (wpn->charge_sec > 0.0f) return;

    /* CTF carrier penalty: secondary slot is fully disabled while
     * carrying a flag, regardless of which path triggers it. Active-slot
     * gate (mech_try_fire) covers active==1; this gate covers active==0
     * with RMB targeting the secondary. */
    if (other_slot == 1 && ctf_is_carrier(w, mid)) return;

    if (m->fire_cooldown > 0.0f) return;
    if (m->reload_timer  > 0.0f) return;
    if (wpn->mag_size > 0 && *ammo_ptr <= 0) return;

    /* P11 — stamp the last-fired slot for the renderer's RMB-flicker
     * window. Stamped before dispatch so even the rare case where the
     * inner spawn fails (e.g. projectile pool full on grapple) leaves
     * the renderer in a coherent state — the flicker just decays
     * through the window. */
    m->last_fired_slot = (int8_t)other_slot;
    m->last_fired_tick = w->tick;

    /* Save active-slot aliases; swap to the inactive slot's stats. */
    int saved_weapon_id = m->weapon_id;
    int saved_ammo      = m->ammo;
    int saved_ammo_max  = m->ammo_max;
    m->weapon_id = weapon_id;
    m->ammo      = *ammo_ptr;
    m->ammo_max  = wpn->mag_size;

    switch ((int)wpn->fire) {
        case WFIRE_HITSCAN:
            weapons_fire_hitscan(w, mid);
            if (wpn->mag_size > 0) m->ammo--;
            break;
        case WFIRE_PROJECTILE:
        case WFIRE_SPREAD:
        case WFIRE_THROW:
            weapons_spawn_projectiles(w, mid, weapon_id);
            if (wpn->mag_size > 0) m->ammo--;
            break;
        case WFIRE_BURST:
            if (m->burst_pending_rounds == 0) {
                weapons_spawn_projectiles(w, mid, weapon_id);   /* round 1 of N */
                if (wpn->mag_size > 0) m->ammo--;
                if (wpn->burst_rounds > 1) {
                    m->burst_pending_rounds = (uint8_t)(wpn->burst_rounds - 1);
                    m->burst_pending_timer  = wpn->burst_interval_sec;
                }
            }
            break;
        case WFIRE_MELEE:
            weapons_fire_melee(w, mid, weapon_id);
            m->fire_cooldown = wpn->fire_rate_sec;
            break;
        case WFIRE_GRAPPLE: {
            /* Mirror the grapple branch from mech_try_fire — same
             * spawn/anchor lifecycle, gated on RMB rather than LMB.
             * ATTACHED → release-and-refire (chain); FLYING → swallow. */
            if (m->grapple.state == GRAPPLE_FLYING) break;
            if (m->grapple.state == GRAPPLE_ATTACHED) mech_grapple_release(w, mid);

            Vec2 hand    = part_pos(w, m, PART_R_HAND);
            Vec2 aim_dir = mech_aim_dir(w, mid);
            /* P11 — head spawns at the launcher muzzle, not bare hand,
             * so visible rope start matches the rendered weapon. */
            const WeaponSpriteDef *wsp_g = weapon_sprite_def(weapon_id);
            Vec2 origin = weapon_muzzle_world(hand, aim_dir, wsp_g,
                                              wpn->muzzle_offset);
            float speed  = wpn->projectile_speed_pxs > 0.0f
                           ? wpn->projectile_speed_pxs : 1200.0f;
            Vec2 vel     = (Vec2){ aim_dir.x * speed, aim_dir.y * speed };
            float life   = wpn->projectile_life_sec > 0.0f
                           ? wpn->projectile_life_sec : (wpn->range_px / speed);
            ProjectileSpawn ps = {
                .kind          = (wpn->projectile_kind != 0)
                                 ? wpn->projectile_kind : PROJ_GRAPPLE_HEAD,
                .weapon_id     = weapon_id,
                .owner_mech_id = mid,
                .owner_team    = m->team,
                .origin        = origin,
                .velocity      = vel,
                .damage        = 0.0f,
                .aoe_radius    = 0.0f,
                .aoe_damage    = 0.0f,
                .aoe_impulse   = 0.0f,
                .life          = life,
                .gravity_scale = wpn->projectile_grav_scale,
                .drag          = wpn->projectile_drag,
                .bouncy        = false,
            };
            int ph = projectile_spawn(w, ps);
            if (ph >= 0) {
                m->grapple.state          = GRAPPLE_FLYING;
                m->grapple.constraint_idx = -1;
                m->grapple.anchor_mech    = -1;
                m->fire_cooldown          = wpn->fire_rate_sec;
                /* Broadcast the head spawn so remote clients can
                 * render it during FLYING (snapshot only mirrors
                 * grapple state, not the head projectile). */
                weapons_record_fire(w, mid, weapon_id, origin, aim_dir);
                SHOT_LOG("t=%llu mech=%d grapple_fire(RMB) ph=%d",
                         (unsigned long long)w->tick, mid, ph);
            }
            break;
        }
        default: break;
    }

    /* Persist any ammo decrement back to the inactive slot's bucket;
     * restore the active-slot aliases so subsequent reads (LMB next
     * tick, snapshot encode, HUD) see the active weapon again. */
    *ammo_ptr    = m->ammo;
    m->weapon_id = saved_weapon_id;
    m->ammo      = saved_ammo;
    m->ammo_max  = saved_ammo_max;
}

/* ---- Try-fire: dispatched on weapon class -------------------------- */

bool mech_try_fire(World *w, int mid, ClientInput in) {
    Mech *m = &w->mechs[mid];
    if (!m->alive || m->is_dummy)            return false;

    /* P09 — BTN_FIRE_SECONDARY (RMB): edge-triggered one-shot of the
     * inactive slot. Runs above the active-slot guards so the inactive
     * primary stays usable when the active secondary is gated (e.g.,
     * flag carrier). Shared fire_cooldown still prevents LMB+RMB double
     * fire on the same tick — fire_other_slot_one_shot sets the
     * cooldown via the inner weapons_fire_* call, and the active-slot
     * dispatch below sees it and bails. */
    if (((~m->prev_buttons) & in.buttons & BTN_FIRE_SECONDARY) != 0) {
        SHOT_LOG("t=%llu mech=%d fire_secondary edge active=%d prim=%d sec=%d cd=%.3f",
                 (unsigned long long)w->tick, mid, m->active_slot,
                 m->primary_id, m->secondary_id, (double)m->fire_cooldown);
        fire_other_slot_one_shot(w, mid);
    }

    const Weapon *wpn = weapon_def(m->weapon_id);
    if (!wpn) return false;
    if (m->fire_cooldown > 0.0f)             return false;
    if (m->reload_timer  > 0.0f)             return false;

    /* P07 — CTF carrier penalty: secondary fire is fully disabled while
     * carrying a flag. Trade-off vs Soldat's partial restriction: see
     * TRADE_OFFS.md "Carrier secondary fully disabled". Primary still
     * fires normally. P09's BTN_FIRE_SECONDARY (RMB one-shot) inherits
     * the same gate via fire_other_slot_one_shot's other_slot==1 check. */
    if (m->active_slot == 1 && ctf_is_carrier(w, mid)) return false;

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
        /* Burst fire (P05): spawn the first round on the press tick,
         * queue `burst_rounds - 1` more to land at `burst_interval_sec`
         * cadence (70 ms for the Burst SMG). mech_step_drive ticks the
         * pending counter at the top of every tick. Self-bink
         * accumulates per round, so the trailing rounds fan wider —
         * matches the design intent in `documents/04-combat.md`. */
        if (!fire_pressed) return false;
        if (m->burst_pending_rounds > 0) return false;       /* burst already in flight */
        if (wpn->mag_size > 0 && m->ammo <= 0) return false;
        weapons_spawn_projectiles(w, mid, m->weapon_id);     /* round 1 of N */
        if (wpn->mag_size > 0) m->ammo--;
        if (wpn->burst_rounds > 1) {
            m->burst_pending_rounds = (uint8_t)(wpn->burst_rounds - 1);
            m->burst_pending_timer  = wpn->burst_interval_sec;
        }
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
        /* P06 — Grappling hook. Edge-triggered (holding fire doesn't
         * re-fire).
         *
         * Re-press while ATTACHED: release the current rope and fire a
         * new one — this is what lets the player chain grapples to
         * swing across a level (Tarzan-style traversal).
         *
         * Re-press while FLYING (head still in flight): swallow it. We
         * don't want to double-fire and end up with two heads in the
         * air. */
        if (!fire_pressed) return false;
        if (m->grapple.state == GRAPPLE_FLYING) return false;
        if (m->grapple.state == GRAPPLE_ATTACHED) {
            mech_grapple_release(w, mid);
            /* Fall through to the fresh-fire path below. */
        }

        Vec2 hand    = part_pos(w, m, PART_R_HAND);
        Vec2 aim_dir = mech_aim_dir(w, mid);
        /* P11 — origin from launcher muzzle, see RMB branch above. */
        const WeaponSpriteDef *wsp_g = weapon_sprite_def(m->weapon_id);
        Vec2 origin  = weapon_muzzle_world(hand, aim_dir, wsp_g,
                                           wpn->muzzle_offset);
        float speed  = wpn->projectile_speed_pxs > 0.0f
                       ? wpn->projectile_speed_pxs : 1200.0f;
        Vec2 vel     = (Vec2){ aim_dir.x * speed, aim_dir.y * speed };
        float life   = wpn->projectile_life_sec > 0.0f
                       ? wpn->projectile_life_sec : (wpn->range_px / speed);
        ProjectileSpawn ps = {
            .kind          = (wpn->projectile_kind != 0)
                             ? wpn->projectile_kind : PROJ_GRAPPLE_HEAD,
            .weapon_id     = m->weapon_id,
            .owner_mech_id = mid,
            .owner_team    = m->team,
            .origin        = origin,
            .velocity      = vel,
            .damage        = 0.0f,
            .aoe_radius    = 0.0f,
            .aoe_damage    = 0.0f,
            .aoe_impulse   = 0.0f,
            .life          = life,
            .gravity_scale = wpn->projectile_grav_scale,
            .drag          = wpn->projectile_drag,
            .bouncy        = false,
        };
        int ph = projectile_spawn(w, ps);
        if (ph < 0) return false;
        m->grapple.state          = GRAPPLE_FLYING;
        m->grapple.constraint_idx = -1;
        m->grapple.anchor_mech    = -1;
        m->fire_cooldown          = wpn->fire_rate_sec;
        /* P09 — broadcast the head spawn so remote clients can render
         * the FLYING head. The snapshot only mirrors `grapple.state`,
         * not the projectile itself, so without a FIRE_EVENT the
         * remote view is empty between fire and attach. */
        weapons_record_fire(w, mid, m->weapon_id, origin, aim_dir);
        SHOT_LOG("t=%llu mech=%d grapple_fire ph=%d aim=(%.2f,%.2f)",
                 (unsigned long long)w->tick, mid, ph,
                 aim_dir.x, aim_dir.y);
    }

    if (m->active_slot == 0) m->ammo_primary   = m->ammo;
    else                     m->ammo_secondary = m->ammo;

    /* P11 — stamp the last-fired slot. The early-return branches above
     * all bail before reaching here, so a stamp at the tail means a
     * shot did leave the barrel via the active slot. (RMB stamps in
     * fire_other_slot_one_shot; on a tick where RMB fired, the active
     * path bails on the shared cooldown above and never reaches this
     * stamp — at most one stamp per tick.) */
    m->last_fired_slot = (int8_t)m->active_slot;
    m->last_fired_tick = w->tick;
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

    /* P06 — release any active grapple so the corpse doesn't keep
     * pulling toward an anchor. */
    if (m->grapple.state != GRAPPLE_IDLE) mech_grapple_release(w, mid);

    /* P07 — CTF: if the dying mech is a flag carrier, drop the flag at
     * the pelvis position (BEFORE the kill impulse displaces it) with
     * a 30-s auto-return timer. ctf_drop_on_death is a no-op when the
     * round isn't CTF or the mech isn't carrying anything. Snapshot
     * the pelvis position now so the drop lands at the death site, not
     * wherever the impulse sends the corpse. */
    {
        Vec2 pelv = (Vec2){
            w->particles.pos_x[m->particle_base + PART_PELVIS],
            w->particles.pos_y[m->particle_base + PART_PELVIS],
        };
        ctf_drop_on_death(w, (MatchModeId)w->match_mode_cached, mid, pelv);
    }

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

    /* P05 — Godmode powerup: incoming damage is zeroed. Returns false
     * (no death) so the caller treats the hit as a no-op. Self-damage
     * still passes through unharmed (no health change either). */
    if (m->powerup_godmode_remaining > 0.0f) return false;

    /* Friendly-fire gating. Self-damage (e.g. own rocket splash) always
     * goes through; friendly damage is dropped when FF is off. */
    if (shooter_mech_id >= 0 && shooter_mech_id != mid) {
        const Mech *shooter = &w->mechs[shooter_mech_id];
        if (shooter->team == m->team && !w->friendly_fire) {
            return false;
        }
    }

    /* P05 — Berserk powerup on the shooter doubles outgoing damage.
     * Centralized here (rather than at every fire path) so hitscan,
     * projectile direct hits, AOE, and melee all benefit uniformly. */
    if (shooter_mech_id >= 0 && shooter_mech_id != mid &&
        shooter_mech_id < w->mech_count) {
        const Mech *shooter = &w->mechs[shooter_mech_id];
        if (shooter->powerup_berserk_remaining > 0.0f) {
            dmg *= 2.0f;
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

    /* Queue a hit event for clients (server-only — clients re-spawn
     * the same FX from the broadcast). main.c drains the queue post-
     * simulate and broadcasts via NET_MSG_HIT_EVENT. */
    if (w->authoritative) {
        int slot = w->hitfeed_count % HITFEED_CAPACITY;
        if (slot < 0) slot += HITFEED_CAPACITY;
        int dmg_byte = (int)(final_dmg + 0.5f);
        if (dmg_byte < 0) dmg_byte = 0;
        if (dmg_byte > 255) dmg_byte = 255;
        w->hitfeed[slot] = (HitFeedEntry){
            .victim_mech_id = (int16_t)mid,
            .hit_part       = (uint8_t)part,
            .damage         = (uint8_t)dmg_byte,
            .pos_x          = hp.x,
            .pos_y          = hp.y,
            .dir_x          = dir.x,
            .dir_y          = dir.y,
        };
        w->hitfeed_count++;
    }

    if (m->health <= 0.0f) {
        mech_kill(w, mid, part, dir, 90.0f, shooter_mech_id,
                  m->last_killshot_weapon);
        return true;
    }

    w->hit_pause_ticks = 1;
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.12f);
    return false;
}
