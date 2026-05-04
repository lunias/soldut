#include "validate.h"

#include "stb_ds.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void emitf(char *buf, int cap, int *used, const char *fmt, ...) {
    if (*used >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(buf + *used, (size_t)(cap - *used), fmt, ap);
    va_end(ap);
    if (wrote > 0) {
        *used += wrote;
        if (*used < cap - 1) {
            buf[(*used)++] = '\n';
            buf[*used] = 0;
        }
    }
}

static int has_solid_at(const EditorDoc *d, int tx, int ty) {
    if (tx < 0 || tx >= d->width || ty < 0 || ty >= d->height) return 1; /* OOB = solid */
    return (d->tiles[ty * d->width + tx].flags & TILE_F_SOLID) != 0;
}

/* Count SOLID tiles within the px-radius around (cx, cy). */
static int count_solid_walls_near(const EditorDoc *d, int cx, int cy, int radius_px) {
    int T = d->tile_size;
    int tx0 = (cx - radius_px) / T - 1;
    int ty0 = (cy - radius_px) / T - 1;
    int tx1 = (cx + radius_px) / T + 1;
    int ty1 = (cy + radius_px) / T + 1;
    if (tx0 < 0) tx0 = 0;
    if (ty0 < 0) ty0 = 0;
    if (tx1 >= d->width)  tx1 = d->width - 1;
    if (ty1 >= d->height) ty1 = d->height - 1;
    int n = 0;
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            if (!(d->tiles[ty * d->width + tx].flags & TILE_F_SOLID)) continue;
            int cell_cx = tx * T + T/2;
            int cell_cy = ty * T + T/2;
            int dx = cell_cx - cx, dy = cell_cy - cy;
            if (dx*dx + dy*dy <= radius_px * radius_px) ++n;
        }
    }
    return n;
}

/* Walk empty tiles around a pickup centroid, return the bounding box of
 * the connected empty volume (capped at 8 tiles each axis). */
static void empty_bounding_tiles(const EditorDoc *d, int cx, int cy,
                                 int *out_w_tiles, int *out_h_tiles,
                                 int *out_min_clear) {
    int T = d->tile_size;
    int ctx = cx / T, cty = cy / T;
    if (ctx < 0 || ctx >= d->width || cty < 0 || cty >= d->height ||
        (d->tiles[cty * d->width + ctx].flags & TILE_F_SOLID)) {
        *out_w_tiles = 0; *out_h_tiles = 0; *out_min_clear = 0;
        return;
    }
    /* Scan outward in 4 directions. */
    int left = 0, right = 0, up = 0, down = 0;
    while (left  < 8 && !has_solid_at(d, ctx - left  - 1, cty)) ++left;
    while (right < 8 && !has_solid_at(d, ctx + right + 1, cty)) ++right;
    while (up    < 8 && !has_solid_at(d, ctx, cty - up   - 1)) ++up;
    while (down  < 8 && !has_solid_at(d, ctx, cty + down + 1)) ++down;
    *out_w_tiles = left + right + 1;
    *out_h_tiles = up   + down  + 1;
    int min_clear_px = T / 2;
    int lpx = (cx - ctx * T);
    int rpx = T - lpx;
    int upx = (cy - cty * T);
    int dpx = T - upx;
    if (left  > 0)        lpx += left  * T;
    if (right > 0)        rpx += right * T;
    if (up    > 0)        upx += up    * T;
    if (down  > 0)        dpx += down  * T;
    if (lpx < min_clear_px) min_clear_px = lpx;
    if (rpx < min_clear_px) min_clear_px = rpx;
    if (upx < min_clear_px) min_clear_px = upx;
    if (dpx < min_clear_px) min_clear_px = dpx;
    *out_min_clear = min_clear_px;
}

