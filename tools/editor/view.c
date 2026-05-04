#include "view.h"

#include "raylib.h"

#include <stdio.h>

#define ZOOM_MIN  0.25f
#define ZOOM_MAX  4.0f
#define ZOOM_STEP 1.1f

void view_init(EditorView *v, const EditorDoc *d) {
    v->cam = (Camera2D){0};
    v->cam.zoom   = 1.0f;
    v->cam.offset = (Vector2){ GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f };
    v->cam.target = (Vector2){ doc_world_w_px(d) * 0.5f, doc_world_h_px(d) * 0.5f };
    v->cam.rotation = 0.0f;
    v->panning = false;
    v->show_tile_grid = true;
    v->show_snap_grid = false;
}

void view_center(EditorView *v, const EditorDoc *d) {
    v->cam.target = (Vector2){ doc_world_w_px(d) * 0.5f, doc_world_h_px(d) * 0.5f };
    v->cam.offset = (Vector2){ GetScreenWidth() * 0.5f, GetScreenHeight() * 0.5f };
    v->cam.zoom   = 1.0f;
}

bool view_update(EditorView *v) {
    bool consumed = false;

    /* Pan: Space-drag, OR middle-mouse drag. */
    bool space = IsKeyDown(KEY_SPACE);
    bool mmb   = IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
    if ((space || mmb) && !v->panning) {
        v->panning = true;
        v->pan_anchor = GetMousePosition();
    }
    if (v->panning) {
        Vector2 m = GetMousePosition();
        Vector2 d = { m.x - v->pan_anchor.x, m.y - v->pan_anchor.y };
        v->cam.target.x -= d.x / v->cam.zoom;
        v->cam.target.y -= d.y / v->cam.zoom;
        v->pan_anchor = m;
        consumed = true;
        if (!space && !mmb) v->panning = false;
    }

    /* Zoom: Ctrl + scroll, OR plain scroll if Ctrl is the modifier we
     * use elsewhere. We accept both since the spec says "Ctrl-scroll." */
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
        Vector2 m_screen = GetMousePosition();
        Vector2 m_world_before = GetScreenToWorld2D(m_screen, v->cam);
        if (wheel > 0)      v->cam.zoom *= ZOOM_STEP;
        else if (wheel < 0) v->cam.zoom /= ZOOM_STEP;
        if (v->cam.zoom < ZOOM_MIN) v->cam.zoom = ZOOM_MIN;
        if (v->cam.zoom > ZOOM_MAX) v->cam.zoom = ZOOM_MAX;
        Vector2 m_world_after = GetScreenToWorld2D(m_screen, v->cam);
        /* Keep mouse anchored over the same world point. */
        v->cam.target.x += m_world_before.x - m_world_after.x;
        v->cam.target.y += m_world_before.y - m_world_after.y;
        consumed = true;
    }

    if (IsKeyPressed(KEY_G)) {
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
            v->show_snap_grid = !v->show_snap_grid;
        } else {
            v->show_tile_grid = !v->show_tile_grid;
        }
    }
    return consumed;
}

Vec2 view_screen_to_world(const EditorView *v, Vec2 screen) {
    return GetScreenToWorld2D(screen, v->cam);
}

Vec2 view_snap(Vec2 world, int snap_px) {
    if (snap_px <= 1) return world;
    float fx = (float)snap_px;
    return (Vec2){
        roundf(world.x / fx) * fx,
        roundf(world.y / fx) * fx,
    };
}

void view_draw_grid(const EditorView *v, const EditorDoc *d) {
    int w = doc_world_w_px(d);
    int h = doc_world_h_px(d);
    /* Level outline. */
    DrawRectangleLinesEx((Rectangle){0, 0, (float)w, (float)h}, 2.0f / v->cam.zoom,
                         (Color){80, 100, 130, 255});
    if (v->show_tile_grid && v->cam.zoom > 0.4f) {
        Color c = (Color){50, 60, 80, 100};
        for (int x = 0; x <= d->width; ++x) {
            DrawLineEx((Vector2){(float)(x * d->tile_size), 0},
                       (Vector2){(float)(x * d->tile_size), (float)h},
                       1.0f / v->cam.zoom, c);
        }
        for (int y = 0; y <= d->height; ++y) {
            DrawLineEx((Vector2){0, (float)(y * d->tile_size)},
                       (Vector2){(float)w, (float)(y * d->tile_size)},
                       1.0f / v->cam.zoom, c);
        }
    }
    if (v->show_snap_grid && v->cam.zoom > 1.5f) {
        Color c = (Color){45, 55, 70, 60};
        for (int x = 0; x <= w; x += 4) {
            DrawLineEx((Vector2){(float)x, 0},
                       (Vector2){(float)x, (float)h},
                       0.5f / v->cam.zoom, c);
        }
        for (int y = 0; y <= h; y += 4) {
            DrawLineEx((Vector2){0, (float)y},
                       (Vector2){(float)w, (float)y},
                       0.5f / v->cam.zoom, c);
        }
    }
}

void view_status_text(const EditorView *v, char *out, int out_cap) {
    snprintf(out, (size_t)out_cap, "zoom %.2fx  cam %.0f,%.0f",
             v->cam.zoom, v->cam.target.x, v->cam.target.y);
}
