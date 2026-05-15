/*
 * tests/loadout_preview_overlay.c — visual smoke test for the M6
 * lobby-loadout-preview feature.
 *
 * Two capture passes:
 *
 * (1) Layout sweep — one lobby + one mid-modal shot at each of
 *     1280x720 / 1920x1080 / 2560x1440 / 3840x2160 to verify the UI
 *     scales without overlap at every supported resolution.
 *
 * (2) Detail pass — at 1920x1080, one shot per animation phase
 *     (IDLE-primary / WALK / SPRINT / JUMP / JET / CROUCH / PRONE /
 *     IDLE-secondary) on the Trooper, plus one JET-phase shot per
 *     jetpack type (STANDARD / BURST / GLIDE_WING / JUMP_JET) on the
 *     Heavy. The phase shots prove the new animations animate; the
 *     jetpack shots prove each jet type renders with its own colour
 *     and shape sourced from `g_jet_fx` in `mech_jet_fx.c`.
 *
 * Run: make loadout-preview-overlay
 * PNGs land in build/shots/loadout_preview_<bucket>.png.
 */

#include "../src/game.h"
#include "../src/log.h"
#include "../src/lobby.h"
#include "../src/lobby_ui.h"
#include "../src/loadout_preview.h"
#include "../src/match.h"
#include "../src/mech.h"
#include "../src/mech_sprites.h"
#include "../src/platform.h"
#include "../src/weapons.h"
#include "../src/weapon_sprites.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    bool        open_modal;
    int         chassis, primary, secondary, armor, jet;
    float       force_cycle_time;
    const char *out;
    const char *what;
} Scene;

static void apply_scene(LobbyUIState *ui, Game *game, const Scene *s) {
    ui->lobby_chassis   = s->chassis;
    ui->lobby_primary   = s->primary;
    ui->lobby_secondary = s->secondary;
    ui->lobby_armor     = s->armor;
    ui->lobby_jet       = s->jet;
    game->lobby.slots[0].loadout = (MechLoadout){
        .chassis_id   = s->chassis,
        .primary_id   = s->primary,
        .secondary_id = s->secondary,
        .armor_id     = s->armor,
        .jetpack_id   = s->jet,
    };
    if (s->open_modal && !loadout_preview_is_open(&ui->lp_state)) {
        loadout_preview_open(&ui->lp_state);
    }
    if (!s->open_modal && loadout_preview_is_open(&ui->lp_state)) {
        loadout_preview_close(&ui->lp_state);
    }
}

static void render_frame(LobbyUIState *ui, Game *game,
                         int sw, int sh, const Scene *scene)
{
    if (scene && loadout_preview_is_open(&ui->lp_state)) {
        ui->lp_state.cycle_time = scene->force_cycle_time;
    }
    BeginDrawing();
        lobby_screen_run(ui, game, sw, sh);
    EndDrawing();
}

static void capture(LobbyUIState *ui, Game *game, const Scene *s,
                    int sw, int sh)
{
    apply_scene(ui, game, s);
    for (int w = 0; w < 6; ++w) {
        lobby_ui_update_match_loading(ui, game);
        render_frame(ui, game, sw, sh, s);
    }
    lobby_ui_update_match_loading(ui, game);
    render_frame(ui, game, sw, sh, s);
    TakeScreenshot(s->out);
    printf("captured %-58s — %s (%dx%d)\n", s->out, s->what, sw, sh);
}

