#include "platform.h"
#include "log.h"

#include "../third_party/raylib/src/raylib.h"

#include <string.h>

static PlatformConfig g_cfg;

/* M5 P13 — TTF font globals. Loaded after InitWindow (LoadFontEx requires
 * a live GL context); unloaded in platform_shutdown. `g_ui_fonts_loaded`
 * gates ui_font_for's fallback decision — when false, every face returns
 * GetFontDefault(). */
Font g_ui_font_body    = {0};
Font g_ui_font_display = {0};
Font g_ui_font_mono    = {0};
bool g_ui_fonts_loaded = false;

Font ui_font_for(UIFontKind kind) {
    if (!g_ui_fonts_loaded) return GetFontDefault();
    switch (kind) {
        case UI_FONT_DISPLAY:
            return (g_ui_font_display.texture.id != 0) ? g_ui_font_display
                 : (g_ui_font_body.texture.id    != 0) ? g_ui_font_body
                 : GetFontDefault();
        case UI_FONT_MONO:
            return (g_ui_font_mono.texture.id    != 0) ? g_ui_font_mono
                 : (g_ui_font_body.texture.id    != 0) ? g_ui_font_body
                 : GetFontDefault();
        case UI_FONT_BODY:
        default:
            return (g_ui_font_body.texture.id != 0) ? g_ui_font_body
                                                    : GetFontDefault();
    }
}

/* Try to LoadFontEx for one face; on failure, leave the slot zeroed so
 * ui_font_for transparently falls back. raylib accepts both .ttf and .otf
 * via stb_truetype as long as the file has TrueType (not CFF) outlines —
 * the three faces we vendor (Atkinson + VG5000 + Steps Mono) all do. */
static Font load_font_or_zero(const char *path, int px) {
    if (!FileExists(path)) {
        LOG_I("font: %s not found; will fall back to default", path);
        return (Font){0};
    }
    Font f = LoadFontEx(path, px, NULL, 0);
    if (f.texture.id == 0 || f.glyphCount == 0) {
        LOG_W("font: LoadFontEx(%s) failed; falling back to default", path);
        if (f.texture.id != 0) UnloadFont(f);
        return (Font){0};
    }
    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    LOG_I("font: loaded %s @ %dpx (%d glyphs)", path, px, f.glyphCount);
    return f;
}

static void load_ui_fonts(void) {
    /* Generation atlas size — large enough that the 720p baseline reads
     * crisp and the 4K scale still doesn't bilinear-mud. 32 px body /
     * 48 px display / 32 px mono mirrors the spec in
     * documents/m5/08-rendering.md §"TTF font". */
    g_ui_font_body    = load_font_or_zero("assets/fonts/Atkinson-Hyperlegible-Regular.ttf", 32);
    g_ui_font_display = load_font_or_zero("assets/fonts/VG5000-Regular.otf",                48);
    g_ui_font_mono    = load_font_or_zero("assets/fonts/Steps-Mono-Thin.otf",               32);
    g_ui_fonts_loaded = (g_ui_font_body.texture.id    != 0 ||
                         g_ui_font_display.texture.id != 0 ||
                         g_ui_font_mono.texture.id    != 0);
}

static void unload_ui_fonts(void) {
    if (g_ui_font_body.texture.id    != 0) UnloadFont(g_ui_font_body);
    if (g_ui_font_display.texture.id != 0) UnloadFont(g_ui_font_display);
    if (g_ui_font_mono.texture.id    != 0) UnloadFont(g_ui_font_mono);
    g_ui_font_body    = (Font){0};
    g_ui_font_display = (Font){0};
    g_ui_font_mono    = (Font){0};
    g_ui_fonts_loaded = false;
}

