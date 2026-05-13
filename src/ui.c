#include "ui.h"

#include "audio.h"
#include "platform.h"

#include "../third_party/raylib/src/raylib.h"

#include <ctype.h>
#include <string.h>

float ui_compute_scale(int screen_h) {
    /* Design baseline: 1280×720. We pick the scale that keeps a 18 px
     * font occupying ~2.5% of the screen height. Round to 0.25 steps so
     * widgets snap to consistent sizes (avoids a jittery 1.07x at
     * unusual resolutions). Capped at 3x so 8K doesn't go silly. */
    if (screen_h <= 0) return 1.0f;
    float raw = (float)screen_h / 720.0f;
    if (raw < 1.0f) raw = 1.0f;
    if (raw > 3.0f) raw = 3.0f;
    /* Snap to 0.25 steps: 1.00, 1.25, 1.50, 1.75, 2.00, 2.25, 2.50, 2.75, 3.00 */
    float snap = (float)((int)((raw * 4.0f) + 0.5f)) / 4.0f;
    if (snap < 1.0f) snap = 1.0f;
    return snap;
}

int ui_font_px(const UIContext *u) {
    int s = (int)((float)u->font_size * u->scale + 0.5f);
    return s < 8 ? 8 : s;
}

void ui_draw_text(const UIContext *u, const char *text, int x, int y,
                  int base_size, Color col)
{
    if (!text || !*text) return;
    int sz = (int)((float)base_size * u->scale + 0.5f);
    if (sz < 8) sz = 8;
    /* P13 — Atkinson Hyperlegible body font when present; raylib default
     * (bilinear-filtered) when not. ui_font_for handles the fallback so
     * a fresh checkout still draws text. The 1/10 letter spacing is
     * what we tuned against the default font; the TTF is wider so we
     * keep the same coefficient — measure_text below stays consistent. */
    DrawTextEx(ui_font_for(UI_FONT_BODY), text,
               (Vector2){ (float)x, (float)y },
               (float)sz, (float)sz / 10.0f, col);
}

int ui_measure(const UIContext *u, const char *text, int base_size) {
    if (!text || !*text) return 0;
    int sz = (int)((float)base_size * u->scale + 0.5f);
    if (sz < 8) sz = 8;
    Vector2 v = MeasureTextEx(ui_font_for(UI_FONT_BODY), text, (float)sz,
                              (float)sz / 10.0f);
    return (int)(v.x + 0.5f);
}

void ui_begin(UIContext *u, Vec2 mouse_screen, float dt, int screen_h) {
    u->mouse              = mouse_screen;
    u->mouse_pressed      = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    u->mouse_released     = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    u->mouse_down         = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    /* wan-fixes-11 — RMB on cycle buttons walks the choice list
     * backward. Track the edge here so any widget can opt in. */
    u->mouse_right_pressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    u->shift_down      = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    u->dt              = dt;
    u->scale           = ui_compute_scale(screen_h);
    u->next_focus_id   = u->focus_id;   /* keep focus unless someone steals it */

    /* Clicking outside any focused text-input clears focus. The text-input
     * widget will rewrite next_focus_id if its rect is hit; if no widget
     * does, ui_end commits the cleared focus. */
    if (u->mouse_pressed) u->next_focus_id = 0;

    if (u->font_size == 0) {
        u->font_size      = 18;
        u->text_col       = (Color){235, 235, 235, 255};
        u->text_dim       = (Color){160, 160, 165, 255};
        u->accent         = (Color){ 80, 180, 255, 255};
        u->panel_bg       = (Color){ 22,  26,  34, 235};
        u->panel_edge     = (Color){ 70,  85, 110, 255};
        u->button_bg      = (Color){ 38,  46,  60, 255};
        u->button_hover   = (Color){ 56,  72,  96, 255};
        u->button_press   = (Color){ 24,  32,  44, 255};
        u->button_disabled= (Color){ 40,  44,  52, 200};
    }
}

