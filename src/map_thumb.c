#include "map_thumb.h"

#include "world.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdlib.h>
#include <string.h>

/* Render a 256×144 top-down sketch of the level. raylib's Image API
 * is software-only (no GL context), so this is safe to call from
 * standalone tools that never open a window.
 *
 * Quality bar is "designer can identify the map from the thumb" — not
 * "pretty." We don't have triangle fill in the Image API, so polygons
 * render as wireframe outlines; spawns / flags / pickups land as
 * coloured dots. Same scheme as the M5 P18 cook_maps render that we
 * factored this out of. */
static Image render_thumb_image(const Level *L) {
    Image img = GenImageColor(MAP_THUMB_W, MAP_THUMB_H,
                              (Color){20, 24, 32, 255});

    if (!L || L->width <= 0 || L->height <= 0 || L->tile_size <= 0) {
        return img;
    }

    float wpx = (float)(L->width  * L->tile_size);
    float hpx = (float)(L->height * L->tile_size);
    if (wpx <= 0.0f || hpx <= 0.0f) return img;
    float sx = (float)MAP_THUMB_W / wpx;
    float sy = (float)MAP_THUMB_H / hpx;

    /* Tiles — solid as 90/100/110, ICE as pale blue, DEADLY as red. */
    if (L->tiles) {
        for (int ty = 0; ty < L->height; ++ty) {
            for (int tx = 0; tx < L->width; ++tx) {
                uint16_t f = L->tiles[ty * L->width + tx].flags;
                if (!(f & TILE_F_SOLID) && !(f & TILE_F_DEADLY)) continue;
                Color c = (Color){90, 100, 110, 255};
                if (f & TILE_F_ICE)    c = (Color){180, 220, 240, 255};
                if (f & TILE_F_DEADLY) c = (Color){200, 80, 60, 255};
                int x0 = (int)(tx        * L->tile_size * sx);
                int y0 = (int)(ty        * L->tile_size * sy);
                int x1 = (int)((tx + 1)  * L->tile_size * sx);
                int y1 = (int)((ty + 1)  * L->tile_size * sy);
                if (x1 - x0 < 1) x1 = x0 + 1;
                if (y1 - y0 < 1) y1 = y0 + 1;
                ImageDrawRectangle(&img, x0, y0, x1 - x0, y1 - y0, c);
            }
        }
    }

    /* Polygons — wireframe outlines. raylib's Image API doesn't ship a
     * triangle fill, so the designer-readable thumb is outline +
     * label-by-color. Per-kind palette mirrors the M5 spec colors. */
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *p = &L->polys[i];
        Color c;
        switch (p->kind) {
            case POLY_KIND_SOLID:      c = (Color){120, 130, 140, 255}; break;
            case POLY_KIND_ICE:        c = (Color){180, 220, 240, 255}; break;
            case POLY_KIND_DEADLY:     c = (Color){200, 80, 60, 255};   break;
            case POLY_KIND_ONE_WAY:    c = (Color){120, 120, 160, 255}; break;
            case POLY_KIND_BACKGROUND: c = (Color){60, 70, 90, 180};    break;
            default:                   c = (Color){120, 130, 140, 255}; break;
        }
        int x0 = (int)(p->v_x[0] * sx), y0 = (int)(p->v_y[0] * sy);
        int x1 = (int)(p->v_x[1] * sx), y1 = (int)(p->v_y[1] * sy);
        int x2 = (int)(p->v_x[2] * sx), y2 = (int)(p->v_y[2] * sy);
        ImageDrawLine(&img, x0, y0, x1, y1, c);
        ImageDrawLine(&img, x1, y1, x2, y2, c);
        ImageDrawLine(&img, x2, y2, x0, y0, c);
    }

    for (int i = 0; i < L->spawn_count; ++i) {
        const LvlSpawn *s = &L->spawns[i];
        Color c = (s->team == 1) ? (Color){220, 80, 80, 255}
                : (s->team == 2) ? (Color){ 80, 140, 220, 255}
                                 : (Color){200, 200, 80, 255};
        int x = (int)(s->pos_x * sx);
        int y = (int)(s->pos_y * sy);
        ImageDrawCircle(&img, x, y, 2, c);
    }

    for (int i = 0; i < L->flag_count; ++i) {
        const LvlFlag *f = &L->flags[i];
        Color c = (f->team == 1) ? (Color){240, 50, 50, 255}
                                 : (Color){ 50, 100, 240, 255};
        int x = (int)(f->pos_x * sx);
        int y = (int)(f->pos_y * sy);
        ImageDrawRectangle(&img, x - 3, y - 3, 7, 7, c);
    }

    for (int i = 0; i < L->pickup_count; ++i) {
        const LvlPickup *p = &L->pickups[i];
        int x = (int)(p->pos_x * sx);
        int y = (int)(p->pos_y * sy);
        ImageDrawPixel(&img, x, y,     (Color){200, 200, 80, 255});
        ImageDrawPixel(&img, x + 1, y, (Color){200, 200, 80, 255});
        ImageDrawPixel(&img, x, y + 1, (Color){200, 200, 80, 255});
    }

    return img;
}

unsigned char *map_thumb_encode_png(const Level *L, int *out_size) {
    if (out_size) *out_size = 0;
    Image img = render_thumb_image(L);
    int size = 0;
    unsigned char *bytes = ExportImageToMemory(img, ".png", &size);
    UnloadImage(img);
    if (!bytes || size <= 0) {
        if (bytes) MemFree(bytes);
        return NULL;
    }
    if (out_size) *out_size = size;
    return bytes;
}

bool map_thumb_write_png(const Level *L, const char *out_path) {
    if (!out_path || !out_path[0]) return false;
    Image img = render_thumb_image(L);
    bool ok = ExportImage(img, out_path);
    UnloadImage(img);
    return ok;
}
