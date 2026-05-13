#include "decal.h"
#include "log.h"

#include "../third_party/raylib/src/raylib.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * P13 — Splat layer with chunking.
 *
 * Small / mid maps (≤4096 px in both dims): one RT the size of the level,
 * same as M4. Big maps (>4096 px in either dim): the layer is divided
 * into 1024×1024 chunks; each chunk's RT is allocated on the FIRST
 * paint that overlaps it. Untouched zones cost zero memory.
 *
 * Why lazy-alloc per chunk: a Citadel-sized 6400×3200 level statically
 * partitioned into 7×4 chunks would be 28 × 4 MB = 112 MB just for the
 * splat layer — past the 80 MB texture budget. In practice a match only
 * stains 3–8 chunks (the high-traffic zones), so peak memory stays
 * around 12–32 MB. See `documents/m5/08-rendering.md` §"Decal-layer
 * chunking" + the TRADE_OFFS.md "Lazy-allocated decal chunks" entry.
 */

#define MAX_PENDING       256
#define DECAL_CHUNK_SIZE  1024
#define DECAL_THRESHOLD   4096
#define DECAL_MAX_CHUNKS  64       /* 8×8 grid worst case; covers any v1 map */

/* M6 P02 — splat kind discriminator. BLOOD uses the existing red
 * outer+inner stamp; SCORCH paints a dark brown/grey pair (warm-grey
 * outer halo + dark-charcoal core) for grounded-jet impingement
 * scorch marks. */
typedef enum {
    SPLAT_BLOOD  = 0,
    SPLAT_SCORCH = 1,
} SplatKind;

typedef struct {
    Vec2    pos;
    float   radius;
    uint8_t kind;     /* SplatKind */
    uint8_t pad[3];
} PendingSplat;

typedef struct {
    bool             chunked;
    int              x_chunks, y_chunks;
    int              level_w, level_h;
    /* For !chunked: only chunks[0] is used; the level-sized RT lives
     * there with a sentinel size {level_w, level_h}. For chunked: each
     * slot is either zero (not yet painted) or an allocated 1024×1024
     * RenderTexture2D. */
    RenderTexture2D  chunks[DECAL_MAX_CHUNKS];
    /* dirty[] is reserved for the M6 selective-redraw path (mark a
     * chunk dirty when paint lands; flush only dirties to the
     * backbuffer). At v1 we redraw every allocated chunk every frame —
     * 1 textured-quad per visible chunk is well inside budget. */
    bool             dirty[DECAL_MAX_CHUNKS];
} DecalLayer;

static DecalLayer g_layer;
static bool       g_layer_ready = false;
static PendingSplat g_pending[MAX_PENDING];
static int          g_pending_count = 0;

/* Initialize a single full-level RT in chunks[0]. */
static bool init_single_rt(int level_w, int level_h) {
    g_layer.chunked = false;
    g_layer.x_chunks = 1;
    g_layer.y_chunks = 1;
    g_layer.chunks[0] = LoadRenderTexture(level_w, level_h);
    if (g_layer.chunks[0].id == 0) {
        LOG_E("decal_init: LoadRenderTexture(%d,%d) failed", level_w, level_h);
        return false;
    }
    BeginTextureMode(g_layer.chunks[0]);
        ClearBackground((Color){0, 0, 0, 0});
    EndTextureMode();
    return true;
}

void decal_init(int level_w, int level_h) {
    /* wan-fixes-5 — `--dedicated` runs without a raylib window / GL
     * context. LoadRenderTexture would assert / crash. The dedicated
     * server doesn't render decals; clients init their own decal RTs
     * after platform_init. Bail cleanly when there's no GL. */
    if (!IsWindowReady()) return;
    if (g_layer_ready) decal_shutdown();
    memset(&g_layer, 0, sizeof g_layer);
    g_layer.level_w = level_w;
    g_layer.level_h = level_h;

    bool needs_chunk = (level_w > DECAL_THRESHOLD || level_h > DECAL_THRESHOLD);
    if (!needs_chunk) {
        if (!init_single_rt(level_w, level_h)) return;
    } else {
        g_layer.chunked = true;
        g_layer.x_chunks = (level_w + DECAL_CHUNK_SIZE - 1) / DECAL_CHUNK_SIZE;
        g_layer.y_chunks = (level_h + DECAL_CHUNK_SIZE - 1) / DECAL_CHUNK_SIZE;
        int n = g_layer.x_chunks * g_layer.y_chunks;
        if (n > DECAL_MAX_CHUNKS) {
            /* This means the level is bigger than 8192×8192 px (200×100
             * tiles at 32 px = 6400×3200 = 7×4 = 28 chunks; any v1
             * authored map is well below the cap). Defensive: fall back
             * to one giant RT and let the cap rip the budget — better
             * than refusing to load a level. */
            LOG_W("decal_init: level %dx%d wants %d chunks (cap %d) — using single RT",
                  level_w, level_h, n, DECAL_MAX_CHUNKS);
            init_single_rt(level_w, level_h);
        } else {
            LOG_I("decal_init: chunked layer %dx%d (%d×%d chunks, lazy-alloc)",
                  level_w, level_h, g_layer.x_chunks, g_layer.y_chunks);
        }
    }
    g_layer_ready = true;
    g_pending_count = 0;
}

