#include "loadout_preview.h"

#include "log.h"
#include "math.h"
#include "mech.h"
#include "mech_ik.h"
#include "mech_jet_fx.h"
#include "mech_sprites.h"
#include "platform.h"
#include "render.h"
#include "ui.h"
#include "weapons.h"

#include "../third_party/raylib/src/raymath.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- Choice arrays ------------------------------------------------- */
/* Mirror the lobby_ui static tables. Cycling inside the modal calls
 * `step_in_cycle` which walks these in either direction. Keeping a
 * local copy avoids pulling lobby_ui.c's statics through a header. */

static const int g_chassis_choices[]   = {
    CHASSIS_TROOPER, CHASSIS_SCOUT, CHASSIS_HEAVY, CHASSIS_SNIPER, CHASSIS_ENGINEER
};
static const int g_primary_choices[]   = {
    WEAPON_PULSE_RIFLE, WEAPON_PLASMA_SMG, WEAPON_RIOT_CANNON,
    WEAPON_RAIL_CANNON, WEAPON_AUTO_CANNON, WEAPON_MASS_DRIVER,
    WEAPON_PLASMA_CANNON, WEAPON_MICROGUN
};
static const int g_secondary_choices[] = {
    WEAPON_SIDEARM, WEAPON_BURST_SMG, WEAPON_FRAG_GRENADES,
    WEAPON_MICRO_ROCKETS, WEAPON_COMBAT_KNIFE, WEAPON_GRAPPLING_HOOK
};
static const int g_armor_choices[]   = { ARMOR_NONE, ARMOR_LIGHT, ARMOR_HEAVY, ARMOR_REACTIVE };
static const int g_jet_choices[]     = { JET_NONE, JET_STANDARD, JET_BURST, JET_GLIDE_WING, JET_JUMP_JET };

#define CHOICES_N(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

static int step_in_cycle(int current, const int *choices, int n, int step) {
    if (n <= 0) return current;
    int idx = 0;
    for (int i = 0; i < n; ++i) if (choices[i] == current) { idx = i; break; }
    int next = ((idx + step) % n + n) % n;
    return choices[next];
}

/* ---- Per-id flavor blurbs ------------------------------------------ */
/* Short one-sentence taglines used to compose the description pane.
 * Tables are static so a NULL entry is unreachable - but `desc_or`
 * guards anyway so an out-of-range id can't crash the renderer. */

static const char *g_chassis_role[CHASSIS_COUNT] = {
    [CHASSIS_TROOPER]  = "Versatile baseline - fast reload, no specialisation.",
    [CHASSIS_SCOUT]    = "Fast and fragile - dashes, vaults, and outflanks.",
    [CHASSIS_HEAVY]    = "Mobile fortress - heavy HP, slow, AOE-resistant.",
    [CHASSIS_SNIPER]   = "Long reach, steady aim - rewards positioning.",
    [CHASSIS_ENGINEER] = "Support frame - deploys repair packs on demand.",
};

static const char *g_chassis_passive[CHASSIS_COUNT] = {
    [CHASSIS_TROOPER]  = "Passive: Fast Reload (-25% reload time).",
    [CHASSIS_SCOUT]    = "Passive: Dash burst on demand (horizontal lunge).",
    [CHASSIS_HEAVY]    = "Passive: -10% damage from explosions.",
    [CHASSIS_SNIPER]   = "Passive: Lower bink/spread when grounded and still.",
    [CHASSIS_ENGINEER] = "Passive: Drop a 50 HP repair pack on cooldown.",
};

static const char *g_chassis_tactic[CHASSIS_COUNT] = {
    [CHASSIS_TROOPER]  = "Best for: learning fundamentals, holding chokes.",
    [CHASSIS_SCOUT]    = "Best for: flag runs, flanking, dodging artillery.",
    [CHASSIS_HEAVY]    = "Best for: pushing chokes, soaking AOE, anchoring.",
    [CHASSIS_SNIPER]   = "Best for: overwatch, picking targets across the map.",
    [CHASSIS_ENGINEER] = "Best for: sustaining teammates near pickups.",
};

static const char *g_weapon_blurb[WEAPON_COUNT] = {
    [WEAPON_PULSE_RIFLE]   = "Steady mid-range hitscan automatic. The reliable workhorse.",
    [WEAPON_PLASMA_SMG]    = "Fast plasma projectiles - high mag, modest damage.",
    [WEAPON_RIOT_CANNON]   = "Close-range shotgun spread - devastating up close.",
    [WEAPON_RAIL_CANNON]   = "Charge-shot precision hitscan - long range, low cadence.",
    [WEAPON_AUTO_CANNON]   = "Heavy projectile rifle - power between SMG and Rail.",
    [WEAPON_MASS_DRIVER]   = "Slow, devastating AOE rocket. One-shot threat.",
    [WEAPON_PLASMA_CANNON] = "Lobbed plasma orbs - small AOE, tactical reach.",
    [WEAPON_MICROGUN]      = "Spin-up suspressive fire - sustained DPS king.",

    [WEAPON_SIDEARM]       = "Reliable hitscan backup - always-on safety net.",
    [WEAPON_BURST_SMG]     = "3-round bursts - focused damage at close range.",
    [WEAPON_FRAG_GRENADES] = "Bouncing AOE - flush cover and clear chokes.",
    [WEAPON_MICRO_ROCKETS] = "Direct-fire mini-rockets with mild AOE on impact.",
    [WEAPON_COMBAT_KNIFE]  = "Lethal melee - backstabs double damage.",
    [WEAPON_GRAPPLING_HOOK]= "Tarzan-swing utility - mobility multiplier.",
};

static const char *g_armor_blurb[ARMOR_COUNT] = {
    [ARMOR_NONE]     = "No armor - maximum mobility, zero buffer.",
    [ARMOR_LIGHT]    = "Light plate - 30 HP buffer, no movement penalty.",
    [ARMOR_HEAVY]    = "Heavy plate - 75 HP buffer at -10% movement.",
    [ARMOR_REACTIVE] = "Reactive - single-shot 100% explosion negation.",
};

static const char *g_jet_blurb[JET_COUNT] = {
    [JET_NONE]       = "Baseline jet - no enhancement.",
    [JET_STANDARD]   = "+20% fuel, +10% thrust - solid all-rounder.",
    [JET_BURST]      = "Boost charge - burns fuel for 2x thrust burst.",
    [JET_GLIDE_WING] = "Provides lift even at empty fuel - chase glider.",
    [JET_JUMP_JET]   = "No thrust - rearms a jump on each ground touch.",
};

static const char *desc_or(const char *s, const char *fallback) {
    return (s && *s) ? s : fallback;
}

/* ---- Spider chart axes --------------------------------------------- */
/* Six axes computed per loadout. All values normalised to 0..1 against
 * the global max across all loadouts (computed lazily on first call).
 * Layout: axis index → label + value derivation.
 *
 *   0 SPEED      - chassis.run_mult * armor.run_mult
 *   1 ARMOR      - chassis.health_max + armor.hp
 *   2 FIREPOWER  - primary DPS (damage / fire_rate_sec)
 *   3 RANGE      - primary.range_px (melee secondaries count too)
 *   4 MOBILITY   - composite jet + fuel capability
 *   5 UTILITY    - secondary's tactical contribution (table-driven)
 */
#define AXIS_COUNT 6

static const char *g_axis_labels[AXIS_COUNT] = {
    "SPEED", "ARMOR", "POWER", "RANGE", "MOBILITY", "UTILITY"
};

/* Hand-tuned utility values per secondary. Mirrors the design intent
 * documented in `documents/04-combat.md` §"Secondary roles". */
static float secondary_utility(int sec_id) {
    switch (sec_id) {
        case WEAPON_SIDEARM:        return 0.30f;
        case WEAPON_BURST_SMG:      return 0.45f;
        case WEAPON_FRAG_GRENADES:  return 0.75f;
        case WEAPON_MICRO_ROCKETS:  return 0.65f;
        case WEAPON_COMBAT_KNIFE:   return 0.55f;
        case WEAPON_GRAPPLING_HOOK: return 1.00f;
    }
    return 0.30f;
}

