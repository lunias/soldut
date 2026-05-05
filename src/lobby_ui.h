#pragma once

#include "lobby.h"
#include "match.h"
#include "ui.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * lobby_ui — title, server browser, connect, lobby, and round summary
 * screens. All built on the small immediate-mode helpers in ui.h. The
 * lobby UI sees Game *g; it READs g->lobby / g->match / g->net /
 * g->config, and WRITEs g->mode, g->pending_host, g->pending_port and
 * a few local UI-state knobs we keep on LobbyUIState.
 *
 * Per-screen entry points return nothing; transitions happen by
 * mutating g->mode or by setting flags the run loop checks.
 */

struct Game;

#define LOBBY_UI_CHAT_DRAFT_BYTES  192
#define LOBBY_UI_CONNECT_BYTES     64
#define LOBBY_UI_NAME_BYTES        24

typedef struct LobbyUIState {
    UIContext  ui;

    /* Browser. */
    int        browser_selected;
    float      browser_refresh_timer;     /* auto-refreshes every 5 s */

    /* Connect screen text input. */
    char       connect_addr[LOBBY_UI_CONNECT_BYTES];

    /* Lobby loadout-picker selections — these mirror the local slot's
     * loadout but are owned by the UI so we can tell when the user
     * actually changes a value (and ship a LOBBY_LOADOUT message). */
    int        lobby_chassis;     /* ChassisId */
    int        lobby_primary;     /* WeaponId (primary range) */
    int        lobby_secondary;
    int        lobby_armor;
    int        lobby_jet;
    int        lobby_team;        /* MATCH_TEAM_* */
    int        lobby_loadout_synced;   /* 0 until we've seen a server slot for ourselves */

    /* Chat draft (lobby chat input field). */
    char       chat_draft[LOBBY_UI_CHAT_DRAFT_BYTES];

    /* User-set display name (carries through host/connect flows). */
    char       player_name[LOBBY_UI_NAME_BYTES];

    /* For the title screen: a one-shot flag that main.c reads + clears. */
    bool       request_quit;
    bool       request_host;
    bool       request_browse;
    bool       request_connect;       /* with pending_host / pending_port */
    bool       request_single_player;

    /* Host-setup screen: working draft of the match config. Initialized
     * from g->config when entering MODE_HOST_SETUP. main.c applies the
     * draft to g->config when the user clicks "Start Hosting". */
    int        setup_mode;            /* MatchModeId */
    int        setup_map_id;          /* MapId — re-chosen if mode-mask incompatible */
    int        setup_score_limit;
    int        setup_time_limit_s;    /* int seconds for the +/- stepper */
    bool       setup_friendly_fire;
    bool       setup_initialized;     /* false until first entry → seeds from g->config */
    bool       request_start_host;    /* one-shot: setup confirmed → main.c bootstraps */
} LobbyUIState;

void lobby_ui_init(LobbyUIState *L);

/* Sample input + render the relevant screen. Caller handles raylib
 * Begin/EndDrawing wrapping. */
void title_screen_run     (LobbyUIState *L, struct Game *g, int sw, int sh);
void host_setup_screen_run(LobbyUIState *L, struct Game *g, int sw, int sh);
void browser_screen_run   (LobbyUIState *L, struct Game *g, int sw, int sh);
void connect_screen_run   (LobbyUIState *L, struct Game *g, int sw, int sh);
void lobby_screen_run     (LobbyUIState *L, struct Game *g, int sw, int sh);
void summary_screen_run   (LobbyUIState *L, struct Game *g, int sw, int sh);

/* In-match overlay (score / timer at the top of the screen, plus the
 * usual HUD which the existing render path already draws). */
void match_overlay_draw (struct Game *g, int sw, int sh);
