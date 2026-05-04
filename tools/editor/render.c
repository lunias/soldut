#include "render.h"

#include "palette.h"

#include "stb_ds.h"

#include <stdint.h>

Color color_from_rgba(uint32_t rgba) {
    return (Color){
        (unsigned char)((rgba >> 24) & 0xff),
        (unsigned char)((rgba >> 16) & 0xff),
        (unsigned char)((rgba >>  8) & 0xff),
        (unsigned char)(rgba & 0xff),
    };
}

Color tile_color(LvlTile t) {
    if (t.flags & TILE_F_DEADLY)     return (Color){180,  60,  60, 255};
    if (t.flags & TILE_F_ICE)        return (Color){120, 200, 240, 255};
    if (t.flags & TILE_F_ONE_WAY)    return (Color){120, 200, 120, 200};
    if (t.flags & TILE_F_BACKGROUND) return (Color){ 80,  80,  90, 130};
    if (t.flags & TILE_F_SOLID)      return (Color){160, 160, 170, 255};
    return (Color){0, 0, 0, 0};
}

Color poly_color(uint16_t kind) {
    int n;
    const PolyKindEntry *kinds = palette_poly_kinds(&n);
    for (int i = 0; i < n; ++i) {
        if (kinds[i].kind == kind) return color_from_rgba(kinds[i].rgba);
    }
    return (Color){200, 200, 200, 200};
}

void draw_doc(const EditorDoc *d) {
    /* Tiles. */
    for (int y = 0; y < d->height; ++y) {
        for (int x = 0; x < d->width; ++x) {
            LvlTile t = d->tiles[y * d->width + x];
            Color c = tile_color(t);
            if (c.a == 0) continue;
            DrawRectangle(x * d->tile_size, y * d->tile_size,
                          d->tile_size, d->tile_size, c);
        }
    }
    /* Polygons (filled). */
    int pn = (int)arrlen(d->polys);
    for (int i = 0; i < pn; ++i) {
        const LvlPoly *p = &d->polys[i];
        Color c = poly_color(p->kind);
        Vector2 v0 = {(float)p->v_x[0], (float)p->v_y[0]};
        Vector2 v1 = {(float)p->v_x[1], (float)p->v_y[1]};
        Vector2 v2 = {(float)p->v_x[2], (float)p->v_y[2]};
        DrawTriangle(v0, v1, v2, c);
        DrawTriangleLines(v0, v1, v2, (Color){c.r/2u, c.g/2u, c.b/2u, 255});
    }
    /* Spawn points. */
    int sn = (int)arrlen(d->spawns);
    for (int i = 0; i < sn; ++i) {
        const LvlSpawn *s = &d->spawns[i];
        Color c =
            (s->team == 1) ? (Color){240, 100, 100, 255} :
            (s->team == 2) ? (Color){100, 160, 240, 255} :
                             (Color){200, 200, 200, 255};
        DrawCircle(s->pos_x, s->pos_y, 6.0f, c);
        DrawCircleLines(s->pos_x, s->pos_y, 16.0f, c);
    }
    /* Pickups. */
    int pkn = (int)arrlen(d->pickups);
    for (int i = 0; i < pkn; ++i) {
        const LvlPickup *p = &d->pickups[i];
        DrawCircle(p->pos_x, p->pos_y, 4.0f, (Color){255, 220, 100, 255});
        DrawCircleLines(p->pos_x, p->pos_y, 12.0f, (Color){255, 220, 100, 255});
    }
    /* Decorations. */
    int dn = (int)arrlen(d->decos);
    for (int i = 0; i < dn; ++i) {
        const LvlDeco *e = &d->decos[i];
        DrawRectangleLines(e->pos_x - 8, e->pos_y - 8, 16, 16,
                           (Color){255, 140, 240, 255});
    }
    /* Ambient zones. */
    int an = (int)arrlen(d->ambis);
    for (int i = 0; i < an; ++i) {
        const LvlAmbi *a = &d->ambis[i];
        Color c =
            (a->kind == AMBI_WIND)   ? (Color){ 80, 240, 120, 90} :
            (a->kind == AMBI_ZERO_G) ? (Color){180, 180, 240, 90} :
            (a->kind == AMBI_ACID)   ? (Color){240, 200,  80, 90} :
                                        (Color){180, 180, 180, 90};
        DrawRectangle(a->rect_x, a->rect_y, a->rect_w, a->rect_h, c);
        DrawRectangleLines(a->rect_x, a->rect_y, a->rect_w, a->rect_h,
                           (Color){c.r, c.g, c.b, 255});
    }
    /* Flag bases. */
    int fn = (int)arrlen(d->flags);
    for (int i = 0; i < fn; ++i) {
        const LvlFlag *f = &d->flags[i];
        Color c = (f->team == 1) ? (Color){240, 80, 80, 255}
                                 : (Color){100, 160, 240, 255};
        DrawRectangle(f->pos_x - 6, f->pos_y - 12, 12, 24, c);
        DrawCircle(f->pos_x, f->pos_y - 14, 6, c);
    }
}