/* Treat JumpJet's "no thrust" as a fixed bonus rather than zero - its
 * mobility value comes from the rearmed-jump mechanic, not from sustained
 * thrust. Without this special case the chart would falsely show every
 * JumpJet loadout as immobile. */
static float jet_effective_thrust(const Jetpack *j) {
    if (!j) return 1.0f;
    if (j->thrust_mult > 0.05f) return j->thrust_mult;
    if (j->jump_on_land)        return 0.60f;
    if (j->glide_thrust > 0.0f) return 0.85f;
    return j->thrust_mult;
}

static float raw_axis(int axis, int chassis_id, int primary_id,
                      int secondary_id, int armor_id, int jet_id)
{
    const Chassis *ch = mech_chassis(chassis_id);
    const Weapon  *pw = weapon_def(primary_id);
    const Armor   *ar = armor_def(armor_id);
    const Jetpack *jt = jetpack_def(jet_id);
    if (!ch || !pw || !ar || !jt) return 0.0f;

    switch (axis) {
        case 0: /* SPEED */
            return ch->run_mult * ar->run_mult;
        case 1: /* ARMOR */
            return ch->health_max + ar->hp;
        case 2: /* FIREPOWER */
            if (pw->fire_rate_sec <= 1e-3f) return pw->damage;
            return pw->damage / pw->fire_rate_sec;
        case 3: /* RANGE */
            return pw->range_px;
        case 4: { /* MOBILITY */
            float thrust = jet_effective_thrust(jt);
            return ch->jet_mult * ch->fuel_max
                   * thrust * jt->fuel_mult
                   * ar->jet_mult;
        }
        case 5: /* UTILITY */
            return secondary_utility(secondary_id);
    }
    return 0.0f;
}

/* Cached per-axis max across all possible loadouts. Filled once on first
 * call so the chart renders absolute-comparison values without scanning
 * every option each frame. */
static float g_axis_max[AXIS_COUNT];
static bool  g_axis_max_ready;

static void compute_axis_max(void) {
    if (g_axis_max_ready) return;
    for (int a = 0; a < AXIS_COUNT; ++a) g_axis_max[a] = 0.0f;
    for (int ci = 0; ci < CHOICES_N(g_chassis_choices); ++ci) {
        for (int pi = 0; pi < CHOICES_N(g_primary_choices); ++pi) {
            for (int si = 0; si < CHOICES_N(g_secondary_choices); ++si) {
                for (int ai = 0; ai < CHOICES_N(g_armor_choices); ++ai) {
                    for (int ji = 0; ji < CHOICES_N(g_jet_choices); ++ji) {
                        for (int a = 0; a < AXIS_COUNT; ++a) {
                            float v = raw_axis(a,
                                g_chassis_choices[ci], g_primary_choices[pi],
                                g_secondary_choices[si], g_armor_choices[ai],
                                g_jet_choices[ji]);
                            if (v > g_axis_max[a]) g_axis_max[a] = v;
                        }
                    }
                }
            }
        }
    }
    /* Floor each max at 1.0 so a degenerate "all zero" axis (shouldn't
     * happen for our tables) doesn't divide by zero downstream. */
    for (int a = 0; a < AXIS_COUNT; ++a) {
        if (g_axis_max[a] <= 1e-6f) g_axis_max[a] = 1.0f;
    }
    g_axis_max_ready = true;
}

static float norm_axis(int axis, int chassis_id, int primary_id,
                       int secondary_id, int armor_id, int jet_id)
{
    compute_axis_max();
    float v = raw_axis(axis, chassis_id, primary_id, secondary_id,
                       armor_id, jet_id);
    float m = g_axis_max[axis];
    if (m <= 1e-6f) return 0.0f;
    float n = v / m;
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    return n;
}

/* ---- Particle pool / mech scratchpad ------------------------------- */

static void bind_pool(LoadoutPreview *st) {
    ParticlePool *p = &st->particles;
    p->pos_x          = st->buf_pos_x;
    p->pos_y          = st->buf_pos_y;
    p->prev_x         = st->buf_prev_x;
    p->prev_y         = st->buf_prev_y;
    p->inv_mass       = st->buf_inv_mass;
    p->flags          = st->buf_flags;
    p->contact_nx_q   = st->buf_contact_nx_q;
    p->contact_ny_q   = st->buf_contact_ny_q;
    p->contact_kind   = st->buf_contact_kind;
    p->render_prev_x  = st->buf_render_prev_x;
    p->render_prev_y  = st->buf_render_prev_y;
    p->count          = PART_COUNT;
    p->capacity       = PART_COUNT;
    /* Empty constraints - render's bone_constraint_active returns true
     * for unknown pairs, so all bones render. */
    st->constraints.items    = NULL;
    st->constraints.count    = 0;
    st->constraints.capacity = 0;
    for (int i = 0; i < PART_COUNT; ++i) {
        st->buf_inv_mass[i]    = 1.0f;
        st->buf_flags[i]       = 0;
        st->buf_contact_nx_q[i]= 0;
        st->buf_contact_ny_q[i]= 0;
        st->buf_contact_kind[i]= 0;
    }
}

static void mech_set_loadout(Mech *m, int chassis_id, int primary_id,
                             int secondary_id, int armor_id, int jet_id)
{
    memset(m, 0, sizeof(*m));
    const Chassis *ch = mech_chassis(chassis_id);
    const Armor   *ar = armor_def(armor_id);
    m->id           = 0;
    m->chassis_id   = chassis_id;
    m->primary_id   = primary_id;
    m->secondary_id = secondary_id;
    m->armor_id     = armor_id;
    m->jetpack_id   = jet_id;
    m->active_slot  = 0;
    m->weapon_id    = primary_id;
    m->team         = 0;
    m->alive        = true;
    m->grounded     = true;
    m->facing_left  = false;
    m->is_dummy     = false;
    m->particle_base= 0;
    m->dismember_mask = 0;
    if (ch) {
        m->health     = ch->health_max;
        m->health_max = ch->health_max;
        m->fuel       = ch->fuel_max;
        m->fuel_max   = ch->fuel_max;
    }
    if (ar) {
        m->armor_hp     = ar->hp;
        m->armor_hp_max = ar->hp;
    }
    m->hp_arm_l = m->hp_arm_r = m->hp_leg_l = m->hp_leg_r = 50.0f;
    m->hp_head  = 50.0f;
    m->anim_id  = ANIM_STAND;
    m->aim_world = (Vec2){ 200.0f, 0.0f };
    m->last_fired_slot = -1;
    m->last_fired_tick = 0;
    m->grapple.state = GRAPPLE_IDLE;
    m->grapple.constraint_idx = -1;
}

/* ---- Animation cycle ----------------------------------------------- */
/* 16-second loop. Walks the player through every locomotion mode +
 * defensive stance + both weapon slots so a single viewing covers the
 * whole expressive surface of the chassis + loadout. */

typedef enum {
    PHASE_IDLE_PRIMARY = 0,
    PHASE_WALK,
    PHASE_SPRINT,
    PHASE_JUMP,
    PHASE_JET,
    PHASE_CROUCH,
    PHASE_PRONE,
    PHASE_IDLE_SECONDARY,
} PreviewPhase;

typedef struct {
    PreviewPhase phase;
    float        t_start;
    float        t_end;
    const char  *label;
} PhaseRange;

/* Phase order is deliberate: IDLE_PRIMARY first, then IDLE_SECONDARY
 * second, so a player who opened the modal and barely glances at it
 * (under 4 s) still sees both weapons in their loadout. The cycle
 * buttons in `loadout_preview_update_and_draw` also jump cycle_time
 * to the matching phase when a slot changes, so an explicit "cycle
 * the secondary" gesture lands the player on PHASE_IDLE_SECONDARY
 * within ~0.2 s of the click. */