void ui_end(UIContext *u) {
    u->focus_id    = u->next_focus_id;
    u->caret_phase = u->caret_phase + u->dt;
    if (u->caret_phase > 1.0f) u->caret_phase -= 1.0f;
}

bool ui_point_in_rect(Vec2 p, Rectangle r) {
    return p.x >= r.x && p.x < r.x + r.width &&
           p.y >= r.y && p.y < r.y + r.height;
}

int ui_text_width(const UIContext *u, const char *text) {
    return ui_measure(u, text, u->font_size);
}

/* ---- Panel --------------------------------------------------------- */

void ui_panel(Rectangle r, Color bg, Color edge) {
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1.0f, edge);
}

void ui_panel_default(const UIContext *u, Rectangle r) {
    ui_panel(r, u->panel_bg, u->panel_edge);
}

/* ---- Label --------------------------------------------------------- */

void ui_label(const UIContext *u, Rectangle r, const char *text, Color col) {
    if (!text) return;
    int fp = ui_font_px(u);
    int ty = (int)(r.y + (r.height - (float)fp) * 0.5f);
    ui_draw_text(u, text, (int)r.x, ty, u->font_size, col);
}

void ui_label_center(const UIContext *u, Rectangle r, const char *text, Color col) {
    if (!text) return;
    int tw = ui_measure(u, text, u->font_size);
    int fp = ui_font_px(u);
    int tx = (int)(r.x + (r.width - (float)tw) * 0.5f);
    int ty = (int)(r.y + (r.height - (float)fp) * 0.5f);
    ui_draw_text(u, text, tx, ty, u->font_size, col);
}

/* ---- Button -------------------------------------------------------- */

bool ui_button(UIContext *u, Rectangle r, const char *label, bool enabled) {
    bool hover = ui_point_in_rect(u->mouse, r);
    Color bg;
    if (!enabled) bg = u->button_disabled;
    else if (hover && u->mouse_down) bg = u->button_press;
    else if (hover) bg = u->button_hover;
    else bg = u->button_bg;

    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, u->scale, u->panel_edge);
    Color text = enabled ? u->text_col : u->text_dim;
    if (label) {
        int tw = ui_measure(u, label, u->font_size);
        int fp = ui_font_px(u);
        int tx = (int)(r.x + (r.width - (float)tw) * 0.5f);
        int ty = (int)(r.y + (r.height - (float)fp) * 0.5f);
        ui_draw_text(u, label, tx, ty, u->font_size, text);
    }
    bool clicked = enabled && hover && u->mouse_pressed;
    if (clicked) audio_play_global(SFX_UI_CLICK);
    return clicked;
}

/* wan-fixes-11 — cycle button. Same look as ui_button but with small
 * "◀" / "▶" arrows on either edge and dual-button input: LMB steps
 * forward (+1), RMB steps backward (-1). Returns the step, or 0 when
 * nothing was clicked this frame. */
