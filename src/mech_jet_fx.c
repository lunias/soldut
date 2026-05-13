#include "mech_jet_fx.h"

#include "audio.h"
#include "decal.h"
#include "hash.h"
#include "level.h"
#include "log.h"
#include "particle.h"

#include "../third_party/raylib/src/raylib.h"

#include <math.h>
#include <string.h>

/* ---- Plume / dust sprite atlases ---------------------------------- */

/* Both atlases stay zero-initialized until the first draw attempt
 * tries to load them; missing files leave the id at 0 and the draw
 * path falls back to a DrawLineEx-and-disc render. This mirrors the
 * weapon_sprites lazy-load pattern. */
static Texture2D s_plume_atlas = {0};
static Texture2D s_dust_atlas  = {0};
static bool      s_plume_load_attempted = false;
static bool      s_dust_load_attempted  = false;

static void try_load_plume_atlas(void) {
    if (s_plume_load_attempted) return;
    s_plume_load_attempted = true;
    const char *path = "assets/sprites/jet_plume.png";
    if (!FileExists(path)) {
        LOG_I("jet_fx: %s not found — using line/disc fallback", path);
        return;
    }
    Texture2D t = LoadTexture(path);
    if (t.id == 0) {
        LOG_W("jet_fx: failed to load %s — using fallback", path);
        return;
    }
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    s_plume_atlas = t;
    LOG_I("jet_fx: loaded plume atlas %s (%dx%d)", path, t.width, t.height);
}

static void try_load_dust_atlas(void) {
    if (s_dust_load_attempted) return;
    s_dust_load_attempted = true;
    const char *path = "assets/sprites/jet_dust.png";
    if (!FileExists(path)) {
        /* Dust uses a circle fallback in fx_draw already, so missing
         * here is silent + cheap. */
        return;
    }
    Texture2D t = LoadTexture(path);
    if (t.id == 0) return;
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    s_dust_atlas = t;
    LOG_I("jet_fx: loaded dust atlas %s (%dx%d)", path, t.width, t.height);
}

/*
 * M6 P02 — Jetpack propulsion FX driver.
 *
 * The tables `g_jet_fx` and `g_chassis_nozzles` below are the
 * authoritative tuning surface. Numbers are first-pass per the spec;
 * iterate after seeing the visual on screen.
 *
 * Phase 2 ships the skeleton + the tables. Phases 3+ wire up the
 * particle spawn / ground-impingement / plume-draw / shimmer / scorch
 * paths. Until then `mech_jet_fx_step` is a no-op and the renderer
 * helpers (`mech_jet_fx_draw_plumes`, `_any_active`,
 * `_collect_hot_zones`) return safely empty.
 */

/* ---- Per-jetpack tuning table ------------------------------------- */