static const PhaseRange g_phases[] = {
    { PHASE_IDLE_PRIMARY,    0.0f,  2.0f,  "IDLE"      },
    { PHASE_IDLE_SECONDARY,  2.0f,  4.0f,  "SECONDARY" },
    { PHASE_WALK,            4.0f,  6.0f,  "WALK"      },
    { PHASE_SPRINT,          6.0f,  8.0f,  "SPRINT"    },
    { PHASE_JUMP,            8.0f,  9.5f,  "JUMP"      },
    { PHASE_JET,             9.5f, 12.5f,  "JET"       },
    { PHASE_CROUCH,         12.5f, 14.0f,  "CROUCH"    },
    { PHASE_PRONE,          14.0f, 16.0f,  "PRONE"     },
};
#define PHASE_ENTRIES ((int)(sizeof g_phases / sizeof g_phases[0]))
#define CYCLE_PERIOD 16.0f

/* Look up the table entry for a given PreviewPhase value — used by the
 * cycle button handlers to know which cycle_time value to jump to. */
static const PhaseRange *phase_entry(PreviewPhase ph) {
    for (int i = 0; i < PHASE_ENTRIES; ++i) {
        if (g_phases[i].phase == ph) return &g_phases[i];
    }
    return &g_phases[0];
}

static const PhaseRange *find_phase(float t) {
    for (int i = 0; i < PHASE_ENTRIES; ++i) {
        if (t >= g_phases[i].t_start && t < g_phases[i].t_end) {
            return &g_phases[i];
        }
    }
    return &g_phases[0];
}

static const char *cycle_phase_label(float t) {
    return find_phase(t)->label;
}

/* True while the synthetic mech is in its JET phase. Used by the
 * exhaust renderer to know whether to emit particles. */
static bool cycle_is_jet(float t) {
    return find_phase(t)->phase == PHASE_JET;
}

static void advance_pose(LoadoutPreview *st, float dt) {
    st->cycle_time += dt;
    float t = fmodf(st->cycle_time, CYCLE_PERIOD);
    const PhaseRange *ph = find_phase(t);
    float phase_dur  = ph->t_end - ph->t_start;
    float t_in_phase = t - ph->t_start;
    float t_to_end   = ph->t_end - t;

    /* Defaults — every case overrides the bits that matter. */
    Mech *m = &st->mech;
    m->anim_id        = ANIM_STAND;
    m->grounded       = true;
    m->jet_state_bits = 0;
    m->active_slot    = 0;
    m->weapon_id      = m->primary_id;
    float pelvis_y    = 0.0f;
    float gait_advance = 0.0f;
    float aim_amp     = 0.45f;   /* radians — multiplier on the aim sweep */

    switch (ph->phase) {
    case PHASE_IDLE_PRIMARY:
        m->anim_id = ANIM_STAND;
        break;

    case PHASE_WALK:
        m->anim_id   = ANIM_RUN;
        gait_advance = 1.6f;
        break;

    case PHASE_SPRINT:
        m->anim_id   = ANIM_RUN;
        gait_advance = 2.8f;
        break;

    case PHASE_JUMP: {
        /* Parabolic pelvis arc: feet leave the ground, peak around the
         * midpoint, settle back. Use ANIM_FALL so the upper body reads
         * as airborne (chest a little forward, arms flared for balance);
         * the pelvis offset is what sells "I jumped" rather than "I'm
         * standing in the air". */
        float t01 = clampf(t_in_phase / phase_dur, 0.0f, 1.0f);
        m->anim_id  = ANIM_FALL;
        m->grounded = false;
        pelvis_y    = -55.0f * sinf(PI * t01);
        break;
    }

    case PHASE_JET: {
        float ease_in  = clampf(t_in_phase / 0.40f, 0.0f, 1.0f);
        float ease_out = clampf(t_to_end   / 0.40f, 0.0f, 1.0f);
        float ease     = ease_in * ease_out;
        float hover    = -22.0f + sinf(st->cycle_time * 5.5f) * 3.0f;
        pelvis_y       = hover * ease;
        m->anim_id     = ANIM_JET;
        m->grounded    = false;
        m->jet_state_bits = MECH_JET_ACTIVE;
        break;
    }

    case PHASE_CROUCH:
        m->anim_id = ANIM_CROUCH;
        /* Lower body slightly so the pose reads as ducked rather than
         * just "standing with bent knees". pose_compute's CROUCH case
         * already shortens the legs; this offset puts the feet near
         * the original ground line. */
        pelvis_y   = 10.0f;
        aim_amp    = 0.20f;
        break;

    case PHASE_PRONE:
        m->anim_id = ANIM_PRONE;
        pelvis_y   = 22.0f;
        aim_amp    = 0.10f;
        break;

    case PHASE_IDLE_SECONDARY:
        /* Showcase the secondary weapon. active_slot=1 also drives the
         * Engineer's "skip right-arm aim when slot=1" quirk through
         * pose_compute, so the player sees that specialisation here. */
        m->anim_id     = ANIM_STAND;
        m->active_slot = 1;
        m->weapon_id   = m->secondary_id;
        break;
    }

    if (gait_advance > 0.0f) {
        st->gait_phase += dt * gait_advance;
        st->gait_phase = fmodf(st->gait_phase, 1.0f);
        if (st->gait_phase < 0.0f) st->gait_phase += 1.0f;
    }

    float sweep = sinf(st->cycle_time * 0.9f) * aim_amp;
    Vec2 aim_dir = (Vec2){ cosf(sweep), sinf(sweep) * 0.35f };
    aim_dir = Vector2Normalize(aim_dir);
    m->aim_world = (Vec2){ aim_dir.x * 200.0f, aim_dir.y * 200.0f };

    Vec2 pelvis = (Vec2){ 0.0f, pelvis_y };
    PoseInputs pi = (PoseInputs){
        .pelvis         = pelvis,
        .aim_dir        = aim_dir,
        .facing_left    = false,
        .is_dummy       = false,
        .anim_id        = m->anim_id,
        .gait_phase     = st->gait_phase,
        .grounded       = m->grounded,
        .chassis_id     = m->chassis_id,
        .active_slot    = m->active_slot,
        .dismember_mask = 0,
        .foregrip_world = NULL,
        .grapple_state  = GRAPPLE_IDLE,
        .grapple_anchor = (Vec2){0, 0},
        .ground_normal  = (Vec2){0, -1},
    };
    PoseBones bones;
    pose_compute(&pi, bones);
    for (int i = 0; i < PART_COUNT; ++i) {
        st->buf_pos_x[i]         = bones[i].x;
        st->buf_pos_y[i]         = bones[i].y;
        st->buf_prev_x[i]        = bones[i].x;
        st->buf_prev_y[i]        = bones[i].y;
        st->buf_render_prev_x[i] = bones[i].x;
        st->buf_render_prev_y[i] = bones[i].y;
    }
}

/* RGBA8 (0xRRGGBBAA) -> raylib Color. Matches the layout used in
 * `mech_jet_fx.c::g_jet_fx`. */
static Color rgba8_to_color(uint32_t v) {
    return (Color){
        (uint8_t)((v >> 24) & 0xFFu),
        (uint8_t)((v >> 16) & 0xFFu),
        (uint8_t)((v >>  8) & 0xFFu),
        (uint8_t)( v        & 0xFFu),
    };
}

static Color color_lerp(Color a, Color b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return (Color){
        (uint8_t)((float)a.r + ((float)b.r - (float)a.r) * t),
        (uint8_t)((float)a.g + ((float)b.g - (float)a.g) * t),
        (uint8_t)((float)a.b + ((float)b.b - (float)a.b) * t),
        (uint8_t)((float)a.a + ((float)b.a - (float)a.a) * t),
    };
}

/* Draw the jet exhaust during PHASE_JET using the same per-jetpack
 * tuning as the in-game FX path (mech_jet_fx.c::g_jet_fx):
 *
 *   - JET_STANDARD : single nozzle, blue ion plume, continuous
 *   - JET_BURST    : twin nozzles, orange/red plume, larger
 *   - JET_GLIDE_WING: twin nozzles, pale cyan, intermittent (4 Hz strobe)
 *   - JET_JUMP_JET : single nozzle, green, brief ignition pop (no
 *                    continuous plume)
 *   - JET_NONE     : nothing
 *
 * Nozzle position uses g_chassis_nozzles[chassis] so the plume anchors
 * to the same back/shoulder/hip point the real game uses. Caller must
 * be inside an active BeginMode2D scope. */
