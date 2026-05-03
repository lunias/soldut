#pragma once

#include "input.h"
#include "math.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Thin wrapper around raylib's rcore. The rest of the codebase shouldn't
 * care that we're sitting on raylib — it talks to platform_* and gets
 * windows, time, and an input snapshot. We let raylib own the GL context
 * and the audio engine because any 2D project that doesn't is reinventing
 * the parts of raylib that work.
 */

typedef struct {
    int   window_w, window_h;
    bool  vsync;
    bool  fullscreen;
    const char *title;
} PlatformConfig;

typedef struct {
    int   window_w, window_h;
    int   render_w, render_h;
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