void decal_shutdown(void) {
    if (g_layer_ready) {
        int n = g_layer.chunked
              ? g_layer.x_chunks * g_layer.y_chunks
              : 1;
        if (n > DECAL_MAX_CHUNKS) n = DECAL_MAX_CHUNKS;
        for (int i = 0; i < n; ++i) {
            if (g_layer.chunks[i].id != 0) UnloadRenderTexture(g_layer.chunks[i]);
        }
        memset(&g_layer, 0, sizeof g_layer);
        g_layer_ready = false;
    }
    g_pending_count = 0;
}

void decal_clear(void) {
    if (!g_layer_ready) return;
    int n = g_layer.chunked
          ? g_layer.x_chunks * g_layer.y_chunks
          : 1;
    if (n > DECAL_MAX_CHUNKS) n = DECAL_MAX_CHUNKS;
    for (int i = 0; i < n; ++i) {
        if (g_layer.chunks[i].id == 0) continue;
        BeginTextureMode(g_layer.chunks[i]);
            ClearBackground((Color){0, 0, 0, 0});
        EndTextureMode();
    }
    g_pending_count = 0;
}

void decal_paint_blood(Vec2 pos, float radius) {
    if (g_pending_count >= MAX_PENDING) return;   /* drop on overflow */
    g_pending[g_pending_count++] = (PendingSplat){ pos, radius, SPLAT_BLOOD, {0,0,0} };
}

void decal_paint_scorch(Vec2 pos, float radius) {
    /* Shares the blood queue + flush — kind discriminator picks color
     * in stamp_splat. Drop on overflow is acceptable: scorch is a
     * polish layer, not a gameplay signal. */
    if (g_pending_count >= MAX_PENDING) return;
    g_pending[g_pending_count++] = (PendingSplat){ pos, radius, SPLAT_SCORCH, {0,0,0} };
}

/* Lazy-alloc a chunk on first paint. Idempotent. */
static void ensure_chunk_alloc(int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= g_layer.x_chunks || cy >= g_layer.y_chunks) return;
    int idx = cy * g_layer.x_chunks + cx;
    if (idx < 0 || idx >= DECAL_MAX_CHUNKS) return;
    if (g_layer.chunks[idx].id != 0) return;
    g_layer.chunks[idx] = LoadRenderTexture(DECAL_CHUNK_SIZE, DECAL_CHUNK_SIZE);
    if (g_layer.chunks[idx].id == 0) {
        LOG_W("decal: chunk(%d,%d) LoadRenderTexture failed", cx, cy);
        return;
    }
    BeginTextureMode(g_layer.chunks[idx]);
        ClearBackground((Color){0, 0, 0, 0});
    EndTextureMode();
}

/* Stamp the two-layer splat (outer translucent + inner opaque) into
 * a render-texture at the supplied chunk-local position. M6 P02
 * adds the SCORCH kind: warm-grey halo + dark-charcoal core, no fade
 * (scorch decals are permanent for the round — see TRADE_OFFS.md
 * "Scorch decals are permanent for the round (no per-decal fade)"). */
static void stamp_splat(Vec2 lp, float r, uint8_t kind) {
    if (kind == SPLAT_SCORCH) {
        DrawCircleV((Vector2){lp.x, lp.y}, r * 1.4f, (Color){ 20, 16, 14, 110});
        DrawCircleV((Vector2){lp.x, lp.y}, r,        (Color){ 40, 32, 28, 200});
    } else {
        DrawCircleV((Vector2){lp.x, lp.y}, r * 1.4f, (Color){120,  0,  0, 110});
        DrawCircleV((Vector2){lp.x, lp.y}, r,        (Color){180, 10, 10, 220});
    }
}

