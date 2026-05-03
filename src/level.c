#include "level.h"
#include "arena.h"
#include "log.h"

#include <string.h>

#define TILE_SIZE_PX  32

static void set_tile(Level *l, int x, int y, TileKind k) {
    if (x < 0 || x >= l->width || y < 0 || y >= l->height) return;
    l->tiles[y * l->width + x] = (uint8_t)k;
}

static void fill_rect(Level *l, int x0, int y0, int x1, int y1, TileKind k) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            set_tile(l, x, y, k);
}

void level_build_tutorial(Level *level, Arena *arena) {
    /* 100×40 tile arena = 3200×1280 px world. Comfortable for the M1
     * loop: enough room to run, jet, miss with the rifle, and find your
     * way back to the dummy. */
    level->width     = 100;
    level->height    = 40;
    level->tile_size = TILE_SIZE_PX;
    level->gravity   = (Vec2){0.0f, 1080.0f};   /* px/s^2 */

    int n = level->width * level->height;
    level->tiles = (uint8_t *)arena_alloc(arena, (size_t)n);
    if (!level->tiles) {
        LOG_E("level_build_tutorial: arena out of memory");
        return;
    }
    memset(level->tiles, TILE_EMPTY, (size_t)n);

    /* Floor: bottom 4 rows. */
    fill_rect(level, 0, level->height - 4, level->width, level->height, TILE_SOLID);

    /* Outer walls — left and right edges, full height (kept thin so the
     * camera doesn't slam against them when running near the edge). */
    fill_rect(level, 0,                level->height - 12, 2,                level->height - 4, TILE_SOLID);
    fill_rect(level, level->width - 2, level->height - 12, level->width,     level->height - 4, TILE_SOLID);

    /* A short platform near the player spawn so jets feel meaningful. */
    fill_rect(level, 12, level->height - 10, 22, level->height - 9, TILE_SOLID);

    /* A wall column at mid-map: the player can shoot around it but not
     * through it; useful to validate hitscan vs map. */
    fill_rect(level, 55, level->height - 9, 56, level->height - 4, TILE_SOLID);

    /* A higher platform on the right where the dummy stands (the floor
     * itself is fine for it; this gives some vertical interest). */
    fill_rect(level, 70, level->height - 8, 80, level->height - 7, TILE_SOLID);

    LOG_I("level: tutorial built %dx%d (tile=%dpx, %d solid)",
          level->width, level->height, level->tile_size, n);
}

TileKind level_tile_at(const Level *l, int tx, int ty) {
    if (tx < 0 || tx >= l->width || ty < 0 || ty >= l->height) return TILE_SOLID;
    return (TileKind)l->tiles[ty * l->width + tx];
}

int level_world_to_tile(const Level *l, float x) {
    return (int)(x / (float)l->tile_size);
}

bool level_point_solid(const Level *l, Vec2 p) {
    int tx = (int)(p.x / (float)l->tile_size);
    int ty = (int)(p.y / (float)l->tile_size);
    return level_tile_at(l, tx, ty) == TILE_SOLID;
}

float level_width_px (const Level *l) { return (float)(l->width  * l->tile_size); }
float level_height_px(const Level *l) { return (float)(l->height * l->tile_size); }

/* DDA traversal, classic Amanatides & Woo. We cap iterations so a
 * pathological diagonal can't loop forever. */
bool level_ray_hits(const Level *l, Vec2 a, Vec2 b, float *out_t) {
    float ts = (float)l->tile_size;
    float dx = b.x - a.x, dy = b.y - a.y;

    int tx = (int)(a.x / ts);
    int ty = (int)(a.y / ts);

    /* Already inside a solid? Treat as hit at t=0. */
    if (level_tile_at(l, tx, ty) == TILE_SOLID) {
        if (out_t) *out_t = 0.0f;
        return true;
    }

    int step_x = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
    int step_y = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);

    /* Distance along the ray to the first vertical/horizontal grid line. */
    float t_max_x = 1e30f, t_delta_x = 1e30f;
    float t_max_y = 1e30f, t_delta_y = 1e30f;
    if (dx != 0.0f) {
        float next_x = (step_x > 0)
            ? (float)(tx + 1) * ts
            : (float)tx * ts;
        t_max_x   = (next_x - a.x) / dx;
        t_delta_x = ts / (dx > 0 ? dx : -dx);
    }
    if (dy != 0.0f) {
        float next_y = (step_y > 0)
            ? (float)(ty + 1) * ts
            : (float)ty * ts;
        t_max_y   = (next_y - a.y) / dy;
        t_delta_y = ts / (dy > 0 ? dy : -dy);
    }

    for (int i = 0; i < 4096; ++i) {
        if (t_max_x < t_max_y) {
            tx += step_x;
            if (t_max_x > 1.0f) return false;
            if (level_tile_at(l, tx, ty) == TILE_SOLID) {
                if (out_t) *out_t = t_max_x;
                return true;
            }
            t_max_x += t_delta_x;
        } else {
            ty += step_y;
            if (t_max_y > 1.0f) return false;
            if (level_tile_at(l, tx, ty) == TILE_SOLID) {
                if (out_t) *out_t = t_max_y;
                return true;
            }
            t_max_y += t_delta_y;
        }
    }
    return false;
}
