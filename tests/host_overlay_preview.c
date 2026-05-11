/*
 * tests/host_overlay_preview.c — visual smoke test for wan-fixes-9.
 *
 * Stands up a real raylib window, builds a minimal Game so
 * host_setup_screen_run has the config + level_arena it needs to draw,
 * sets ui.host_starting = true, ticks the screen for ~120 frames so
 * the indeterminate-bar sweeps a couple of cycles, and saves three
 * PNGs at known phases of the animation. Bypasses the actual
 * dedicated-child spawn — we want to verify the overlay LOOKS right,
 * not retest the network bootstrap.
 *
 * Run: make host-overlay-preview ; ./build/host_overlay_preview
 * PNGs land at build/shots/host_overlay_*.png
 */

#include "../src/game.h"
#include "../src/log.h"
#include "../src/lobby_ui.h"
#include "../src/platform.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    log_init("/tmp/host_overlay_preview.log");

    PlatformConfig cfg = {
        .window_w = 1280, .window_h = 720,
        .vsync = false, .fullscreen = false,
        .title = "soldut — host-overlay preview",
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
     * re-seed from g->config every frame. Pick a non-default mode +
     * map so the captured PNG shows non-trivial widget state under
     * the overlay. */
    ui.setup_initialized   = true;
    ui.setup_mode          = 1;             /* TDM */
    ui.setup_map_id        = 1;             /* second registry slot */
    ui.setup_score_limit   = 25;
    ui.setup_time_limit_s  = 300;
    ui.setup_friendly_fire = false;

    /* Make the overlay active. */
    ui.host_starting    = true;
    ui.host_starting_t0 = GetTime();
    snprintf(ui.host_starting_status, sizeof ui.host_starting_status,
             "Spawning dedicated server...");

    const struct {
        int    frame;
        double progress_seconds;
        const char *status;
        const char *out;
    } captures[] = {
        {  15, 0.20, "Spawning dedicated server...",  "build/shots/host_overlay_t0_spawning.png"  },
        {  60, 0.80, "Connecting to server...",       "build/shots/host_overlay_t1_connecting.png"},
        { 100, 1.60, "Waiting for server to accept...","build/shots/host_overlay_t2_waiting.png" },
    };
    const int N = (int)(sizeof captures / sizeof captures[0]);
    int next = 0;

    int frame = 0;
    while (frame < 130 && !WindowShouldClose()) {
        /* Lie about t0 so the indeterminate-bar position depends on
         * the current synthetic elapsed value we want to capture.
         * Real run uses GetTime() - t0; we feed t0 = now - desired. */
        if (next < N) {
            ui.host_starting_t0 = GetTime() - captures[next].progress_seconds;
            snprintf(ui.host_starting_status,
                     sizeof ui.host_starting_status, "%s",
                     captures[next].status);
        }

        BeginDrawing();
        host_setup_screen_run(&ui, &game, 1280, 720);
        host_setup_screen_draw_overlay(&ui, 1280, 720);
        EndDrawing();

        if (next < N && frame == captures[next].frame) {
            TakeScreenshot(captures[next].out);
            printf("captured %s\n", captures[next].out);
            ++next;
        }
        ++frame;
    }

    game_shutdown(&game);
    platform_shutdown();
    log_shutdown();
    return 0;
}
