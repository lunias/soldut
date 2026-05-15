#include "placement_validate.h"

#include "world.h"

#include <stdbool.h>
#include <stddef.h>

const char *placement_issue_kind_str(PlacementIssueKind k) {
    switch (k) {
        case PLACEMENT_OK:            return "ok";
        case PLACEMENT_OUT_OF_BOUNDS: return "out_of_bounds";
        case PLACEMENT_IN_SOLID_TILE: return "in_solid_tile";
        case PLACEMENT_IN_SOLID_POLY: return "in_solid_poly";
    }
    return "unknown";
}

const char *placement_entity_kind_str(PlacementEntityKind e) {
    switch (e) {
        case PLACEMENT_ENTITY_PICKUP: return "pickup";
        case PLACEMENT_ENTITY_SPAWN:  return "spawn";
        case PLACEMENT_ENTITY_FLAG:   return "flag";
    }
    return "unknown";
}

static bool tile_is_solid(const Level *L, int wx, int wy) {
    if (L->tile_size <= 0) return false;
    int tx = wx / L->tile_size;
    int ty = wy / L->tile_size;
    if (tx < 0 || ty < 0 || tx >= L->width || ty >= L->height) return false;
    if (!L->tiles) return false;
    return (L->tiles[ty * L->width + tx].flags & TILE_F_SOLID) != 0;
}

static bool poly_kind_blocking(uint16_t kind) {
    /* Same vocabulary the runtime collision solver uses to reject body
     * intrusion: solid push-back, ice push-back (slippery surface),
     * deadly push-back (lethal). One-way is drop-through; background
     * is decoration. */
    switch ((PolyKind)kind) {
        case POLY_KIND_SOLID:
        case POLY_KIND_ICE:
        case POLY_KIND_DEADLY:
            return true;
        case POLY_KIND_ONE_WAY:
        case POLY_KIND_BACKGROUND:
            return false;
    }
    return false;
}

/* Point-in-triangle via sign-of-cross. Triangles are emitted by cook_
 * maps in screen-clockwise winding; signs are consistent across edges
 * for an interior point. Use the all-same-sign test so degenerate
 * winding still classifies points cleanly. 64-bit intermediate avoids
 * overflow for our int16 coordinate range. */
static bool point_in_tri(int px, int py,
                         int ax, int ay,
                         int bx, int by,
                         int cx, int cy) {
    long long e0 = (long long)(bx - ax) * (py - ay) -
                   (long long)(by - ay) * (px - ax);
    long long e1 = (long long)(cx - bx) * (py - by) -
                   (long long)(cy - by) * (px - bx);
    long long e2 = (long long)(ax - cx) * (py - cy) -
                   (long long)(ay - cy) * (px - cx);
    return (e0 >= 0 && e1 >= 0 && e2 >= 0) ||
           (e0 <= 0 && e1 <= 0 && e2 <= 0);
}

static int point_in_blocking_poly(const Level *L, int wx, int wy) {
    if (!L->polys) return -1;
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *p = &L->polys[i];
        if (!poly_kind_blocking(p->kind)) continue;
        if (point_in_tri(wx, wy,
                         p->v_x[0], p->v_y[0],
                         p->v_x[1], p->v_y[1],
                         p->v_x[2], p->v_y[2])) {
            return i;
        }
    }
    return -1;
}

static void check_point(const Level *L, int x, int y,
                        PlacementEntityKind entity, int index,
                        PlacementIssue *out, int *count, int max) {
    if (*count >= max) return;

    int wmax = L->width  * L->tile_size;
    int hmax = L->height * L->tile_size;
    if (x < 0 || y < 0 || x >= wmax || y >= hmax) {
        out[(*count)++] = (PlacementIssue){
            .kind = PLACEMENT_OUT_OF_BOUNDS,
            .entity = entity, .index = index, .x = x, .y = y, .detail = 0,
        };
        return;
    }

    if (tile_is_solid(L, x, y)) {
        int tx = x / L->tile_size;
        int ty = y / L->tile_size;
        out[(*count)++] = (PlacementIssue){
            .kind = PLACEMENT_IN_SOLID_TILE,
            .entity = entity, .index = index, .x = x, .y = y,
            .detail = ty * L->width + tx,
        };
        return;
    }

    int poly_idx = point_in_blocking_poly(L, x, y);
    if (poly_idx >= 0) {
        out[(*count)++] = (PlacementIssue){
            .kind = PLACEMENT_IN_SOLID_POLY,
            .entity = entity, .index = index, .x = x, .y = y,
            .detail = poly_idx,
        };
    }
}

int placement_validate(const Level *L, PlacementIssue *issues, int max) {
    int count = 0;
    if (!L) return 0;

    for (int i = 0; i < L->pickup_count; ++i) {
        const LvlPickup *p = &L->pickups[i];
        check_point(L, p->pos_x, p->pos_y,
                    PLACEMENT_ENTITY_PICKUP, i, issues, &count, max);
    }
    for (int i = 0; i < L->spawn_count; ++i) {
        const LvlSpawn *s = &L->spawns[i];
        check_point(L, s->pos_x, s->pos_y,
                    PLACEMENT_ENTITY_SPAWN, i, issues, &count, max);
    }
    for (int i = 0; i < L->flag_count; ++i) {
        const LvlFlag *f = &L->flags[i];
        check_point(L, f->pos_x, f->pos_y,
                    PLACEMENT_ENTITY_FLAG, i, issues, &count, max);
    }
    return count;
}