static void draw_jet_exhaust(LoadoutPreview *st, float cycle_time) {
    float t = fmodf(cycle_time, CYCLE_PERIOD);
    if (!cycle_is_jet(t)) return;

    int jet_id = st->mech.jetpack_id;
    if (jet_id <= JET_NONE || jet_id >= JET_COUNT) return;
    const JetFxDef *def = &g_jet_fx[jet_id];

    int chassis_id = st->mech.chassis_id;
    if (chassis_id < 0 || chassis_id >= CHASSIS_COUNT) return;
    const ChassisNozzleSet *nset = &g_chassis_nozzles[chassis_id];

    /* Phase-local timeline. PHASE_JET runs from CYCLE_SPRINT_END..
     * CYCLE_JET_END; pull those out via the phase table so this stays
     * in sync if I retune the cycle later. */
    const PhaseRange *ph = find_phase(t);
    float t_in_phase = t - ph->t_start;
    float t_to_end   = ph->t_end - t;

    /* Base intensity envelope - eases in/out so the flame doesn't
     * pop on at full brightness. */
    float ease_in  = clampf(t_in_phase / 0.30f, 0.0f, 1.0f);
    float ease_out = clampf(t_to_end   / 0.30f, 0.0f, 1.0f);
    float intensity = ease_in * ease_out;

    /* Per-jetpack modulation on top of the envelope:
     *
     *   STANDARD       : steady (intensity unchanged)
     *   BURST          : steady, treated as "boosting" for the visual
     *                    (length-scale boost matches the in-game x1.6)
     *   GLIDE_WING     : strobe at 4 Hz so it reads as intermittent
     *                    (mirrors the sustain_tick_divisor=4 cadence)
     *   JUMP_JET       : `has_continuous_plume=false` -> burst-only.
     *                    Pop a fresh ignition every ~1.2 s of phase,
     *                    fading over 0.4 s so the player still sees
     *                    the colour without a constant green stream. */
    float length_scale = 1.0f;
    if (jet_id == JET_BURST) {
        length_scale = 1.6f;        /* boost-equivalent visual size */
    }
    if (def->sustain_tick_divisor > 1) {
        float strobe = fmodf(cycle_time * 6.0f, 1.0f);
        intensity *= (strobe < 0.45f) ? 1.0f : 0.25f;
    }
    if (!def->has_continuous_plume) {
        /* Repeating ignition burst — visible 0..0.4s of each 1.2s cycle. */
        float pop = fmodf(t_in_phase, 1.2f);
        intensity *= (pop < 0.40f) ? (1.0f - pop / 0.40f) : 0.0f;
        length_scale = 0.85f;
    }
    if (intensity <= 0.01f) return;

    /* Anchor the visible flame near the FEET rather than the in-game
     * chassis nozzle (which is back/shoulder-mounted on most chassis).
     * In-game the camera is zoomed out and the long plumes extend far
     * past the body silhouette so the chassis nozzle reads. The
     * preview camera is much tighter, and the shorter jets (GLIDE_WING
     * 36 px, JUMP_JET 0 px) would draw entirely inside the mech sprite
     * and get covered. Anchoring at the feet guarantees every jet's
     * colour is visible. The chassis nozzle's x-offset is still used
     * to keep a small per-chassis lateral variation. */
    float pelv_x = st->buf_pos_x[PART_PELVIS];
    float foot_y = (st->buf_pos_y[PART_L_FOOT]
                  + st->buf_pos_y[PART_R_FOOT]) * 0.5f;
    const float face_dir = 1.0f;   /* preview mech always faces right */

    Color rim  = rgba8_to_color(def->plume_color_rim);
    Color core = rgba8_to_color(def->plume_color_core);
    Color hot  = rgba8_to_color(def->particle_color_hot);
    Color cool = rgba8_to_color(def->particle_color_cool);

    int n_nozzles = (def->nozzle_count == 2) ? 2 : 1;
    float plume_len = def->plume_length_px * length_scale;
    float plume_w   = def->plume_width_px;
    /* Readability floors. JUMP_JET in particular has plume_length_px=0
     * in the table (it's a burst-only jet); we still want SOMETHING
     * coloured to look at, sized comparably to the other plumes. */
    if (plume_len < 50.0f) plume_len = 50.0f;
    if (plume_w   < 12.0f) plume_w   = 12.0f;

    BeginBlendMode(BLEND_ADDITIVE);

    for (int k = 0; k < n_nozzles; ++k) {
        /* Both slots share slot[0]'s offset + mirror around body-local
         * x — matches `mech_jet_fx_draw_plumes`. */
        const ChassisNozzle *nz = &nset->slot[0];
        if (!nz->active) continue;
        float mirror = (n_nozzles == 2 && k == 1) ? -1.0f : 1.0f;
        /* For dual nozzles, splay them ~8 px to either side of the
         * pelvis centerline so the two flames don't render on top of
         * each other. The in-game chassis offsets give a similar 6-8
         * px splay relative to the chassis silhouette. */
        float splay = (n_nozzles == 2) ? (mirror * 8.0f) : 0.0f;
        Vec2 noz = (Vec2){
            pelv_x + face_dir * (nz->offset.x * mirror) + splay,
            foot_y - 4.0f,
        };
        /* thrust_dir is the unit vector along the exhaust direction
         * (mostly down with a small chassis-specific outward angle). */
        Vec2 tdir = nz->thrust_dir;
        if (n_nozzles == 2 && k == 1) tdir.x = -tdir.x;

        /* Plume body — tapered series of overlapping ovals along
         * thrust_dir, fading from core color near the nozzle to rim
         * color at the tip. */
        int   steps = 9;
        float pulse = sinf(cycle_time * 14.0f + (float)k * 0.6f) * 0.10f + 1.0f;
        for (int i = 0; i < steps; ++i) {
            float f      = (float)i / (float)(steps - 1);
            float along  = plume_len * f * pulse;
            float jitter = sinf(cycle_time * 22.0f + (float)i * 1.8f
                                + (float)k * 0.9f) * 1.1f;
            Vec2 perp = (Vec2){ -tdir.y, tdir.x };
            Vec2 pos  = (Vec2){
                noz.x + tdir.x * along + perp.x * jitter,
                noz.y + tdir.y * along + perp.y * jitter,
            };
            float radius = (plume_w * 0.55f) * (1.0f - 0.60f * f);
            Color c = color_lerp(core, rim, f);
            uint8_t alpha = (uint8_t)((float)c.a * (1.0f - f) * intensity);
            DrawCircleV((Vector2){pos.x, pos.y}, radius,
                        (Color){c.r, c.g, c.b, alpha});
        }

        /* Trailing particles — hot near the nozzle, cooling toward the
         * tail, alpha decaying so the stream fades out. */
        int   particles = (def->sustain_particles_per_tick > 0)
                            ? (def->sustain_particles_per_tick + 4) : 5;
        if (particles > 8) particles = 8;
        for (int i = 0; i < particles; ++i) {
            float f      = (float)i / (float)particles;
            float drift  = fmodf(cycle_time * (def->particle_speed_pxs *
                                               0.012f) + f, 1.0f);
            float along  = plume_len * (0.30f + drift * 0.95f);
            float jitter = sinf(cycle_time * 17.0f + (float)i * 2.3f
                                + (float)k * 1.1f) * 2.0f;
            Vec2 perp = (Vec2){ -tdir.y, tdir.x };
            Vec2 pos  = (Vec2){
                noz.x + tdir.x * along + perp.x * jitter,
                noz.y + tdir.y * along + perp.y * jitter,
            };
            float radius = lerpf(def->particle_size_min,
                                 def->particle_size_max,
                                 sinf(cycle_time * 9.0f + (float)i) * 0.5f + 0.5f);
            Color c = color_lerp(hot, cool, drift);
            uint8_t alpha = (uint8_t)((float)c.a * (1.0f - drift) * intensity);
            DrawCircleV((Vector2){pos.x, pos.y}, radius,
                        (Color){c.r, c.g, c.b, alpha});
        }

        /* Bright inner-hot core anchored at the nozzle. */
        DrawCircleV((Vector2){noz.x + tdir.x * 2.0f,
                              noz.y + tdir.y * 2.0f},
                    (plume_w * 0.45f) * (0.85f + 0.25f *
                        sinf(cycle_time * 24.0f + (float)k)),
                    (Color){
                        (uint8_t)((core.r + 255) / 2),
                        (uint8_t)((core.g + 255) / 2),
                        (uint8_t)((core.b + 255) / 2),
                        (uint8_t)(220.0f * intensity),
                    });
    }
    EndBlendMode();
}

