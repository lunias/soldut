#include "particle.h"

#include "arena.h"
#include "audio.h"
#include "decal.h"
#include "hash.h"
#include "level.h"
#include "log.h"
#include "platform.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void fx_pool_init(FxPool *pool, Arena *arena, int capacity) {
    pool->capacity = capacity;
    pool->count = 0;
    pool->items = (FxParticle *)arena_alloc_aligned(
        arena, sizeof(FxParticle) * (size_t)capacity, _Alignof(FxParticle));
    if (!pool->items) {
        LOG_E("fx_pool_init: out of memory for %d particles", capacity);
        pool->capacity = 0;
        return;
    }
    memset(pool->items, 0, sizeof(FxParticle) * (size_t)capacity);
}

void fx_clear(FxPool *pool) { pool->count = 0; }

static int fx_alloc(FxPool *pool) {
    /* Compact-on-the-fly: prefer reusing dead slots before extending. */
    for (int i = 0; i < pool->count; ++i) {
        if (!pool->items[i].alive) return i;
    }
    if (pool->count < pool->capacity) {
        return pool->count++;
    }
    /* Pool full — overwrite the oldest. Not glamorous, but FX drop
     * gracefully and we hit this only in extreme spew. */
    return 0;
}

/* M6 P04 — Damage-number color/size tiers. The threshold cliffs
 * (10 / 30 / 60 / 100) intentionally mirror the M5 P12 decal cliffs in
 * src/mech.c::mech_record_damage_decal so a 75-damage Frag paints a
 * SCORCH decal AND shows an orange-red "75". Color choices stay in the
 * yellow→red blood-family per documents/00-vision.md §Aesthetic.
 *
 * RGBA8 packed as 0xRRGGBBAA. The outline alpha climbs by tier so
 * crits punch through any decal/blood/jet noise behind them; light
 * hits get a softer outline so they don't hammer the screen.
 *
 * speed_min/max + upward_bias drive the spew velocity at spawn (per
 * fx_spawn_damage_number); life_s sets the total visible duration
 * including the 0.50 s alpha-fade tail. */
typedef enum {
    DMG_TIER_LIGHT = 0,    /* 1-9 dmg — pale yellow */
    DMG_TIER_NORMAL,       /* 10-29 dmg — yellow */
    DMG_TIER_MEDIUM,       /* 30-59 dmg — orange */
    DMG_TIER_HEAVY,        /* 60-99 dmg — red-orange */
    DMG_TIER_CRIT,         /* >=100 dmg — bright red */
    DMG_TIER_COUNT
} DamageTier;

typedef struct {
    uint32_t color;            /* RGBA8 glyph color */
    uint32_t outline;          /* RGBA8 outline color (tier-rising alpha) */
    float    font_px;          /* point size for DrawTextPro */
    float    speed_min;        /* initial speed (px/s) lower bound */
    float    speed_max;        /* initial speed upper bound */
    float    upward_bias;      /* extra -Y velocity at spawn (px/s) */
    float    life_s;           /* total life including fade */
} DmgTierDef;

/* font_px values are the base size at 1× camera zoom (the spec's
 * stated frame of reference, §6). At runtime fx_draw_damage_numbers
 * scales by camera.zoom (default 1.4×) and the M6 P03 blit_scale,
 * so a 30 px MEDIUM glyph reads at ~42 px on a 1080p window and
 * ~84 px on a 4K-windowed run with the 1080-cap internal RT. The
 * spec's 10..18 range read too small on contemporary monitors —
 * bumped first to 14..32, then to 20..44 after the user reported
 * "still a bit hard to read" on 4K. */
static const DmgTierDef g_dmg_tier[DMG_TIER_COUNT] = {
    /* LIGHT  */ { 0xFFF0B4FFu, 0x00000080u, 20.0f,  60.0f,  90.0f,  50.0f, 1.4f },
    /* NORMAL */ { 0xFFDC50FFu, 0x000000A0u, 24.0f,  80.0f, 110.0f,  70.0f, 1.7f },
    /* MEDIUM */ { 0xFF8C28FFu, 0x000000C0u, 30.0f, 100.0f, 140.0f,  90.0f, 1.9f },
    /* HEAVY  */ { 0xFF5018FFu, 0x000000E0u, 36.0f, 120.0f, 170.0f, 110.0f, 2.1f },
    /* CRIT   */ { 0xFF2828FFu, 0x000000FFu, 44.0f, 140.0f, 200.0f, 130.0f, 2.4f },
};

static inline DamageTier dmg_tier_for(uint8_t dmg) {
    if (dmg >= 100) return DMG_TIER_CRIT;
    if (dmg >=  60) return DMG_TIER_HEAVY;
    if (dmg >=  30) return DMG_TIER_MEDIUM;
    if (dmg >=  10) return DMG_TIER_NORMAL;
    return DMG_TIER_LIGHT;
}

int fx_spawn_blood(FxPool *pool, Vec2 pos, Vec2 vel, pcg32_t *rng) {
    int idx = fx_alloc(pool);
    if (idx < 0) return -1;
    FxParticle *fp = &pool->items[idx];
    /* Velocity randomization: cone of variation around the impact dir. */
    float speed = 220.0f + pcg32_float01(rng) * 360.0f;
    float ang   = atan2f(vel.y, vel.x) + (pcg32_float01(rng) - 0.5f) * 1.4f;
    fp->pos     = pos;
    fp->render_prev_pos = pos;
    fp->vel     = (Vec2){ cosf(ang) * speed, sinf(ang) * speed };
    fp->life    = 0.5f + pcg32_float01(rng) * 1.0f;
    fp->life_max = fp->life;
    fp->size    = 2.0f + pcg32_float01(rng) * 1.5f;
    /* Red core fading toward orange — store as RGBA8. */
    uint8_t r = 200 + (uint8_t)(pcg32_float01(rng) * 55);
    uint8_t g = (uint8_t)(pcg32_float01(rng) * 60);
    uint8_t b = (uint8_t)(pcg32_float01(rng) * 30);
    fp->color = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFFu;
    fp->kind  = FX_BLOOD;
    fp->alive = 1;
    return idx;
}

