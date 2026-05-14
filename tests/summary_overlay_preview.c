/*
 * tests/summary_overlay_preview.c — visual smoke test for the summary
 * screen's map-vote picker, exercised at the resolutions the shipped
 * game actually runs at. Verifies thumbnails render in the three
 * vote cards.
 *
 * Stands up a real raylib window, fakes a finished round, arms a map
 * vote with three real registry entries, and runs `summary_screen_run`.
 * Captures PNGs into build/shots/summary_overlay_*.png.
 *
 * Each capture exercises a different path through `map_thumb_get`:
 *   1. Sidecar PNGs present (assets/maps/<short>_thumb.png) — the host
 *      / bundled-asset fast path.
 *   2. Sidecars temporarily renamed → forces the .lvl THMB-lump
 *      fallback, the path downloaded maps take.
 *
 * Run: make summary-overlay-preview
 */

#include "../src/game.h"
#include "../src/log.h"
#include "../src/lobby.h"
#include "../src/lobby_ui.h"
#include "../src/maps.h"
#include "../src/match.h"
#include "../src/platform.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int find_map_id(const char *short_name) {
    for (int i = 0; i < g_map_registry.count; ++i) {
        if (strcasecmp(g_map_registry.entries[i].short_name,
                       short_name) == 0) return i;
    }
    return -1;
}

static void rename_pair(const char *a, const char *b) {
    if (rename(a, b) != 0) {
        fprintf(stderr, "rename(%s, %s) failed\n", a, b);
    }
}

/* raylib's TakeScreenshot writes to cwd regardless of path argument;
 * mirror bot_tier_preview's workaround. */
static void capture_to(const char *out_path) {
    const char *basename = out_path;
    for (const char *p = out_path; *p; ++p) if (*p == '/') basename = p + 1;
    TakeScreenshot(basename);
    (void)mkdir("build", 0755);
    (void)mkdir("build/shots", 0755);
    (void)rename(basename, out_path);
    printf("captured %s\n", out_path);
}

int main(void) {
    log_init("/tmp/summary_overlay_preview.log");

    PlatformConfig cfg = {
        .window_w = 1920, .window_h = 1080,
        .vsync = false, .fullscreen = false,
        .title = "soldut — summary-overlay preview",
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

    int a = find_map_id("foundry");
    int b = find_map_id("concourse");
    int c = find_map_id("citadel");
    if (a < 0 || b < 0 || c < 0) {
        fprintf(stderr, "registry missing one of foundry/concourse/citadel\n");
        return 1;
    }

    game.lobby.slots[0] = (LobbySlot){
        .in_use = true, .peer_id = -1, .mech_id = -1,
        .team = MATCH_TEAM_FFA, .is_host = true,
        .loadout = mech_default_loadout(),
    };
    snprintf(game.lobby.slots[0].name, sizeof game.lobby.slots[0].name, "HostA");
    game.lobby.slots[1] = (LobbySlot){
        .in_use = true, .peer_id = -1, .mech_id = -1,
        .team = MATCH_TEAM_FFA, .is_host = false,
        .loadout = mech_default_loadout(),
    };
    snprintf(game.lobby.slots[1].name, sizeof game.lobby.slots[1].name, "ClientB");
    game.lobby.slot_count    = 2;
    game.local_slot_id       = 0;
    game.match.phase         = MATCH_PHASE_SUMMARY;
    game.match.mode          = MATCH_MODE_FFA;
    game.match.map_id        = a;
    game.match.rounds_per_match = 3;
    game.match.summary_remaining = 5.0f;
    game.world.local_mech_id = -1;

    lobby_vote_start(&game.lobby, a, b, c, 5.0f);

    LobbyUIState ui;
    lobby_ui_init(&ui);
    snprintf(ui.player_name, sizeof ui.player_name, "HostA");

    /* Render a few warm-up frames so any one-shot init lands. */
    for (int i = 0; i < 4; ++i) {
        BeginDrawing();
        ClearBackground((Color){4, 6, 10, 255});
        summary_screen_run(&ui, &game, 1920, 1080);
        EndDrawing();
    }

    BeginDrawing();
    ClearBackground((Color){4, 6, 10, 255});
    summary_screen_run(&ui, &game, 1920, 1080);
    EndDrawing();
    capture_to("build/shots/summary_overlay_sidecar.png");

    /* Force the .lvl THMB-lump fallback by temporarily hiding the
     * sidecar PNGs. */
    const char *paths[3] = {
        "assets/maps/foundry_thumb.png",
        "assets/maps/concourse_thumb.png",
        "assets/maps/citadel_thumb.png",
    };
    const char *hidden[3] = {
        "assets/maps/foundry_thumb.png.preview-hidden",
        "assets/maps/concourse_thumb.png.preview-hidden",
        "assets/maps/citadel_thumb.png.preview-hidden",
    };
    for (int i = 0; i < 3; ++i) rename_pair(paths[i], hidden[i]);

    /* Bust the in-process cache so map_thumb_get reruns from scratch
     * and exercises the .lvl path. lobby_ui owns the cache as static
     * state in its TU — exposed via a debug helper. */
    extern void lobby_ui_clear_thumb_cache(void);
    lobby_ui_clear_thumb_cache();

    for (int i = 0; i < 4; ++i) {
        BeginDrawing();
        ClearBackground((Color){4, 6, 10, 255});
        summary_screen_run(&ui, &game, 1920, 1080);
        EndDrawing();
    }
    BeginDrawing();
    ClearBackground((Color){4, 6, 10, 255});
    summary_screen_run(&ui, &game, 1920, 1080);
    EndDrawing();
    capture_to("build/shots/summary_overlay_thmb_lump.png");

    /* Restore the sidecars. */
    for (int i = 0; i < 3; ++i) rename_pair(hidden[i], paths[i]);

    game_shutdown(&game);
    platform_shutdown();
    log_shutdown();
    return 0;
}
