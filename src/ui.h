#pragma once

#include "math.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ui — tiny immediate-mode UI helpers built on raylib primitives.
 *
 * documents/06-rendering-audio.md gestures at raygui.h here. We don't
 * vendor it; the lobby UI needs ~6 widgets (button, toggle, list pick,
 * text input, label, panel) and rolling them in 250 LOC of straight
 * raylib calls beats adding a 5K-LOC dependency for cases we don't use.
 *
 * One-frame lifecycle:
 *   ui_begin(&u, mouse_screen, dt);   // refreshes input snapshot
 *   if (ui_button(&u, ...)) ...
 *   ui_end(&u);                       // commits text-input focus, etc.
 */

typedef struct UIContext {
    Vec2  mouse;             /* screen-space cursor */
    bool  mouse_pressed;     /* this frame's left-button down edge */
    bool  mouse_released;
    bool  mouse_down;
    /* wan-fixes-11 — right-button edge. Cycle buttons in the lobby
     * read this to walk the choice list backward (LMB = forward). */
    bool  mouse_right_pressed;
    bool  shift_down;
    float dt;

    /* Text-input focus tracking. Each text-input widget passes its own
     * stable id; we remember which is focused across frames. 0 = none. */
    uint32_t focus_id;        /* committed focus from previous frame */
    uint32_t next_focus_id;   /* set by widgets clicked this frame */

    /* Caret blink phase, ticked by ui_end(). */
    float caret_phase;

    /* DPI/UI scale factor. 1.0 at the 1280×720 design baseline; ~3.0
     * on a 4K display. Computed each ui_begin from the current screen
     * height so widgets stay legible regardless of resolution. The
     * helper functions below already multiply font_size, padding,
     * etc. by this — callers using ui_button / ui_label / etc. don't
     * need to scale themselves. */
    float scale;

    /* Style — the BASE values, in 720p units. The helpers multiply
     * by `scale` at draw time. */
    int   font_size;
    Color text_col;
    Color text_dim;
    Color accent;
    Color panel_bg;
    Color panel_edge;
    Color button_bg;
    Color button_hover;
    Color button_press;
    Color button_disabled;
} UIContext;

/* mouse_screen + dt come from raylib; sw/sh are the current backbuffer
 * size (we use sh to pick the scale factor). */
void ui_begin(UIContext *u, Vec2 mouse_screen, float dt, int screen_h);
void ui_end  (UIContext *u);

/* Compute the UI scale for the current screen size. Public so screens
 * that need to lay out their own raw raylib draws can match. */
float ui_compute_scale(int screen_h);

/* Returns the actual on-screen font height the helpers will use.
 * Always >= the base font_size; scales with the UI scale factor. */
int   ui_font_px(const UIContext *u);

/* Draw a string with the UI font at the helper's scaled size. Wraps
 * raylib's DrawTextEx so callers don't have to thread a Font handle. */
void  ui_draw_text(const UIContext *u, const char *text, int x, int y,
                   int base_size, Color col);
int   ui_measure(const UIContext *u, const char *text, int base_size);

/* Compute a stable id from (file, line, salt). Use UI_ID() to get one
 * that's unique per call site. The salt distinguishes multiple widgets
 * on the same line (e.g. inside a loop). */
#define UI_ID()       ((uint32_t)((uint32_t)__LINE__ * 2654435761u))
#define UI_ID_S(s)    ((uint32_t)((uint32_t)__LINE__ * 2654435761u ^ (uint32_t)(s) * 1597334677u))

/* Filled rectangle with a 1-px edge. Used as the visual base for cards
 * and frames. */
void ui_panel(Rectangle r, Color bg, Color edge);
void ui_panel_default(const UIContext *u, Rectangle r);

/* Draw a label string left-aligned within `r` (vertical-centered). */
void ui_label(const UIContext *u, Rectangle r, const char *text, Color col);
void ui_label_center(const UIContext *u, Rectangle r, const char *text, Color col);

/* Text width helper (wraps MeasureText so callers don't pull in raylib). */
int  ui_text_width(const UIContext *u, const char *text);

/* Simple button. Returns true on the press-edge that lands inside. */
bool ui_button(UIContext *u, Rectangle r, const char *label, bool enabled);

/* wan-fixes-11 — cycle button. Same look as ui_button plus small
 * "<" / ">" arrows on the edges. Returns +1 for LMB (next), -1 for
 * RMB (previous), 0 otherwise. Callers walk their choice list by the
 * returned step. */
int  ui_cycle_button(UIContext *u, Rectangle r, const char *label, bool enabled);

/* On/off toggle. Stores state in *on; returns true if toggled this frame. */
bool ui_toggle(UIContext *u, Rectangle r, const char *label, bool *on);

/* Single-line text input. `buf` is mutated; `*focus` tracks which widget
 * holds focus across frames (set to UI_ID() when this widget gains focus,
 * 0 otherwise). Returns true if Enter was pressed while focused. */
bool ui_text_input(UIContext *u, Rectangle r, char *buf, int buf_cap,
                   uint32_t widget_id, const char *placeholder);

/* Vertical list. Returns the index the user clicked this frame, or -1.
 * `selected` is highlighted with the accent color. Items render full-width
 * within `r` at `row_h` per row. The list does NOT scroll at M4 — it just
 * clips to `r`. */
int  ui_list_pick(UIContext *u, Rectangle r, const char **items, int n,
                  int selected, int row_h);

/* Generic vertical scroll-clipped list with custom row drawing. The
 * caller draws each row inside the supplied rectangle; we do the
 * hit-testing. Returns the row clicked this frame, or -1.
 *
 * Useful when rows aren't a single string (e.g. server browser with
 * multiple columns). The caller is responsible for clipping. */
typedef void (*UIRowDrawFn)(const UIContext *u, Rectangle row, int idx,
                            bool hover, bool selected, void *user);
int  ui_list_custom(UIContext *u, Rectangle r, int n, int row_h,
                    int selected, UIRowDrawFn draw, void *user);

/* Helpers used by UI screens. */
bool ui_point_in_rect(Vec2 p, Rectangle r);
