#pragma once

/*
 * Camera + snapping for the editor canvas.
 *
 * Wraps raylib's Camera2D. Pan via Space-drag, zoom via Ctrl-scroll
 * (1.1× per notch, clamped). Also owns the grid-visibility toggle.
 *
 * Snap: 32-px tile snap for the tile tool, 4-px sub-tile snap for
 * polygon vertices / object placement. Holding Alt disables snap
 * during a click so designers can place sub-pixel.
 */

#include "doc.h"

#include "raylib.h"

#include <stdbool.h>

typedef struct EditorView {
    Camera2D cam;
    Vec2     pan_anchor;        /* during Space-drag */
    bool     panning;
    bool     show_tile_grid;    /* G */
    bool     show_snap_grid;    /* Shift+G */
} EditorView;

void view_init        (EditorView *v, const EditorDoc *d);

/* Center camera on the level. */
void view_center      (EditorView *v, const EditorDoc *d);

/* Per-frame: handles Space-drag pan, Ctrl-scroll zoom, G/Shift+G grid
 * toggles. Returns true if the camera consumed the input (so tool
 * dispatch should skip mouse events that frame). */
bool view_update      (EditorView *v);

/* Convert a screen coordinate to world-space (no snap). */
Vec2 view_screen_to_world(const EditorView *v, Vec2 screen);

/* Convert + snap. `snap_px` is 32 (tile) or 4 (sub-tile) typically.
 * Pass 1 for "no snap." */
Vec2 view_snap        (Vec2 world_pos, int snap_px);

/* Draw the world-space grid lines + level outline. Call inside
 * BeginMode2D / EndMode2D. */
void view_draw_grid   (const EditorView *v, const EditorDoc *d);

/* Status line text describing zoom + camera position. */
void view_status_text (const EditorView *v, char *out, int out_cap);
