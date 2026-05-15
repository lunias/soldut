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
Font g_ui_font_huge    = {0};
bool g_ui_fonts_loaded = false;

Font ui_font_for(UIFontKind kind) {
    if (!g_ui_fonts_loaded) return GetFontDefault();
    switch (kind) {
        case UI_FONT_HUGE:
            return (g_ui_font_huge.texture.id    != 0) ? g_ui_font_huge
                 : (g_ui_font_display.texture.id != 0) ? g_ui_font_display
                 : (g_ui_font_body.texture.id    != 0) ? g_ui_font_body
                 : GetFontDefault();
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

/* m6-ui-fixes — TTF codepoint coverage. Passing `codepoints=NULL,
 * count=0` to LoadFontEx generates only the ASCII printable range
 * (32-126), so any UI string containing en-dash, em-dash, ellipsis,
 * middle dot, true minus, smart quotes, arrows, or geometric
 * triangles renders as the missing-glyph "?" (or as nothing, with
 * leading whitespace squashed).
 *
 * The list below covers every non-ASCII codepoint that appears in
 * a `"..."` literal anywhere under src/, plus a handful of common
 * Latin Supplement / arrow / triangle codepoints we want available
 * for future UI work. Coverage per face (verified via fc-query):
 *   - Atkinson-Hyperlegible-Regular.ttf — full Latin/Punctuation,
 *     U+2014 em-dash, U+2026 ellipsis, U+00B7 middle dot, U+2212
 *     minus, U+2122 trademark. (No triangle/arrow glyphs.)
 *   - VG5000-Regular.otf — adds U+2190..2193 arrows, U+25B2/▲,
 *     U+25B6/▶, U+25BC/▼, U+25C0/◀.
 *   - Steps-Mono-Thin.otf — limited ASCII; has em-dash, ellipsis,
 *     minus, smart quotes.
 * Codepoints missing from the source font silently produce a
 * zero-area slot, which raylib's DrawTextEx then renders as the
 * fallback "?" glyph (or blank when no fallback exists) — so this
 * list can be a superset without producing visible artefacts in
 * the per-face atlases. */
static const int g_ui_codepoints[] = {
    /* ASCII printable 32..126 (95 codepoints) */
    0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,
    0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,
    0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,
    0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,
    0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
    0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,
    0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,
    0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,
    /* Latin-1 punctuation + symbols we use or expect to use */
    0x00A1, /* ¡ */ 0x00A2, /* ¢ */ 0x00A3, /* £ */ 0x00A9, /* © */
    0x00AB, /* « */ 0x00AE, /* ® */ 0x00B0, /* ° */ 0x00B1, /* ± */
    0x00B5, /* µ */ 0x00B7, /* · — middle dot, lobby status separator */
    0x00BB, /* » */ 0x00BF, /* ¿ */
    /* General punctuation */
    0x2010, /* ‐ hyphen */         0x2013, /* – en dash */
    0x2014, /* — em dash */        0x2018, /* ' left single quote */
    0x2019, /* ' right single quote */ 0x201C, /* " left double quote */
    0x201D, /* " right double quote */ 0x2022, /* • bullet */
    0x2026, /* … ellipsis — "RELOADING…", "Loading match…" */
    0x2030, /* ‰ per mille */      0x2039, /* ‹ */ 0x203A, /* › */
    /* Currencies + tech symbols */
    0x20AC, /* € */               0x2122, /* ™ */
    /* Math: minus sign (distinct from hyphen-minus) */
    0x2212, /* − — stepper "minus" button in host-setup */
    /* Arrows — present in VG5000 + as a fallback in some Atkinson builds */
    0x2190, /* ← */ 0x2191, /* ↑ */ 0x2192, /* → */ 0x2193, /* ↓ */
    /* Geometric triangles (cycle arrows / spinners) */
    0x25B2, /* ▲ */ 0x25B6, /* ▶ — "▶" used in lobby map labels */
    0x25BC, /* ▼ */ 0x25C0, /* ◀ */
    /* Check / cross marks (commonly useful for toggles) */
    0x2713, /* ✓ */ 0x2717, /* ✗ */
};

#define UI_CODEPOINT_COUNT (int)(sizeof g_ui_codepoints / sizeof g_ui_codepoints[0])

/* Try to LoadFontEx for one face; on failure, leave the slot zeroed so
 * ui_font_for transparently falls back. raylib accepts both .ttf and .otf
 * via stb_truetype as long as the file has TrueType (not CFF) outlines —
 * the three faces we vendor (Atkinson + VG5000 + Steps Mono) all do. */
static Font load_font_or_zero(const char *path, int px) {
    if (!FileExists(path)) {
        LOG_I("font: %s not found; will fall back to default", path);
        return (Font){0};
    }
    /* Pass our explicit codepoint list so non-ASCII glyphs (em-dash,
     * ellipsis, middle dot, minus, triangles) get rasterised into the
     * atlas. raylib casts away the const internally — the array is
     * read-only from LoadFontEx's POV. */
    Font f = LoadFontEx(path, px, (int *)g_ui_codepoints, UI_CODEPOINT_COUNT);
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
    /* M6 countdown-fix — 192 px atlas for the screen-center countdown
     * numerals + GO! splash. Renders at ~240 px in interactive play; a
     * 48 px atlas (UI_FONT_DISPLAY) bilinear-scaled 5× looked muddy. */
    g_ui_font_huge    = load_font_or_zero("assets/fonts/VG5000-Regular.otf",                192);
    g_ui_fonts_loaded = (g_ui_font_body.texture.id    != 0 ||
                         g_ui_font_display.texture.id != 0 ||
                         g_ui_font_mono.texture.id    != 0 ||
                         g_ui_font_huge.texture.id    != 0);
}

static void unload_ui_fonts(void) {
    if (g_ui_font_body.texture.id    != 0) UnloadFont(g_ui_font_body);
    if (g_ui_font_display.texture.id != 0) UnloadFont(g_ui_font_display);
    if (g_ui_font_mono.texture.id    != 0) UnloadFont(g_ui_font_mono);
    if (g_ui_font_huge.texture.id    != 0) UnloadFont(g_ui_font_huge);
    g_ui_font_body    = (Font){0};
    g_ui_font_display = (Font){0};
    g_ui_font_mono    = (Font){0};
    g_ui_font_huge    = (Font){0};
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

    /* M6 lobby-loadout-preview — disable raylib's default "ESC closes the
     * window" so dismissable modals can intercept ESC without losing the
     * session. Each ESC handler (match end-round, lobby Controls modal,
     * loadout preview modal) is explicit via IsKeyPressed(KEY_ESCAPE). */
    SetExitKey(KEY_NULL);

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