int fx_spawn_spark(FxPool *pool, Vec2 pos, Vec2 vel, pcg32_t *rng) {
    int idx = fx_alloc(pool);
    if (idx < 0) return -1;
    FxParticle *fp = &pool->items[idx];
    float speed = 120.0f + pcg32_float01(rng) * 220.0f;
    float ang   = atan2f(vel.y, vel.x) + (pcg32_float01(rng) - 0.5f) * 2.4f;
    fp->pos      = pos;
    fp->render_prev_pos = pos;
    fp->vel      = (Vec2){ cosf(ang) * speed, sinf(ang) * speed };
    fp->life     = 0.15f + pcg32_float01(rng) * 0.25f;
    fp->life_max = fp->life;
    fp->size     = 1.5f + pcg32_float01(rng) * 1.0f;
    fp->color    = 0xFFE6A0FFu;   /* warm yellow */
    fp->kind     = FX_SPARK;
    fp->alive    = 1;
    return idx;
}

int fx_spawn_smoke(FxPool *pool, Vec2 pos, Vec2 vel, pcg32_t *rng) {
    int idx = fx_alloc(pool);
    if (idx < 0) return -1;
    FxParticle *fp = &pool->items[idx];
    /* Velocity already chosen by caller — small upward drift with
     * lateral jitter. Add a small extra random angle so successive
     * puffs from the same source spread visibly. */
    float ang = atan2f(vel.y, vel.x) + (pcg32_float01(rng) - 0.5f) * 0.6f;
    float speed = sqrtf(vel.x * vel.x + vel.y * vel.y);
    fp->pos       = pos;
    fp->render_prev_pos = pos;
    fp->vel       = (Vec2){ cosf(ang) * speed, sinf(ang) * speed };
    fp->life      = 0.9f + pcg32_float01(rng) * 0.6f;
    fp->life_max  = fp->life;
    fp->size      = 3.0f + pcg32_float01(rng) * 2.0f;
    /* fx_draw's FX_SMOKE branch hardcodes the render color to dark grey;
     * we just need the LSB (alpha-byte) to be 0xFF so the life-ratio
     * fade reaches full visibility at spawn. */
    fp->color     = 0x000000FFu;
    fp->kind      = FX_SMOKE;
    fp->alive     = 1;
    fp->pin_mech_id = -1;
    fp->pin_limb    = 0;
    fp->pin_pad     = 0;
    return idx;
}

int fx_spawn_stump_emitter(FxPool *pool, int mech_id, int limb,
                           float duration_s)
{
    int idx = fx_alloc(pool);
    if (idx < 0) return -1;
    FxParticle *fp = &pool->items[idx];
    fp->pos       = (Vec2){ 0.0f, 0.0f };   /* updated each tick to parent particle */
    fp->render_prev_pos = fp->pos;
    fp->vel       = (Vec2){ 0.0f, 0.0f };
    fp->life      = duration_s;
    fp->life_max  = duration_s;
    fp->size      = 0.0f;
    fp->color     = 0x00000000u;     /* invisible — emitter doesn't render */
    fp->kind      = FX_STUMP;
    fp->alive     = 1;
    fp->pin_mech_id = (int16_t)mech_id;
    fp->pin_limb    = (uint8_t)limb;
    fp->pin_pad     = 0;
    return idx;
}

/* M6 P02 — Jet exhaust + ground dust. fx_pool_init already zeros every
 * slot so the unused pin_* fields stay clean; we set them defensively
 * here in case fx_alloc returns an overwritten slot. */
int fx_spawn_jet_exhaust(FxPool *pool, Vec2 pos, Vec2 vel,
                         float life, float size,
                         uint32_t color_hot, uint32_t color_cool)
{
    int idx = fx_alloc(pool);
    if (idx < 0) return -1;
    FxParticle *fp = &pool->items[idx];
    fp->pos             = pos;
    fp->render_prev_pos = pos;
    fp->vel             = vel;
    fp->life            = life;
    fp->life_max        = life;
    fp->size            = size;
    fp->color           = color_hot;
    fp->color_cool      = color_cool;
    fp->kind            = FX_JET_EXHAUST;
    fp->alive           = 1;
    fp->pin_mech_id     = -1;
    fp->pin_limb        = 0;
    fp->pin_pad         = 0;
    return idx;
}

int fx_spawn_ground_dust(FxPool *pool, Vec2 pos, Vec2 vel,
                         float life, float size, uint32_t color)
{
    int idx = fx_alloc(pool);
    if (idx < 0) return -1;
    FxParticle *fp = &pool->items[idx];
    fp->pos             = pos;
    fp->render_prev_pos = pos;
    fp->vel             = vel;
    fp->life            = life;
    fp->life_max        = life;
    fp->size            = size;
    fp->color           = color;
    /* Cool color = same RGB with alpha 0, so the puff fades to zero
     * over its life without a colour shift. Packed as RGBA8 with the
     * alpha byte cleared. */
    fp->color_cool      = color & 0xFFFFFF00u;
    fp->kind            = FX_GROUND_DUST;
    fp->alive           = 1;
    fp->pin_mech_id     = -1;
    fp->pin_limb        = 0;
    fp->pin_pad         = 0;
    return idx;
}

