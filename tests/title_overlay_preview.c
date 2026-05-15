/*
 * tests/title_overlay_preview.c — visual smoke test for m6-ui-fixes.
 *
 * Stands up a real raylib window, runs `title_screen_run` for ~60
 * frames, and captures two PNGs:
 *   1. Plain title — verifies the SOLDUT banner + subtitle clear the
 *      button stack at 1080p (m6-ui-fixes title-spacing).
 *   2. Controls modal open — verifies the updated keybind table
 *      (m6-ui-fixes controls).
 *
 * Same shape as tests/host_overlay_preview.c — bypasses interaction
 * (the renderer is what we want to inspect), needs DISPLAY, not run
 * in CI.
 *
 * Run: make title-overlay-preview
 * PNGs land in build/shots/title_overlay_*.png
 */

#include "../src/game.h"
#include "../src/log.h"
#include "../src/lobby_ui.h"
#include "../src/platform.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    log_init("/tmp/title_overlay_preview.log");

    PlatformConfig cfg = {
        .window_w = 1920, .window_h = 1080,
        .vsync = false, .fullscreen = false,
        .title = "soldut - title-overlay preview",
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
    snprintf(ui.player_name, sizeof ui.player_name, "Ethan");

    const struct {
        int    frame;
        bool   show_controls;
        const char *out;
    } captures[] = {
        { 20, false, "build/shots/title_overlay_plain.png" },
        { 40, true,  "build/shots/title_overlay_controls.png" },
    };
    const int N = (int)(sizeof captures / sizeof captures[0]);
    int next = 0;

    int frame = 0;
    while (frame < 70 && !WindowShouldClose()) {
        if (next < N) ui.show_keybinds = captures[next].show_controls;

        BeginDrawing();
        title_screen_run(&ui, &game, 1920, 1080);
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
