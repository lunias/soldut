#pragma once

#include "input.h"
#include "math.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Thin wrapper around raylib's rcore. The rest of the codebase shouldn't
 * care that we're sitting on raylib — it talks to platform_* and gets
 * windows, time, and an input snapshot. We let raylib own the GL context
 * and the audio engine because any 2D project that doesn't is reinventing
 * the parts of raylib that work.
 */

/* M5 P13 — TTF fonts. Four faces ship under assets/fonts/:
 *   BODY    — Atkinson Hyperlegible Regular (high-contrast accessible body)
 *   DISPLAY — VG5000 Regular (Velvetyne; map titles, kill-feed flag chips)
 *   MONO    — Steps Mono Thin (Velvetyne; HUD numerics)
 *   HUGE    — VG5000 Regular at 192 px atlas; only used by full-screen
 *             countdown numerals (rendered at ~240 px) where DISPLAY's
 *             48 px atlas blurred when bilinear-upscaled 5×. (M6
 *             countdown-fix.)
 * `ui_font_for` returns the matching Font when loaded, or raylib's default
 * font as graceful fallback so a fresh checkout without `assets/fonts/`
 * still renders text (just bilinear-blurry like M4). */
typedef enum {
    UI_FONT_BODY    = 0,
    UI_FONT_DISPLAY = 1,
    UI_FONT_MONO    = 2,
    UI_FONT_HUGE    = 3,
    UI_FONT_COUNT
} UIFontKind;

extern Font g_ui_font_body;
extern Font g_ui_font_display;
extern Font g_ui_font_mono;
extern Font g_ui_font_huge;
extern bool g_ui_fonts_loaded;

Font ui_font_for(UIFontKind kind);

typedef struct {
    int   window_w, window_h;
    bool  vsync;
    bool  fullscreen;
    const char *title;
    /* M6 P03 — line count for the internal world+post render target.
     *   0     = match window height (no cap; pixel-byte-identical to
     *           pre-P03 rendering; what shotmode + paired-process
     *           tests use).
     *   else  = cap to this many lines. Width is derived to match the
     *           window aspect ratio (so 3440×1440 → 2580×1080 at
     *           internal_h=1080). 1080 is the default for interactive
     *           builds — see documents/m6/03-perf-4k-enhancements.md. */
    int   internal_h;
} PlatformConfig;

typedef struct {
    int   window_w, window_h;
    int   render_w, render_h;      /* physical backbuffer pixel count */
    int   internal_w, internal_h;  /* capped render-target dims */
    bool  should_close;
    double time_seconds;
} PlatformFrame;

/* Initialize the window, GL context, and audio device. Returns false on
 * failure. After this raylib's API is live; modules call into raylib
 * directly when they need to draw, but everything window/lifecycle
 * shaped flows through here. */
bool platform_init(const PlatformConfig *cfg);
void platform_shutdown(void);

/* Pump events, sample inputs, capture window-size changes. Call once at
 * the start of each frame. */
void platform_begin_frame(PlatformFrame *out);

/* Sample input into a bitmask + aim vector. Aim is provided in screen
 * space; the renderer/camera converts to world. */
void platform_sample_input(ClientInput *out);