int ui_cycle_button(UIContext *u, Rectangle r, const char *label, bool enabled) {
    bool hover = ui_point_in_rect(u->mouse, r);
    Color bg;
    if (!enabled) bg = u->button_disabled;
    else if (hover && u->mouse_down) bg = u->button_press;
    else if (hover) bg = u->button_hover;
    else bg = u->button_bg;

    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, u->scale, u->panel_edge);

    int fp = ui_font_px(u);

    /* Arrows. Dim when not hovered so they read as affordances, not
     * primary content. The "◀" / "▶" glyphs aren't in raylib's
     * default 7-bit font, but every TTF in our atlas (Atkinson,
     * VG5000, Steps Mono) carries U+25C0 / U+25B6 — `ui_draw_text`
     * falls back gracefully when missing. */
    Color arrow_col = enabled
        ? (hover ? u->text_col : u->text_dim)
        : u->text_dim;
    int arrow_y = (int)(r.y + (r.height - (float)fp) * 0.5f);
    int pad     = (int)(u->scale * 12.0f);
    int aw_l    = ui_measure(u, "<", u->font_size);
    int aw_r    = ui_measure(u, ">", u->font_size);
    ui_draw_text(u, "<", (int)(r.x + pad), arrow_y, u->font_size, arrow_col);
    ui_draw_text(u, ">",
                 (int)(r.x + r.width - pad - aw_r),
                 arrow_y, u->font_size, arrow_col);

    Color text = enabled ? u->text_col : u->text_dim;
    if (label) {
        /* Center the label in the area BETWEEN the arrows, not over
         * them — pad + arrow + small gap on each side. Without this
         * carve-out, long labels (e.g. "Tier: Veteran" in a
         * 128-px-wide button) overlap the arrow glyphs. If the label
         * still won't fit, we let it bleed past the arrows rather
         * than truncating — better that the user can read it than
         * we hide it for the sake of clean geometry. */
        int gap      = (int)(u->scale * 6.0f);
        int inner_x0 = (int)(r.x) + pad + aw_l + gap;
        int inner_x1 = (int)(r.x + r.width) - pad - aw_r - gap;
        int inner_w  = inner_x1 - inner_x0;
        int tw       = ui_measure(u, label, u->font_size);
        int tx       = (tw < inner_w)
                       ? inner_x0 + (inner_w - tw) / 2
                       : (int)(r.x + (r.width - (float)tw) * 0.5f);
        int ty       = (int)(r.y + (r.height - (float)fp) * 0.5f);
        ui_draw_text(u, label, tx, ty, u->font_size, text);
    }

    if (!enabled) return 0;
    int step = 0;
    if (hover && (u->mouse_pressed || u->mouse_right_pressed)) {
        /* Split the button into three hit regions: left quarter is
         * "left arrow", right quarter is "right arrow", middle is
         * "label area." Arrow clicks act DIRECTIONALLY regardless
         * of mouse button — clicking the right arrow always cycles
         * forward (LMB or RMB), and clicking the left arrow always
         * cycles backward. Clicking the middle (over the label)
         * keeps the original convention: LMB forward, RMB backward.
         * This way the arrows behave the way users expect from a
         * "stepper" while the body retains the dual-button fast-
         * scroll shape. */
        float qx_l = r.x + r.width * 0.25f;
        float qx_r = r.x + r.width * 0.75f;
        bool on_left_arrow  = (u->mouse.x <  qx_l);
        bool on_right_arrow = (u->mouse.x >= qx_r);
        if (on_left_arrow)       step = -1;
        else if (on_right_arrow) step = +1;
        else                     step = u->mouse_pressed ? +1 : -1;
    }
    if (step != 0) audio_play_global(SFX_UI_CLICK);
    return step;
}

/* ---- Toggle -------------------------------------------------------- */

bool ui_toggle(UIContext *u, Rectangle r, const char *label, bool *on) {
    bool hover = ui_point_in_rect(u->mouse, r);
    Color bg;
    if (*on)        bg = (Color){40, 110, 60, 255};
    else if (hover) bg = u->button_hover;
    else            bg = u->button_bg;

    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, u->scale, u->panel_edge);

    /* Indicator: small filled circle on the left side, scaled with UI. */
    float cx = r.x + 14.0f * u->scale;
    float cy = r.y + r.height * 0.5f;
    DrawCircleLines((int)cx, (int)cy, 8.0f * u->scale, u->panel_edge);
    if (*on) DrawCircle((int)cx, (int)cy, 5.0f * u->scale, u->accent);

    if (label) {
        int fp = ui_font_px(u);
        int ty = (int)(r.y + (r.height - (float)fp) * 0.5f);
        ui_draw_text(u, label, (int)(r.x + 32.0f * u->scale), ty,
                     u->font_size, u->text_col);
    }

    if (hover && u->mouse_pressed) {
        *on = !*on;
        audio_play_global(SFX_UI_TOGGLE);
        return true;
    }
    return false;
}

