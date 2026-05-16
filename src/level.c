#include "level.h"
#include "arena.h"
#include "level_io.h"
#include "log.h"

#include <math.h>
#include <string.h>

#define TILE_SIZE_PX  32

static void set_tile(Level *l, int x, int y, TileKind k) {
    if (x < 0 || x >= l->width || y < 0 || y >= l->height) return;
    uint16_t f = (k == TILE_SOLID) ? TILE_F_SOLID : TILE_F_EMPTY;
    l->tiles[y * l->width + x] = (LvlTile){ .id = 0, .flags = f };
}

static void fill_rect(Level *l, int x0, int y0, int x1, int y1, TileKind k) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            set_tile(l, x, y, k);
}

/* P02 — pre-bake an edge-normal triangle. Vertex order is screen-
 * clockwise (in screen coords y-down, v0→v1→v2 turns clockwise), so
 * (b-a)'s right-perpendicular (dy, -dx) gives the outward normal —
 * matching the runtime push-out convention in physics.c. */
static void set_tri(LvlPoly *q, PolyKind kind,
                    int x0, int y0, int x1, int y1, int x2, int y2)
{
    q->v_x[0] = (int16_t)x0; q->v_y[0] = (int16_t)y0;
    q->v_x[1] = (int16_t)x1; q->v_y[1] = (int16_t)y1;
    q->v_x[2] = (int16_t)x2; q->v_y[2] = (int16_t)y2;
    q->kind   = (uint16_t)kind;
    for (int e = 0; e < 3; ++e) {
        int a = e, b = (e + 1) % 3;
        float dx = (float)(q->v_x[b] - q->v_x[a]);
        float dy = (float)(q->v_y[b] - q->v_y[a]);
        float nx =  dy;
        float ny = -dx;
        float L  = sqrtf(nx * nx + ny * ny);
        if (L > 1e-4f) { nx /= L; ny /= L; }
        else           { nx = 0.0f; ny = -1.0f; }
        int qx = (int)(nx * 32767.0f);
        int qy = (int)(ny * 32767.0f);
        if (qx >  32767) qx =  32767;
        if (qx < -32768) qx = -32768;
        if (qy >  32767) qy =  32767;
        if (qy < -32768) qy = -32768;
        q->normal_x[e] = (int16_t)qx;
        q->normal_y[e] = (int16_t)qy;
    }
}

void level_build_tutorial(Level *level, Arena *arena) {
    /* 100×40 tile arena = 3200×1280 px world. Comfortable for the M1
     * loop: enough room to run, jet, miss with the rifle, and find your
     * way back to the dummy.
     *
     * Zero the Level first — matches level_load + map_alloc_tiles so
     * stale counts/pointers from a prior build don't leak into this
     * one. See map_alloc_tiles in src/maps.c for the long form of the
     * cross-build leak this prevents. */
    memset(level, 0, sizeof *level);
    level->width     = 100;
    level->height    = 40;
    level->tile_size = TILE_SIZE_PX;
    level->gravity   = (Vec2){0.0f, 1080.0f};   /* px/s^2 */

    int n = level->width * level->height;
    level->tiles = (LvlTile *)arena_alloc(arena, sizeof(LvlTile) * (size_t)n);
    if (!level->tiles) {
        LOG_E("level_build_tutorial: arena out of memory");
        return;
    }
    memset(level->tiles, 0, sizeof(LvlTile) * (size_t)n);

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

    /* P02 slope test bed — three slopes side by side at floor level for
     * shot tests (tests/shots/m5_slope.shot). The headless harness
     * + shotmode both use level_build_tutorial; in-match play uses
     * src/maps.c::build_foundry which doesn't carry these. Goes away
     * once authored .lvl maps land at P17. */
    {
        const int floor_y = (level->height - 4) * level->tile_size; /* 1152 */
        level->poly_count = 3;
        level->polys = (LvlPoly *)arena_alloc(arena,
                          sizeof(LvlPoly) * (size_t)level->poly_count);
        if (level->polys) {
            memset(level->polys, 0, sizeof(LvlPoly) * (size_t)level->poly_count);
            /* 45° slope (hypotenuse from upper-left to lower-right). */
            set_tri(&level->polys[0], POLY_KIND_SOLID,
                    1500, floor_y - 128,
                    1628, floor_y,
                    1500, floor_y);
            /* 60° slope. */
            set_tri(&level->polys[1], POLY_KIND_SOLID,
                    1700, floor_y - 128,
                    1774, floor_y,
                    1700, floor_y);
            /* 5° slope (shallow ramp). */
            set_tri(&level->polys[2], POLY_KIND_SOLID,
                    1850, floor_y - 12,
                    1987, floor_y,
                    1850, floor_y);
            level_build_poly_broadphase(level, arena);
        } else {
            level->poly_count = 0;
        }
    }

    LOG_I("level: tutorial built %dx%d (tile=%dpx, %d cells, %d polys)",
          level->width, level->height, level->tile_size, n,
          level->poly_count);
}