int validate_doc(const EditorDoc *d, char *out_buf, int out_cap) {
    int used = 0; int problems = 0;
    out_buf[0] = 0;

    if (d->width <= 0 || d->height <= 0) {
        emitf(out_buf, out_cap, &used, "Map dimensions are zero (%d × %d).",
              d->width, d->height);
        ++problems;
    }

    int spawn_n = (int)arrlen(d->spawns);
    if (spawn_n < 1) {
        emitf(out_buf, out_cap, &used, "At least 1 spawn point required (have 0).");
        ++problems;
    }

    int flag_n = (int)arrlen(d->flags);
    if (flag_n > 0) {
        int red_flag = 0, blue_flag = 0;
        int red_spawn = 0, blue_spawn = 0;
        for (int i = 0; i < flag_n; ++i) {
            if (d->flags[i].team == 1) ++red_flag;
            if (d->flags[i].team == 2) ++blue_flag;
        }
        for (int i = 0; i < spawn_n; ++i) {
            if (d->spawns[i].team == 1) ++red_spawn;
            if (d->spawns[i].team == 2) ++blue_spawn;
        }
        if (red_flag  > 0 && red_spawn  == 0) {
            emitf(out_buf, out_cap, &used,
                  "CTF: red flag base placed but no team-1 spawns exist.");
            ++problems;
        }
        if (blue_flag > 0 && blue_spawn == 0) {
            emitf(out_buf, out_cap, &used,
                  "CTF: blue flag base placed but no team-2 spawns exist.");
            ++problems;
        }
        if (red_flag > 1 || blue_flag > 1) {
            emitf(out_buf, out_cap, &used,
                  "CTF: more than 1 flag per team (%d red, %d blue).",
                  red_flag, blue_flag);
            ++problems;
        }
    }

    /* Polygon triangulate-cleanly check: each LvlPoly is already a
     * triangle; we re-validate that vertices are in-bounds and not
     * degenerate (zero area). Self-intersection isn't possible at
     * this layer (each is a single triangle). */
    int W_px = d->width  * d->tile_size;
    int H_px = d->height * d->tile_size;
    int poly_n = (int)arrlen(d->polys);
    int bad_polys = 0;
    for (int i = 0; i < poly_n; ++i) {
        const LvlPoly *p = &d->polys[i];
        for (int k = 0; k < 3; ++k) {
            if (p->v_x[k] < -64 || p->v_x[k] > W_px + 64 ||
                p->v_y[k] < -64 || p->v_y[k] > H_px + 64) {
                ++bad_polys; goto next_poly;
            }
        }
        long ax = p->v_x[1] - p->v_x[0], ay = p->v_y[1] - p->v_y[0];
        long bx = p->v_x[2] - p->v_x[0], by = p->v_y[2] - p->v_y[0];
        if (ax * by - ay * bx == 0) { ++bad_polys; }
    next_poly: ;
    }
    if (bad_polys > 0) {
        emitf(out_buf, out_cap, &used,
              "%d polygon(s) are degenerate or out of bounds.", bad_polys);
        ++problems;
    }

    /* Pickup spawners must not sit inside SOLID tiles. */
    int pn = (int)arrlen(d->pickups);
    for (int i = 0; i < pn; ++i) {
        const LvlPickup *p = &d->pickups[i];
        int tx = p->pos_x / d->tile_size;
        int ty = p->pos_y / d->tile_size;
        if (has_solid_at(d, tx, ty)) {
            emitf(out_buf, out_cap, &used,
                  "Pickup #%d at (%d,%d) sits inside a SOLID tile.",
                  i, p->pos_x, p->pos_y);
            ++problems;
        }
    }

    /* META display name. */
    const char *name = doc_str_lookup(d, d->meta.name_str_idx);
    if (!name || !name[0]) {
        emitf(out_buf, out_cap, &used,
              "META: display name is empty.");
        ++problems;
    }

    /* Alcove sizing — for each pickup whose neighborhood is enclosed
     * (≥3 SOLID tiles within 96 px), enforce the minimum interior
     * volume from documents/m5/07-maps.md. */
    for (int i = 0; i < pn; ++i) {
        const LvlPickup *p = &d->pickups[i];
        int tx = p->pos_x / d->tile_size;
        int ty = p->pos_y / d->tile_size;
        if (has_solid_at(d, tx, ty)) continue;       /* already counted above */
        int walls = count_solid_walls_near(d, p->pos_x, p->pos_y, 96);
        if (walls < 3) continue;                      /* not enclosed */
        int W, H, min_clear;
        empty_bounding_tiles(d, p->pos_x, p->pos_y, &W, &H, &min_clear);
        if (H < 3 || W < 2 || min_clear < 16) {
            emitf(out_buf, out_cap, &used,
                  "Pickup #%d is in an enclosed alcove %dx%d tiles "
                  "(min clearance %d px) — needs ≥2x3 with ≥16 px.",
                  i, W, H, min_clear);
            ++problems;
        }
    }

    return problems;
}
