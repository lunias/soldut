#include "particle.h"

#include "arena.h"
#include "decal.h"
#include "hash.h"
#include "level.h"
#include "log.h"

#include <math.h>
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
        /* Skip kinds handled in later passes. STUMP is invisible. */
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
            case FX_JET_EXHAUST:
            case FX_STUMP:
            case FX_KIND_COUNT: break;
        }
    }

    /* ---- Pass 2: additive (FX_JET_EXHAUST) ---- */
    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 0; i < pool->count; ++i) {
        const FxParticle *fp = &pool->items[i];
        if (!fp->alive) continue;
        if (fp->kind != FX_JET_EXHAUST) continue;

        float life_frac = fp->life /
                          (fp->life_max > 0.0f ? fp->life_max : 1.0f);
        Vector2 pos = {
            fp->render_prev_pos.x + (fp->pos.x - fp->render_prev_pos.x) * alpha,
            fp->render_prev_pos.y + (fp->pos.y - fp->render_prev_pos.y) * alpha,
        };
        Color cc = fx_lerp_hot_cool(fp->color, fp->color_cool,
                                    1.0f - life_frac);
        fx_draw_particle(pos, fp->size, cc);
    }
    EndBlendMode();
}