const JetFxDef g_jet_fx[JET_COUNT] = {
    [JET_NONE] = {0},

    [JET_STANDARD] = {
        .plume_color_rim       = 0xDCF0FFC8u,  /* 220,240,255,200 — clean ion-thruster rim */
        .plume_color_core      = 0x50B4FFFFu,  /*  80,180,255,255 — saturated blue core */
        .particle_color_hot    = 0xC8E0FFFFu,  /* 200,224,255,255 */
        .particle_color_cool   = 0x3C5AB400u,  /*  60, 90,180,  0 — alpha 0 at life end */
        .nozzle_count                = 1,
        .sustain_particles_per_tick  = 2,
        .boost_particles_per_tick    = 0,      /* no boost for standard */
        .ignition_particles          = 6,
        .sustain_tick_divisor        = 1,
        /* Length tuned so the plume's tail extends below the body
         * (Trooper foot is ~36 px below pelvis; nozzle is 10 px above
         * pelvis; 56 px length puts the tail at pelvis+46 — past the
         * knees, slightly above feet). Visible-feet readability over
         * a "fully below body" extension; the dense particle stream
         * carries the exhaust character further. */
        .plume_length_px       = 56.0f,
        .plume_width_px        = 14.0f,
        .particle_speed_pxs    = 240.0f,
        .particle_life_min     = 0.25f,
        .particle_life_max     = 0.45f,
        .particle_size_min     = 2.0f,
        .particle_size_max     = 4.0f,
        .has_continuous_plume  = true,
    },

    [JET_BURST] = {
        .plume_color_rim       = 0xFFDC78DCu,  /* 255,220,120,220 — bright yellow rim */
        .plume_color_core      = 0xFF7828FFu,  /* 255,120, 40,255 — orange-red core */
        .particle_color_hot    = 0xFFC850FFu,  /* 255,200, 80,255 */
        .particle_color_cool   = 0xC8281400u,  /* 200, 40, 20,  0 */
        .nozzle_count                = 2,
        .sustain_particles_per_tick  = 1,
        .boost_particles_per_tick    = 8,      /* 8× spike on MECH_JET_BOOSTING */
        .ignition_particles          = 12,
        .sustain_tick_divisor        = 1,
        /* Burst plume sits a notch longer than Standard so the visual
         * weight matches the louder cue; boost-time scale of 1.6×
         * lifts it to ~106 px which dominates the screen. */
        .plume_length_px       = 64.0f,
        .plume_width_px        = 16.0f,
        .particle_speed_pxs    = 320.0f,
        .particle_life_min     = 0.30f,
        .particle_life_max     = 0.55f,
        .particle_size_min     = 2.5f,
        .particle_size_max     = 5.0f,
        .has_continuous_plume  = true,
    },

    [JET_GLIDE_WING] = {
        .plume_color_rim       = 0xC8F0FFA0u,  /* 200,240,255,160 — pale "draft" rim */
        .plume_color_core      = 0xA0DCFFDCu,  /* 160,220,255,220 — cool cyan core */
        .particle_color_hot    = 0xB4F0FFFFu,
        .particle_color_cool   = 0x5078C800u,
        .nozzle_count                = 2,
        .sustain_particles_per_tick  = 1,
        .boost_particles_per_tick    = 0,
        .ignition_particles          = 4,
        .sustain_tick_divisor        = 4,      /* intermittent — every 4th tick */
        /* Glide-wing wisps are intentionally shorter than thrust
         * plumes (lift comes from the wing, not raw thrust). */
        .plume_length_px       = 36.0f,
        .plume_width_px        = 11.0f,
        .particle_speed_pxs    = 160.0f,
        .particle_life_min     = 0.50f,
        .particle_life_max     = 0.90f,
        .particle_size_min     = 1.5f,
        .particle_size_max     = 3.0f,
        .has_continuous_plume  = true,
    },

    [JET_JUMP_JET] = {
        .plume_color_rim       = 0xDCFFCBFFu,  /* 220,255,200,255 — green rim */
        .plume_color_core      = 0x78FFB4FFu,  /* 120,255,180,255 — bright green-cyan core */
        .particle_color_hot    = 0x8CFFC8FFu,
        .particle_color_cool   = 0x3CB47800u,
        .nozzle_count                = 1,
        .sustain_particles_per_tick  = 0,      /* discrete-event jet — no sustain */
        .boost_particles_per_tick    = 0,
        .ignition_particles          = 16,
        .sustain_tick_divisor        = 1,
        .plume_length_px       =  0.0f,        /* no continuous plume */
        .plume_width_px        =  0.0f,
        .particle_speed_pxs    = 360.0f,
        .particle_life_min     = 0.35f,
        .particle_life_max     = 0.60f,
        .particle_size_min     = 3.0f,
        .particle_size_max     = 5.0f,
        .has_continuous_plume  = false,
    },
};

