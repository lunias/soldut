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
    /* wan-fixes-7 — "have we PUSHED our cached UI draft to the
     * current server" tracker. After disconnect + re-host, the UI's
     * loadout cache survives but the new dedicated server starts
     * everyone at defaults; without pushing, the draft is visible in
     * the lobby UI but the round spawns with defaults. Reset to 0
     * by lobby_ui_reset_session on disconnect / leave so the lobby's
     * first sync re-publishes the cached draft to the new server. */
    int        lobby_loadout_pushed;

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

    /* P09 — host kick/ban confirmation modal. -1 = no modal showing.
     * The lobby player-list row writes one of these when the host
     * clicks [Kick] / [Ban]; the modal is rendered + dismissed in
     * lobby_screen_run after the list iteration. */
    int        kick_target_slot;
    int        ban_target_slot;

    /* P09 — title-screen keybinds modal. Toggle via the About button. */
    bool       show_keybinds;
} LobbyUIState;

void lobby_ui_init(LobbyUIState *L);

/* wan-fixes-7 — clear session-scoped UI state so the next host /
 * connect attempt starts fresh on the wire side. Preserves player_name
 * + cached loadout/team drafts so the user doesn't re-pick what they
 * already chose, but flips the "pushed to server" + "synced from
 * server" trackers so the next lobby entry pushes the cached draft
 * to the new server. Called from every disconnect/leave path in
 * main.c. */
void lobby_ui_reset_session(LobbyUIState *L);

/* wan-fixes-8 — write the current UI draft (player_name + loadout +
 * team + connect_addr) to soldut-prefs.cfg. Idempotent; safe to call
 * from any commit point (title screen transitions, lobby cycle
 * clicks, direct-connect commit). The lobby UI calls this internally
 * after loadout/team changes; main.c calls it on title-screen
 * transitions to capture name edits. */
void lobby_ui_save_prefs(const LobbyUIState *L);

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