TileKind level_tile_at(const Level *l, int tx, int ty) {
    if (tx < 0 || tx >= l->width || ty < 0 || ty >= l->height) return TILE_SOLID;
    return (l->tiles[ty * l->width + tx].flags & TILE_F_SOLID) ? TILE_SOLID : TILE_EMPTY;
}

uint16_t level_flags_at(const Level *l, int tx, int ty) {
    if (tx < 0 || tx >= l->width || ty < 0 || ty >= l->height) return TILE_F_SOLID;
    return l->tiles[ty * l->width + tx].flags;
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
 * pathological diagonal can't loop forever.
 *
 * M6 P09 — `allow_oneway_up` (passed from level_ray_hits_kinematic)
 * lets a particle move UPWARD through a TILE_F_ONE_WAY tile without
 * the integrate-time clamp catching it. Without this gate,
 * physics_integrate clamps a jet'ing particle to just below an
 * ONE_WAY platform, then constraint relaxation pushes it INTO the
 * platform — the foot ends up sandwiched. Mirrors the poly-side
 * "came from passable side" check at src/physics.c:717. The
 * TILE_F_BACKGROUND case is uniformly non-blocking (decorative). */
static bool level_ray_hits_tile_filtered(const Level *l, Vec2 a, Vec2 b,
                                         bool allow_oneway_up, float *out_t) {
    float ts = (float)l->tile_size;
    float dx = b.x - a.x, dy = b.y - a.y;
    bool moving_up = (dy < -0.001f);

    int tx = (int)(a.x / ts);
    int ty = (int)(a.y / ts);

    /* Check the start tile; treat ONE_WAY as non-solid when we're
     * already inside and moving up (jet from below) — the collide
     * pass will gate appropriately. BACKGROUND is always non-solid. */
    uint16_t start_f = level_flags_at(l, tx, ty);
    if ((start_f & TILE_F_SOLID) && !(start_f & TILE_F_BACKGROUND) &&
        !(allow_oneway_up && moving_up && (start_f & TILE_F_ONE_WAY)))
    {
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
            uint16_t f = level_flags_at(l, tx, ty);
            if ((f & TILE_F_SOLID) && !(f & TILE_F_BACKGROUND) &&
                !(allow_oneway_up && moving_up && (f & TILE_F_ONE_WAY)))
            {
                if (out_t) *out_t = t_max_x;
                return true;
            }
            t_max_x += t_delta_x;
        } else {
            ty += step_y;
            if (t_max_y > 1.0f) return false;
            uint16_t f = level_flags_at(l, tx, ty);
            if ((f & TILE_F_SOLID) && !(f & TILE_F_BACKGROUND) &&
                !(allow_oneway_up && moving_up && (f & TILE_F_ONE_WAY)))
            {
                if (out_t) *out_t = t_max_y;
                return true;
            }
            t_max_y += t_delta_y;
        }
    }
    return false;
}

/* Wrapper kept for back-compat; uses the conservative (treats ONE_WAY
 * as solid) behavior so non-kinematic callers like grapple rope sweep
 * and line-of-sight checks still see all solid tiles. */
static bool level_ray_hits_tile(const Level *l, Vec2 a, Vec2 b, float *out_t) {
    return level_ray_hits_tile_filtered(l, a, b, /*allow_oneway_up*/ false, out_t);
}

/* Segment-vs-segment intersection in 2D. Returns true if segments
 * (a, b) and (c, d) cross at a point inside both; writes the
 * parametric t along (a, b) to *out_t. */
static bool seg_seg_intersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d, float *out_t) {
    float rx = b.x - a.x, ry = b.y - a.y;
    float sx = d.x - c.x, sy = d.y - c.y;
    float denom = rx * sy - ry * sx;
    if (denom > -1e-6f && denom < 1e-6f) return false;   /* parallel */
    float qpx = c.x - a.x, qpy = c.y - a.y;
    float t = (qpx * sy - qpy * sx) / denom;
    float u = (qpx * ry - qpy * rx) / denom;
    if (t < 0.0f || t > 1.0f) return false;
    if (u < 0.0f || u > 1.0f) return false;
    *out_t = t;
    return true;
}

/* Walk every non-BACKGROUND polygon and test the ray against each of
 * its three edges. Returns the smallest hit `t`. At <5000 polys per
 * map this is ~15000 segment tests per ray, well under 0.05 ms in
 * playtest. The polygon broadphase grid is a future optimization if
 * profiling shows pressure. */
