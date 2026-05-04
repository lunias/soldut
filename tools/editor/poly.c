#include "poly.h"

#include "log.h"

#include <math.h>
#include <string.h>

#define POLY_MIN_EDGE_PX  8

/* Signed area of a polygon in screen-space (Y-down). Positive ⇒ CW
 * visually, negative ⇒ CCW visually. */
double poly_signed_area(const EditorPolyVert *v, int n) {
    double a = 0.0;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        a += (double)v[i].x * (double)v[j].y - (double)v[j].x * (double)v[i].y;
    }
    return a * 0.5;
}

static int seg_intersect(int ax, int ay, int bx, int by,
                         int cx, int cy, int dx, int dy) {
    /* Test (a,b) vs (c,d). Endpoints touching are NOT counted as
     * intersection (the polygon edges share endpoints with neighbors).
     */
    long d1x = bx - ax, d1y = by - ay;
    long d2x = dx - cx, d2y = dy - cy;
    long denom = d1x * d2y - d1y * d2x;
    if (denom == 0) return 0;            /* parallel/colinear; reject parity */
    long t_num = (cx - ax) * d2y - (cy - ay) * d2x;
    long u_num = (cx - ax) * d1y - (cy - ay) * d1x;
    /* 0 < t < 1 strict, 0 < u < 1 strict — endpoints excluded. */
    if (denom > 0) {
        return (t_num > 0 && t_num < denom && u_num > 0 && u_num < denom);
    } else {
        return (t_num < 0 && t_num > denom && u_num < 0 && u_num > denom);
    }
}

PolyValidate poly_validate(const EditorPolyVert *v, int n) {
    if (n < 3) return POLY_TOO_FEW_VERTS;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        long dx = v[j].x - v[i].x, dy = v[j].y - v[i].y;
        if (dx * dx + dy * dy < (long)(POLY_MIN_EDGE_PX * POLY_MIN_EDGE_PX)) {
            return POLY_EDGE_TOO_SHORT;
        }
    }
    /* Self-intersection: every non-adjacent edge pair. */
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        for (int k = i + 2; k < n; ++k) {
            int l = (k + 1) % n;
            if (l == i) continue;        /* adjacent (wraparound) */
            if (seg_intersect(v[i].x, v[i].y, v[j].x, v[j].y,
                              v[k].x, v[k].y, v[l].x, v[l].y)) {
                return POLY_SELF_INTERSECT;
            }
        }
    }
    if (fabs(poly_signed_area(v, n)) < 1.0) return POLY_DEGENERATE;
    return POLY_VALID;
}

/* Cross product of (b - a) × (c - b). */
static long cross_z(EditorPolyVert a, EditorPolyVert b, EditorPolyVert c) {
    long abx = b.x - a.x, aby = b.y - a.y;
    long bcx = c.x - b.x, bcy = c.y - b.y;
    return abx * bcy - aby * bcx;
}

/* Is point p inside triangle (a, b, c)? Uses three signed-area tests
 * (handles both windings). */
static int point_in_tri(EditorPolyVert p,
                        EditorPolyVert a, EditorPolyVert b, EditorPolyVert c) {
    long s0 = cross_z(a, b, p);
    long s1 = cross_z(b, c, p);
    long s2 = cross_z(c, a, p);
    int has_neg = (s0 < 0) || (s1 < 0) || (s2 < 0);
    int has_pos = (s0 > 0) || (s1 > 0) || (s2 > 0);
    return !(has_neg && has_pos);        /* all same sign or zero ⇒ inside or on edge */
}

/* Is vertex `b` an ear of the polygon (with prev/next chain)?
 *
 * CW polygon (positive signed area in screen-space) has convex vertices
 * with cross_z > 0. We assume the caller flipped the polygon to CW
 * before calling. */
