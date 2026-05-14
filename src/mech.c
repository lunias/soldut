#include "mech.h"

#include "audio.h"
#include "ctf.h"
#include "level.h"
#include "log.h"
#include "mech_sprites.h"
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
/* M6 P07 Phase 1 — ground accel rates for the add-toward-target model
 * (§5A of documents/m6/07-movement-gamefeel.md). Run input is a SPEED
 * CAP that the body lerps toward at one of three rates; external sources
 * (slopes, dashes, recoil) can push past the cap and friction grinds
 * excess speed back down over multiple ticks instead of clamping in one. */
#define GROUND_ACCEL_PXS2     2800.0f /* ~6 frames to RUN_SPEED from rest */
#define GROUND_DECEL_PXS2     4666.0f /* ~4 frames to zero from RUN_SPEED, input opposed */
#define GROUND_FRICTION_PXS2  1400.0f /* ~12 frames to zero from RUN_SPEED on flat, no input */
/* M6 P07 Phase 2 — air accel for the same add-toward-target model
 * (§5A/§5D). Single rate (no separate decel) — flipping LEFT mid-air
 * from a RUN_SPEED dash reverses over ~20 frames (Soldat-feel: you can
 * nudge mid-jet but you can't instantly reverse). Replaces the
 * pre-Phase-2 SET-velocity AIR_CONTROL=0.35 clamp that wiped 65 % of
 * horizontal momentum the instant the feet left the ground. */