int fx_spawn_damage_number(FxPool *pool, Vec2 pos, Vec2 dir,
                           uint8_t damage_u8, uint8_t weapon_id,
                           pcg32_t *rng)
{
    /* Don't render fully-absorbed-by-armor hits — those still spew a
     * few sparks via mech_apply_damage's "armor ate it" branch but the
     * meaningful "you took 0 damage" feedback is the spark + the unchanged
     * HP bar, not a "+0" glyph. */
    if (damage_u8 == 0) return -1;

    int idx = fx_alloc(pool);
    if (idx < 0) return -1;
    FxParticle *fp = &pool->items[idx];

    DamageTier tier = dmg_tier_for(damage_u8);
    const DmgTierDef *def = &g_dmg_tier[tier];

    /* Spew direction: perpendicular to dir, then random-flipped sign so
     * sequential hits alternate sides (anti-stack), plus a small ±0.3 rad
     * cone jitter so two identical hits don't trace the same arc. dir is
     * the bullet direction (toward victim); perpendicular is (-dir.y,
     * +dir.x). When dir is degenerate (dir.x == dir.y == 0 — e.g. an
     * environmental damage tick from a DEADLY tile), default to straight
     * up so the glyph still reads as airborne. */
    float dlen = sqrtf(dir.x * dir.x + dir.y * dir.y);
    Vec2  ndir = (dlen > 1e-4f)
                 ? (Vec2){ dir.x / dlen, dir.y / dlen }
                 : (Vec2){ 0.0f, -1.0f };
    float perp_sign = (pcg32_next(rng) & 1u) ? 1.0f : -1.0f;
    Vec2  perp = { -ndir.y * perp_sign, ndir.x * perp_sign };
    float cone = pcg32_float01(rng) * 0.6f - 0.3f;   /* ±0.3 rad ≈ ±17° */
    float c = cosf(cone), s = sinf(cone);
    Vec2  spew = { perp.x * c - perp.y * s, perp.x * s + perp.y * c };

    float speed = def->speed_min +
                  (def->speed_max - def->speed_min) * pcg32_float01(rng);

    fp->pos             = (Vec2){ pos.x, pos.y - 8.0f };   /* lift 8 px so spawn isn't inside the limb */
    fp->vel             = (Vec2){ spew.x * speed,
                                  spew.y * speed - def->upward_bias };
    fp->render_prev_pos = fp->pos;
    fp->life            = def->life_s;
    fp->life_max        = def->life_s;
    fp->size            = def->font_px;
    fp->color           = def->color;
    fp->color_cool      = def->outline;
    fp->kind            = FX_DAMAGE_NUMBER;
    fp->alive           = 1;
    fp->pin_mech_id     = (int16_t)damage_u8;   /* digits to render */
    fp->pin_limb        = 0;                    /* bounce counter */
    fp->pin_pad         = (uint8_t)(((tier == DMG_TIER_CRIT)  ? 0x1u : 0u) |
                                    ((tier == DMG_TIER_HEAVY) ? 0x2u : 0u));
    fp->angle           = (pcg32_float01(rng) * 0.4f) - 0.2f;     /* tiny initial tilt */
    /* ±1.0 rad/s ≈ ±57°/s — a gentle tumble (~1/6 revolution per
     * second). The earlier ±3 rad/s spec value spun the glyphs faster
     * than the eye could read at 60 Hz (full rotation every 2 s).
     * Bounce friction (0.60×) damps it further on each contact. */
    fp->ang_vel         = (pcg32_float01(rng) * 2.0f) - 1.0f;

    SHOT_LOG("dmgnum spawn pos=%.1f,%.1f dir=%.2f,%.2f dmg=%u tier=%d",
             pos.x, pos.y, dir.x, dir.y, (unsigned)damage_u8, (int)tier);

    (void)weapon_id;   /* reserved — v1 ignores; HIT_EVENT doesn't carry it */
    return idx;
}

int fx_spawn_tracer(FxPool *pool, Vec2 a, Vec2 b) {
    int idx = fx_alloc(pool);
    if (idx < 0) return -1;
    FxParticle *fp = &pool->items[idx];
    fp->pos      = a;             /* start point */
    fp->render_prev_pos = a;
    fp->vel      = (Vec2){ b.x - a.x, b.y - a.y };  /* end-point delta */
    fp->life     = 0.06f;
    fp->life_max = 0.06f;
    fp->size     = 1.0f;
    fp->color    = 0xC8FAFFFFu;   /* pale cyan */
    fp->kind     = FX_TRACER;
    fp->alive    = 1;
    return idx;
}

/* Per-tick update: blood and sparks fall under gravity, age out,
 * and deposit decals when they expire. P12 — FX_STUMP emitters pin to
 * a parent particle (looked up via pin_mech_id + pin_limb each tick)
 * and spawn 1–2 blood drops per tick at that world position; they don't
 * integrate or wall-collide themselves. */