static int is_ear(const EditorPolyVert *v, const int *prev, const int *next,
                  int n, int b) {
    int a = prev[b], c = next[b];
    if (cross_z(v[a], v[b], v[c]) <= 0) return 0;     /* concave or colinear */
    /* Check no other live vertex is inside (a, b, c). */
    for (int k = next[c]; k != a; k = next[k]) {
        if (point_in_tri(v[k], v[a], v[b], v[c])) return 0;
    }
    return 1;
}

static void bake_edge_normals(LvlPoly *t) {
    /* For a CW triangle in screen-space, the outward normal of edge a→b
     * is (by - ay, -(bx - ax)) normalized. Q1.15: 32767 = +1.0. */
    for (int k = 0; k < 3; ++k) {
        int j = (k + 1) % 3;
        double ex = t->v_x[j] - t->v_x[k];
        double ey = t->v_y[j] - t->v_y[k];
        double nx =  ey;
        double ny = -ex;
        double len = sqrt(nx * nx + ny * ny);
        if (len < 1e-6) { nx = 0; ny = -1; len = 1; }
        nx /= len; ny /= len;
        t->normal_x[k] = (int16_t)(nx * 32767.0);
        t->normal_y[k] = (int16_t)(ny * 32767.0);
    }
}

int poly_triangulate(const EditorPolyVert *verts, int n,
                     uint16_t kind, LvlPoly *out_tris, int out_cap) {
    if (n < 3) return -1;
    if (n - 2 > out_cap) return -1;
    if (n > POLY_MAX_VERTS) return -1;

    /* Local copy, flipped to CW (positive signed area in screen-space)
     * if needed. */
    EditorPolyVert v[POLY_MAX_VERTS];
    if (poly_signed_area(verts, n) >= 0) {
        memcpy(v, verts, sizeof(EditorPolyVert) * (size_t)n);
    } else {
        for (int i = 0; i < n; ++i) v[i] = verts[n - 1 - i];
    }

    int prev[POLY_MAX_VERTS], next[POLY_MAX_VERTS];
    for (int i = 0; i < n; ++i) {
        prev[i] = (i - 1 + n) % n;
        next[i] = (i + 1) % n;
    }
    int alive = n;
    int cur = 0;
    int guard = 0;
    int max_guard = n * n + 8;        /* safety: O(n²) total work expected */
    int out_count = 0;

    while (alive > 3) {
        if (guard++ > max_guard) {
            LOG_W("poly_triangulate: bailed after %d iters; out_count=%d alive=%d",
                  guard, out_count, alive);
            return -1;
        }
        if (is_ear(v, prev, next, n, cur)) {
            int a = prev[cur], c = next[cur];
            LvlPoly *t = &out_tris[out_count++];
            memset(t, 0, sizeof(*t));
            t->v_x[0] = (int16_t)v[a].x;   t->v_y[0] = (int16_t)v[a].y;
            t->v_x[1] = (int16_t)v[cur].x; t->v_y[1] = (int16_t)v[cur].y;
            t->v_x[2] = (int16_t)v[c].x;   t->v_y[2] = (int16_t)v[c].y;
            t->kind   = kind;
            bake_edge_normals(t);

            /* Splice cur out. */
            next[a] = c; prev[c] = a;
            alive--;
            cur = a;
        } else {
            cur = next[cur];
        }
    }

    /* Final triangle. */
    if (alive == 3) {
        int a = prev[cur], c = next[cur];
        LvlPoly *t = &out_tris[out_count++];
        memset(t, 0, sizeof(*t));
        t->v_x[0] = (int16_t)v[a].x;   t->v_y[0] = (int16_t)v[a].y;
        t->v_x[1] = (int16_t)v[cur].x; t->v_y[1] = (int16_t)v[cur].y;
        t->v_x[2] = (int16_t)v[c].x;   t->v_y[2] = (int16_t)v[c].y;
        t->kind   = kind;
        bake_edge_normals(t);
    }
    return out_count;
}