/* ---- Chassis nozzle offsets -------------------------------------- *
 *
 * Source coords: pelvis-relative px, +X behind the mech facing right
 * (negated when facing_left). Y is screen-space (down = +). For 2-nozzle
 * jetpacks the spawn function mirrors slot[0]'s offset around body-local
 * x to produce a left/right pair — so the chassis only declares ONE
 * offset and the jetpack table drives the nozzle count.
 *
 * Values picked to read cleanly against each chassis silhouette per
 * spec §3. Tune in playtest. */
const ChassisNozzleSet g_chassis_nozzles[CHASSIS_COUNT] = {
    /* Trooper — single chest-back, exhaust straight down. */
    [CHASSIS_TROOPER] = {
        .slot = {
            {{-4.0f, -10.0f}, {0.0f, 1.0f}, true, {0,0,0}},
            {{ 0.0f,   0.0f}, {0.0f, 0.0f}, false, {0,0,0}},
        },
    },
    /* Scout — lighter shoulder-mount; small outward angle. */
    [CHASSIS_SCOUT] = {
        .slot = {
            {{-3.0f, -14.0f}, {0.10f, 0.995f}, true, {0,0,0}},
            {{ 0.0f,   0.0f}, {0.0f,  0.0f},  false, {0,0,0}},
        },
    },
    /* Heavy — pelvis-low, big single nozzle. Matches the 03-physics
     * doc's "Heavy gets pelvis-mounted thrust" wording even though the
     * impulse code is uniform across particles. */
    [CHASSIS_HEAVY] = {
        .slot = {
            {{-2.0f,  -4.0f}, {0.0f, 1.0f}, true, {0,0,0}},
            {{ 0.0f,   0.0f}, {0.0f, 0.0f}, false, {0,0,0}},
        },
    },
    /* Sniper — hip-mounted, thin profile. */
    [CHASSIS_SNIPER] = {
        .slot = {
            {{-3.0f,  -8.0f}, {0.0f, 1.0f}, true, {0,0,0}},
            {{ 0.0f,   0.0f}, {0.0f, 0.0f}, false, {0,0,0}},
        },
    },
    /* Engineer — back-mounted, like a tool case. */
    [CHASSIS_ENGINEER] = {
        .slot = {
            {{-4.0f, -12.0f}, {0.0f, 1.0f}, true, {0,0,0}},
            {{ 0.0f,   0.0f}, {0.0f, 0.0f}, false, {0,0,0}},
        },
    },
};

/* ---- Driver ------------------------------------------------------- */

/* Approximate downward step query — walk a few pixels from the nozzle
 * straight down and find the first SOLID tile. Returns the surface
 * flag bits via *out_flags so the caller can pick the right dust color
 * + ignition SFX variant. Slope polygons aren't covered today; tile
 * coverage handles the bulk of authored map area. */
#define JET_IMPINGE_MAX_DIST   48.0f
#define JET_IMPINGE_STEP_PX     4.0f

static bool jet_fx_ground_query(World *w, Vec2 from,
                                Vec2 *out_hit, uint16_t *out_flags)
{
    Level *L = &w->level;
    if (L->tile_size <= 0) return false;
    float ts = (float)L->tile_size;
    for (float dy = JET_IMPINGE_STEP_PX; dy <= JET_IMPINGE_MAX_DIST; dy += JET_IMPINGE_STEP_PX) {
        Vec2 p = (Vec2){ from.x, from.y + dy };
        int tx = (int)(p.x / ts);
        int ty = (int)(p.y / ts);
        uint16_t flags = level_flags_at(L, tx, ty);
        if (flags & TILE_F_SOLID) {
            /* Snap y to the top of the tile we hit. */
            float ty_top = (float)ty * ts;
            *out_hit   = (Vec2){ from.x, ty_top };
            *out_flags = flags;
            return true;
        }
    }
    return false;
}

/* PI in float — math.h M_PI is double + GNU-extension; spell out the
 * literal to avoid platform variance under -Wpedantic. */
#define JET_FX_PI_F  3.14159265358979323846f