void fx_update(World *w, float dt) {
    FxPool *pool = &w->fx;
    Vec2 g = w->level.gravity;
    int last_alive = -1;

    for (int i = 0; i < pool->count; ++i) {
        FxParticle *fp = &pool->items[i];
        if (!fp->alive) continue;

        fp->life -= dt;
        if (fp->life <= 0.0f) {
            if (fp->kind == FX_BLOOD) {
                /* On death, half of blood drops a decal. The decal RT
                 * doesn't care about the particle's velocity, just its
                 * final position. */
                if ((pcg32_next(w->rng) & 1u) == 0u) {
                    decal_paint_blood(fp->pos, fp->size + 1.5f);
                }
            }
            fp->alive = 0;
            continue;
        }

        if (fp->kind == FX_STUMP) {
            /* Look up the parent particle on the pinned mech each tick
             * so the emitter tracks the still-moving torso wherever it
             * tumbles. parent_part = the joint particle the dismembered
             * limb USED to attach to. */
            int mid = fp->pin_mech_id;
            int parent_part = -1;
            switch (fp->pin_limb) {
                case LIMB_HEAD:  parent_part = PART_NECK;       break;
                case LIMB_L_ARM: parent_part = PART_L_SHOULDER; break;
                case LIMB_R_ARM: parent_part = PART_R_SHOULDER; break;
                case LIMB_L_LEG: parent_part = PART_L_HIP;      break;
                case LIMB_R_LEG: parent_part = PART_R_HIP;      break;
                default:         parent_part = -1;              break;
            }
            if (mid < 0 || mid >= w->mech_count || parent_part < 0) {
                fp->alive = 0;
                continue;
            }
            const Mech *m = &w->mechs[mid];
            int idx = (int)m->particle_base + parent_part;
            Vec2 pos = particle_pos(&w->particles, idx);
            fp->pos = pos;
            /* 1–2 blood drops per tick, downward-biased so the trail
             * hangs below the joint regardless of the body's pose. */
            int n = 1 + (int)(pcg32_next(w->rng) & 1u);
            for (int k = 0; k < n; ++k) {
                Vec2 vd = {
                    (pcg32_float01(w->rng) - 0.5f) * 200.0f,
                    100.0f + pcg32_float01(w->rng) * 200.0f,
                };
                fx_spawn_blood(pool, pos, vd, w->rng);
            }
            if (i > last_alive) last_alive = i;
            continue;     /* skip integrate + wall-collide */
        }

        if (fp->kind == FX_JET_EXHAUST) {
            /* M6 P02 — additive exhaust. Linear drag (gas dissipates
             * fast) + mild upward buoyancy fights gravity so the trail
             * rises a touch as it cools. JET_BUOY_PXS2 = 80 px/s²
             * comfortably under JET_THRUST so the trail still drifts
             * downward when the body is climbing. */
            const float JET_BUOY_PXS2 = 80.0f;
            fp->vel.x *= 0.92f;
            fp->vel.y *= 0.92f;
            fp->vel.y -= JET_BUOY_PXS2 * dt;
            fp->pos.x += fp->vel.x * dt;
            fp->pos.y += fp->vel.y * dt;
            /* No wall-collide — exhaust passes through; tile collision
             * here would just kill particles spawned half a pixel below
             * the nozzle the moment a mech is on a slope. */
            if (i > last_alive) last_alive = i;
            continue;
        }

        if (fp->kind == FX_GROUND_DUST) {
            /* M6 P02 — dust + steam. Heavy drag, mild gravity so the
             * cloud hangs low and dissipates. No tile-collide death
             * (the dust spawns AT ground level; one frame later it'd
             * always be inside a tile and die instantly). */
            const float GROUND_DUST_GRAVITY_PXS2 = 120.0f;
            fp->vel.x *= 0.88f;
            fp->vel.y *= 0.88f;
            fp->vel.y += GROUND_DUST_GRAVITY_PXS2 * dt * 0.3f;
            fp->pos.x += fp->vel.x * dt;
            fp->pos.y += fp->vel.y * dt;
            if (i > last_alive) last_alive = i;
            continue;
        }

        if (fp->kind == FX_DAMAGE_NUMBER) {
            /* M6 P04 — flying damage glyph: gravity + mild drag,
             * tile-collide BOUNCE (not die) up to 2 times, then rest.
             * Matches blood's gravity (`g.y`) so the digit falls with
             * the same gravity-feel as the blood it spawns alongside.
             *
             * Hover phase: gravity ramps from 0 → full over the first
             * DMGNUM_HOVER_S seconds of life. The initial upward-bias
             * launches the glyph; with gravity damped during this
             * window the number genuinely *pops up and hangs* for a
             * beat before falling — closer to the spec's "pops" feel
             * than a pure-projectile arc, which read as "falls
             * immediately" because the up-bias is small relative to
             * gravity at 60 Hz.
             *
             * Bounce model: per-axis solid-cell test on the next-tick
             * candidate position. Walls flip and damp x; floors/ceilings
             * flip and damp y. Crude but sufficient — the numbers
             * spend <2.4 s airborne and the §15 trade-off accepts that
             * weird 45° geometry can occasionally fall through. */
            const float DMGNUM_HOVER_S = 0.30f;
            float age = fp->life_max - fp->life;
            float grav_scale = (age < DMGNUM_HOVER_S)
                               ? (age / DMGNUM_HOVER_S)
                               : 1.0f;
            fp->ang_vel *= 0.995f;
            fp->angle   += fp->ang_vel * dt;
            fp->vel.x   *= 0.99f;
            fp->vel.x   += g.x * dt * grav_scale;
            fp->vel.y   += g.y * dt * grav_scale;

            Vec2 next = { fp->pos.x + fp->vel.x * dt,
                          fp->pos.y + fp->vel.y * dt };

            uint8_t bounces = fp->pin_limb;
            bool hit = level_point_solid(&w->level, next);

            if (bounces < 2 && hit) {
                /* Resolve which axis the contact came from by probing
                 * each axis independently against the candidate. */
                bool hit_x = level_point_solid(&w->level,
                                               (Vec2){ next.x, fp->pos.y });
                bool hit_y = level_point_solid(&w->level,
                                               (Vec2){ fp->pos.x, next.y });
                if (hit_x) fp->vel.x = -fp->vel.x * 0.55f;
                if (hit_y) fp->vel.y = -fp->vel.y * 0.55f;
                if (!hit_x && !hit_y) {
                    /* Diagonal contact — flip both for a corner bounce. */
                    fp->vel.x = -fp->vel.x * 0.55f;
                    fp->vel.y = -fp->vel.y * 0.55f;
                }
                fp->vel.x   *= 0.70f;     /* friction on horizontal */
                fp->ang_vel *= 0.60f;     /* spin damps on hit */
                fp->pin_limb = (uint8_t)(bounces + 1);

                /* Quiet metallic clink. 0.30 base volume in the manifest;
                 * audio_play_at silently no-ops if the asset failed to
                 * load (fresh checkout / asset PR not yet landed). */
                audio_play_at(SFX_DAMAGE_TINK, fp->pos);
            } else if (bounces >= 2 &&
                       level_point_solid(&w->level,
                                         (Vec2){ next.x, next.y + 1.0f })) {
                /* Rest contact — pin to ground and let the life-fade
                 * carry the glyph out. */
                fp->vel.x = fp->vel.y = 0.0f;
                fp->ang_vel = 0.0f;
            } else {
                fp->pos = next;
            }
            if (i > last_alive) last_alive = i;
            continue;
        }

        /* M6 P09 — Weather particles (WORLD-space, post-user-feedback).
         *
         * Per-kind gravity + AMBI_WIND zone push + tile-collide death.
         * Flakes visibly hit the ground (where they die at the contact
         * point) — paired with the global snow_accum pile growing via
         * atmosphere_advance_accumulators, the user sees both "snow
         * falling" and "snow accumulating" in lockstep.
         *
         * Spawn always happens above the local mech viewport (the
         * atmosphere.c spawn helper) so particles only burn cycles in
         * the player's visible region. */
        if (fp->kind == FX_WEATHER_SNOW || fp->kind == FX_WEATHER_RAIN ||
            fp->kind == FX_WEATHER_DUST || fp->kind == FX_WEATHER_EMBER)
        {
            /* Per-kind gravity (px/s²). SNOW gentle, RAIN heavy, DUST
             * buoyant, EMBER inverted. */
            float grav_pxs2 = 0.0f;
            switch (fp->kind) {
                case FX_WEATHER_SNOW:  grav_pxs2 =  120.0f; break;
                case FX_WEATHER_RAIN:  grav_pxs2 =  900.0f; break;
                case FX_WEATHER_DUST:  grav_pxs2 =   -8.0f; break;
                case FX_WEATHER_EMBER: grav_pxs2 = -180.0f; break;
                default: break;
            }
            fp->vel.y += grav_pxs2 * dt;

            /* Apply each AMBI_WIND zone the particle is currently in.
             * Same wind direction the mech feels — visible streak when
             * a strong wind blows across falling snow. The strength
             * cap here (300 px/s²) is a fraction of the mech wind
             * (1500 px/s²) so flakes don't get swept off-screen. */
            const Level *L = &w->level;
            for (int z = 0; z < L->ambi_count; ++z) {
                const LvlAmbi *a = &L->ambis[z];
                if (a->kind != AMBI_WIND) continue;
                if (fp->pos.x < a->rect_x ||
                    fp->pos.x > a->rect_x + a->rect_w ||
                    fp->pos.y < a->rect_y ||
                    fp->pos.y > a->rect_y + a->rect_h) continue;
                float sx = (float)a->dir_x_q / 32767.0f;
                float sy = (float)a->dir_y_q / 32767.0f;
                float st = (float)a->strength_q / 32767.0f;
                fp->vel.x += sx * st * 300.0f * dt;
                fp->vel.y += sy * st * 300.0f * dt;
            }
            /* Sinusoidal sway breaks the linearity for snow + dust. */
            if (fp->kind == FX_WEATHER_SNOW) {
                fp->vel.x += sinf((fp->pos.y * 0.02f) +
                                  (float)w->tick * 0.012f) * 6.0f * dt;
            } else if (fp->kind == FX_WEATHER_DUST) {
                fp->vel.x += sinf((fp->pos.y * 0.015f) +
                                  (float)w->tick * 0.010f) * 4.0f * dt;
            }

            /* Integrate. */
            fp->pos.x += fp->vel.x * dt;
            fp->pos.y += fp->vel.y * dt;

            /* Tile-collide: a SOLID, non-BACKGROUND tile kills the
             * particle at the contact point. The persistent snow pile
             * comes from atmosphere_advance_accumulators (a global
             * 0..1 depth that grows over the round); these dying
             * flakes provide the immediate "the snow landed here"
             * visual. */
            int ts = w->level.tile_size;
            if (ts > 0) {
                int tx = (int)(fp->pos.x / (float)ts);
                int ty = (int)(fp->pos.y / (float)ts);
                uint16_t f = level_flags_at(&w->level, tx, ty);
                if ((f & TILE_F_SOLID) && !(f & TILE_F_BACKGROUND)) {
                    fp->alive = 0;
                    continue;
                }
            }
            /* Off-world bounds: die so we don't leak particles. */
            if (ts > 0) {
                float wpx = (float)w->level.width  * (float)ts;
                float wph = (float)w->level.height * (float)ts;
                if (fp->pos.y > wph + 100.0f || fp->pos.y < -1200.0f ||
                    fp->pos.x < -400.0f || fp->pos.x > wpx + 400.0f) {
                    fp->alive = 0;
                    continue;
                }
            }
            if (i > last_alive && fp->alive) last_alive = i;
            continue;
        }
        if (fp->kind == FX_AMBIENT_WIND_STREAK ||
            fp->kind == FX_AMBIENT_ZEROG_MOTE  ||
            fp->kind == FX_AMBIENT_ACID_BUBBLE)
        {
            /* World-space; no gravity (these are decorative). Simple
             * integrate; tile collision would kill the bubbles instantly
             * against the floor of the ACID rect. */
            fp->pos.x += fp->vel.x * dt;
            fp->pos.y += fp->vel.y * dt;
            if (i > last_alive) last_alive = i;
            continue;
        }

        if (fp->kind != FX_TRACER) {
            /* semi-implicit Euler: v += g*dt; p += v*dt */
            fp->vel.x += g.x * dt;
            fp->vel.y += g.y * dt;
            fp->pos.x += fp->vel.x * dt;
            fp->pos.y += fp->vel.y * dt;

            /* Map collision: blood and sparks die when they hit a wall.
             * We deliberately splat at the contact point (decal). */
            if (level_point_solid(&w->level, fp->pos)) {
                if (fp->kind == FX_BLOOD) {
                    decal_paint_blood(fp->pos, fp->size + 2.0f);
                }
                fp->alive = 0;
                continue;
            }
        }

        if (i > last_alive) last_alive = i;
    }

    /* Trim count down to the highest live slot to keep the integrate
     * loop tight. */
    pool->count = last_alive + 1;
}

