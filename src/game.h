#pragma once

#include "arena.h"
#include "config.h"
#include "hash.h"
#include "input.h"
#include "lobby.h"
#include "match.h"
#include "net.h"
#include "reconcile.h"
#include "world.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * `Game` is the spine. One process, one Game. Pointers to subsystems
 * we'll add at later milestones live here; at M1 it owns the allocators,
 * the RNG, and the World.
 *
 * See [09-codebase-architecture.md].
 */

typedef enum {
    MODE_BOOT = 0,    /* before init has completed */
    MODE_TITLE,       /* main menu (Single Player / Host / Browse / Connect / Quit) */
    MODE_HOST_SETUP,  /* pre-lobby: host picks mode / map / limits / friendly-fire */
    MODE_BROWSER,     /* server browser screen */
    MODE_CONNECT,     /* enter host:port to direct-connect */
    MODE_CONNECTING,  /* enet/handshake in flight */
    MODE_LOBBY,       /* lobby UI; no match running */
    MODE_COUNTDOWN,   /* pre-round briefing (5 s) */
    MODE_MATCH,       /* in-round simulation */
    MODE_SUMMARY,     /* round summary screen */
    MODE_QUIT,        /* shutdown requested */
} GameMode;

typedef struct Game {
    Arena permanent;
    Arena level_arena;
    Arena frame_arena;

    /* The world's RNG. Single owner so simulate() stays a pure function. */
    pcg32_t rng;

    GameMode mode;
    uint64_t tick;          /* monotonic simulation tick counter */
    double   time_seconds;  /* wall clock since process start */

    /* Most recent input sampled by the platform layer. */
    ClientInput input;

    /* The simulation. */
    World world;

    /* Networking. M2 lives here. NET_ROLE_OFFLINE for single-player; set
     * by main.c after parsing CLI flags. */
    NetState net;

    /* Client-side prediction + reconciliation buffer. Only used when
     * net.role == NET_ROLE_CLIENT. The host doesn't predict (server is
     * authoritative on the same process). */
    Reconcile reconcile;

    /* M4 — lobby + match flow. The server owns these; clients receive
     * both via reliable LOBBY-channel messages (lobby slot table + a
     * compact MatchState delta on round transitions). */
    LobbyState   lobby;
    MatchState   match;
    ServerConfig config;

    /* Index of the local player's lobby slot (host's own slot on the
     * server; assigned slot on a client). -1 outside a lobby. */
    int          local_slot_id;

    /* Round counter — incremented each time the server transitions out
     * of MATCH_PHASE_SUMMARY. Drives map / mode rotation. */
    int          round_counter;

    /* Pending direct-connect target (host:port) entered on the connect
     * screen. Filled by the UI; consumed by main.c. */
    char         pending_host[64];
    uint16_t     pending_port;

    /* "Single-player auto-host" hint — set by the title screen when the
     * user clicks "Single Player". main.c bootstraps a self-hosted
     * server with auto-start enabled. */
    bool         auto_start_single_player;

    /* "Host the match without networking" — used by the offline path
     * so the lobby + match flow runs even with no peers. */
    bool         offline_solo;

    /* M5 P04 — editor F5 test-play. When non-empty, both the initial
     * bootstrap_host and every start_round call load this .lvl file
     * directly (via map_build_from_path) instead of going through the
     * MapId rotation. Set in main.c when --test-play <path> is parsed,
     * cleared at process exit. The path is absolute (the editor runs
     * realpath() before forking us). */
    char         test_play_lvl[256];
} Game;

bool game_init(Game *g);
void game_shutdown(Game *g);

/* Called once per render frame, after the platform has sampled input.
 * At M0 this just bumps tick and clears the frame arena. */
void game_step(Game *g, double dt);