/* ---- Lifecycle ----------------------------------------------------- */

void loadout_preview_init(LoadoutPreview *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    bind_pool(st);
    mech_set_loadout(&st->mech, CHASSIS_TROOPER, WEAPON_PULSE_RIFLE,
                     WEAPON_SIDEARM, ARMOR_LIGHT, JET_STANDARD);
    st->pulse_base_time = 0.0;
    st->cycle_time      = 0.0f;
    st->gait_phase      = 0.0f;
    st->open            = false;
    st->rt_ready        = false;
    st->rt_w = 0;
    st->rt_h = 0;
}

void loadout_preview_shutdown(LoadoutPreview *st) {
    if (!st) return;
    if (st->rt_ready) {
        UnloadRenderTexture(st->rt);
        st->rt_ready = false;
    }
}

bool loadout_preview_is_open(const LoadoutPreview *st) {
    return st && st->open;
}

void loadout_preview_open(LoadoutPreview *st) {
    if (!st) return;
    st->open = true;
    st->cycle_time = 0.0f;
    st->gait_phase = 0.0f;
}

void loadout_preview_close(LoadoutPreview *st) {
    if (!st) return;
    st->open = false;
}

/* ---- Preview button ------------------------------------------------ */

bool loadout_preview_draw_button(LoadoutPreview *st, struct UIContext *u,
                                 Rectangle rect, bool enabled)
{
    if (!st || !u) return false;
    bool hover = enabled && ui_point_in_rect(u->mouse, rect);

    /* Cyan-purple gradient pulse - distinct from the Ready button's
     * green so the eye treats them as separate affordances. The pulse
     * is brighter when not hovered (announcing itself); flattens on
     * hover (reads as "ready to click"). */
    double now = GetTime();
    float pulse = 0.5f + 0.5f * sinf((float)(now - st->pulse_base_time) * 2.4f);
    Color bg;
    if (!enabled) {
        bg = (Color){50, 56, 70, 200};
    } else if (hover) {
        bg = (Color){120, 180, 240, 255};
    } else {
        unsigned char r = (unsigned char)(60  + (int)(pulse * 30));
        unsigned char G = (unsigned char)(140 + (int)(pulse * 40));
        unsigned char B = (unsigned char)(200 + (int)(pulse * 40));
        bg = (Color){r, G, B, 255};
    }
    DrawRectangleRec(rect, bg);

    Color edge = enabled ? (Color){180, 230, 255, 255}
                         : (Color){90, 100, 120, 200};
    DrawRectangleLinesEx(rect, u->scale * 2.0f, edge);

    /* Glow ring - a faint outer rectangle that pulses outward to draw
     * the eye without crowding the rest of the loadout panel. Skipped
     * when disabled. */
    if (enabled && !hover) {
        float glow_inset = -u->scale * 3.0f * pulse;
        Rectangle gr = (Rectangle){
            rect.x + glow_inset, rect.y + glow_inset,
            rect.width - glow_inset * 2.0f,
            rect.height - glow_inset * 2.0f,
        };
        Color glow = (Color){ 140, 200, 255, (uint8_t)(60 + (int)(pulse * 60)) };
        DrawRectangleLinesEx(gr, u->scale * 1.5f, glow);
    }

    /* ASCII-only label. The lobby's previous design exposed five cycle
     * buttons + a preview button; M6 round 2 makes the modal the single
     * place to change loadout, so the chrome reads as "CHOOSE LOADOUT"
     * rather than the read-only "PREVIEW" connotation. */
    const char *label = "[ CHOOSE LOADOUT ]";
    int font_px = 22;
    int tw = ui_measure(u, label, font_px);
    int th = (int)((float)font_px * u->scale + 0.5f);
    Color text_col = enabled ? (Color){12, 22, 32, 255}
                             : (Color){140, 150, 160, 255};
    ui_draw_text(u, label,
                 (int)(rect.x + (rect.width - (float)tw) * 0.5f),
                 (int)(rect.y + (rect.height - (float)th) * 0.5f),
                 font_px, text_col);

    return hover && u->mouse_pressed;
}

/* ---- Render: treadmill backdrop + mech ----------------------------- */

static void draw_treadmill_bg(int rt_w, int rt_h, float cycle_time,
                              const char *phase_label, const UIContext *u)
{
    /* Soft graduated background - top dark blue, bottom darker. */
    DrawRectangle(0, 0, rt_w, rt_h, (Color){ 16, 24, 38, 255 });
    for (int i = 0; i < 24; ++i) {
        int y = (rt_h * i) / 24;
        Color c = (Color){
            (uint8_t)(16 + i),
            (uint8_t)(24 + i / 2),
            (uint8_t)(38 + i / 3),
            255,
        };
        DrawRectangle(0, y, rt_w, rt_h / 24 + 1, c);
    }

    /* Horizontal floor strip at the bottom of the RT. */
    int floor_y = rt_h - 70;
    DrawRectangle(0, floor_y, rt_w, 4, (Color){80, 90, 110, 255});
    DrawRectangle(0, floor_y + 4, rt_w, rt_h - floor_y - 4,
                  (Color){22, 30, 44, 255});

    /* Scrolling diagonal hash marks below the floor line - gives the
     * "treadmill is moving" illusion even when the mech is in IDLE.
     * Stride 28 px so it's clearly noticeable but not visually busy. */
    int   stride = 28;
    float scroll = -fmodf(cycle_time * 140.0f, (float)stride);
    int   hash_w = 14;
    Color hash_c = (Color){42, 56, 80, 255};
    for (int x = (int)scroll - stride; x < rt_w + stride; x += stride) {
        DrawTriangle(
            (Vector2){ (float)x,             (float)(floor_y + 8)  },
            (Vector2){ (float)(x + hash_w),  (float)(floor_y + 8)  },
            (Vector2){ (float)(x + hash_w/2),(float)(floor_y + 22) },
            hash_c);
    }

    /* Phase label badge - top-right of the RT. */
    if (phase_label && u) {
        int pad = 8;
        int tw  = ui_measure(u, phase_label, 18);
        int th  = (int)((float)18 * u->scale + 0.5f);
        Rectangle pr = (Rectangle){
            (float)(rt_w - tw - pad * 4), (float)pad,
            (float)(tw + pad * 2), (float)(th + pad)
        };
        DrawRectangleRec(pr, (Color){ 30, 50, 80, 200 });
        DrawRectangleLinesEx(pr, 1.5f, (Color){120, 180, 240, 255});
        ui_draw_text(u, phase_label,
                     (int)(pr.x + pad), (int)(pr.y + pad / 2),
                     18, (Color){220, 240, 255, 255});
    }
}

/* ---- Spider chart -------------------------------------------------- */