static void spawn_exhaust_particle(World *w, const JetFxDef *def,
                                   Vec2 origin, Vec2 thrust_dir,
                                   float spread_rad, float speed_mul)
{
    pcg32_t *rng = w->rng;
    /* The thrust_dir is the unit vector along the desired exhaust
     * direction (typically down). Cone-jitter the angle so the stream
     * fans visibly. */
    float ang0  = atan2f(thrust_dir.y, thrust_dir.x);
    float jit   = (pcg32_float01(rng) * 2.0f - 1.0f) * spread_rad;
    float ang   = ang0 + jit;
    float speed = def->particle_speed_pxs * speed_mul *
                  (0.85f + pcg32_float01(rng) * 0.30f);
    Vec2 vel = (Vec2){ cosf(ang) * speed, sinf(ang) * speed };
    float life = def->particle_life_min +
                 pcg32_float01(rng) *
                 (def->particle_life_max - def->particle_life_min);
    float size = def->particle_size_min +
                 pcg32_float01(rng) *
                 (def->particle_size_max - def->particle_size_min);
    fx_spawn_jet_exhaust(&w->fx, origin, vel, life, size,
                         def->particle_color_hot, def->particle_color_cool);
}

static void emit_ground_dust(World *w, Vec2 hit, uint16_t surf_flags,
                             bool is_boosting)
{
    pcg32_t *rng = w->rng;
    int n = is_boosting ? 8 : 3;
    /* Steam on ice; warm dust otherwise. Per spec §5: pale-blue cyan
     * on TILE_F_ICE, warm grey-brown elsewhere. */
    uint32_t color = (surf_flags & TILE_F_ICE) ? 0xE0F0FFC0u   /* pale steam */
                                               : 0xA08060C0u; /* warm dust */
    for (int k = 0; k < n; ++k) {
        /* Fan outward horizontally with a small upward component.
         * Angles in (-PI/2 - 0.45*PI .. -PI/2 + 0.45*PI) = roughly
         * (-PI .. 0) — i.e. above the horizontal, fanning into the
         * upper half-plane. */
        float a = -JET_FX_PI_F * 0.5f +
                  (pcg32_float01(rng) * 2.0f - 1.0f) * (JET_FX_PI_F * 0.45f);
        float speed = 90.0f + pcg32_float01(rng) * 80.0f;
        Vec2 vel = (Vec2){ cosf(a) * speed, sinf(a) * speed };
        float life = 0.6f + pcg32_float01(rng) * 0.4f;
        float size = 3.0f + pcg32_float01(rng) * 3.0f;
        fx_spawn_ground_dust(&w->fx, hit, vel, life, size, color);
    }
}