void decal_flush_pending(void) {
    if (!g_layer_ready || g_pending_count == 0) return;

    if (!g_layer.chunked) {
        if (g_layer.chunks[0].id == 0) { g_pending_count = 0; return; }
        BeginTextureMode(g_layer.chunks[0]);
            for (int i = 0; i < g_pending_count; ++i) {
                stamp_splat(g_pending[i].pos, g_pending[i].radius,
                            g_pending[i].kind);
            }
        EndTextureMode();
        g_pending_count = 0;
        return;
    }

    /* Chunked: walk chunks, find overlapping pending splats, batch paints
     * inside one BeginTextureMode pair per chunk that needs one. With
     * 7×4 chunks × 256 splats max, the inner double-loop is ~7K ops per
     * flush — well inside slack. */
    int n_chunks = g_layer.x_chunks * g_layer.y_chunks;
    if (n_chunks > DECAL_MAX_CHUNKS) n_chunks = DECAL_MAX_CHUNKS;
    for (int ci = 0; ci < n_chunks; ++ci) {
        int cx = ci % g_layer.x_chunks;
        int cy = ci / g_layer.x_chunks;
        float wx0 = (float)(cx * DECAL_CHUNK_SIZE);
        float wy0 = (float)(cy * DECAL_CHUNK_SIZE);
        float wx1 = wx0 + (float)DECAL_CHUNK_SIZE;
        float wy1 = wy0 + (float)DECAL_CHUNK_SIZE;

        bool has_overlap = false;
        for (int i = 0; i < g_pending_count; ++i) {
            float rx = g_pending[i].radius * 1.4f;
            if (g_pending[i].pos.x + rx >= wx0 &&
                g_pending[i].pos.x - rx <  wx1 &&
                g_pending[i].pos.y + rx >= wy0 &&
                g_pending[i].pos.y - rx <  wy1) { has_overlap = true; break; }
        }
        if (!has_overlap) continue;

        ensure_chunk_alloc(cx, cy);
        if (g_layer.chunks[ci].id == 0) continue;

        BeginTextureMode(g_layer.chunks[ci]);
            for (int i = 0; i < g_pending_count; ++i) {
                Vec2  p  = g_pending[i].pos;
                float r  = g_pending[i].radius;
                float rx = r * 1.4f;
                if (p.x + rx < wx0 || p.x - rx >= wx1 ||
                    p.y + rx < wy0 || p.y - rx >= wy1) continue;
                Vec2 lp = { p.x - wx0, p.y - wy0 };
                stamp_splat(lp, r, g_pending[i].kind);
            }
        EndTextureMode();
        g_layer.dirty[ci] = true;
    }
    g_pending_count = 0;
}

void decal_draw_layer(void) {
    if (!g_layer_ready) return;

    if (!g_layer.chunked) {
        if (g_layer.chunks[0].id == 0) return;
        /* raylib's render textures come back Y-flipped — pass a negative
         * source-rectangle height to flip on draw. (See
         * [06-rendering-audio.md] "Y-flip gotcha.") */
        Rectangle src = {
            0, 0,
            (float)g_layer.chunks[0].texture.width,
            -(float)g_layer.chunks[0].texture.height
        };
        DrawTextureRec(g_layer.chunks[0].texture, src, (Vector2){0, 0}, WHITE);
        return;
    }

    /* Chunked: draw every allocated chunk at its world-space origin.
     * The caller is inside BeginMode2D so world coords map naturally.
     * Untouched chunks (id == 0) skip silently. Per-frame cost is one
     * DrawTextureRec per allocated chunk — typical match has 3–8. */
    for (int cy = 0; cy < g_layer.y_chunks; ++cy) {
        for (int cx = 0; cx < g_layer.x_chunks; ++cx) {
            int idx = cy * g_layer.x_chunks + cx;
            if (idx >= DECAL_MAX_CHUNKS) continue;
            if (g_layer.chunks[idx].id == 0) continue;
            Rectangle src = {
                0, 0,
                (float)DECAL_CHUNK_SIZE,
                -(float)DECAL_CHUNK_SIZE
            };
            Vector2 dst = {
                (float)(cx * DECAL_CHUNK_SIZE),
                (float)(cy * DECAL_CHUNK_SIZE)
            };
            DrawTextureRec(g_layer.chunks[idx].texture, src, dst, WHITE);
        }
    }
}