static void draw_spider_chart(Rectangle r, const UIContext *u,
                              int chassis_id, int primary_id,
                              int secondary_id, int armor_id, int jet_id)
{
    /* Center + radius chosen with margin for labels around the perimeter. */
    Vec2 center = (Vec2){ r.x + r.width * 0.5f,
                          r.y + r.height * 0.5f + r.height * 0.04f };
    float radius = (r.width < r.height ? r.width : r.height) * 0.38f;

    /* Background panel. */
    DrawRectangleRec(r, (Color){18, 24, 38, 200});
    DrawRectangleLinesEx(r, 1.0f, (Color){60, 90, 120, 255});

    /* Axis angles - start straight up, distribute clockwise. */
    Vec2 axis_dir[AXIS_COUNT];
    for (int i = 0; i < AXIS_COUNT; ++i) {
        float angle = -PI * 0.5f + (float)i * (2.0f * PI / (float)AXIS_COUNT);
        axis_dir[i] = (Vec2){ cosf(angle), sinf(angle) };
    }

    /* Concentric gridlines at 25 / 50 / 75 / 100 percent. */
    Color grid_dim = (Color){45, 60, 85, 255};
    Color grid_mid = (Color){70, 90, 120, 255};
    for (int g = 1; g <= 4; ++g) {
        float t = (float)g / 4.0f;
        for (int i = 0; i < AXIS_COUNT; ++i) {
            int j = (i + 1) % AXIS_COUNT;
            Vec2 a = (Vec2){ center.x + axis_dir[i].x * radius * t,
                             center.y + axis_dir[i].y * radius * t };
            Vec2 b = (Vec2){ center.x + axis_dir[j].x * radius * t,
                             center.y + axis_dir[j].y * radius * t };
            DrawLineEx((Vector2){a.x, a.y}, (Vector2){b.x, b.y},
                       (g == 4 ? 1.5f : 1.0f),
                       (g == 4 ? grid_mid : grid_dim));
        }
    }

    /* Axis spokes. */
    for (int i = 0; i < AXIS_COUNT; ++i) {
        Vec2 tip = (Vec2){ center.x + axis_dir[i].x * radius,
                           center.y + axis_dir[i].y * radius };
        DrawLineEx((Vector2){center.x, center.y},
                   (Vector2){tip.x, tip.y}, 1.0f, grid_dim);
    }

    /* Loadout polygon - filled (semi-transparent fan from center) +
     * outlined. Fan triangles around the center; raylib's DrawTriangle
     * has the CCW-winding requirement, but for a convex fan from the
     * center this is fine. */
    Vec2 poly[AXIS_COUNT];
    for (int i = 0; i < AXIS_COUNT; ++i) {
        float v = norm_axis(i, chassis_id, primary_id,
                            secondary_id, armor_id, jet_id);
        poly[i] = (Vec2){ center.x + axis_dir[i].x * radius * v,
                          center.y + axis_dir[i].y * radius * v };
    }
    Color fill = (Color){ 100, 200, 240, 90 };
    Color edge = (Color){ 140, 220, 255, 255 };
    for (int i = 0; i < AXIS_COUNT; ++i) {
        int j = (i + 1) % AXIS_COUNT;
        /* DrawTriangle: vertex winding must be CCW for fill - center,
         * poly[j], poly[i] gives CCW in screen-space (+y down). */
        DrawTriangle(
            (Vector2){center.x, center.y},
            (Vector2){poly[j].x, poly[j].y},
            (Vector2){poly[i].x, poly[i].y},
            fill);
    }
    /* Outline. */
    for (int i = 0; i < AXIS_COUNT; ++i) {
        int j = (i + 1) % AXIS_COUNT;
        DrawLineEx((Vector2){poly[i].x, poly[i].y},
                   (Vector2){poly[j].x, poly[j].y}, 2.0f, edge);
    }
    /* Vertex dots. */
    for (int i = 0; i < AXIS_COUNT; ++i) {
        DrawCircleV((Vector2){poly[i].x, poly[i].y}, 3.0f, edge);
    }

    /* Axis labels at the perimeter. */
    if (u) {
        for (int i = 0; i < AXIS_COUNT; ++i) {
            const char *L = g_axis_labels[i];
            int font_px = 16;
            int tw = ui_measure(u, L, font_px);
            int th = (int)((float)font_px * u->scale + 0.5f);
            float lx = center.x + axis_dir[i].x * (radius + 18.0f);
            float ly = center.y + axis_dir[i].y * (radius + 18.0f);
            /* Anchor each label so its visual center sits at (lx, ly).
             * For axes near the right edge the label can run off - clamp. */
            int x = (int)(lx - (float)tw * 0.5f);
            int y = (int)(ly - (float)th * 0.5f);
            ui_draw_text(u, L, x, y, font_px,
                         (Color){200, 220, 240, 255});
        }
    }

    /* Chart title omitted on purpose - the six axis labels around the
     * perimeter already say what each spoke means, and a centred title
     * at the top of the panel collided with the SPEED axis label at
     * 4K scale (where the radius grows). The modal's own header strip
     * gives the surrounding context. */
}

/* ---- Description pane ---------------------------------------------- */

static void draw_description(Rectangle r, const UIContext *u,
                             int chassis_id, int primary_id,
                             int secondary_id, int armor_id, int jet_id)
{
    if (!u) return;
    DrawRectangleRec(r, (Color){18, 22, 32, 200});
    DrawRectangleLinesEx(r, 1.0f, (Color){60, 80, 110, 255});

    const Chassis *ch = mech_chassis(chassis_id);
    const Weapon  *pw = weapon_def(primary_id);
    const Weapon  *sw = weapon_def(secondary_id);
    const Armor   *ar = armor_def(armor_id);
    const Jetpack *jt = jetpack_def(jet_id);

    int x = (int)(r.x + 18);
    int y = (int)(r.y + 14);

    /* Title line: chassis name. */
    char title[96];
    snprintf(title, sizeof title, "%s",
             ch ? ch->name : "?");
    ui_draw_text(u, title, x, y, 28, (Color){240, 250, 255, 255});
    y += (int)(28.0f * u->scale + 6.0f);

    /* Subtitle: role tagline. */
    ui_draw_text(u, desc_or(g_chassis_role[chassis_id], " - "),
                 x, y, 16, (Color){170, 200, 230, 255});
    y += (int)(16.0f * u->scale + 14.0f);

    /* Each bullet - name + colon + blurb. */
    char buf[160];

    snprintf(buf, sizeof buf, "PRIMARY  %s",
             pw ? pw->name : "?");
    ui_draw_text(u, buf, x, y, 17, (Color){220, 230, 240, 255});
    y += (int)(17.0f * u->scale + 2.0f);
    ui_draw_text(u, desc_or(g_weapon_blurb[primary_id], " - "),
                 x + 24, y, 14, (Color){160, 175, 195, 255});
    y += (int)(14.0f * u->scale + 8.0f);

    snprintf(buf, sizeof buf, "SECONDARY  %s",
             sw ? sw->name : "?");
    ui_draw_text(u, buf, x, y, 17, (Color){220, 230, 240, 255});
    y += (int)(17.0f * u->scale + 2.0f);
    ui_draw_text(u, desc_or(g_weapon_blurb[secondary_id], " - "),
                 x + 24, y, 14, (Color){160, 175, 195, 255});
    y += (int)(14.0f * u->scale + 8.0f);

    snprintf(buf, sizeof buf, "ARMOR  %s",
             ar ? ar->name : "?");
    ui_draw_text(u, buf, x, y, 17, (Color){220, 230, 240, 255});
    y += (int)(17.0f * u->scale + 2.0f);
    ui_draw_text(u, desc_or(g_armor_blurb[armor_id], " - "),
                 x + 24, y, 14, (Color){160, 175, 195, 255});
    y += (int)(14.0f * u->scale + 8.0f);

    snprintf(buf, sizeof buf, "JETPACK  %s",
             jt ? jt->name : "?");
    ui_draw_text(u, buf, x, y, 17, (Color){220, 230, 240, 255});
    y += (int)(17.0f * u->scale + 2.0f);
    ui_draw_text(u, desc_or(g_jet_blurb[jet_id], " - "),
                 x + 24, y, 14, (Color){160, 175, 195, 255});
    y += (int)(14.0f * u->scale + 12.0f);

    /* Passive + tactic. */
    ui_draw_text(u, desc_or(g_chassis_passive[chassis_id], " - "),
                 x, y, 15, (Color){200, 220, 200, 255});
    y += (int)(15.0f * u->scale + 4.0f);
    ui_draw_text(u, desc_or(g_chassis_tactic[chassis_id], " - "),
                 x, y, 15, (Color){200, 200, 230, 255});
}