/* Inline hot→cool color lerp helper. The byte-unpack + lerp + repack
 * is otherwise duplicated across FX_JET_EXHAUST and FX_GROUND_DUST
 * branches. Hoisted so the hot loop stays small. */
static inline Color fx_lerp_hot_cool(uint32_t hot, uint32_t cool, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float hr = (float)((hot  >> 24) & 0xFF);
    float hg = (float)((hot  >> 16) & 0xFF);
    float hb = (float)((hot  >>  8) & 0xFF);
    float ha = (float)((hot       ) & 0xFF);
    float cr = (float)((cool >> 24) & 0xFF);
    float cg = (float)((cool >> 16) & 0xFF);
    float cb = (float)((cool >>  8) & 0xFF);
    float ca = (float)((cool      ) & 0xFF);
    return (Color){
        (unsigned char)(hr + (cr - hr) * t),
        (unsigned char)(hg + (cg - hg) * t),
        (unsigned char)(hb + (cb - hb) * t),
        (unsigned char)(ha + (ca - ha) * t),
    };
}

/* Octagon stand-in for DrawCircleV. The raylib default (36 segments,
 * 108 vertices, 72 trig calls per call) is overkill at particle scale
 * — 8 segments (24 vertices, 16 trig) reads as a circle at any
 * radius < ~12 px while folding into raylib's auto-batcher with the
 * default texture binding. 4.5× cheaper than DrawCircleV; the BIG
 * win is still the per-blend-mode batching (one BlendMode pair per
 * pass instead of one per particle). */