void mech_jet_fx_step(World *w, int mech_id, float dt) {
    (void)dt;   /* spawn rates are absolute per-tick; sim is fixed 60 Hz */
    if (!w || mech_id < 0 || mech_id >= w->mech_count) return;
    Mech *m = &w->mechs[mech_id];
    if (!m->alive) return;
    if (m->jetpack_id <= JET_NONE || m->jetpack_id >= JET_COUNT) return;

    const JetFxDef *def = &g_jet_fx[m->jetpack_id];
    bool just_ignited = (m->jet_state_bits & MECH_JET_IGNITION_TICK) != 0;
    bool is_active    = (m->jet_state_bits & MECH_JET_ACTIVE) != 0;
    bool is_boosting  = (m->jet_state_bits & MECH_JET_BOOSTING) != 0;
    if (!is_active && !just_ignited) return;

    if (m->chassis_id < 0 || m->chassis_id >= CHASSIS_COUNT) return;
    const ChassisNozzleSet *nset = &g_chassis_nozzles[m->chassis_id];

    int b = m->particle_base;
    Vec2 pelv = (Vec2){
        w->particles.pos_x[b + PART_PELVIS],
        w->particles.pos_y[b + PART_PELVIS],
    };
    float face_dir = m->facing_left ? -1.0f : 1.0f;

    int n_nozzles = (def->nozzle_count == 2) ? 2 : 1;
    for (int i = 0; i < n_nozzles; ++i) {
        const ChassisNozzle *nz = &nset->slot[0];   /* base offset; mirrored for slot 1 */
        if (!nz->active) continue;
        float mirror = (n_nozzles == 2 && i == 1) ? -1.0f : 1.0f;
        Vec2 noz = (Vec2){
            pelv.x + face_dir * (nz->offset.x * mirror),
            pelv.y +            nz->offset.y,
        };
        Vec2 thrust = nz->thrust_dir;

        /* Ignition burst — once on the grounded → airborne edge. */
        if (just_ignited) {
            for (int k = 0; k < def->ignition_particles; ++k) {
                spawn_exhaust_particle(w, def, noz, thrust,
                                       /*spread_rad*/ 0.80f,
                                       /*speed_mul */ 1.40f);
            }
        }

        /* Sustain — per-tick rate (1× normal, BURST's `boost_particles
         * _per_tick` on MECH_JET_BOOSTING), gated by the divisor. */
        int sustain_n = is_boosting
            ? (int)def->boost_particles_per_tick
            : (int)def->sustain_particles_per_tick;
        uint8_t divisor = def->sustain_tick_divisor ? def->sustain_tick_divisor : 1u;
        if (is_active && sustain_n > 0 && (w->tick % divisor) == 0u) {
            for (int k = 0; k < sustain_n; ++k) {
                spawn_exhaust_particle(w, def, noz, thrust,
                                       /*spread_rad*/ 0.30f,
                                       /*speed_mul */ 1.00f);
            }
        }

        /* Ignition impingement — the tick we transition grounded →
         * airborne, the mech has barely moved upward; the floor is
         * still within JET_IMPINGE_MAX_DIST below the nozzle. Query
         * down, fire the rocket-takeoff thump SFX keyed on the
         * surface kind, scorch the ground, and emit a fat dust burst.
         * Non-grounded ignitions (mid-air re-press) miss the floor
         * test and stay silent — SFX_JET_PULSE covers those.
         *
         * Note: `just_ignited` implies !m->grounded for the current
         * tick (we set the bit exactly when was_grounded && !grounded).
         * The ground-query reaches back down to the floor we just
         * left, which is what we want. */
        if (just_ignited) {
            Vec2 ground_hit;
            uint16_t surf_flags;
            if (jet_fx_ground_query(w, noz, &ground_hit, &surf_flags)) {
                emit_ground_dust(w, ground_hit, surf_flags, /*boost*/true);
                decal_paint_scorch(ground_hit, 10.0f);
                SfxId cue = (surf_flags & TILE_F_ICE)
                          ? SFX_JET_IGNITION_ICE
                          : SFX_JET_IGNITION_CONCRETE;
                audio_play_at(cue, noz);
            }
        }

        /* Sustained grounded-jet impingement (rare — most active jet
         * ticks are airborne). Boost-while-grounded keeps painting
         * scorch so a sustained burst leaves a deeper mark. */
        if (is_active && m->grounded) {
            Vec2 ground_hit;
            uint16_t surf_flags;
            if (jet_fx_ground_query(w, noz, &ground_hit, &surf_flags)) {
                emit_ground_dust(w, ground_hit, surf_flags, is_boosting);
                if (is_boosting) {
                    decal_paint_scorch(ground_hit, 8.0f);
                }
            }
        }
    }
}

/* ---- Render helpers ---------------------------------------------- */

/* Pack two RGBA8 colors with a linear mix, returning a raylib Color
 * for direct use in DrawTexturePro / DrawLineEx. `t` is 0..1; t=0
 * yields `a`, t=1 yields `b`. */