/* ---- RT helpers ---------------------------------------------------- */

static void ensure_render_target(LoadoutPreview *st, int w, int h) {
    if (st->rt_ready && st->rt_w == w && st->rt_h == h) return;
    if (st->rt_ready) {
        UnloadRenderTexture(st->rt);
        st->rt_ready = false;
    }
    if (w <= 0 || h <= 0) return;
    st->rt = LoadRenderTexture(w, h);
    if (st->rt.texture.id == 0) {
        LOG_W("loadout_preview: LoadRenderTexture(%d,%d) failed", w, h);
        return;
    }
    SetTextureFilter(st->rt.texture, TEXTURE_FILTER_BILINEAR);
    st->rt_w = w;
    st->rt_h = h;
    st->rt_ready = true;
}

static void render_mech_into_rt(LoadoutPreview *st, float cycle_time,
                                const UIContext *u)
{
    if (!st->rt_ready) return;

    /* Adaptive camera zoom — pick a zoom that lets the worst-case
     * chassis-and-pose still fit inside the available treadmill area.
     * The vertical extent we need to display covers everything from a
     * Sniper's head while jet-bobbed up (~-77 world) to a Trooper's
     * feet planted on the floor in IDLE (~+35 world), so ~112 world
     * units. Aim for the mech to occupy ~62% of the RT vertically;
     * smaller RTs (720p) just get a smaller mech rather than a
     * head-clipped close-up. Offset.y anchors world +35 (IDLE feet)
     * at rt_h-70 (the treadmill floor strip), so the JET phase bobs
     * UP from there naturally. */
    const float MECH_WORLD_H  = 112.0f;
    const float FILL_FRAC     = 0.62f;
    const float FEET_WORLD_Y  = 35.0f;
    const float FLOOR_RT_Y    = 70.0f;   /* px above bottom of RT */
    float zoom = (FILL_FRAC * (float)st->rt_h) / MECH_WORLD_H;
    if (zoom < 1.0f) zoom = 1.0f;
    if (zoom > 5.5f) zoom = 5.5f;
    Camera2D cam = (Camera2D){
        .target   = (Vector2){ 0.0f, 0.0f },
        .offset   = (Vector2){ (float)st->rt_w * 0.5f,
                               (float)st->rt_h - FLOOR_RT_Y
                                   - FEET_WORLD_Y * zoom },
        .rotation = 0.0f,
        .zoom     = zoom,
    };

    BeginTextureMode(st->rt);
        draw_treadmill_bg(st->rt_w, st->rt_h, cycle_time,
                          cycle_phase_label(fmodf(cycle_time, CYCLE_PERIOD)),
                          u);
        BeginMode2D(cam);
            /* Exhaust draws BEFORE the body so the mech sits on top of
             * the brightest flame core - looks like the flames erupt
             * from beneath the feet rather than around them. */
            draw_jet_exhaust(st, cycle_time);
            Vec2 aim_dir = Vector2Normalize(st->mech.aim_world);
            render_draw_mech_preview(&st->mech, &st->particles,
                                     &st->constraints, aim_dir);
        EndMode2D();
    EndTextureMode();
}

/* ---- Modal layout + driver ----------------------------------------- */