#define FX_PARTICLE_SEGMENTS 8
static inline void fx_draw_particle(Vector2 center, float radius, Color col) {
    DrawCircleSector(center, radius, 0.0f, 360.0f,
                     FX_PARTICLE_SEGMENTS, col);
}

void fx_draw(const FxPool *pool, float alpha) {
    /* M6 P02-perf — Two-pass batched render.
     *
     * The prior implementation wrapped each FX_JET_EXHAUST particle
     * in a BeginBlendMode/EndBlendMode pair. raylib's BeginBlendMode
     * calls rlDrawRenderBatchActive() which forces a GPU draw call.
     * At the Burst-boost peak (~7680 live additive particles) the
     * old code triggered THOUSANDS of GPU flushes per frame — the
     * dominant cost of jetting at 4K. Two passes (alpha-blend, then
     * additive) keep raylib's auto-batcher happy: every particle in
     * the same blend mode folds into the default VBO and flushes
     * once per ~1365 quads (RL_DEFAULT_BATCH_BUFFER_ELEMENTS / 6).
     *
     * Within each pass DrawRectangleV replaces DrawCircleV — 1 quad
     * (6 vertices, 0 trig) per particle vs a 36-segment fan (108
     * vertices, 72 trig calls). At small particle sizes the visual
     * difference is imperceptible under motion. */

    /* ---- Pass 1: alpha-blended particles ---- */
    for (int i = 0; i < pool->count; ++i) {
        const FxParticle *fp = &pool->items[i];
        if (!fp->alive) continue;
        FxKind kind = (FxKind)fp->kind;
        /* Skip kinds handled in later passes. STUMP is invisible.
         * M6 P09 — Weather is drawn in atmosphere_draw_weather at
         * window resolution (sharp pixels outside the internal RT
         * upscale); ambient zones get their own pass below. */
        if (kind == FX_JET_EXHAUST || kind == FX_STUMP) continue;

        float life_frac = fp->life /
                          (fp->life_max > 0.0f ? fp->life_max : 1.0f);
        Vector2 pos = {
            fp->render_prev_pos.x + (fp->pos.x - fp->render_prev_pos.x) * alpha,
            fp->render_prev_pos.y + (fp->pos.y - fp->render_prev_pos.y) * alpha,
        };

        switch (kind) {
            case FX_TRACER: {
                /* tracer's vel is the end-point delta — fixed for the
                 * particle's life, so no interp needed on the endpoint. */
                unsigned char r = (unsigned char)((fp->color >> 24) & 0xFF);
                unsigned char g = (unsigned char)((fp->color >> 16) & 0xFF);
                unsigned char b = (unsigned char)((fp->color >>  8) & 0xFF);
                unsigned char a = (unsigned char)(((fp->color >> 0) & 0xFF) * life_frac);
                DrawLineEx(pos,
                           (Vector2){ pos.x + fp->vel.x, pos.y + fp->vel.y },
                           1.5f, (Color){ r, g, b, a });
                break;
            }
            case FX_GROUND_DUST: {
                Color cc = fx_lerp_hot_cool(fp->color, fp->color_cool,
                                            1.0f - life_frac);
                fx_draw_particle(pos, fp->size, cc);
                break;
            }
            case FX_SMOKE: {
                unsigned char a = (unsigned char)(((fp->color >> 0) & 0xFF) * life_frac);
                fx_draw_particle(pos, fp->size * 1.4f,
                             (Color){ 60, 60, 60, (unsigned char)(a / 2) });
                break;
            }
            case FX_BLOOD:
            case FX_SPARK: {
                unsigned char r = (unsigned char)((fp->color >> 24) & 0xFF);
                unsigned char g = (unsigned char)((fp->color >> 16) & 0xFF);
                unsigned char b = (unsigned char)((fp->color >>  8) & 0xFF);
                unsigned char a = (unsigned char)(((fp->color >> 0) & 0xFF) * life_frac);
                fx_draw_particle(pos, fp->size, (Color){ r, g, b, a });
                break;
            }
            /* M6 P09 — ambient zone particles (world-space). WIND
             * streaks are short lines; ZERO_G + ACID are alpha discs.
             * Render here inside the world-camera-transform so the
             * particles pan with the camera. */
            case FX_AMBIENT_WIND_STREAK: {
                unsigned char r = (unsigned char)((fp->color >> 24) & 0xFF);
                unsigned char g = (unsigned char)((fp->color >> 16) & 0xFF);
                unsigned char b = (unsigned char)((fp->color >>  8) & 0xFF);
                unsigned char a = (unsigned char)(((fp->color >> 0) & 0xFF) * life_frac);
                /* Streak length proportional to velocity for a sense
                 * of motion. */
                float len = fp->size;
                float vlen = sqrtf(fp->vel.x * fp->vel.x +
                                   fp->vel.y * fp->vel.y);
                Vec2 unit = (vlen > 1e-4f)
                          ? (Vec2){ fp->vel.x / vlen, fp->vel.y / vlen }
                          : (Vec2){ 1.0f, 0.0f };
                Vector2 a0 = { pos.x, pos.y };
                Vector2 a1 = { pos.x - unit.x * len, pos.y - unit.y * len };
                DrawLineEx(a0, a1, 1.5f, (Color){ r, g, b, a });
                break;
            }
            case FX_AMBIENT_ZEROG_MOTE: {
                unsigned char r = (unsigned char)((fp->color >> 24) & 0xFF);
                unsigned char g = (unsigned char)((fp->color >> 16) & 0xFF);
                unsigned char b = (unsigned char)((fp->color >>  8) & 0xFF);
                unsigned char a = (unsigned char)(((fp->color >> 0) & 0xFF) * life_frac);
                fx_draw_particle(pos, fp->size, (Color){ r, g, b, a });
                break;
            }
            case FX_AMBIENT_ACID_BUBBLE: {
                unsigned char r = (unsigned char)((fp->color >> 24) & 0xFF);
                unsigned char g = (unsigned char)((fp->color >> 16) & 0xFF);
                unsigned char b = (unsigned char)((fp->color >>  8) & 0xFF);
                unsigned char a = (unsigned char)(((fp->color >> 0) & 0xFF) * life_frac);
                fx_draw_particle(pos, fp->size, (Color){ r, g, b, a });
                /* Highlight on the upward edge for the "bubble" read. */
                fx_draw_particle((Vector2){ pos.x - fp->size * 0.3f,
                                            pos.y - fp->size * 0.3f },
                                 fp->size * 0.3f,
                                 (Color){ 220, 255, 200, (unsigned char)(a * 0.7f) });
                break;
            }
            case FX_JET_EXHAUST:
            case FX_STUMP:
            /* M6 P04 — damage numbers render in fx_draw_damage_numbers,
             * a separate pass run AFTER the internal-RT upscale blit so
             * the glyphs land at sharp window pixels. fx_draw runs
             * inside the internal-RT world pass; skip here. */
            case FX_DAMAGE_NUMBER:
            /* M6 P09 — weather flakes (world-space, post-user-feedback).
             * Each kind has a distinct render: snow = small white
             * circles, rain = thin diagonal lines, dust = warm tan
             * discs, ember = (additive — handled in pass 2 below). */
            case FX_WEATHER_SNOW: {
                unsigned char r = (unsigned char)((fp->color >> 24) & 0xFF);
                unsigned char g = (unsigned char)((fp->color >> 16) & 0xFF);
                unsigned char b = (unsigned char)((fp->color >>  8) & 0xFF);
                unsigned char a = (unsigned char)((fp->color >>  0) & 0xFF);
                fx_draw_particle(pos, fp->size, (Color){ r, g, b, a });
                break;
            }
            case FX_WEATHER_RAIN: {
                unsigned char r = (unsigned char)((fp->color >> 24) & 0xFF);
                unsigned char g = (unsigned char)((fp->color >> 16) & 0xFF);
                unsigned char b = (unsigned char)((fp->color >>  8) & 0xFF);
                unsigned char a = (unsigned char)((fp->color >>  0) & 0xFF);
                /* Streak along velocity direction. */
                float vlen = sqrtf(fp->vel.x * fp->vel.x +
                                   fp->vel.y * fp->vel.y);
                if (vlen > 1e-3f) {
                    Vector2 tail = {
                        pos.x - (fp->vel.x / vlen) * fp->size,
                        pos.y - (fp->vel.y / vlen) * fp->size,
                    };
                    DrawLineEx(pos, tail, 1.2f, (Color){ r, g, b, a });
                }
                break;
            }
            case FX_WEATHER_DUST: {
                unsigned char r = (unsigned char)((fp->color >> 24) & 0xFF);
                unsigned char g = (unsigned char)((fp->color >> 16) & 0xFF);
                unsigned char b = (unsigned char)((fp->color >>  8) & 0xFF);
                unsigned char a = (unsigned char)((fp->color >>  0) & 0xFF);
                fx_draw_particle(pos, fp->size, (Color){ r, g, b, a });
                break;
            }
            case FX_WEATHER_EMBER:
                /* Additive pass below handles embers — skip here. */
                break;
            case FX_KIND_COUNT: break;
        }
    }

    /* ---- Pass 2: additive (FX_JET_EXHAUST + FX_WEATHER_EMBER) ---- */
    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 0; i < pool->count; ++i) {
        const FxParticle *fp = &pool->items[i];
        if (!fp->alive) continue;
        if (fp->kind != FX_JET_EXHAUST && fp->kind != FX_WEATHER_EMBER) continue;

        float life_frac = fp->life /
                          (fp->life_max > 0.0f ? fp->life_max : 1.0f);
        Vector2 pos = {
            fp->render_prev_pos.x + (fp->pos.x - fp->render_prev_pos.x) * alpha,
            fp->render_prev_pos.y + (fp->pos.y - fp->render_prev_pos.y) * alpha,
        };
        if (fp->kind == FX_JET_EXHAUST) {
            Color cc = fx_lerp_hot_cool(fp->color, fp->color_cool,
                                        1.0f - life_frac);
            fx_draw_particle(pos, fp->size, cc);
        } else {
            unsigned char r = (unsigned char)((fp->color >> 24) & 0xFF);
            unsigned char g = (unsigned char)((fp->color >> 16) & 0xFF);
            unsigned char b = (unsigned char)((fp->color >>  8) & 0xFF);
            unsigned char a = (unsigned char)(((fp->color >>  0) & 0xFF) * life_frac);
            fx_draw_particle(pos, fp->size, (Color){ r, g, b, a });
        }
    }
    EndBlendMode();
}