static Color blend_rgba8(uint32_t a, uint32_t b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float ar = (float)((a >> 24) & 0xFF);
    float ag = (float)((a >> 16) & 0xFF);
    float ab = (float)((a >>  8) & 0xFF);
    float aa = (float)(a         & 0xFF);
    float br = (float)((b >> 24) & 0xFF);
    float bg = (float)((b >> 16) & 0xFF);
    float bb = (float)((b >>  8) & 0xFF);
    float ba = (float)(b         & 0xFF);
    return (Color){
        (unsigned char)(ar + (br - ar) * t),
        (unsigned char)(ag + (bg - ag) * t),
        (unsigned char)(ab + (bb - ab) * t),
        (unsigned char)(aa + (ba - aa) * t),
    };
}

void mech_jet_fx_draw_plumes(const World *w, float interp_alpha) {
    (void)interp_alpha;   /* plume is per-tick deterministic; no interp */
    if (!w) return;
    try_load_plume_atlas();

    for (int i = 0; i < w->mech_count; ++i) {
        const Mech *m = &w->mechs[i];
        if (!m->alive) continue;
        if (!(m->jet_state_bits & MECH_JET_ACTIVE)) continue;
        if (m->jetpack_id <= JET_NONE || m->jetpack_id >= JET_COUNT) continue;
        const JetFxDef *def = &g_jet_fx[m->jetpack_id];
        if (!def->has_continuous_plume) continue;
        if (m->chassis_id < 0 || m->chassis_id >= CHASSIS_COUNT) continue;
        const ChassisNozzleSet *nset = &g_chassis_nozzles[m->chassis_id];

        bool boosting = (m->jet_state_bits & MECH_JET_BOOSTING) != 0;
        /* Plume length grows under boost, shrinks slightly when fuel
         * is nearly empty (visual fuel cue). 0.6..1.0× scale band. */
        float fuel_frac = (m->fuel_max > 0.01f) ? (m->fuel / m->fuel_max) : 0.0f;
        if (fuel_frac < 0.0f) fuel_frac = 0.0f;
        if (fuel_frac > 1.0f) fuel_frac = 1.0f;
        float len_scale = (boosting ? 1.6f : 1.0f) * (0.6f + 0.4f * fuel_frac);
        float plume_len = def->plume_length_px * len_scale;
        float plume_w   = def->plume_width_px;

        int b = m->particle_base;
        Vec2 pelv = (Vec2){
            w->particles.pos_x[b + PART_PELVIS],
            w->particles.pos_y[b + PART_PELVIS],
        };
        float face_dir = m->facing_left ? -1.0f : 1.0f;
        Color tint = blend_rgba8(def->plume_color_rim, def->plume_color_core, 0.5f);

        int n_nozzles = (def->nozzle_count == 2) ? 2 : 1;
        for (int k = 0; k < n_nozzles; ++k) {
            const ChassisNozzle *nz = &nset->slot[0];
            if (!nz->active) continue;
            float mirror = (n_nozzles == 2 && k == 1) ? -1.0f : 1.0f;
            Vec2 noz = (Vec2){
                pelv.x + face_dir * (nz->offset.x * mirror),
                pelv.y +            nz->offset.y,
            };
            float angle_deg = RAD2DEG *
                              atan2f(nz->thrust_dir.y, nz->thrust_dir.x) - 90.0f;

            if (s_plume_atlas.id != 0) {
                /* Textured plume. Source = full atlas; dest at noz
                 * (sized plume_w × plume_len); origin at (plume_w/2,
                 * 0) so the top-middle of the quad pins to the
                 * nozzle and the rotation pivots there. Additive
                 * blend so overlapping plume cores brighten. */
                BeginBlendMode(BLEND_ADDITIVE);
                Rectangle src = {
                    0, 0,
                    (float)s_plume_atlas.width,
                    (float)s_plume_atlas.height,
                };
                Rectangle dst = { noz.x, noz.y, plume_w, plume_len };
                Vector2 origin = { plume_w * 0.5f, 0.0f };
                DrawTexturePro(s_plume_atlas, src, dst, origin,
                               angle_deg, tint);
                EndBlendMode();
            } else {
                /* Atlas-free fallback: bright line from nozzle along
                 * thrust_dir, sized to plume_len. Additive blend so
                 * overlapping lines brighten. */
                BeginBlendMode(BLEND_ADDITIVE);
                Vector2 a = { noz.x, noz.y };
                Vector2 t = {
                    noz.x + nz->thrust_dir.x * plume_len,
                    noz.y + nz->thrust_dir.y * plume_len,
                };
                DrawLineEx(a, t, plume_w * 0.45f, tint);
                EndBlendMode();
            }
        }
    }
}