/* ---- Text input ---------------------------------------------------- */

bool ui_text_input(UIContext *u, Rectangle r, char *buf, int buf_cap,
                   uint32_t widget_id, const char *placeholder)
{
    bool hover         = ui_point_in_rect(u->mouse, r);
    bool was_focused   = (u->focus_id == widget_id);
    bool gained_focus  = false;
    if (hover && u->mouse_pressed) {
        u->next_focus_id = widget_id;
        gained_focus     = !was_focused;
    }
    bool focused = was_focused || (hover && u->mouse_pressed);

    Color bg = focused ? (Color){34, 42, 56, 255} : u->button_bg;
    DrawRectangleRec(r, bg);
    Color edge = focused ? u->accent : u->panel_edge;
    DrawRectangleLinesEx(r, focused ? 2.0f : 1.0f, edge);

    /* Hand-rolled strnlen — strnlen is POSIX-only, and we don't need
     * to drag in a feature-test macro for one call. */
    int len = 0;
    while (len < buf_cap && buf[len] != '\0') len++;
    bool committed = false;

    /* wan-fixes-12 — caret position. On focus-gain (a click on this
     * widget that wasn't already focused), seed to end of buffer so
     * the user can keep typing where they left off. The clamp lives
     * inside the focused branch so a non-focused widget can't truncate
     * the focused widget's caret to its own (shorter) buf. */
    if (gained_focus) {
        u->caret_pos = len;
    }

    if (focused) {
        if (u->caret_pos < 0)   u->caret_pos = 0;
        if (u->caret_pos > len) u->caret_pos = len;
        /* Pull characters off raylib's input queue. We accept printable
         * ASCII; everything else (codepoints >127) is dropped at this
         * scale of typing. Insertion happens at `caret_pos`, sliding
         * the trailing tail right one byte. */
        for (;;) {
            int ch = GetCharPressed();
            if (ch == 0) break;
            if (ch < 32 || ch > 126) continue;
            if (len + 1 >= buf_cap) continue;
            int cp = u->caret_pos;
            for (int i = len; i > cp; --i) buf[i] = buf[i - 1];
            buf[cp]     = (char)ch;
            buf[++len]  = '\0';
            u->caret_pos = cp + 1;
        }
        /* Backspace — deletes the byte BEFORE the caret (the
         * conventional behavior). Repeats while held. */
        if (IsKeyPressed(KEY_BACKSPACE) ||
            (IsKeyDown(KEY_BACKSPACE) && IsKeyPressedRepeat(KEY_BACKSPACE)))
        {
            if (u->caret_pos > 0) {
                int cp = u->caret_pos - 1;
                for (int i = cp; i < len; ++i) buf[i] = buf[i + 1];
                len--;
                u->caret_pos = cp;
            }
        }
        /* Delete — deletes the byte AT the caret (the byte to the
         * right of the cursor). Repeats while held. */
        if (IsKeyPressed(KEY_DELETE) ||
            (IsKeyDown(KEY_DELETE) && IsKeyPressedRepeat(KEY_DELETE)))
        {
            if (u->caret_pos < len) {
                int cp = u->caret_pos;
                for (int i = cp; i < len; ++i) buf[i] = buf[i + 1];
                len--;
            }
        }
        /* Left / Right cursor walk. Repeats so holding scrubs. */
        if (IsKeyPressed(KEY_LEFT) ||
            (IsKeyDown(KEY_LEFT) && IsKeyPressedRepeat(KEY_LEFT)))
        {
            if (u->caret_pos > 0) u->caret_pos--;
        }
        if (IsKeyPressed(KEY_RIGHT) ||
            (IsKeyDown(KEY_RIGHT) && IsKeyPressedRepeat(KEY_RIGHT)))
        {
            if (u->caret_pos < len) u->caret_pos++;
        }
        /* Home / End — jump to extremes. */
        if (IsKeyPressed(KEY_HOME)) u->caret_pos = 0;
        if (IsKeyPressed(KEY_END))  u->caret_pos = len;
        /* Enter commits. We don't clear the buffer; the caller decides. */
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            committed = true;
        }
        /* Escape blurs without committing. */
        if (IsKeyPressed(KEY_ESCAPE)) {
            u->next_focus_id = 0;
        }
    }

    /* Render text. Padding scales with UI so the text doesn't kiss
     * the box edge at high DPI. */
    int pad = (int)(8.0f * u->scale + 0.5f);
    int fp = ui_font_px(u);
    int ty = (int)(r.y + (r.height - (float)fp) * 0.5f);
    if (len == 0 && !focused && placeholder) {
        ui_draw_text(u, placeholder, (int)r.x + pad, ty, u->font_size, u->text_dim);
    } else {
        ui_draw_text(u, buf, (int)r.x + pad, ty, u->font_size, u->text_col);
    }
    /* Caret. Measure the substring up to caret_pos for the x-offset
     * so the bar sits between glyphs no matter where the user typed.
     * Temporary NUL-terminate the prefix to reuse `ui_measure` without
     * allocating; we restore the byte immediately. */
    if (focused && u->caret_phase < 0.5f) {
        int cw = 0;
        if (u->caret_pos > 0) {
            char saved = buf[u->caret_pos];
            buf[u->caret_pos] = '\0';
            cw = ui_measure(u, buf, u->font_size);
            buf[u->caret_pos] = saved;
        }
        int cx = (int)r.x + pad + cw + 1;
        DrawRectangle(cx, ty, (int)(2 * u->scale + 0.5f), fp, u->text_col);
    }

    return committed;
}

