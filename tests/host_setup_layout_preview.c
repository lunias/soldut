/*
 * tests/host_setup_layout_preview.c — visual proof that the M6
 * round-shape redesign's new "Rounds / match" row fits cleanly in
 * the host setup screen without overlapping adjacent rows, the
 * Back / Start Hosting buttons, or the footer tip.
 *
 * Renders the screen at 1280x720 (sc=1.0) and 1920x1080 (sc=1.5) so
 * the DPI-aware layout is checked at both common breakpoints.
 *
 * Run: make host-setup-layout-preview ; ./build/host_setup_layout_preview
 * PNGs land at build/shots/host_setup_layout_{720,1080}.png
 */

#include "../src/game.h"
#include "../src/log.h"
#include "../src/lobby_ui.h"
#include "../src/platform.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <string.h>

static void capture(LobbyUIState *ui, Game *game, int w, int h,
                    const char *out)
{
    /* Tick once to settle hover state etc. */
    BeginDrawing();
    host_setup_screen_run(ui, game, w, h);
    EndDrawing();

    BeginDrawing();
    host_setup_screen_run(ui, game, w, h);
    EndDrawing();

    TakeScreenshot(out);
    printf("captured %s (%dx%d)\n", out, w, h);
}

int main(void) {
    log_init("/tmp/host_setup_layout_preview.log");

    PlatformConfig cfg = {
        .window_w = 1920, .window_h = 1080,
        .vsync = false, .fullscreen = false,
        .title = "soldut — host-setup layout preview",
    };
    if (!platform_init(&cfg)) {
        fprintf(stderr, "platform_init failed\n");
        return 1;
    }

    Game game = {0};
    if (!game_init(&game)) {
        fprintf(stderr, "game_init failed\n");
        platform_shutdown();
        return 1;
    }

    LobbyUIState ui;
    lobby_ui_init(&ui);

    /* Pre-seed setup_initialized so host_setup_screen_run doesn't
     * re-seed from g->config every frame, and so the new
     * `setup_rounds_per_match` field carries a sensible default. */
    ui.setup_initialized      = true;
    ui.setup_mode             = 0;      /* FFA */
    ui.setup_map_id           = 0;      /* foundry */
    ui.setup_score_limit      = 5;
    ui.setup_time_limit_s     = 600;
    ui.setup_rounds_per_match = 3;
    ui.setup_friendly_fire    = false;
    ui.host_starting          = false;  /* no overlay — full row layout visible */

    /* Capture both 720p (sc=1.0) and 1080p (sc=1.5) so the DPI-aware
     * panel math is verified at both common breakpoints. The window
     * stays open at 1080p; we re-render with smaller sw/sh values
     * for the 720p capture (raylib's window can be queried at any
     * resolution by the rendering code). */
    capture(&ui, &game, 1920, 1080,
            "build/shots/host_setup_layout_1080.png");

    /* For the 720p variant we shrink the window so the rendering
     * code's `ui_compute_scale` returns sc=1.0. */
    SetWindowSize(1280, 720);
    /* Let raylib digest the resize. */
    for (int i = 0; i < 5; ++i) {
        BeginDrawing();
        EndDrawing();
    }
    capture(&ui, &game, 1280, 720,
            "build/shots/host_setup_layout_720.png");

    game_shutdown(&game);
    platform_shutdown();
    log_shutdown();
    return 0;
}
