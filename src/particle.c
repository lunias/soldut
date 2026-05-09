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

void fx_draw(const FxPool *pool, float alpha) {
    for (int i = 0; i < pool->count; ++i) {
        const FxParticle *fp = &pool->items[i];
        if (!fp->alive) continue;

        float t = fp->life / (fp->life_max > 0.0f ? fp->life_max : 1.0f);
        unsigned char r = (unsigned char)((fp->color >> 24) & 0xFF);
        unsigned char g = (unsigned char)((fp->color >> 16) & 0xFF);
        unsigned char b = (unsigned char)((fp->color >>  8) & 0xFF);
        unsigned char a = (unsigned char)(((fp->color >> 0) & 0xFF) * t);
        Color col = { r, g, b, a };
        /* P03: lerp between start-of-tick pos and latest pos so FX
         * motion stays smooth when render rate exceeds sim rate. */
        Vector2 pos = {
            fp->render_prev_pos.x + (fp->pos.x - fp->render_prev_pos.x) * alpha,
            fp->render_prev_pos.y + (fp->pos.y - fp->render_prev_pos.y) * alpha,
        };

        switch ((FxKind)fp->kind) {
            case FX_BLOOD:
                DrawCircleV(pos, fp->size, col);
                break;
            case FX_SPARK:
                DrawCircleV(pos, fp->size, col);
                break;
            case FX_TRACER:
                /* tracer's vel is the end-point delta — fixed for the
                 * particle's life, so no interp needed on the endpoint. */
                DrawLineEx(pos,
                           (Vector2){ pos.x + fp->vel.x, pos.y + fp->vel.y },
                           1.5f, col);
                break;
            case FX_SMOKE:
                /* M3 reserves a slot for smoke; for now we render it as
                 * a darker circle. The proper soft-puff additive
                 * version lands with the M5 art pass. */
                DrawCircleV(pos, fp->size * 1.4f,
                    (Color){ 60, 60, 60, (unsigned char)(a / 2) });
                break;
            case FX_STUMP:
                /* Pinned dismemberment emitter — invisible itself; the
                 * blood drops it spawns each tick are what's visible. */
                break;
            case FX_KIND_COUNT: break;
        }
    }
}
