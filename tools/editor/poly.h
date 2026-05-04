#pragma once

/*
 * Polygon authoring math: ear-clipping triangulation + validation.
 *
 * The editor lets users draw arbitrary closed polygons; on close we run
 * Eberly's ear-clipping algorithm to produce a list of triangles that
 * the runtime stores as packed `LvlPoly` records. Each `LvlPoly` has
 * pre-baked Q1.15 edge normals; we compute those here too so the
 * runtime never normalizes at load.
 *
 * Reference:
 *   D. Eberly, "Triangulation by Ear Clipping" (geometrictools.com),
 *   2008. We don't ship the holes-via-bridges extension — the editor's
 *   polygon tool is single-loop only.
 */

#include "world.h"          /* for LvlPoly */

#include <stdbool.h>
#include <stdint.h>

#define POLY_MAX_VERTS  64

typedef struct EditorPolyVert {
    int x, y;            /* 4 px-snapped world space */
} EditorPolyVert;

typedef enum {
    POLY_VALID = 0,
    POLY_TOO_FEW_VERTS,
    POLY_EDGE_TOO_SHORT,
    POLY_SELF_INTERSECT,
    POLY_DEGENERATE,
} PolyValidate;

PolyValidate poly_validate(const EditorPolyVert *verts, int n);

/* Triangulate an n-vertex simple polygon. Caller passes a buffer of
 * (n - 2) `LvlPoly` records; returns the actual triangle count.
 *
 * `kind` is the POLY_KIND_* enum stored on every emitted triangle.
 * Edge normals are pre-baked in Q1.15. Returns -1 on failure
 * (degenerate / non-simple). */
int poly_triangulate(const EditorPolyVert *verts, int n,
                     uint16_t kind, LvlPoly *out_tris, int out_cap);

/* Signed area of the polygon (for winding detection). Negative means
 * clockwise in screen-space (raylib's Y points down, so a CCW polygon
 * in mathematical sense reads as CW in screen coords — we don't care
 * which we get; we flip if negative before triangulation). */
double poly_signed_area(const EditorPolyVert *verts, int n);