bool platform_init(const PlatformConfig *cfg) {
    g_cfg = *cfg;

    /* raylib's vsync hint must be set before InitWindow.
     *
     * M6 P03 — MSAA dropped from the backbuffer. The world goes
     * through a capped internal RT + bilinear upscale + halftone
     * screen, all of which mask sub-pixel aliasing. The HUD is
     * axis-aligned rects + bilinear-sampled glyphs — neither
     * benefits from MSAA on the backbuffer. Recovers ~1 ms / frame
     * and ~57 MB of VRAM at maximised 3440×1440. See
     * documents/m6/03-perf-4k-enhancements.md §3 Phase 3.
     *
     * M6 P03-hotfix — HIGHDPI dropped too. The flag used to be on so
     * raylib would give us a backbuffer at the monitor's physical
     * pixel count on hi-DPI displays (4K-scaled Windows, Retina).
     * Reality on Windows: with fractional DPI scaling (anything other
     * than 100 %) the framebuffer comes back at physical-pixel size
     * but the OS does NOT scale it down to the logical window — the
     * window's client area only covers a fraction of the framebuffer
     * (≈ 1 / scale-factor), so the centered UI lands far off to the
     * right of the visible area. We hit this on a friend's machine
     * at ~192 % DPI: the title bar showed "SOL" pinned to the right
     * edge with the rest of "SOLDUT" off-screen. The internal-RT
     * pipeline below already handles "render at a fixed internal
     * size, blit to window" explicitly, so HIGHDPI buys us nothing
     * post-P03 and the OS-side downscale of a non-HiDPI window is
     * the well-behaved path on every platform. Trade-off: HUD text
     * on a true 4K monitor with 200 % scaling is now upscaled by the
     * OS compositor instead of rendered at native; it'll look a hair
     * softer but read at the right size. If anyone wants the old
     * behaviour back, this is the single line to flip. */
    unsigned flags = FLAG_WINDOW_RESIZABLE;
    if (cfg->vsync) flags |= FLAG_VSYNC_HINT;
    if (cfg->fullscreen) flags |= FLAG_FULLSCREEN_MODE;
    SetConfigFlags(flags);

    /* Quiet raylib's stdout noise; we have our own logger.
     * LOG_WARNING here is raylib's TraceLogLevel, not ours. */
    SetTraceLogLevel(LOG_WARNING);

    InitWindow(cfg->window_w, cfg->window_h, cfg->title ? cfg->title : "soldut");
    if (!IsWindowReady()) {
        LOG_E("platform_init: raylib InitWindow failed");
        return false;
    }

    /* Smooth out the default font when it's drawn at non-1x sizes.
     * raylib's default is a 10-pixel bitmap; without bilinear it
     * looks blocky at 18+ px. The TEXTURE_FILTER_BILINEAR call has
     * to come AFTER InitWindow because it touches a GL texture.
     *
     * P13 wires three TTF faces (Atkinson / VG5000 / Steps Mono) on
     * top — the default font stays as the fallback when the TTF files
     * aren't on disk, so we keep the bilinear-filter call. */
    Font def = GetFontDefault();
    if (def.texture.id != 0) {
        SetTextureFilter(def.texture, TEXTURE_FILTER_BILINEAR);
    }

    /* P13 — TTF fonts. Must come after InitWindow because LoadFontEx
     * uploads the rasterized atlas to GL. Missing files log INFO and
     * leave g_ui_fonts_loaded == false; ui_font_for then returns
     * GetFontDefault() and the build still renders text. */
    load_ui_fonts();

    /* We manage our own pacing inside the simulation loop; raylib's
     * SetTargetFPS(0) gives us free-running render bound only by vsync. */
    SetTargetFPS(0);

    InitAudioDevice();
    if (!IsAudioDeviceReady()) {
        LOG_W("platform_init: audio device failed to open; continuing silently");
    }

    LOG_I("platform_init: %dx%d internal_h=%d vsync=%d",
          cfg->window_w, cfg->window_h, cfg->internal_h, cfg->vsync);
    return true;
}

void platform_shutdown(void) {
    unload_ui_fonts();
    if (IsAudioDeviceReady()) CloseAudioDevice();
    if (IsWindowReady()) CloseWindow();
}

void platform_begin_frame(PlatformFrame *out) {
    out->window_w = GetScreenWidth();
    out->window_h = GetScreenHeight();
    out->render_w = GetRenderWidth();
    out->render_h = GetRenderHeight();
    out->should_close = WindowShouldClose();
    out->time_seconds = GetTime();

    /* M6 P03 — compute the internal-render-target dims from the cap.
     * cap <= 0 OR cap > render_h ⇒ 1:1 with the backbuffer (no cap).
     * Otherwise scale the width to preserve the window aspect ratio
     * (rounded-to-nearest int). Defensive minimums prevent a 1-pixel
     * tall RT from a malformed config. */
    int cap = g_cfg.internal_h;
    if (out->render_w <= 0 || out->render_h <= 0) {
        out->internal_w = out->render_w;
        out->internal_h = out->render_h;
    } else if (cap <= 0 || cap >= out->render_h) {
        out->internal_w = out->render_w;
        out->internal_h = out->render_h;
    } else {
        out->internal_h = cap;
        /* Round-to-nearest, integer math: (a*b + b/2) / b. */
        out->internal_w = (out->render_w * cap + out->render_h / 2) / out->render_h;
        if (out->internal_w < 16) out->internal_w = 16;
    }
}

void platform_sample_input(ClientInput *out) {
    uint16_t b = 0;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))   b |= BTN_LEFT;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT))  b |= BTN_RIGHT;
    if (IsKeyDown(KEY_SPACE))                      b |= BTN_JUMP;
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))     b |= BTN_JET;
    if (IsKeyDown(KEY_LEFT_CONTROL) ||
        IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))   b |= BTN_CROUCH;
    if (IsKeyDown(KEY_X))                          b |= BTN_PRONE;
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))      b |= BTN_FIRE;
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))     b |= BTN_FIRE_SECONDARY;
    if (IsKeyDown(KEY_R))                          b |= BTN_RELOAD;
    if (IsKeyDown(KEY_F))                          b |= BTN_MELEE;
    if (IsKeyDown(KEY_E))                          b |= BTN_USE;
    if (IsKeyDown(KEY_Q))                          b |= BTN_SWAP;
    if (IsKeyDown(KEY_LEFT_SHIFT))                 b |= BTN_DASH;

    out->buttons = b;

    Vector2 m = GetMousePosition();
    out->aim_x = m.x;
    out->aim_y = m.y;
    /* seq and dt are filled by the simulation loop, not here. */
}
