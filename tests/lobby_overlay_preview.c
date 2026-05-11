/*
 * tests/lobby_overlay_preview.c — visual smoke test for wan-fixes-11.
 *
 * Stands up a real raylib window and captures three PNGs:
 *   1. Lobby UI with not-ready Ready button (pulse + bright green).
 *   2. Lobby UI with ready=true Ready button (confirmed darker green).
 *   3. Match-start loading overlay (the new one we wired into
 *      lobby_ui.c).
 *
 * Bypasses networking — we hand-set the relevant LobbyUIState +
 * Lobby slots so the renderer paints the new buttons + overlay
 * without spinning up an actual match.
 *
 * Run: make lobby-overlay-preview
 * PNGs land in build/shots/lobby_overlay_*.png
 */

#include "../src/game.h"
#include "../src/log.h"
#include "../src/lobby.h"
#include "../src/lobby_ui.h"
#include "../src/match.h"
#include "../src/platform.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    log_init("/tmp/lobby_overlay_preview.log");

    PlatformConfig cfg = {
        .window_w = 1280, .window_h = 720,
        .vsync = false, .fullscreen = false,
        .title = "soldut — lobby-overlay preview",
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

    /* Stand up a minimal lobby so lobby_screen_run has something to
     * render — one host slot, one joiner slot, both Playing on FFA. */
    game.lobby.slots[0] = (LobbySlot){
        .in_use = true, .peer_id = -1, .mech_id = -1,
        .team = MATCH_TEAM_FFA, .is_host = true,
        .loadout = mech_default_loadout(),
        .ready = false,
    };
    snprintf(game.lobby.slots[0].name, sizeof game.lobby.slots[0].name, "Ethan");
    game.lobby.slots[1] = (LobbySlot){
        .in_use = true, .peer_id = -1, .mech_id = -1,
        .team = MATCH_TEAM_FFA, .is_host = false,
        .loadout = mech_default_loadout(),
        .ready = true,
    };
    snprintf(game.lobby.slots[1].name, sizeof game.lobby.slots[1].name, "Friend");
    game.lobby.slot_count    = 2;
    game.local_slot_id       = 0;
    game.match.phase         = MATCH_PHASE_LOBBY;
    game.match.mode          = MATCH_MODE_FFA;
    game.match.map_id        = 0;
    game.match.rounds_per_match = 3;
    game.world.local_mech_id = -1;

    LobbyUIState ui;
    lobby_ui_init(&ui);
    snprintf(ui.player_name, sizeof ui.player_name, "Ethan");

    const struct {
        int    frame;
        bool   slot0_ready;
        bool   match_loading;
        const char *out;
    } captures[] = {
        {  20, false, false, "build/shots/lobby_overlay_not_ready.png" },
        {  40, true,  false, "build/shots/lobby_overlay_ready.png" },
        {  80, true,  true,  "build/shots/lobby_overlay_match_loading.png" },
    };
    const int N = (int)(sizeof captures / sizeof captures[0]);
    int next = 0;

    int frame = 0;
    while (frame < 110 && !WindowShouldClose()) {
        bool capturing = (next < N);
        if (capturing) {
            game.lobby.slots[0].ready = captures[next].slot0_ready;
            if (captures[next].match_loading) {
                /* Pretend we're in the last sliver of countdown so
                 * lobby_ui_update_match_loading flips match_loading
                 * on. */
                game.match.phase             = MATCH_PHASE_COUNTDOWN;
                game.match.countdown_remaining = 0.4f;
                ui.match_loading_t0          = GetTime() - 1.2;
            } else {
                game.match.phase             = MATCH_PHASE_LOBBY;
                game.match.countdown_remaining = 0.0f;
            }
        }

        lobby_ui_update_match_loading(&ui, &game);

        BeginDrawing();
        lobby_screen_run(&ui, &game, 1280, 720);
        if (ui.match_loading) {
            match_loading_overlay_draw(&ui, &game, 1280, 720);
        }
        EndDrawing();

        if (capturing && frame == captures[next].frame) {
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