int main(void) {
    log_init("/tmp/loadout_preview_overlay.log");

    PlatformConfig cfg = {
        .window_w = 1280, .window_h = 720,
        .vsync = false, .fullscreen = false,
        .title = "soldut — loadout preview overlay",
    };
    if (!platform_init(&cfg)) {
        fprintf(stderr, "platform_init failed\n");
        return 1;
    }
    mech_sprites_load_all();
    weapons_atlas_load();

    Game game = {0};
    if (!game_init(&game)) {
        fprintf(stderr, "game_init failed\n");
        platform_shutdown();
        return 1;
    }

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
    game.lobby.slot_count       = 2;
    game.local_slot_id          = 0;
    game.match.phase            = MATCH_PHASE_LOBBY;
    game.match.mode             = MATCH_MODE_FFA;
    game.match.map_id           = 0;
    game.match.rounds_per_match = 3;
    game.world.local_mech_id    = -1;

    LobbyUIState ui;
    lobby_ui_init(&ui);
    snprintf(ui.player_name, sizeof ui.player_name, "Ethan");

    /* ---- (1) Resolution sweep -------------------------------------- */
    struct { int sw, sh; const char *label; } reses[] = {
        { 1280,  720, "720p"  },
        { 1920, 1080, "1080p" },
        { 2560, 1440, "1440p" },
        { 3840, 2160, "4k"    },
    };
    const int RN = (int)(sizeof reses / sizeof reses[0]);

    for (int ri = 0; ri < RN; ++ri) {
        SetWindowSize(reses[ri].sw, reses[ri].sh);
        for (int w = 0; w < 3; ++w) {
            BeginDrawing();
                ClearBackground((Color){0, 0, 0, 255});
            EndDrawing();
        }
        int aw = GetScreenWidth(), ah = GetScreenHeight();

        char path_lobby[160], path_modal[160];
        snprintf(path_lobby, sizeof path_lobby,
                 "build/shots/loadout_preview_%s_lobby.png", reses[ri].label);
        snprintf(path_modal, sizeof path_modal,
                 "build/shots/loadout_preview_%s_modal.png", reses[ri].label);

        Scene lobby_scene = {
            .open_modal = false,
            .chassis = CHASSIS_TROOPER, .primary = WEAPON_PULSE_RIFLE,
            .secondary = WEAPON_SIDEARM,
            .armor = ARMOR_LIGHT, .jet = JET_STANDARD,
            .force_cycle_time = 0.0f,
            .out = path_lobby,
            .what = "lobby (CHOOSE LOADOUT button + summary panel)",
        };
        capture(&ui, &game, &lobby_scene, aw, ah);

        Scene modal_scene = {
            .open_modal = true,
            .chassis = CHASSIS_SCOUT, .primary = WEAPON_PLASMA_SMG,
            .secondary = WEAPON_GRAPPLING_HOOK,
            .armor = ARMOR_NONE, .jet = JET_BURST,
            /* SPRINT phase — busy enough to show the layout under stress */
            .force_cycle_time = 5.0f,
            .out = path_modal,
            .what = "modal — Scout SPRINT (layout-only spot check)",
        };
        capture(&ui, &game, &modal_scene, aw, ah);
    }

    /* ---- (2) Detail pass at 1080p ---------------------------------- */
    SetWindowSize(1920, 1080);
    for (int w = 0; w < 3; ++w) {
        BeginDrawing(); ClearBackground((Color){0,0,0,255}); EndDrawing();
    }
    int dw = GetScreenWidth(), dh = GetScreenHeight();

    /* Phase showcase — Sniper holding Rail Cannon (primary) + Combat
     * Knife (secondary). Big size delta between the two weapons makes
     * the secondary-in-hand transition unmistakable.
     *
     * cycle_time values land mid-phase for the post-reorder timeline:
     *   IDLE_PRIMARY    0.0-2.0
     *   IDLE_SECONDARY  2.0-4.0
     *   WALK            4.0-6.0
     *   SPRINT          6.0-8.0
     *   JUMP            8.0-9.5
     *   JET             9.5-12.5
     *   CROUCH          12.5-14.0
     *   PRONE           14.0-16.0 */
    struct { float t; const char *name; const char *what; } phases[] = {
        {  1.0f, "phase_idle_primary",   "IDLE - rail cannon primary in hand"      },
        {  3.0f, "phase_idle_secondary", "IDLE - combat knife secondary in hand"   },
        {  5.0f, "phase_walk",           "WALK - gait cycle"                       },
        {  7.0f, "phase_sprint",         "SPRINT - faster gait"                    },
        {  8.7f, "phase_jump",           "JUMP - pelvis arc + FALL pose"           },
        { 11.0f, "phase_jet",            "JET - hover + plume"                     },
        { 13.0f, "phase_crouch",         "CROUCH - body lowered"                   },
        { 15.0f, "phase_prone",          "PRONE - body flat"                       },
    };
    const int PN = (int)(sizeof phases / sizeof phases[0]);

    for (int pi = 0; pi < PN; ++pi) {
        char p[160];
        snprintf(p, sizeof p, "build/shots/loadout_preview_%s.png",
                 phases[pi].name);
        Scene s = {
            .open_modal = true,
            .chassis = CHASSIS_SNIPER, .primary = WEAPON_RAIL_CANNON,
            .secondary = WEAPON_COMBAT_KNIFE,
            .armor = ARMOR_LIGHT, .jet = JET_STANDARD,
            .force_cycle_time = phases[pi].t,
            .out = p,
            .what = phases[pi].what,
        };
        capture(&ui, &game, &s, dw, dh);
    }

    /* Per-jetpack colour showcase. All in JET phase on the Heavy
     * (single nozzle, big plume area). cycle_time lands a bit past
     * the JET phase start so the ease-in is complete + the ignition
     * burst pattern is mid-cycle for JUMP_JET. */
    struct { int jet; const char *name; const char *what; } jets[] = {
        { JET_STANDARD,   "jet_standard",   "JET_STANDARD — blue ion plume"    },
        { JET_BURST,      "jet_burst",      "JET_BURST — orange/red double nozzle" },
        { JET_GLIDE_WING, "jet_glide_wing", "JET_GLIDE_WING — cyan intermittent"   },
        { JET_JUMP_JET,   "jet_jump_jet",   "JET_JUMP_JET — green ignition pop"    },
    };
    const int JN = (int)(sizeof jets / sizeof jets[0]);

    for (int ji = 0; ji < JN; ++ji) {
        char p[160];
        snprintf(p, sizeof p, "build/shots/loadout_preview_%s.png",
                 jets[ji].name);
        /* JET phase runs 9.5-12.5. Sample 11.0 for continuous plumes.
         * For JUMP_JET, sample 10.7 — that's exactly at the second
         * ignition-pop (start of cycle + 1.2s) AFTER the 0.3 s ease-in
         * has completed, so the green is at full brightness. */
        float t = 11.0f;
        if (jets[ji].jet == JET_JUMP_JET) t = 10.7f;
        Scene s = {
            .open_modal = true,
            .chassis = CHASSIS_HEAVY, .primary = WEAPON_MASS_DRIVER,
            .secondary = WEAPON_FRAG_GRENADES,
            .armor = ARMOR_HEAVY, .jet = jets[ji].jet,
            .force_cycle_time = t,
            .out = p,
            .what = jets[ji].what,
        };
        capture(&ui, &game, &s, dw, dh);
    }

    loadout_preview_shutdown(&ui.lp_state);
    game_shutdown(&game);
    platform_shutdown();
    log_shutdown();
    return 0;
}