/* M6 P04 — render pass for FX_DAMAGE_NUMBER particles. Runs OUTSIDE the
 * internal-RT world pass (called from renderer_draw_frame after the
 * upscale blit, before hud_draw) so glyphs land at sharp window pixels.
 * `internal_cam` is the same camera the world pass used (in internal
 * pixels); blit_scale + blit_dx + blit_dy are the M6 P03 letterbox
 * numbers that map internal → window. */
void fx_draw_damage_numbers(const FxPool *pool,
                            Camera2D internal_cam,
                            float blit_scale,
                            float blit_dx, float blit_dy)
{
    if (pool->count == 0) return;

    /* Steps Mono Thin atlas — M5 P13 vendored at 32 px source size, the
     * size used everywhere HUD numerics render. We scale at draw time
     * via DrawTextPro's fontSize. Falls back to raylib's default font
     * if the TTF didn't load (fresh checkout / missing assets). */
    Font font = (g_ui_fonts_loaded && g_ui_font_mono.texture.id != 0)
                ? g_ui_font_mono
                : GetFontDefault();

    for (int i = 0; i < pool->count; ++i) {
        const FxParticle *fp = &pool->items[i];
        if (!fp->alive) continue;
        if (fp->kind != FX_DAMAGE_NUMBER) continue;

        /* World → internal-RT pixels via raylib's GetWorldToScreen2D
         * against the internal camera, then internal → window via the
         * letterbox numbers. */
        Vector2 internal_xy = GetWorldToScreen2D(
            (Vector2){ fp->pos.x, fp->pos.y }, internal_cam);
        Vector2 screen_xy = {
            internal_xy.x * blit_scale + blit_dx,
            internal_xy.y * blit_scale + blit_dy,
        };

        /* Decode tier from the flag bits set at spawn. The CRIT/HEAVY
         * flags are explicit so the render pass doesn't have to redo
         * the threshold compare; LIGHT/NORMAL/MEDIUM fall back to
         * dmg_tier_for on the damage value itself. */
        DamageTier tier;
        if      (fp->pin_pad & 0x1u) tier = DMG_TIER_CRIT;
        else if (fp->pin_pad & 0x2u) tier = DMG_TIER_HEAVY;
        else                         tier = dmg_tier_for((uint8_t)fp->pin_mech_id);
        const DmgTierDef *def = &g_dmg_tier[tier];

        /* "%d" of the damage value lives in pin_mech_id (i16, 0..255). */
        char buf[8];
        int dmg = (int)fp->pin_mech_id;
        if (dmg < 0)   dmg = 0;
        if (dmg > 999) dmg = 999;   /* future-proofs against >u8 widening */
        snprintf(buf, sizeof buf, "%d", dmg);

        /* Alpha fade in the last 0.5 s of life. Before that, glyph
         * holds full alpha so it's readable while it's still moving. */
        float alpha = (fp->life < 0.5f) ? (fp->life / 0.5f) : 1.0f;
        if (alpha < 0.0f) alpha = 0.0f;

        Color glyph = (Color){
            (unsigned char)((def->color   >> 24) & 0xFF),
            (unsigned char)((def->color   >> 16) & 0xFF),
            (unsigned char)((def->color   >>  8) & 0xFF),
            (unsigned char)(((def->color  >>  0) & 0xFF) * alpha),
        };
        Color outln = (Color){
            (unsigned char)((def->outline >> 24) & 0xFF),
            (unsigned char)((def->outline >> 16) & 0xFF),
            (unsigned char)((def->outline >>  8) & 0xFF),
            (unsigned char)(((def->outline >> 0) & 0xFF) * alpha),
        };
        float deg = fp->angle * (180.0f / 3.14159265358979323846f);

        /* Bounce-squash: each ground contact compresses the glyph by 5%
         * vertically. Free game-feel beat — the digit visibly squishes
         * on the floor. Caps at 2 so bounces × 0.05 ≤ 0.10. */
        float bounce_squash = 1.0f - 0.05f * (float)fp->pin_limb;
        /* Render-pixel font size:
         *   font_px            — tier-defined base (10..18 px @ 1× zoom)
         *   * internal_cam.zoom — scale with player zoom so the glyph
         *     feels world-attached (default cam zoom is 1.4×, so a CRIT
         *     reads at ~25 px — matches the spec §6 "~1/4 mech height"
         *     intent which assumed 1× zoom in its math).
         *   * blit_scale       — match the window upscale so on a 4K
         *     monitor with a 1080-cap internal RT, a 25 px logical
         *     glyph paints at 50 window-px (sharp text either way).
         *   * bounce_squash    — vertical compression on ground contact. */
        float fs = def->font_px * internal_cam.zoom * blit_scale * bounce_squash;
        if (fs < 4.0f) fs = 4.0f;   /* never sub-pixel */

        Vector2 origin = { 0, 0 };

        /* 4-pass cardinal-offset outline at +/-1 px. Cheaper than a
         * shader; visually equivalent at 10–18 px font sizes. */
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                if (dx == 0 && dy == 0) continue;
                if (dx != 0 && dy != 0) continue;
                Vector2 off = { screen_xy.x + (float)dx,
                                screen_xy.y + (float)dy };
                DrawTextPro(font, buf, off, origin, deg, fs, 1.0f, outln);
            }
        }
        DrawTextPro(font, buf, screen_xy, origin, deg, fs, 1.0f, glyph);
    }
}