static bool level_ray_hits_poly(const Level *l, Vec2 a, Vec2 b, float *out_t) {
    if (l->poly_count == 0) return false;
    float t_min = 1.0f;
    bool  hit   = false;
    for (int i = 0; i < l->poly_count; ++i) {
        const LvlPoly *poly = &l->polys[i];
        if ((PolyKind)poly->kind == POLY_KIND_BACKGROUND) continue;
        Vec2 v0 = { (float)poly->v_x[0], (float)poly->v_y[0] };
        Vec2 v1 = { (float)poly->v_x[1], (float)poly->v_y[1] };
        Vec2 v2 = { (float)poly->v_x[2], (float)poly->v_y[2] };
        float t;
        if (seg_seg_intersect(a, b, v0, v1, &t) && t < t_min) { t_min = t; hit = true; }
        if (seg_seg_intersect(a, b, v1, v2, &t) && t < t_min) { t_min = t; hit = true; }
        if (seg_seg_intersect(a, b, v2, v0, &t) && t < t_min) { t_min = t; hit = true; }
    }
    if (hit && out_t) *out_t = t_min;
    return hit;
}

bool level_ray_hits(const Level *l, Vec2 a, Vec2 b, float *out_t) {
    float t_tile = 1.0f, t_poly = 1.0f;
    bool tile_hit = level_ray_hits_tile(l, a, b, &t_tile);
    bool poly_hit = level_ray_hits_poly(l, a, b, &t_poly);
    if (!tile_hit && !poly_hit) return false;
    float t = (tile_hit && poly_hit) ? (t_tile < t_poly ? t_tile : t_poly)
            :  tile_hit              ? t_tile
                                     : t_poly;
    if (out_t) *out_t = t;
    return true;
}

/* DDA tile test that returns BOTH t AND the cardinal normal at the
 * hit face. Mirrors level_ray_hits_tile_filtered (no ONE_WAY-up
 * filtering — bouncy projectiles treat ONE_WAY as solid in both
 * directions). The normal is the axis-aligned face we crossed,
 * pointing back toward `a`. */
static bool level_ray_hits_tile_with_normal(const Level *l, Vec2 a, Vec2 b,
                                            float *out_t,
                                            float *out_nx, float *out_ny) {
    float ts = (float)l->tile_size;
    float dx = b.x - a.x, dy = b.y - a.y;

    int tx = (int)(a.x / ts);
    int ty = (int)(a.y / ts);

    /* Already inside a solid tile at start — output an approximate
     * normal pointing back along the ray (best-effort). */
    uint16_t start_f = level_flags_at(l, tx, ty);
    if ((start_f & TILE_F_SOLID) && !(start_f & TILE_F_BACKGROUND)) {
        if (out_t) *out_t = 0.0f;
        float mag = sqrtf(dx*dx + dy*dy);
        if (mag > 1e-4f) {
            if (out_nx) *out_nx = -dx / mag;
            if (out_ny) *out_ny = -dy / mag;
        } else {
            if (out_nx) *out_nx = 0.0f;
            if (out_ny) *out_ny = -1.0f;
        }
        return true;
    }

    int step_x = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
    int step_y = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);

    float t_max_x = 1e30f, t_delta_x = 1e30f;
    float t_max_y = 1e30f, t_delta_y = 1e30f;
    if (dx != 0.0f) {
        float next_x = (step_x > 0) ? (float)(tx + 1) * ts : (float)tx * ts;
        t_max_x   = (next_x - a.x) / dx;
        t_delta_x = ts / (dx > 0 ? dx : -dx);
    }
    if (dy != 0.0f) {
        float next_y = (step_y > 0) ? (float)(ty + 1) * ts : (float)ty * ts;
        t_max_y   = (next_y - a.y) / dy;
        t_delta_y = ts / (dy > 0 ? dy : -dy);
    }

    for (int i = 0; i < 4096; ++i) {
        if (t_max_x < t_max_y) {
            tx += step_x;
            if (t_max_x > 1.0f) return false;
            uint16_t f = level_flags_at(l, tx, ty);
            if ((f & TILE_F_SOLID) && !(f & TILE_F_BACKGROUND)) {
                if (out_t)  *out_t  = t_max_x;
                /* X-face hit — normal opposes step_x (points back to
                 * the tile we came from along x). */
                if (out_nx) *out_nx = (float)(-step_x);
                if (out_ny) *out_ny = 0.0f;
                return true;
            }
            t_max_x += t_delta_x;
        } else {
            ty += step_y;
            if (t_max_y > 1.0f) return false;
            uint16_t f = level_flags_at(l, tx, ty);
            if ((f & TILE_F_SOLID) && !(f & TILE_F_BACKGROUND)) {
                if (out_t)  *out_t  = t_max_y;
                if (out_nx) *out_nx = 0.0f;
                if (out_ny) *out_ny = (float)(-step_y);
                return true;
            }
            t_max_y += t_delta_y;
        }
    }
    return false;
}

