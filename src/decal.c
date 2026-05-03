#include "decal.h"
#include "log.h"

#include "../third_party/raylib/src/raylib.h"

#include <stddef.h>

#define MAX_PENDING 256

typedef struct {
    Vec2  pos;
    float radius;
} PendingSplat;

static RenderTexture2D g_layer;
static bool   g_layer_ready = false;
static int    g_layer_w, g_layer_h;
static PendingSplat g_pending[MAX_PENDING];
static int    g_pending_count = 0;

void decal_init(int level_w, int level_h) {
    if (g_layer_ready) decal_shutdown();
    g_layer_w = level_w;
    g_layer_h = level_h;
    g_layer = LoadRenderTexture(level_w, level_h);
    if (g_layer.id == 0) {
        LOG_E("decal_init: LoadRenderTexture(%d,%d) failed", level_w, level_h);
        return;
    }
    /* Initialize to transparent. */
    BeginTextureMode(g_layer);
        ClearBackground((Color){0, 0, 0, 0});
    EndTextureMode();
    g_layer_ready = true;
    g_pending_count = 0;
}

void decal_shutdown(void) {
    if (g_layer_ready) {
        UnloadRenderTexture(g_layer);
        g_layer_ready = false;
    }
    g_pending_count = 0;
}

void decal_clear(void) {
    if (!g_layer_ready) return;
    BeginTextureMode(g_layer);
        ClearBackground((Color){0, 0, 0, 0});
    EndTextureMode();
    g_pending_count = 0;
}

void decal_paint_blood(Vec2 pos, float radius) {
    if (g_pending_count >= MAX_PENDING) return;   /* drop on overflow */
    g_pending[g_pending_count++] = (PendingSplat){ pos, radius };
}

void decal_flush_pending(void) {
    if (!g_layer_ready || g_pending_count == 0) return;
    BeginTextureMode(g_layer);
        for (int i = 0; i < g_pending_count; ++i) {
            const PendingSplat *s = &g_pending[i];
            /* Layered splats — outer translucent, inner opaque. */
            DrawCircleV(s->pos, s->radius * 1.4f, (Color){120,  0,  0, 110});
            DrawCircleV(s->pos, s->radius,        (Color){180, 10, 10, 220});
        }
    EndTextureMode();
    g_pending_count = 0;
}

void decal_draw_layer(void) {
    if (!g_layer_ready) return;
    /* raylib's render textures come back Y-flipped — pass a negative
     * source-rectangle height to flip on draw. (See [06-rendering-audio.md]
     * "Y-flip gotcha.") */
    Rectangle src = { 0, 0, (float)g_layer.texture.width, -(float)g_layer.texture.height };
    Vector2   dst = { 0, 0 };
    DrawTextureRec(g_layer.texture, src, dst, WHITE);
}