/* ---- Lists --------------------------------------------------------- */

int ui_list_pick(UIContext *u, Rectangle r, const char **items, int n,
                 int selected, int row_h)
{
    ui_panel_default(u, r);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
    int picked = -1;
    int scaled_row_h = (int)((float)row_h * u->scale + 0.5f);
    int rows = (int)(r.height / (float)scaled_row_h);
    if (n < rows) rows = n;
    for (int i = 0; i < rows; ++i) {
        Rectangle row = (Rectangle){
            r.x, r.y + (float)(i * scaled_row_h),
            r.width, (float)scaled_row_h
        };
        bool hover = ui_point_in_rect(u->mouse, row) &&
                     ui_point_in_rect(u->mouse, r);
        Color row_bg = (i == selected) ? u->accent
                     : hover           ? u->button_hover
                                       : (Color){0,0,0,0};
        if (row_bg.a > 0) DrawRectangleRec(row, row_bg);
        Color tc = (i == selected) ? (Color){12, 18, 26, 255} : u->text_col;
        if (items[i]) {
            int fp = ui_font_px(u);
            int ty = (int)(row.y + ((float)scaled_row_h - (float)fp) * 0.5f);
            ui_draw_text(u, items[i], (int)row.x + (int)(12 * u->scale),
                         ty, u->font_size, tc);
        }
        if (hover && u->mouse_pressed) picked = i;
    }
    EndScissorMode();
    return picked;
}

int ui_list_custom(UIContext *u, Rectangle r, int n, int row_h,
                   int selected, UIRowDrawFn draw, void *user)
{
    ui_panel_default(u, r);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
    int picked = -1;
    int scaled_row_h = (int)((float)row_h * u->scale + 0.5f);
    int rows = (int)(r.height / (float)scaled_row_h);
    if (n < rows) rows = n;
    for (int i = 0; i < rows; ++i) {
        Rectangle row = (Rectangle){
            r.x, r.y + (float)(i * scaled_row_h),
            r.width, (float)scaled_row_h
        };
        bool hover = ui_point_in_rect(u->mouse, row) &&
                     ui_point_in_rect(u->mouse, r);
        if (draw) draw(u, row, i, hover, i == selected, user);
        if (hover && u->mouse_pressed) picked = i;
    }
    EndScissorMode();
    return picked;
}