bool loadout_preview_update_and_draw(LoadoutPreview *st,
                                     struct UIContext *u,
                                     int sw, int sh, float dt,
                                     bool can_edit,
                                     int *chassis_id,
                                     int *primary_id,
                                     int *secondary_id,
                                     int *armor_id,
                                     int *jetpack_id)
{
    if (!st || !u || !chassis_id || !primary_id || !secondary_id
        || !armor_id || !jetpack_id) return false;
    if (!st->open) return false;

    /* Resize the modal proportionally to the window size. Aim for
     * roughly 78% × 82% with a minimum-readable baseline at 720p. */
    int margin_x = (int)(sw * 0.10f);
    int margin_y = (int)(sh * 0.07f);
    if (margin_x < 24) margin_x = 24;
    if (margin_y < 24) margin_y = 24;
    Rectangle modal = (Rectangle){
        (float)margin_x, (float)margin_y,
        (float)(sw - margin_x * 2),
        (float)(sh - margin_y * 2),
    };

    /* Full-screen scrim. Clicking on the scrim (outside the panel)
     * closes the modal - same behaviour as a fly-out menu. */
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 200});

    /* Detect which loadout slot changed since last frame. If exactly
     * one slot changed, jump the animation cycle to the phase that
     * showcases that slot — so cycling the secondary lands the player
     * on PHASE_IDLE_SECONDARY immediately, and cycling the jetpack
     * lands them mid-PHASE_JET (where the new plume colour is
     * visible). Any other change (chassis / primary / armor) resets
     * to the start of the cycle. */
    bool diff_chassis   = st->mech.chassis_id   != *chassis_id;
    bool diff_primary   = st->mech.primary_id   != *primary_id;
    bool diff_secondary = st->mech.secondary_id != *secondary_id;
    bool diff_armor     = st->mech.armor_id     != *armor_id;
    bool diff_jet       = st->mech.jetpack_id   != *jetpack_id;
    if (diff_chassis || diff_primary || diff_secondary ||
        diff_armor   || diff_jet)
    {
        mech_set_loadout(&st->mech, *chassis_id, *primary_id,
                         *secondary_id, *armor_id, *jetpack_id);
        st->gait_phase = 0.0f;
        int n_diff = (int)diff_chassis + (int)diff_primary +
                     (int)diff_secondary + (int)diff_armor +
                     (int)diff_jet;
        if (n_diff == 1 && diff_secondary) {
            st->cycle_time = phase_entry(PHASE_IDLE_SECONDARY)->t_start
                           + 0.10f;
        } else if (n_diff == 1 && diff_jet) {
            /* Land 0.5 s into PHASE_JET so the ease-in is past and the
             * full plume colour is on display. */
            st->cycle_time = phase_entry(PHASE_JET)->t_start + 0.50f;
        } else {
            st->cycle_time = 0.0f;
        }
    }

    /* Advance the animation cycle each frame so the mech is "alive"
     * even while the player is reading the description. */
    advance_pose(st, dt);

    /* Panel background. */
    DrawRectangleRec(modal, (Color){14, 18, 28, 245});
    DrawRectangleLinesEx(modal, u->scale * 2.0f,
                         (Color){120, 180, 240, 255});

    /* ---- Header bar ---- */
    int header_h = (int)(56.0f * u->scale);
    Rectangle header = (Rectangle){
        modal.x, modal.y, modal.width, (float)header_h
    };
    DrawRectangleRec(header, (Color){22, 30, 46, 255});
    DrawLineEx((Vector2){header.x, header.y + header.height},
               (Vector2){header.x + header.width, header.y + header.height},
               1.5f, (Color){80, 120, 170, 255});

    const char *title = "LOADOUT PREVIEW";
    int title_px = 28;
    int title_w  = ui_measure(u, title, title_px);
    int title_h  = (int)((float)title_px * u->scale + 0.5f);
    ui_draw_text(u, title,
                 (int)(header.x + 24),
                 (int)(header.y + (header.height - (float)title_h) * 0.5f),
                 title_px, (Color){220, 240, 255, 255});
    (void)title_w;

    /* Close [X] button at top-right. */
    int close_sz = (int)(40.0f * u->scale);
    Rectangle close_btn = (Rectangle){
        header.x + header.width - (float)close_sz - 14.0f,
        header.y + (header.height - (float)close_sz) * 0.5f,
        (float)close_sz, (float)close_sz,
    };
    bool close_hover = ui_point_in_rect(u->mouse, close_btn);
    DrawRectangleRec(close_btn,
                     close_hover ? (Color){180, 60, 60, 255}
                                 : (Color){50, 60, 80, 255});
    DrawRectangleLinesEx(close_btn, 1.5f,
                         close_hover ? (Color){240, 180, 180, 255}
                                     : (Color){120, 140, 170, 255});
    {
        const char *x = "X";
        int xw = ui_measure(u, x, 22);
        int xh = (int)(22.0f * u->scale + 0.5f);
        ui_draw_text(u, x,
                     (int)(close_btn.x + (close_btn.width - (float)xw) * 0.5f),
                     (int)(close_btn.y + (close_btn.height - (float)xh) * 0.5f),
                     22, (Color){240, 240, 240, 255});
    }

    /* ---- Body: 2-column layout ---- */
    int body_y = (int)(modal.y + (float)header_h + 12.0f);
    int body_h = (int)(modal.height - (float)header_h - 60.0f - 12.0f);
    int gap    = (int)(18.0f * u->scale);
    int left_w = (int)((modal.width - 48.0f - (float)gap) * 0.50f);
    int right_w= (int)(modal.width - 48.0f - (float)gap - (float)left_w);

    Rectangle left_panel = (Rectangle){
        modal.x + 24.0f, (float)body_y,
        (float)left_w, (float)body_h
    };
    Rectangle right_panel = (Rectangle){
        left_panel.x + left_panel.width + (float)gap,
        (float)body_y,
        (float)right_w, (float)body_h
    };

    /* Left column = treadmill (top ~55%) + 5 stacked full-width cycle
     * buttons (bottom ~45%). Stacking lets every label render at
     * comfortable size - the previous single-row layout truncated
     * "Grappling Hook" etc. at 1080p+ because each slot was only ~120px. */
    int cycle_rows  = 5;
    int cycle_gap_y = (int)(8.0f * u->scale);
    int cycle_h     = (int)(36.0f * u->scale);
    int cycles_h    = cycle_rows * cycle_h + (cycle_rows - 1) * cycle_gap_y;
    int tread_h     = (int)left_panel.height - cycles_h - (int)(18.0f * u->scale);
    if (tread_h < (int)(left_panel.height * 0.40f)) {
        tread_h = (int)(left_panel.height * 0.40f);
    }

    int rt_w_target = (int)left_panel.width;
    int rt_h_target = tread_h;
    /* Cap RT internal size so we don't allocate a 4K texture for the
     * preview on hi-DPI windows. The bilinear-upscale on the blit keeps
     * the visuals smooth. */
    if (rt_w_target > 1200) rt_w_target = 1200;
    if (rt_h_target > 900)  rt_h_target = 900;
    ensure_render_target(st, rt_w_target, rt_h_target);
    render_mech_into_rt(st, st->cycle_time, u);

    /* Blit RT - flip Y because raylib RTs are bottom-up. */
    Rectangle src = (Rectangle){ 0.0f, 0.0f,
                                 (float)st->rt_w, -(float)st->rt_h };
    Rectangle dst = (Rectangle){ left_panel.x, left_panel.y,
                                 left_panel.width, (float)tread_h };
    if (st->rt_ready) {
        DrawTexturePro(st->rt.texture, src, dst, (Vector2){0,0}, 0.0f,
                       (Color){255, 255, 255, 255});
        DrawRectangleLinesEx(dst, 1.5f, (Color){80, 120, 170, 255});
    } else {
        DrawRectangleRec(dst, (Color){8, 12, 22, 255});
        const char *msg = "preview unavailable";
        ui_draw_text(u, msg, (int)(dst.x + 16),
                     (int)(dst.y + dst.height * 0.5f),
                     18, (Color){200, 200, 200, 255});
    }

    /* Cycle controls - five full-width rows under the treadmill, each
     * labelled "Category: Value" so the player can tell what they're
     * cycling at a glance. */
    int ctrl_y = (int)(dst.y + dst.height + 18.0f * u->scale);
    bool any_change = false;
    int step;

    char btn_label[80];

    snprintf(btn_label, sizeof btn_label, "Chassis: %s",
             mech_chassis(*chassis_id) ? mech_chassis(*chassis_id)->name : "?");
    step = ui_cycle_button(u,
        (Rectangle){ left_panel.x, (float)ctrl_y,
                     left_panel.width, (float)cycle_h },
        btn_label, can_edit);
    if (step != 0) {
        *chassis_id = step_in_cycle(*chassis_id, g_chassis_choices,
                                    CHOICES_N(g_chassis_choices), step);
        any_change = true;
    }
    ctrl_y += cycle_h + cycle_gap_y;

    snprintf(btn_label, sizeof btn_label, "Primary: %s",
             weapon_def(*primary_id) ? weapon_def(*primary_id)->name : "?");
    step = ui_cycle_button(u,
        (Rectangle){ left_panel.x, (float)ctrl_y,
                     left_panel.width, (float)cycle_h },
        btn_label, can_edit);
    if (step != 0) {
        *primary_id = step_in_cycle(*primary_id, g_primary_choices,
                                    CHOICES_N(g_primary_choices), step);
        any_change = true;
    }
    ctrl_y += cycle_h + cycle_gap_y;

    snprintf(btn_label, sizeof btn_label, "Secondary: %s",
             weapon_def(*secondary_id) ? weapon_def(*secondary_id)->name : "?");
    step = ui_cycle_button(u,
        (Rectangle){ left_panel.x, (float)ctrl_y,
                     left_panel.width, (float)cycle_h },
        btn_label, can_edit);
    if (step != 0) {
        *secondary_id = step_in_cycle(*secondary_id, g_secondary_choices,
                                      CHOICES_N(g_secondary_choices), step);
        any_change = true;
    }
    ctrl_y += cycle_h + cycle_gap_y;

    snprintf(btn_label, sizeof btn_label, "Armor: %s",
             armor_def(*armor_id) ? armor_def(*armor_id)->name : "?");
    step = ui_cycle_button(u,
        (Rectangle){ left_panel.x, (float)ctrl_y,
                     left_panel.width, (float)cycle_h },
        btn_label, can_edit);
    if (step != 0) {
        *armor_id = step_in_cycle(*armor_id, g_armor_choices,
                                  CHOICES_N(g_armor_choices), step);
        any_change = true;
    }
    ctrl_y += cycle_h + cycle_gap_y;

    snprintf(btn_label, sizeof btn_label, "Jetpack: %s",
             jetpack_def(*jetpack_id) ? jetpack_def(*jetpack_id)->name : "?");
    step = ui_cycle_button(u,
        (Rectangle){ left_panel.x, (float)ctrl_y,
                     left_panel.width, (float)cycle_h },
        btn_label, can_edit);
    if (step != 0) {
        *jetpack_id = step_in_cycle(*jetpack_id, g_jet_choices,
                                    CHOICES_N(g_jet_choices), step);
        any_change = true;
    }

    /* Right column: chart (top 45%) + description (rest). */
    float chart_h = right_panel.height * 0.46f;
    Rectangle chart_r = (Rectangle){
        right_panel.x, right_panel.y,
        right_panel.width, chart_h
    };
    Rectangle desc_r = (Rectangle){
        right_panel.x, right_panel.y + chart_h + 12.0f,
        right_panel.width, right_panel.height - chart_h - 12.0f
    };
    draw_spider_chart(chart_r, u, *chassis_id, *primary_id,
                      *secondary_id, *armor_id, *jetpack_id);
    draw_description(desc_r, u, *chassis_id, *primary_id,
                     *secondary_id, *armor_id, *jetpack_id);

    /* Footer hint. */
    {
        const char *hint = "ESC or [X] to close   |   LMB next, RMB previous";
        int tw = ui_measure(u, hint, 14);
        int th = (int)(14.0f * u->scale + 0.5f);
        ui_draw_text(u, hint,
                     (int)(modal.x + (modal.width - (float)tw) * 0.5f),
                     (int)(modal.y + modal.height - (float)th - 14.0f),
                     14, (Color){140, 160, 190, 255});
    }

    /* ---- Input: close paths ----
     * 1) ESC key
     * 2) Click on the X button
     * 3) Click outside the modal panel (on the scrim) */
    if (IsKeyPressed(KEY_ESCAPE)) {
        st->open = false;
    } else if (close_hover && u->mouse_pressed) {
        st->open = false;
    } else if (u->mouse_pressed && !ui_point_in_rect(u->mouse, modal)) {
        st->open = false;
    }

    return any_change;
}