bool mech_jet_fx_any_active(const World *w) {
    if (!w) return false;
    for (int i = 0; i < w->mech_count; ++i) {
        const Mech *m = &w->mechs[i];
        if (!m->alive) continue;
        if (m->jet_state_bits & MECH_JET_ACTIVE) return true;
    }
    return false;
}

int mech_jet_fx_collect_hot_zones(const World *w, const Camera2D *cam,
                                  JetHotZone *out, int max)
{
    if (!w || !out || max <= 0) return 0;
    int n = 0;
    /* One zone per active mech (collected at pelvis, not per nozzle)
     * so the 16-slot cap holds against MAX_LOBBY_SLOTS = 16 jetting
     * mechs. Boost / ignition widen the radius + bump intensity. See
     * spec §16 trade-off "Heat shimmer uniform loop has a 16-zone
     * hard cap." */
    for (int i = 0; i < w->mech_count && n < max; ++i) {
        const Mech *m = &w->mechs[i];
        if (!m->alive) continue;
        if (!(m->jet_state_bits & MECH_JET_ACTIVE)) continue;
        if (m->jetpack_id <= JET_NONE || m->jetpack_id >= JET_COUNT) continue;
        const JetFxDef *def = &g_jet_fx[m->jetpack_id];
        if (!def->has_continuous_plume) continue;   /* JUMP_JET: ignition only */

        bool boosting = (m->jet_state_bits & MECH_JET_BOOSTING) != 0;
        bool igniting = (m->jet_state_bits & MECH_JET_IGNITION_TICK) != 0;
        float radius_px = 40.0f;
        float intensity = 0.6f;
        if (boosting) { radius_px = 80.0f;  intensity = 1.0f; }
        if (igniting) { radius_px = 120.0f; intensity = 1.0f; }

        int b = m->particle_base;
        Vec2 pelv = (Vec2){
            w->particles.pos_x[b + PART_PELVIS],
            w->particles.pos_y[b + PART_PELVIS],
        };
        /* World → screen. raylib returns floats; the shader treats the
         * uniform as screen-pixel coords. */
        Vector2 sc = GetWorldToScreen2D((Vector2){ pelv.x, pelv.y }, *cam);
        out[n++] = (JetHotZone){
            .x         = sc.x,
            .y         = sc.y,
            .radius    = radius_px,
            .intensity = intensity,
        };
    }
    return n;
}

void mech_jet_fx_reload_atlases(const char *path) {
    /* Re-load whichever atlas the hot-reload watcher reported on. We
     * cheap-reset BOTH attempt flags so a once-failed-then-appeared
     * file is picked up on the next draw call. */
    if (!path) return;
    if (strstr(path, "jet_plume.png")) {
        if (s_plume_atlas.id != 0) {
            UnloadTexture(s_plume_atlas);
            s_plume_atlas = (Texture2D){0};
        }
        s_plume_load_attempted = false;
        try_load_plume_atlas();
    } else if (strstr(path, "jet_dust.png")) {
        if (s_dust_atlas.id != 0) {
            UnloadTexture(s_dust_atlas);
            s_dust_atlas = (Texture2D){0};
        }
        s_dust_load_attempted = false;
        try_load_dust_atlas();
    }
}