/* Same as level_ray_hits_poly but ALSO writes the unit-vector normal
 * of the closest hit edge (pointing AWAY from the polygon interior,
 * using the precomputed normal_x/normal_y arrays). */
static bool level_ray_hits_poly_with_normal(const Level *l, Vec2 a, Vec2 b,
                                            float *out_t,
                                            float *out_nx, float *out_ny) {
    if (l->poly_count == 0) return false;
    float t_min   = 1.0f;
    float hit_nx  = 0.0f;
    float hit_ny  = -1.0f;
    bool  hit     = false;
    for (int i = 0; i < l->poly_count; ++i) {
        const LvlPoly *poly = &l->polys[i];
        if ((PolyKind)poly->kind == POLY_KIND_BACKGROUND) continue;
        Vec2 v0 = { (float)poly->v_x[0], (float)poly->v_y[0] };
        Vec2 v1 = { (float)poly->v_x[1], (float)poly->v_y[1] };
        Vec2 v2 = { (float)poly->v_x[2], (float)poly->v_y[2] };
        float t;
        if (seg_seg_intersect(a, b, v0, v1, &t) && t < t_min) {
            t_min  = t; hit = true;
            hit_nx = poly->normal_x[0] / 32767.0f;
            hit_ny = poly->normal_y[0] / 32767.0f;
        }
        if (seg_seg_intersect(a, b, v1, v2, &t) && t < t_min) {
            t_min  = t; hit = true;
            hit_nx = poly->normal_x[1] / 32767.0f;
            hit_ny = poly->normal_y[1] / 32767.0f;
        }
        if (seg_seg_intersect(a, b, v2, v0, &t) && t < t_min) {
            t_min  = t; hit = true;
            hit_nx = poly->normal_x[2] / 32767.0f;
            hit_ny = poly->normal_y[2] / 32767.0f;
        }
    }
    if (hit) {
        if (out_t)  *out_t  = t_min;
        if (out_nx) *out_nx = hit_nx;
        if (out_ny) *out_ny = hit_ny;
    }
    return hit;
}

bool level_ray_hits_normal(const Level *l, Vec2 a, Vec2 b,
                           float *out_t, float *out_nx, float *out_ny) {
    float t_tile = 1.0f, t_poly = 1.0f;
    float nx_tile = 0.0f, ny_tile = -1.0f;
    float nx_poly = 0.0f, ny_poly = -1.0f;
    bool tile_hit = level_ray_hits_tile_with_normal(l, a, b,
                                                     &t_tile, &nx_tile, &ny_tile);
    bool poly_hit = level_ray_hits_poly_with_normal(l, a, b,
                                                     &t_poly, &nx_poly, &ny_poly);
    if (!tile_hit && !poly_hit) return false;
    bool use_tile = tile_hit && (!poly_hit || t_tile < t_poly);
    if (out_t)  *out_t  = use_tile ? t_tile  : t_poly;
    if (out_nx) *out_nx = use_tile ? nx_tile : nx_poly;
    if (out_ny) *out_ny = use_tile ? ny_tile : ny_poly;
    return true;
}

/* M6 P09 — kinematic variant. Treats TILE_F_ONE_WAY as non-blocking
 * when the ray is moving UP (dy < 0) so a jetting particle isn't
 * pre-clamped just below the platform by the integrate sweep. The
 * poly side already supports this — POLY_KIND_ONE_WAY's `dprev > 0`
 * check in collide_polys_one_pass is the runtime gate; physics_integrate
 * never pre-clamps for polys because level_ray_hits_poly always returns
 * the geometric hit (the runtime sorts by direction later). The tile
 * branch needs this dedicated entry so jet-through-ONE_WAY works on
 * tile-flag ONE_WAY just as well as it works on poly ONE_WAY. */
bool level_ray_hits_kinematic(const Level *l, Vec2 a, Vec2 b, float *out_t) {
    float t_tile = 1.0f, t_poly = 1.0f;
    bool tile_hit = level_ray_hits_tile_filtered(
        l, a, b, /*allow_oneway_up*/ true, &t_tile);
    bool poly_hit = level_ray_hits_poly(l, a, b, &t_poly);
    if (!tile_hit && !poly_hit) return false;
    float t = (tile_hit && poly_hit) ? (t_tile < t_poly ? t_tile : t_poly)
            :  tile_hit              ? t_tile
                                     : t_poly;
    if (out_t) *out_t = t;
    return true;
}
