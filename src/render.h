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
 * `interp_alpha` is the in-between-ticks fraction in [0, 1). At P03
 * the renderer lerps every particle position between its
 * `render_prev_*` (start of the most-recent simulate tick) and `pos_*`
 * (latest physics result) by this factor — so a 144 Hz render on a
 * 60 Hz sim gets smooth motion without accelerating physics.
 *
 * `local_visual_offset` is added to every drawn particle of the local
 * mech only — used to smooth out reconciliation snaps (the local mech
 * snaps to the server-corrected position; this offset decays toward
 * zero over ~6 frames so the snap doesn't read as a glitch). Pass
 * {0,0} on the host (no reconcile) or before any snap has happened.
 *
 * `overlay_cb` (nullable) is invoked between the HUD and EndDrawing()
 * so callers can paint a screen-space diagnostic strip. */
typedef void (*RendererOverlayFn)(void *user, int screen_w, int screen_h);

void renderer_draw_frame(Renderer *r, World *w,
                         int screen_w, int screen_h,
                         float interp_alpha,
                         Vec2 local_visual_offset,
                         Vec2 cursor_screen,
                         RendererOverlayFn overlay_cb,
                         void *overlay_user);

/* M5 P13 — drop the halftone shader + post-process render target. Called
 * from main.c right before platform_shutdown so GL resources are freed
 * while the GL context is still live. Safe to call when nothing was ever
 * loaded (idempotent). */
void renderer_post_shutdown(void);

/* M5 P13 — drop the global `assets/sprites/decorations.png` atlas.
 * Called from the same shutdown sites; idempotent. The atlas is lazy-
 * loaded on the first `draw_decorations` invocation, so calling
 * `renderer_decorations_unload` and then drawing again triggers a
 * reload — that's the hot-reload path P13 Task 9 plugs into. */
void renderer_decorations_unload(void);
