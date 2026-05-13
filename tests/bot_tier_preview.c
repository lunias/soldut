/*
 * tests/bot_tier_preview.c — visual smoke test for the bot tier chip
 * in the lobby player list. Stands up a fake lobby with one human + 4
 * bots (Recruit / Veteran / Elite / Champion) and captures three PNGs
 * at HD, FHD, and QHD resolutions so we can verify the chip text is
 * legible at every supported size.
 *
 * Run: make bot-tier-preview
 * Output: build/shots/bot_tier_chip_{720,1080,1440}.png
 */

#include "../src/bot.h"
#include "../src/game.h"
#include "../src/log.h"
#include "../src/lobby.h"
#include "../src/lobby_ui.h"
#include "../src/match.h"
#include "../src/platform.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void seed_lobby(Game *game) {
    /* 1 human (host) + 1 of each bot tier. */
    LobbyState *L = &game->lobby;
    memset(L, 0, sizeof *L);

    /* Host slot. */
    LobbySlot *h = &L->slots[0];
    h->in_use  = true;
    h->peer_id = -1;
    h->mech_id = -1;
    h->is_host = true;
    h->team    = MATCH_TEAM_RED;
    h->loadout = mech_default_loadout();
    snprintf(h->name, sizeof h->name, "Ethan");

    /* Four bots — one per tier. */
    for (int t = 0; t < BOT_TIER_COUNT; ++t) {
        LobbySlot *s = &L->slots[t + 1];
        s->in_use   = true;
        s->peer_id  = -1;
        s->mech_id  = -1;
        s->is_bot   = true;
        s->bot_tier = (uint8_t)t;
        s->ready    = true;
        s->team     = (t & 1) ? MATCH_TEAM_BLUE : MATCH_TEAM_RED;
        bot_default_loadout_for_tier(t, (BotTier)t, &s->loadout);
        bot_name_for_index(t, (BotTier)t, s->name, (int)sizeof s->name);
    }
    L->slot_count = BOT_TIER_COUNT + 1;
    game->local_slot_id     = 0;
    game->match.phase       = MATCH_PHASE_LOBBY;
    game->match.mode        = MATCH_MODE_TDM;
    game->match.map_id      = 0;
    game->match.rounds_per_match = 3;
    game->world.local_mech_id = -1;
}

static void capture(int win_w, int win_h, const char *out_path) {
    PlatformConfig cfg = {
        .window_w = win_w, .window_h = win_h,
        .vsync = false, .fullscreen = false,
        .title = "soldut — bot-tier preview",
    };
    if (!platform_init(&cfg)) {
        fprintf(stderr, "platform_init failed for %dx%d\n", win_w, win_h);
        return;
    }

    Game game = {0};
    if (!game_init(&game)) {
        fprintf(stderr, "game_init failed for %dx%d\n", win_w, win_h);
        platform_shutdown();
        return;
    }
    seed_lobby(&game);

    LobbyUIState ui;
    lobby_ui_init(&ui);
    snprintf(ui.player_name, sizeof ui.player_name, "Ethan");

    /* Warm-up frames so font glyph atlas + scale settle, then capture. */
    for (int f = 0; f < 8 && !WindowShouldClose(); ++f) {
        BeginDrawing();
        lobby_screen_run(&ui, &game, win_w, win_h);
        EndDrawing();
    }
    /* raylib's TakeScreenshot on Linux writes to cwd regardless of
     * the path argument — drop the basename, then rename the file
     * into the intended build/shots/ destination. */
    const char *basename = out_path;
    for (const char *p = out_path; *p; ++p) if (*p == '/') basename = p + 1;
    TakeScreenshot(basename);
    (void)mkdir("build", 0755);
    (void)mkdir("build/shots", 0755);
    (void)rename(basename, out_path);
    printf("captured %s\n", out_path);

    /* Linger a few frames so the user can see the on-screen state if
     * running interactively. */
    for (int f = 0; f < 4 && !WindowShouldClose(); ++f) {
        BeginDrawing();
        lobby_screen_run(&ui, &game, win_w, win_h);
        EndDrawing();
    }

    game_shutdown(&game);
    platform_shutdown();
}

int main(void) {
    log_init("/tmp/bot_tier_preview.log");
    capture(1280, 720,  "build/shots/bot_tier_chip_720.png");
    capture(1920, 1080, "build/shots/bot_tier_chip_1080.png");
    capture(2560, 1440, "build/shots/bot_tier_chip_1440.png");
    log_shutdown();
    return 0;
}