#define AIR_ACCEL_PXS2        1680.0f /* ~10 frames to RUN_SPEED in air, ~0.6× ground accel */
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

    LOG_I("mech_create: id=%d chassis=%s%s armor=%s jet=%s(id=%d) "
          "fuel_max=%.3f primary=%s secondary=%s",
          mid, ch->name, is_dummy ? " (dummy)" : "",
          ar->name, jp->name, (int)m->jetpack_id, (double)m->fuel_max,
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
    Vec2 pelv = (Vec2){
        w->particles.pos_x[m->particle_base + PART_PELVIS],
        w->particles.pos_y[m->particle_base + PART_PELVIS],
    };
    audio_play_at(SFX_GRAPPLE_RELEASE, pelv);
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

/* ---- Pose drive (gait phase + footstep SFX) -----------------------
 *
 * M6 retired the old `build_pose` / `apply_pose_to_particles` pair —
 * bone positions for live mechs are now produced by `pose_compute`
 * (see `mech_ik.c`) run after the physics pass. What remains here is
 * the side-effectful state every owner of a mech still needs to
 * advance:
 *   - `anim_time` increments while running (drives gait_phase from a
 *     speed-derived cycle frequency).
 *   - `gait_phase_l / _r` track the cycle position used by both
 *     pose_compute and the footstep SFX swing→stance trigger.
 *   - `m->facing_left` is set from the aim direction.
 *
 * Gate: this runs only on the side that OWNS the mech's prediction —
 * authoritative (server / offline) for every mech, plus the local
 * mech on a client. For remote mechs on the client, the snapshot
 * stream carries gait_phase_l + facing_left + anim_id; snapshot_apply
 * writes them, and the footstep wrap SFX fires from that path
 * (see `snapshot_apply` in src/snapshot.c).
 */
static void mech_update_gait(const Chassis *ch, World *w, Mech *m, float dt) {
    if (!m->alive) return;

    int b = m->particle_base;
    ParticlePool *p = &w->particles;
    Vec2 aim_dir = mech_aim_dir(w, m->id);
    m->facing_left = aim_dir.x < 0.0f;

    if ((AnimId)m->anim_id != ANIM_RUN) {
        /* Outside the run cycle keep the gait phases at 0 so a
         * subsequent ANIM_RUN starts on a fresh stride. anim_time can
         * stay where it is (it'll reset effectively on the next RUN
         * start because we don't multiply by an absolute timestamp). */
        m->gait_phase_l = 0.0f;
        m->gait_phase_r = 0.0f;
        return;
    }

    m->anim_time += dt;
    const float stride     = 28.0f;
    const float run_v      = RUN_SPEED_PXS * ch->run_mult;
    const float cycle_freq = run_v / (2.0f * stride);

    float p_l = m->anim_time * cycle_freq;
    p_l -= floorf(p_l);
    float p_r = p_l + 0.5f;
    if (p_r >= 1.0f) p_r -= 1.0f;

    /* P14 — footstep SFX on swing→stance wrap. The plant location
     * uses the pelvis particle's current x (post-physics on the local
     * mech; one tick stale on remote interpolated mechs — close
     * enough for spatialization at 200–1500 px attenuation range). */
    if (m->grounded) {
        float pelvis_x = p->pos_x[b + PART_PELVIS];
        float foot_y_ground = p->pos_y[b + PART_PELVIS] +
                              ch->bone_thigh + ch->bone_shin;
        float dir   = m->facing_left ? -1.0f : 1.0f;
        float front = stride * 0.5f * dir;
        if (m->gait_phase_l > 0.5f && p_l < 0.5f) {
            float plant_x = (pelvis_x - 7.0f) + front;
            audio_play_at(SFX_FOOTSTEP_CONCRETE,
                          (Vec2){ plant_x, foot_y_ground });
        }
        if (m->gait_phase_r > 0.5f && p_r < 0.5f) {
            float plant_x = (pelvis_x + 7.0f) + front;
            audio_play_at(SFX_FOOTSTEP_CONCRETE,
                          (Vec2){ plant_x, foot_y_ground });
        }
    }
    m->gait_phase_l = p_l;
    m->gait_phase_r = p_r;
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
        /* M6 P07 Phase 2 — air control via add-toward-target with cap
         * (§5A/§5D). Same CAP-not-target invariant as the ground branch:
         * if the body is already moving at >= RUN_SPEED in the input
         * direction (running jump, slope-launch, recoil), input does
         * NOTHING — only PHYSICS_VELOCITY_DAMP (0.99/tick) bleeds excess
         * back down. Replaces the pre-Phase-2 SET-velocity clamp that
         * wiped 65 % of horizontal momentum at the ground→air boundary
         * and produced "jumps kill your forward motion" feel.
         *
         * Horizontal-only — gravity drives vy, jet adds vy, jump sets
         * vy. The air branch never touches the Y component.
         *
         * Caller only invokes apply_run_velocity in air when an input
         * is held (`grounded && !moving` gate excludes air+no-input),
         * so vx_pxs != 0 here in practice. Defensive return for 0. */
        if (vx_pxs == 0.0f) return;

        int pelv = m->particle_base + PART_PELVIS;
        float vx_now = p->pos_x[pelv] - p->prev_x[pelv];

        float input_dir = (vx_pxs > 0.0f) ? +1.0f : -1.0f;
        float vx_cap    = fabsf(vx_pxs) * dt;       /* px/tick, unsigned */
        float v_in_dir  = input_dir * vx_now;       /* signed component along input */

        /* Above the cap in the input direction → input does nothing.
         * External sources (slope-launched jump, dash, recoil) keep
         * their excess momentum; only universal drag erodes it. */
        if (v_in_dir >= vx_cap) return;

        /* Below cap. Single accel rate — accelerating-from-rest and
         * reversing-against-motion both use AIR_ACCEL_PXS2. (Ground
         * has a separate decel rate to make flick-reverses snappier
         * on the ground; in air the lower rate IS the air-control
         * inertia we want.) */
        float vx_target = input_dir * vx_cap;
        float vx_delta  = vx_target - vx_now;
        float max_delta = AIR_ACCEL_PXS2 * dt * dt;
        if (vx_delta >  max_delta) vx_delta =  max_delta;
        if (vx_delta < -max_delta) vx_delta = -max_delta;

        for (int part = 0; part < PART_COUNT; ++part) {
            int idx = m->particle_base + part;
            p->prev_x[idx] -= vx_delta;
        }
        return;
    }

    /* Average foot contact normal from the previous tick. Used to
     * decide slope-vs-flat (no-input behavior differs) and to project
     * the run target onto the slope tangent so a held run input climbs
     * / descends the surface instead of dragging horizontally through it. */
    int lf = m->particle_base + PART_L_FOOT;
    int rf = m->particle_base + PART_R_FOOT;
    float nx = (p->contact_nx_q[lf] + p->contact_nx_q[rf]) / 254.0f;
    float ny = (p->contact_ny_q[lf] + p->contact_ny_q[rf]) / 254.0f;
    float nlen = sqrtf(nx * nx + ny * ny);
    bool flat;
    if (nlen < 0.5f) {
        nx = 0.0f; ny = -1.0f;
        flat = true;
    } else {
        nx /= nlen; ny /= nlen;
        flat = (ny < -0.92f);
    }

    /* M6 P07 Phase 1 — Sonic-style ground move (§5A). Run input is a
     * SPEED CAP that the body lerps toward at one of three accel rates;
     * external sources (slopes, dashes, recoil) can push past the cap
     * and friction bleeds the excess back over multiple ticks. Two key
     * properties baked in here that the first cut of Phase 1 missed:
     *
     *   1. CAP, not target — when the velocity component along the
     *      input direction is already >= the input cap (sliding down
     *      a slope, post-dash, post-recoil), do NOTHING. Letting the
     *      lerp pull the body DOWN to the cap is what wiped slope
     *      momentum on flat ground after a slide.
     *
     *   2. Tangent-aligned delta — apply the per-tick velocity change
     *      ONLY along the slope tangent. A normal-axis component
     *      (which the per-component cap of v0 produced on diagonal
     *      slopes) lifts the foot off the slope, the foot loses
     *      contact, grounded flips false, and apply_run_velocity
     *      enters the air branch. Net effect: the body "collapses"
     *      mid-climb. Tangent-only push keeps the foot planted.
     *
     * Pelvis is the canonical body-velocity read — the constraint
     * solver pulls all 16 parts to the same per-tick velocity in
     * steady state. */
    int pelv = m->particle_base + PART_PELVIS;
    float vx_now = p->pos_x[pelv] - p->prev_x[pelv];
    float vy_now = p->pos_y[pelv] - p->prev_y[pelv];

    float input_dir = 0.0f;
    if (vx_pxs > 0.0f) input_dir = +1.0f;
    if (vx_pxs < 0.0f) input_dir = -1.0f;

    if (input_dir == 0.0f) {
        /* No input on slope: return — gravity-along-tangent + the
         * per-contact friction in physics.c::contact_with_velocity
         * drive passive slide. On flat: friction-decel X toward 0;
         * leave Y to gravity / contact resolution. */
        if (!flat) return;
        float dx = -vx_now;
        float max_delta_x = GROUND_FRICTION_PXS2 * dt * dt;
        if (dx >  max_delta_x) dx =  max_delta_x;
        if (dx < -max_delta_x) dx = -max_delta_x;
        for (int part = 0; part < PART_COUNT; ++part) {
            int idx = m->particle_base + part;
            p->prev_x[idx] -= dx;
        }
        return;
    }

    /* Tangent rotated 90° from contact normal, signed by input
     * direction so it always points "the way the player wants to
     * go" along the surface. */
    float tx = -ny * input_dir;
    float ty =  nx * input_dir;
    float speed     = fabsf(vx_pxs);
    float vt_target = speed * dt;                       /* px/tick along tangent */
    float vt_now    = vx_now * tx + vy_now * ty;        /* signed scalar */

    /* Above the cap in the input direction → input does nothing. The
     * existing contact friction (physics.c:contact_with_velocity) is
     * the only thing that bleeds excess speed back toward the cap. */
    if (vt_now >= vt_target) return;

    /* Below the cap. ACCEL when motion is in input direction (or at
     * rest); DECEL (snappier) when motion opposes input — flick the
     * opposite direction for a fast reverse without springing the
     * steady-state ramp. */
    bool opposing = (vt_now < 0.0f);
    float rate_pxs2 = opposing ? GROUND_DECEL_PXS2 : GROUND_ACCEL_PXS2;
    float vt_delta  = vt_target - vt_now;       /* > 0 by construction */
    float max_delta = rate_pxs2 * dt * dt;
    if (vt_delta > max_delta) vt_delta = max_delta;

    /* Tangent-aligned delta — feet stay on the slope, body climbs /
     * descends along the surface, no normal-axis push. */
    float dx = vt_delta * tx;
    float dy = vt_delta * ty;
    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        p->prev_x[idx] -= dx;
        p->prev_y[idx] -= dy;
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

    /* P14 — jet pulse SFX, rate-limited every 4 ticks (~67 ms at
     * 60 Hz). Holding W must not machine-gun the cue. last_jet_pulse_
     * tick lives on the mutable world.mechs entry; the const Mech *
     * arg gives us read-only physics access but we re-derive the
     * non-const pointer for the field write. */
    if (w->tick - w->mechs[m->id].last_jet_pulse_tick >= 4) {
        Vec2 pelv = (Vec2){
            p->pos_x[b + PART_PELVIS],
            p->pos_y[b + PART_PELVIS],
        };
        audio_play_at(SFX_JET_PULSE, pelv);
        w->mechs[m->id].last_jet_pulse_tick = w->tick;
    }

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
        /* `grounded` for the rest of this block + footstep firing in
         * build_pose. Source depends on who owns physics for this mech:
         *   - Authoritative (server, offline-solo): all mechs run real
         *     physics, so `PARTICLE_FLAG_GROUNDED` is current.
         *   - Client local mech: prediction runs real physics, ditto.
         *   - Client remote mech: wan-fixes-3 sets inv_mass=0 on every
         *     remote particle, so the physics collision pass skips
         *     them and never sets PARTICLE_FLAG_GROUNDED. We must
         *     trust the snapshot-supplied `m->grounded` (decoded from
         *     SNAP_STATE_GROUNDED). Without this gate,
         *     any_foot_grounded() returns false for every remote mech,
         *     which silently breaks remote footstep audio (the
         *     swing→stance trigger in build_pose's ANIM_RUN case is
         *     gated on `m->grounded`). */
        bool grounded;
        if (w->authoritative || mid == w->local_mech_id) {
            grounded = any_foot_grounded(w, m);
            m->grounded = grounded;
        } else {
            grounded = m->grounded;
        }

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
                /* M6 P02 — boost cue. Fires on the owner side (server,
                 * or this client's local mech). Remote mechs on a pure
                 * client fire from snapshot_apply's leading-edge
                 * detection so the cue lands within one snapshot of
                 * the server's boost trigger. */
                audio_play_at(SFX_JET_BOOST, mech_chest_pos(w, mid));
                SHOT_LOG("t=%llu mech=%d jet_burst sfx=jet_boost",
                         (unsigned long long)w->tick, mid);
            }
            if (m->boost_timer > 0.0f) m->boost_timer -= dt;
        }

        if ((in.buttons & BTN_JUMP) && grounded) {
            apply_jump(w, m, JUMP_IMPULSE_PXS * ch->jump_mult, dt);
        }

        /* M6 P02 — Visual jet state flags. Owner side (server, or this
         * client's local mech) writes from real input + boost_timer +
         * grounded edge; remote mechs on a pure client get these from
         * snapshot_apply instead, so we leave them alone here.
         *
         * "Active this tick" mirrors the visible exhaust source:
         *   - standard/burst — BTN_JET held & fuel > 0 (the `jetting`
         *     local above)
         *   - glide-wing — same, plus the glide-while-empty branch
         *     (which produces lift at 0 fuel via jp->glide_thrust)
         *   - jump-jet — only on the press edge that consumes fuel
         *     (continuous BTN_JET holds produce no thrust). */
        bool fx_owner = w->authoritative || mid == w->local_mech_id;
        if (fx_owner) {
            bool was_grounded = m->jet_prev_grounded != 0;
            uint8_t new_bits = 0;
            bool jet_btn          = (in.buttons & BTN_JET) != 0;
            bool jet_press        = (pressed    & BTN_JET) != 0;
            bool jet_glide_empty  = !jp->jump_on_land
                                  && jet_btn
                                  && jp->glide_thrust > 0.0f
                                  && m->fuel <= 0.0f;
            bool jet_jump_active  = jp->jump_on_land
                                  && jet_press
                                  && m->fuel > 0.05f;
            bool jet_active = jetting || jet_glide_empty || jet_jump_active;
            if (jet_active) {
                new_bits |= MECH_JET_ACTIVE;
                /* Ignition edge — set on grounded → airborne for
                 * continuous-thrust jets. For JET_JUMP_JET the press
                 * IS the ignition (the airborne edge fires on the
                 * NEXT tick when BTN_JET is no longer held), so we
                 * set IGNITION_TICK directly on the press tick. */
                if (jet_jump_active) {
                    new_bits |= MECH_JET_IGNITION_TICK;
                } else if (was_grounded && !m->grounded) {
                    new_bits |= MECH_JET_IGNITION_TICK;
                }
                if (m->boost_timer > 0.0f) {
                    new_bits |= MECH_JET_BOOSTING;
                }
            }
            m->jet_state_bits    = new_bits;
            m->jet_prev_grounded = m->grounded ? 1u : 0u;
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
         * input-based path there.
         *
         * wan-fixes-3 followup — gait-lift hysteresis. The walk
         * cycle (build_pose's ANIM_RUN) lifts each foot off the
         * ground for ~3 ticks per swing. Without hysteresis, the
         * grounded check flipped tick-to-tick during walking and
         * anim_id flickered RUN → FALL → STAND → RUN. Invisible on
         * the server (constraints smooth it) but visible on the
         * client after wan-fixes-3 made remote mechs kinematic.
         * Track consecutive airborne ticks and only transition to
         * FALL after we've stayed off the ground long enough for it
         * to be a real jump / fall rather than a stride. */
        enum { GAIT_LIFT_MAX = 6 };
        if (grounded) m->air_ticks = 0;
        else if (m->air_ticks < 255u) m->air_ticks++;
        bool really_airborne = (m->air_ticks > (uint8_t)GAIT_LIFT_MAX);

        /* M6 — BTN_PRONE (X) and BTN_CROUCH (S/Down) drive new anim
         * states. Priority: a held-down state OVERRIDES walk/stand
         * but airborne states (JET / FALL) still take precedence.
         * Prone is more committed than crouch (a player who taps
         * crouch then prone ends up on the ground regardless of
         * which key they hold longest). */
        bool prone_held  = (in.buttons & BTN_PRONE)  != 0;
        bool crouch_held = (in.buttons & BTN_CROUCH) != 0;

        bool input_drives_anim = w->authoritative || mid == w->local_mech_id;
        if (input_drives_anim) {
            if (jetting)              m->anim_id = ANIM_JET;
            else if (really_airborne) m->anim_id = ANIM_FALL;
            else if (prone_held)      m->anim_id = ANIM_PRONE;
            else if (crouch_held)     m->anim_id = ANIM_CROUCH;
            else if (moving)          m->anim_id = ANIM_RUN;
            else                      m->anim_id = ANIM_STAND;
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
            /* M6 audit — was 800 px/s, dropped to 400. At the old rate
             * the rope shrank ~13 px/tick: combined with the hard
             * (non-stretchy) constraint solver the firer's pelvis was
             * yanked through any geometry between it and the anchor.
             * 400 px/s (~6.7 px/tick) is slow enough that the swept-
             * test inside solve_fixed_anchor + the stretchy constraint
             * keep the firer on the correct side of solid walls and
             * platforms. */
            const float GRAPPLE_RETRACT_PXS  = 400.0f;
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

    /* M6 — gait phase + footstep SFX runs only on the side that owns
     * prediction for this mech. Remote mechs on the client get their
     * gait_phase from snapshot_apply, and the wrap SFX fires there. */
    bool owns_mech = w->authoritative || mid == w->local_mech_id;
    if (owns_mech) {
        mech_update_gait(ch, w, m, dt);
    }
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
    /* M6 — also anchor CROUCH so the pelvis sits at the right height
     * for the new pose rules (crouching mech is half-height above
     * ground). PRONE is NOT anchored: in prone the legs rotate to
     * lie horizontally next to the pelvis, so `foot_y_avg` becomes
     * `pelvis.y` and a target of `foot_y - 14` would track the pelvis
     * itself and drag it ever-lower. Gravity + tile collision place
     * the pelvis at the ground surface naturally — the post-pose
     * terrain push then keeps it there. */
    if (m->anim_id != ANIM_STAND && m->anim_id != ANIM_RUN &&
        m->anim_id != ANIM_CROUCH) return;

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

    /* M6 — anchor height depends on anim_id. Standing: pelvis at full
     * leg-chain above ground. Crouching: pelvis at ~60% chain above
     * ground (knees bent). Prone: pelvis essentially at ground level
     * (body lying flat). */
    float chain_len = ch->bone_thigh + ch->bone_shin;
    float pelvis_y_target;
    if (m->anim_id == ANIM_CROUCH) {
        pelvis_y_target = foot_y - chain_len * 0.55f;
    } else {
        pelvis_y_target = foot_y - chain_len;
    }
    float knee_y_target = foot_y - ch->bone_shin;
    float dy_pelvis = pelvis_y_target - p->pos_y[b + PART_PELVIS];

    /* For STAND / RUN the anchor only pulls UP (counteracts gravity
     * sag). Pushing pelvis DOWN there would fight a falling mech.
     * For CROUCH the target is LOWER than standing height, so we need
     * to allow the anchor to push pelvis DOWN as well. */
    bool allow_downward = (m->anim_id == ANIM_CROUCH);
    if (allow_downward) {
        if (fabsf(dy_pelvis) < 0.1f) return;
    } else {
        if (dy_pelvis >= -0.1f) return;
    }

    /* Ceiling-clamp the upward shove. The kinematic translate below
     * does NOT collide; raw it would punch the head straight into a
     * low platform, after which the next-tick collision push-out
     * fights this anchor every tick and the mech locks in place
     * (the same "stuck in the ceiling" symptom called out at the
     * GRAPPLE_MIN_REST_LEN comment above). Sweep upward from the
     * head and clamp `dy_pelvis` so the head stops just under the
     * blocking tile/poly. When clamped to zero the mech visibly
     * stoops under the overhang — correct behavior. */
    if (dy_pelvis < 0.0f) {
        int   head_idx = b + PART_HEAD;
        Vec2  ha = (Vec2){ p->pos_x[head_idx], p->pos_y[head_idx] };
        Vec2  hb = (Vec2){ ha.x, ha.y + dy_pelvis };
        float t_hit = 1.0f;
        if (level_ray_hits(&w->level, ha, hb, &t_hit)) {
            float seg_len = -dy_pelvis;
            float t_clamp = t_hit - (PHYSICS_PARTICLE_RADIUS + 0.5f) / seg_len;
            if (t_clamp < 0.0f) t_clamp = 0.0f;
            dy_pelvis *= t_clamp;
            if (fabsf(dy_pelvis) < 0.1f) return;
        }
    }

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
            /* Lag-comp: pass m->input_view_tick (set by net.c on remote
             * peer inputs; 0 for host's own mech / AI). The lag-comp
             * function falls back to current-time hitscan when the tick
             * is 0 or out of LAG_HIST_TICKS range, so this is safe for
             * all callers. */
            weapons_fire_hitscan_lag_comp(w, mid, m->input_view_tick);
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
                audio_play_at(SFX_GRAPPLE_FIRE, origin);
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
        /* Route through lag-comp; falls back to current-time on
         * input_view_tick == 0 (host's own / AI mechs) or out-of-range. */
        weapons_fire_hitscan_lag_comp(w, mid, m->input_view_tick);
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
        audio_play_at(SFX_GRAPPLE_FIRE, origin);
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

void mech_record_damage_decal(World *w, int mid, int part, Vec2 hit_world,
                              float damage)
{
    if (mid < 0 || mid >= w->mech_count) return;
    Mech *m = &w->mechs[mid];
    MechSpriteId sp = mech_part_to_sprite_id(part);
    int p_a = -1, p_b = -1;
    mech_sprite_part_endpoints(sp, &p_a, &p_b);
    Vec2 mid_w; float decal_angle;
    if (p_a >= 0) {
        Vec2 va = part_pos(w, m, p_a);
        Vec2 vb = part_pos(w, m, p_b);
        mid_w.x = (va.x + vb.x) * 0.5f;
        mid_w.y = (va.y + vb.y) * 0.5f;
        /* Sprite art is authored vertically (parent end at top); the
         * renderer subtracts 90° from atan2. Match that here so the
         * decal's local-y axis aligns with the sprite's vertical. */
        decal_angle = atan2f(vb.y - va.y, vb.x - va.x) - 1.5707963267948966f;
    } else {
        mid_w = part_pos(w, m, p_b);
        decal_angle = 0.0f;
    }
    float dx = hit_world.x - mid_w.x;
    float dy = hit_world.y - mid_w.y;
    float c = cosf(-decal_angle), s = sinf(-decal_angle);
    float lx = dx * c - dy * s;
    float ly = dx * s + dy * c;
    if (lx < -127.0f) lx = -127.0f;
    if (lx >  127.0f) lx =  127.0f;
    if (ly < -127.0f) ly = -127.0f;
    if (ly >  127.0f) ly =  127.0f;
    uint8_t decal_kind = DAMAGE_DECAL_DENT;
    if (damage >= 80.0f)      decal_kind = DAMAGE_DECAL_GOUGE;
    else if (damage >= 30.0f) decal_kind = DAMAGE_DECAL_SCORCH;
    MechLimbDecals *ring = &m->damage_decals[sp];
    int slot = (int)((unsigned)ring->count % DAMAGE_DECALS_PER_LIMB);
    ring->items[slot] = (MechDamageDecal){
        .local_x  = (int8_t)lx,
        .local_y  = (int8_t)ly,
        .kind     = decal_kind,
        .reserved = 0,
    };
    if (ring->count < 255) ring->count++;
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

    /* P12 — Drive the matching limb HP to 0 so post-dismember smoke
     * fires at full intensity on BOTH server and client. On the
     * server limb HP is already ≤0 when mech_dismember triggers (it's
     * the trigger condition); on the client mech_dismember is called
     * from snapshot.c when a new dismember bit arrives, where the
     * client's limb HP may still read full because hit events are
     * unreliable. Zeroing here keeps the per-limb smoke check in
     * simulate_step consistent across both sides. */
    switch (limb) {
        case LIMB_HEAD:  m->hp_head  = 0.0f; break;
        case LIMB_L_ARM: m->hp_arm_l = 0.0f; break;
        case LIMB_R_ARM: m->hp_arm_r = 0.0f; break;
        case LIMB_L_LEG: m->hp_leg_l = 0.0f; break;
        case LIMB_R_LEG: m->hp_leg_r = 0.0f; break;
        default: break;
    }

    Vec2 sp = (Vec2){ w->particles.pos_x[base + joint_part],
                      w->particles.pos_y[base + joint_part] };
    /* P12 — Heavy initial spray: 64-particle radial fan from the joint.
     * `fx_spawn_blood` randomizes the angle within ±0.7 rad of the
     * passed direction; sweeping the full circle here gives even
     * coverage. Replaces the M3 `blood_spew_at` (32 particles, single
     * direction). */
    for (int i = 0; i < 64; ++i) {
        float angle = pcg32_float01(w->rng) * 6.283185307179586f;
        Vec2 vd = { cosf(angle) * 220.0f, sinf(angle) * 280.0f };
        fx_spawn_blood(&w->fx, sp, vd, w->rng);
    }
    /* P12 — Pinned dripping emitter for ~1.5 s. Tracks the parent
     * particle each tick so the trail follows the still-moving torso
     * even as the body ragdolls (or keeps fighting after a single
     * limb-loss). */
    fx_spawn_stump_emitter(&w->fx, mid, limb, 1.5f);

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
    /* (M6) — `clear_pose` removed along with `build_pose`. Once dead,
     * `pose_compute` is skipped (alive guard in simulate.c), and the
     * Verlet body free-falls per the ragdoll path. */
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

    /* P14 — death audio. Kill fanfare plays globally (no spatial pan)
     * for the local killer; death grunt plays at the victim's pelvis
     * for everyone (spectators, etc.). The local-mech checks fire on
     * whichever side runs mech_kill — server / offline solo runs the
     * full path; pure clients mirror death via snapshots and don't
     * call mech_kill, so client-side death audio is a v1 gap (TODO:
     * fold into client_handle_hit_event when victim hp goes to 0). */
    if (killer_mech_id >= 0 && killer_mech_id == w->local_mech_id &&
        killer_mech_id != mid)
    {
        audio_play_global(SFX_KILL_FANFARE);
    }
    {
        Vec2 vp = (Vec2){
            w->particles.pos_x[m->particle_base + PART_PELVIS],
            w->particles.pos_y[m->particle_base + PART_PELVIS],
        };
        audio_play_at(SFX_DEATH_GRUNT, vp);
    }

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
    /* P12 — Hit-flash tint: kicks every damage event to ~6 ticks of
     * white-additive flash on the body. Decayed each tick in
     * simulate_step. Distinct from `last_damage_taken` (above), which
     * stays pegged at the killing-blow amount for OVERKILL detection
     * in mech_kill. */
    m->hit_flash_timer = 0.10f;
    SHOT_LOG("t=%llu mech=%d damage part=%d dmg=%.1f hp=%.1f/%.1f mask=0x%02x",
             (unsigned long long)w->tick, mid, part, final_dmg,
             m->health, m->health_max, m->dismember_mask);

    /* P12 — Persistent damage decal. Records the hit's position in the
     * sprite-local space of whichever MSP covers the hit part, so the
     * decal stays glued to the limb sprite as the body moves. Shared
     * with the client-side hit-event handler in net.c so host/client
     * decal rings stay in lockstep. */
    mech_record_damage_decal(w, mid, part, part_pos(w, m, part), final_dmg);

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
