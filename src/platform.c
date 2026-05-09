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
     * MSAA_4X smooths the polygon-mech bones, decal edges, and UI
     * line-art that would otherwise jaggy at any resolution, but is
     * especially noticeable at 1080p+ window sizes. ~1ms cost on
     * integrated GPU for our scene complexity.
     *
     * HIGHDPI tells raylib to give us a backbuffer at the monitor's
     * physical pixel count on hi-DPI displays (Retina, 4K-scaled
     * Windows). We then use GetScreenHeight() to pick our UI scale
     * — at 4K we want fonts/widgets ~3× the pixel size of 720p. */
    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT |
                     FLAG_WINDOW_HIGHDPI;
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

    LOG_I("platform_init: %dx%d (msaa=4x highdpi), vsync=%d",
          cfg->window_w, cfg->window_h, cfg->vsync);
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
