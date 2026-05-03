#include "platform.h"
#include "log.h"

#include "../third_party/raylib/src/raylib.h"

#include <string.h>

static PlatformConfig g_cfg;

bool platform_init(const PlatformConfig *cfg) {
    g_cfg = *cfg;

    /* raylib's vsync hint must be set before InitWindow. */
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

    /* We manage our own pacing inside the simulation loop; raylib's
     * SetTargetFPS(0) gives us free-running render bound only by vsync. */
    SetTargetFPS(0);

    InitAudioDevice();
    if (!IsAudioDeviceReady()) {
        LOG_W("platform_init: audio device failed to open; continuing silently");
    }

    LOG_I("platform_init: %dx%d, vsync=%d", cfg->window_w, cfg->window_h, cfg->vsync);
    return true;
}

void platform_shutdown(void) {
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
