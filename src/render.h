#pragma once

#include "world.h"

#include "../third_party/raylib/src/raylib.h"

/* Render state owned by the renderer (camera, rng for shake). The simulate
 * step writes into world.shake_intensity; this module reads it and applies
 * a small camera rotation/offset. */
typedef struct {
    Camera2D camera;
    float    shake_phase;
    Vec2     last_cursor_screen;
    Vec2     last_cursor_world;

    /* When > 0, update_camera() uses this instead of GetFrameTime().
     * Shot mode sets it to 1/60 so camera smoothing moves at the same
     * rate it would in interactive play, regardless of how fast we
     * render frames (tens of ms each, vs. wall-clock 16 ms). */
    float    cam_dt_override;
} Renderer;

void renderer_init(Renderer *r, int screen_w, int screen_h, Vec2 follow);

/* Convert a screen-space point (cursor) into world space using the
 * current camera. Used by main.c every tick to feed mech.aim_world. */
Vec2 renderer_screen_to_world(const Renderer *r, Vec2 screen);

/* One frame: BeginDrawing → world → HUD → EndDrawing.
 *
 * `interp_alpha` is the in-between-ticks fraction in [0, 1) used for
 * smoothing the camera follow; it isn't used to interpolate physics
 * itself at M1 (we render the most-recent simulate output directly).
 *
 * `overlay_cb` (nullable) is invoked between the HUD and EndDrawing()
 * so callers can paint a screen-space diagnostic strip. */
typedef void (*RendererOverlayFn)(void *user, int screen_w, int screen_h);

void renderer_draw_frame(Renderer *r, World *w,
                         int screen_w, int screen_h,
                         float interp_alpha,
                         Vec2 cursor_screen,
                         RendererOverlayFn overlay_cb,
                         void *overlay_user);
