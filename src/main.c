#include "config.h"
#include "decal.h"
#include "game.h"
#include "level.h"
#include "lobby.h"
#include "lobby_ui.h"
#include "log.h"
#include "maps.h"
#include "match.h"
#include "mech.h"
#include "net.h"
#include "platform.h"
#include "reconcile.h"
#include "render.h"
#include "shotmode.h"
#include "simulate.h"
#include "snapshot.h"
#include "version.h"
#include "weapons.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/*
 * Entry point.
 *
 * M4 game flow:
 *   ./soldut                      title screen → user picks
 *   ./soldut --host [PORT]        skip title; host server, sit in lobby
 *   ./soldut --connect HOST[:P]   skip title; connect to server, sit in lobby
 *   ./soldut --shot scripts/x     scripted scene → PNGs (legacy)
 *
 * The simulation step is the same in every mode; the wrapping state
 * machine in this file decides whether we're showing the title screen,
 * a server browser, the lobby, an active round, or a summary.
 */

#define SIM_HZ        60
static const double TICK_DT = 1.0 / (double)SIM_HZ;
#define MAX_FRAME_DT  0.25

typedef enum {
    LAUNCH_OFFLINE = 0,
    LAUNCH_HOST,
    LAUNCH_CLIENT,
} LaunchMode;

typedef struct {
    LaunchMode  mode;
    uint16_t    port;
    char        host[64];
    char        name[24];
    char        chassis[16];
    char        primary[24];
    char        secondary[24];
    char        armor[16];
    char        jetpack[16];
    char        test_play_lvl[256];   /* M5 P04 — editor F5 hands us a .lvl path */
    bool        friendly_fire;
    bool        skip_title;
    bool        ff_set;
} LaunchArgs;

static void parse_args(int argc, char **argv, LaunchArgs *out) {
    memset(out, 0, sizeof *out);
    out->port = SOLDUT_DEFAULT_PORT;
    snprintf(out->name, sizeof out->name, "player");
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0) {
            out->mode = LAUNCH_HOST;
            out->skip_title = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                out->port = (uint16_t)atoi(argv[++i]);
                if (out->port == 0) out->port = SOLDUT_DEFAULT_PORT;
            }
        }
        else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            out->mode = LAUNCH_CLIENT;
            out->skip_title = true;
            const char *s = argv[++i];
            const char *colon = strrchr(s, ':');
            if (colon) {
                size_t hl = (size_t)(colon - s);
                if (hl >= sizeof out->host) hl = sizeof out->host - 1;
                memcpy(out->host, s, hl); out->host[hl] = '\0';
                int port = atoi(colon + 1);
                out->port = (uint16_t)(port > 0 ? port : SOLDUT_DEFAULT_PORT);
            } else {
                snprintf(out->host, sizeof out->host, "%s", s);
                out->port = SOLDUT_DEFAULT_PORT;
            }
        }
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            snprintf(out->name, sizeof out->name, "%s", argv[++i]);
        }
        /* M3 loadout flags — pre-fill the lobby loadout for the local
         * mech; the lobby UI still lets the user retoss. */
        else if (strcmp(argv[i], "--chassis") == 0 && i + 1 < argc) {
            snprintf(out->chassis, sizeof out->chassis, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--primary") == 0 && i + 1 < argc) {
            snprintf(out->primary, sizeof out->primary, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--secondary") == 0 && i + 1 < argc) {
            snprintf(out->secondary, sizeof out->secondary, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--armor") == 0 && i + 1 < argc) {
            snprintf(out->armor, sizeof out->armor, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--jetpack") == 0 && i + 1 < argc) {
            snprintf(out->jetpack, sizeof out->jetpack, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--ff") == 0) {
            out->friendly_fire = true; out->ff_set = true;
        }
        /* M5 P04 — editor F5 test-play. Boots into an offline-solo round
         * on the supplied .lvl file. We force LAUNCH_HOST + skip_title;
         * the main flow detects test_play_lvl[0] and switches to offline
         * + FFA + 1 s auto-start. */
        else if (strcmp(argv[i], "--test-play") == 0 && i + 1 < argc) {
            snprintf(out->test_play_lvl, sizeof out->test_play_lvl, "%s", argv[++i]);
            out->mode = LAUNCH_HOST;
            out->skip_title = true;
        }
    }
}

static int resolve_weapon_id(const char *name, int default_id) {
    if (!name || !*name) return default_id;
    for (int i = 0; i < 32; ++i) {
        const char *n = weapon_short_name(i);
        if (!n || strcmp(n, "?") == 0) continue;
        size_t L = strlen(name);
        bool ok = true;
        for (size_t k = 0; k < L && n[k]; ++k) {
            char a = name[k]; if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            char b = n[k];    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) { ok = false; break; }
        }
        if (ok) return i;
    }
    LOG_W("resolve_weapon_id: unknown '%s' — defaulting", name);
    return default_id;
}

static int resolve_armor_id(const char *name, int default_id) {
    if (!name || !*name) return default_id;
    for (int i = 0; i < 8; ++i) {
        const Armor *a = armor_def(i);
        if (!a || !a->name) continue;
        if (strcasecmp(name, a->name) == 0) return i;
    }
    return default_id;
}

static int resolve_jetpack_id(const char *name, int default_id) {
    if (!name || !*name) return default_id;
    for (int i = 0; i < 8; ++i) {
        const Jetpack *j = jetpack_def(i);
        if (!j || !j->name) continue;
        if (strcasecmp(name, j->name) == 0) return i;
    }
    return default_id;
}

/* ---- Match flow controller (host-side) --------------------------- */

/* Walk newly-recorded killfeed entries and credit the lobby slots. We
 * track how many we've consumed so subsequent ticks pick up only the
 * new ones. */
static int g_killfeed_processed  = 0;
static int g_hitfeed_processed   = 0;
static int g_firefeed_processed  = 0;

/* Server: walk new hit events from the world's hitfeed queue and
 * broadcast each one to clients so they can spawn matching blood/spark
 * FX (without this, the client falls back to chest-pos blood from
 * snapshot health-decrease which renders visibly different). */
static void broadcast_new_hits(Game *g) {
    if (g->net.role != NET_ROLE_SERVER) return;
    int cur = g->world.hitfeed_count;
    int begin = g_hitfeed_processed;
    if (cur - begin > HITFEED_CAPACITY) {
        /* Fell behind by more than the ring; skip the lost ones. */
        begin = cur - HITFEED_CAPACITY;
    }
    for (int n = begin; n < cur; ++n) {
        int idx = n % HITFEED_CAPACITY;
        if (idx < 0) idx += HITFEED_CAPACITY;
        const HitFeedEntry *h = &g->world.hitfeed[idx];
        net_server_broadcast_hit(&g->net,
            (int)h->victim_mech_id, (int)h->hit_part,
            h->pos_x, h->pos_y, h->dir_x, h->dir_y, (int)h->damage);
    }
    g_hitfeed_processed = cur;
}

/* Server: walk new fire events and broadcast so clients can spawn
 * matching tracer (hitscan) or visual-only projectile (everything
 * else). Without this, remote players' shots are invisible on the
 * client — only the local shooter's predict path puts FX on screen. */
static void broadcast_new_fires(Game *g) {
    if (g->net.role != NET_ROLE_SERVER) return;
    int cur = g->world.firefeed_count;
    int begin = g_firefeed_processed;
    if (cur - begin > FIREFEED_CAPACITY) {
        begin = cur - FIREFEED_CAPACITY;
    }
    for (int n = begin; n < cur; ++n) {
        int idx = n % FIREFEED_CAPACITY;
        if (idx < 0) idx += FIREFEED_CAPACITY;
        const FireFeedEntry *f = &g->world.firefeed[idx];
        net_server_broadcast_fire(&g->net,
            (int)f->shooter_mech_id, (int)f->weapon_id,
            f->origin_x, f->origin_y, f->dir_x, f->dir_y);
    }
    g_firefeed_processed = cur;
}

static void apply_new_kills(Game *g) {
    /* killfeed_count is a monotonic counter; killfeed[] is a CAP-sized
     * ring. If we've fallen behind by more than the ring capacity, we
     * silently skip the missed kills (they're scoreboard only). */
    int cur = g->world.killfeed_count;
    int begin = g_killfeed_processed;
    if (cur - begin > KILLFEED_CAPACITY) {
        begin = cur - KILLFEED_CAPACITY;
    }
    bool any = false;
    for (int n = begin; n < cur; ++n) {
        int idx = n % KILLFEED_CAPACITY;
        if (idx < 0) idx += KILLFEED_CAPACITY;
        const KillFeedEntry *k = &g->world.killfeed[idx];
        int killer_slot = (k->killer_mech_id >= 0)
            ? lobby_find_slot_by_mech(&g->lobby, k->killer_mech_id) : -1;
        int victim_slot = lobby_find_slot_by_mech(&g->lobby, k->victim_mech_id);
        match_apply_kill(&g->match, &g->lobby, killer_slot, victim_slot, k->flags);
        net_server_broadcast_kill(&g->net, k->killer_mech_id,
                                   k->victim_mech_id, k->weapon_id);
        any = true;
    }
    g_killfeed_processed = cur;
    /* Slot scores changed → reship the lobby table on the next net_poll
     * so client scoreboards stay current and the summary MVP is right. */
    if (any) g->lobby.dirty = true;
}

static void start_round(Game *g) {
    /* Pick map + mode from rotation. */
    g->match.map_id = config_pick_map (&g->config, g->round_counter);
    g->match.mode   = config_pick_mode(&g->config, g->round_counter);
    /* Re-derive limits from config to account for mode-rotation. */
    g->match.score_limit  = g->config.score_limit;
    g->match.time_limit   = g->config.time_limit;
    g->match.friendly_fire= g->config.friendly_fire;
    /* FFA mode: every player is on team 1 (MATCH_TEAM_FFA aliases
     * MATCH_TEAM_RED). The friendly-fire check in mech_apply_damage
     * compares teams and drops same-team hits when ff is off — so
     * with FFA + ff=off, NO damage ever lands (every kill request
     * gets dropped because shooter and victim are same-team). Force
     * ff on for FFA so hits register; ff toggle remains meaningful
     * for TDM/CTF where teams are distinct. */
    g->world.friendly_fire= g->config.friendly_fire ||
                            (g->match.mode == MATCH_MODE_FFA);

    /* Build map. M5 P04: in test-play mode, ignore the rotation and
     * reload the scratch .lvl so successive rounds keep using the
     * editor's map. */
    arena_reset(&g->level_arena);
    if (g->test_play_lvl[0]) {
        map_build_from_path(&g->world, &g->level_arena, g->test_play_lvl);
    } else {
        map_build((MapId)g->match.map_id, &g->world, &g->level_arena);
    }
    decal_init((int)level_width_px(&g->world.level),
               (int)level_height_px(&g->world.level));

    /* Spawn mechs for every active slot. lobby_spawn_round_mechs sets
     * each slot's mech_id; mark the table dirty so the next net_poll
     * iteration broadcasts the updated mapping to clients. The client
     * uses slot.mech_id to resolve its own world.local_mech_id —
     * without this it stays at -1, the camera doesn't follow, and the
     * client renders a black screen. */
    lobby_spawn_round_mechs(&g->lobby, &g->world,
                            g->match.map_id, g->local_slot_id,
                            g->match.mode);
    g->lobby.dirty = true;

    match_begin_round(&g->match);
    g->mode = MODE_MATCH;
    g_killfeed_processed = g->world.killfeed_count;

    if (g->net.role == NET_ROLE_SERVER) {
        /* Order: ship the lobby table first so clients have mech_id
         * mappings *before* ROUND_START and the snapshot stream. Both
         * are reliable+ordered on NET_CH_LOBBY. */
        net_server_broadcast_lobby_list  (&g->net, &g->lobby);
        g->lobby.dirty = false;
        net_server_broadcast_round_start (&g->net, &g->match);
    }
    LOG_I("match_flow: round %d begin (mode=%s map=%s)",
          g->round_counter,
          match_mode_name(g->match.mode),
          map_def(g->match.map_id)->display_name);
}

static void end_round(Game *g) {
    match_end_round(&g->match, &g->lobby);
    g->mode = MODE_SUMMARY;
    /* Snapshots stop flowing now (gated in net_poll on
     * MATCH_PHASE_ACTIVE), so corpses freeze nicely.
     *
     * Order matters here: ship the lobby table first (carries final
     * per-slot scores) so the round-end broadcast that follows lands
     * with the client already holding accurate score data — it can
     * compute MVP locally if mvp_slot didn't ride the wire. Both are
     * reliable+ordered on NET_CH_LOBBY. */
    if (g->net.role == NET_ROLE_SERVER) {
        net_server_broadcast_lobby_list(&g->net, &g->lobby);
        g->lobby.dirty = false;
        net_server_broadcast_round_end (&g->net, &g->match);
    }
    LOG_I("match_flow: round %d end (mvp=%d)", g->round_counter, g->match.mvp_slot);
}

static void begin_next_lobby(Game *g) {
    g->round_counter++;
    /* Pre-stage next round's mode/map for the lobby UI. */
    g->match.map_id = config_pick_map (&g->config, g->round_counter);
    g->match.mode   = config_pick_mode(&g->config, g->round_counter);
    g->match.score_limit  = g->config.score_limit;
    g->match.time_limit   = g->config.time_limit;
    g->match.friendly_fire= g->config.friendly_fire;
    g->match.phase        = MATCH_PHASE_LOBBY;
    /* Tear down mechs; lobby drawing doesn't need them. */
    lobby_clear_round_mechs(&g->lobby, &g->world);
    /* Reset round stats + ready flags. */
    lobby_reset_round_stats(&g->lobby);
    g->mode = MODE_LOBBY;

    /* If we have at least 2 active slots, auto-arm the start countdown. */
    int active = 0;
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i)
        if (g->lobby.slots[i].in_use && g->lobby.slots[i].team != MATCH_TEAM_NONE) active++;
    if (active >= 2) lobby_auto_start_arm(&g->lobby, g->lobby.auto_start_default);

    if (g->net.role == NET_ROLE_SERVER) {
        net_server_broadcast_match_state(&g->net, &g->match);
        net_server_broadcast_lobby_list (&g->net, &g->lobby);
    }
}

/* Track whether the host's countdown was active last tick so we can
 * detect arm/cancel transitions and ship them to clients immediately,
 * AND throttle a periodic refresh while it's live. The client decays
 * its own local copy each frame for smooth display between broadcasts. */
static bool  g_prev_auto_start_active = false;
static float g_countdown_broadcast_accum = 0.0f;

static void host_broadcast_countdown_if_changed(Game *g, float dt) {
    if (g->net.role != NET_ROLE_SERVER) return;
    bool now_active = g->lobby.auto_start_active;
    bool transition = (now_active != g_prev_auto_start_active);
    g_prev_auto_start_active = now_active;

    if (transition) {
        /* Ship the new state right away so the client UI updates
         * without waiting for the periodic refresh. */
        net_server_broadcast_countdown(&g->net,
            now_active ? g->lobby.auto_start_remaining : 0.0f,
            /*reason*/ now_active ? 1u : 0u);
        g_countdown_broadcast_accum = 0.0f;
        return;
    }
    if (now_active) {
        /* Refresh every 0.5 s. With the client decaying locally, this
         * keeps drift to <0.5 s without flooding the wire. */
        g_countdown_broadcast_accum += dt;
        if (g_countdown_broadcast_accum >= 0.5f) {
            g_countdown_broadcast_accum -= 0.5f;
            net_server_broadcast_countdown(&g->net,
                g->lobby.auto_start_remaining, /*reason*/ 1u);
        }
    }
}

static void host_match_flow_step(Game *g, float dt) {
    switch (g->match.phase) {
        case MATCH_PHASE_LOBBY: {
            /* All-ready accelerator. The auto-start countdown may
             * already be running with the long default (60 s); when
             * everyone hits Ready, override it to a short 3 s so the
             * round actually starts soon — otherwise players see "all
             * ready ✓" but nothing happens. Only override down (don't
             * extend a shorter timer). */
            bool all_ready = lobby_all_ready(&g->lobby);
            if (all_ready) {
                if (!g->lobby.auto_start_active) {
                    lobby_auto_start_arm(&g->lobby, 3.0f);
                } else if (g->lobby.auto_start_remaining > 3.0f) {
                    g->lobby.auto_start_remaining = 3.0f;
                    g->lobby.dirty = true;
                }
            }
            if (lobby_tick(&g->lobby, dt)) {
                /* Auto-start fired → enter countdown. test-play uses
                 * a 1 s countdown (set in match.countdown_default at
                 * startup) so designers' F5 round-trip stays short. */
                float secs = g->test_play_lvl[0]
                                 ? g->match.countdown_default : 5.0f;
                match_begin_countdown(&g->match, secs);
                if (g->net.role == NET_ROLE_SERVER) {
                    net_server_broadcast_match_state(&g->net, &g->match);
                }
            }
            host_broadcast_countdown_if_changed(g, dt);
            lobby_chat_age(&g->lobby, dt);
            break;
        }
        case MATCH_PHASE_COUNTDOWN:
            if (match_tick(&g->match, dt)) {
                start_round(g);
            }
            break;
        case MATCH_PHASE_ACTIVE: {
            apply_new_kills(g);
            broadcast_new_hits(g);
            broadcast_new_fires(g);
            /* End on score limit (FFA = any per-player slot >= cap). */
            bool end = false;
            if (g->match.mode == MATCH_MODE_FFA) {
                for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
                    if (g->lobby.slots[i].in_use &&
                        g->lobby.slots[i].score >= g->match.score_limit)
                    {
                        end = true; break;
                    }
                }
            }
            if (!end && match_round_should_end(&g->match)) end = true;
            if (match_tick(&g->match, dt)) end = true;
            if (end) end_round(g);
            break;
        }
        case MATCH_PHASE_SUMMARY:
            if (match_tick(&g->match, dt)) {
                begin_next_lobby(g);
            }
            break;
    }
}

/* ---- Bootstrap helpers ------------------------------------------- */

static void apply_loadout_flags(Game *g, const LaunchArgs *args) {
    if (g->local_slot_id < 0) return;
    LobbySlot *me = lobby_slot(&g->lobby, g->local_slot_id);
    if (!me) return;
    MechLoadout lo = me->loadout;
    if (args->chassis[0]) lo.chassis_id = chassis_id_from_name(args->chassis);
    lo.primary_id   = resolve_weapon_id  (args->primary,   lo.primary_id);
    lo.secondary_id = resolve_weapon_id  (args->secondary, lo.secondary_id);
    lo.armor_id     = resolve_armor_id   (args->armor,     lo.armor_id);
    lo.jetpack_id   = resolve_jetpack_id (args->jetpack,   lo.jetpack_id);
    lobby_set_loadout(&g->lobby, g->local_slot_id, lo);
}

/* For host & offline solo: stand up a server (no listening for offline)
 * and seat the local player in slot 0. */
static bool bootstrap_host(Game *g, const LaunchArgs *args, bool offline) {
    if (!offline) {
        if (!net_init()) return false;
        if (!net_server_start(&g->net, g->config.port, g)) {
            LOG_E("host: failed to bind UDP %u", (unsigned)g->config.port);
            return false;
        }
        net_discovery_open(&g->net);
        LOG_I("host: ready on port %u", (unsigned)g->config.port);
    } else {
        g->net.role = NET_ROLE_OFFLINE;
        g->offline_solo = true;
    }

    /* Host's own slot — slot 0, peer_id = -1, is_host = true. */
    int slot = lobby_add_slot(&g->lobby, /*peer_id*/-1,
                              args ? args->name : "player", true);
    g->local_slot_id = slot;
    g->world.authoritative = true;

    apply_loadout_flags(g, args);

    /* Pre-build the level so the first-time lobby has *something* to
     * draw under the UI. ROUND_START rebuilds it for the chosen map.
     *
     * M5 P04 — when the editor's F5 forks us with --test-play, load the
     * scratch .lvl directly; subsequent start_round calls also pick this
     * path up via g->test_play_lvl. */
    if (g->test_play_lvl[0]) {
        map_build_from_path(&g->world, &g->level_arena, g->test_play_lvl);
    } else {
        map_build(MAP_FOUNDRY, &g->world, &g->level_arena);
    }
    decal_init((int)level_width_px(&g->world.level),
               (int)level_height_px(&g->world.level));
    return true;
}

/* Pure client: connect, sit in lobby until ROUND_START. */
static bool bootstrap_client(Game *g, const LaunchArgs *args) {
    if (!net_init()) return false;
    if (!net_client_connect(&g->net, args->host, args->port,
                            args->name, g))
    {
        LOG_E("client: connect to %s:%u failed", args->host, (unsigned)args->port);
        return false;
    }
    /* Initial state has been applied in net.c — local_slot_id is set,
     * mode is MODE_LOBBY. */
    g->world.authoritative = false;
    return true;
}

/* ---- Diag overlay ------------------------------------------------- */

/* Frame context the overlay callbacks need. raylib's overlay-callback
 * signature gives us one void* — we pack Game + LobbyUIState into this
 * struct so both the match HUD overlay and the summary panel can run
 * inside renderer_draw_frame's single Begin/EndDrawing pair. */
typedef struct {
    Game         *game;
    LobbyUIState *ui;
} OverlayCtx;

static void draw_diag(void *user, int sw, int sh) {
    OverlayCtx *ctx = (OverlayCtx *)user;
    Game *g = ctx->game;
    int fps = GetFPS();
    Color st = (fps >= 55) ? GREEN : (fps >= 30) ? YELLOW : RED;
    DrawText("soldut " SOLDUT_VERSION_STRING, 12, 10, 18, RAYWHITE);
    DrawText(TextFormat("FPS %d", fps), 12, 32, 18, st);
    DrawText(TextFormat("tick %llu  mechs %d  parts %d  phase=%s",
                        (unsigned long long)g->world.tick,
                        g->world.mech_count,
                        g->world.particles.count,
                        match_phase_name(g->match.phase)),
             12, 52, 14, LIGHTGRAY);

    const char *role = "offline";
    Color rc = GRAY;
    if (g->net.role == NET_ROLE_SERVER) { role = "host";   rc = GREEN; }
    if (g->net.role == NET_ROLE_CLIENT) { role = "client"; rc = SKYBLUE; }

    NetStats st_n; net_get_stats(&g->net, &st_n);
    DrawText(TextFormat("%s peers=%d rtt=%ums tx=%uKB rx=%uKB",
                        role, st_n.peer_count, st_n.rtt_ms_max,
                        st_n.bytes_sent / 1024u, st_n.bytes_recv / 1024u),
             12, 70, 14, rc);

    DrawText("WASD: move/jet  SPACE: jump  LMB: fire  ESC: leave",
             12, sh - 22, 14, GRAY);

    /* Match score/timer banner. Drawn here (inside the renderer's
     * single Begin/EndDrawing pair) instead of in a second pair —
     * doing two swaps per frame produces a per-other-frame "blank +
     * banner only" present, which reads as flicker. */
    match_overlay_draw(g, sw, sh);
}

/* Summary overlay: draws the round-summary panel on top of the frozen
 * world frame inside the renderer's single Begin/EndDrawing pair (same
 * flicker logic as draw_diag). */
static void draw_summary_overlay(void *user, int sw, int sh) {
    OverlayCtx *ctx = (OverlayCtx *)user;
    summary_screen_run(ctx->ui, ctx->game, sw, sh);
}

/* ---- Main --------------------------------------------------------- */

int main(int argc, char **argv) {
    log_init("soldut.log");
    LOG_I("soldut " SOLDUT_VERSION_STRING " starting");

    /* Shot mode early-exit. */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            int rc = shotmode_run(argv[i + 1]);
            log_shutdown();
            return rc;
        }
    }

    LaunchArgs args;
    parse_args(argc, argv, &args);

    Game game;
    if (!game_init(&game)) { log_shutdown(); return EXIT_FAILURE; }
    reconcile_init(&game.reconcile);

    /* Server config — load file, then let CLI flags override. */
    config_load(&game.config, "soldut.cfg");
    if (args.mode == LAUNCH_HOST && args.port != 0) game.config.port = args.port;
    if (args.ff_set) game.config.friendly_fire = args.friendly_fire;
    /* M5 P04 — editor test-play overrides match config: FFA, 60 s round,
     * 1 s auto-start, no networking. Stash the .lvl path on Game so
     * bootstrap_host + start_round can find it. */
    if (args.test_play_lvl[0]) {
        snprintf(game.test_play_lvl, sizeof game.test_play_lvl, "%s",
                 args.test_play_lvl);
        game.config.mode               = MATCH_MODE_FFA;
        game.config.time_limit         = 60;
        game.config.score_limit        = 50;
        game.config.auto_start_seconds = 1;
        game.config.friendly_fire      = true;
        /* Turn on the SHOT_LOG sink so test-play emits the diagnostic
         * trail shipped via the existing `SHOT_LOG()` macro across the
         * codebase (anim transitions, grounded toggles, hitscan, etc.)
         * plus the per-second pelvis-pos line in this file's MATCH
         * branch. SHOT_LOG is a one-branch no-op when g_shot_mode is
         * 0 — the production code path never sees these messages. */
        g_shot_mode = 1;
    }
    /* Re-apply MatchState defaults from the loaded config. */
    match_init(&game.match, game.config.mode, game.config.score_limit,
               game.config.time_limit, game.config.friendly_fire);
    if (args.test_play_lvl[0]) {
        /* Drop the in-match countdown to 1 s as well so a designer's
         * F5 round-trip time stays short (default is 5 s). */
        game.match.countdown_default = 1.0f;
    }
    game.match.map_id = config_pick_map(&game.config, 0);
    game.lobby.auto_start_default = game.config.auto_start_seconds;
    game.world.friendly_fire = game.config.friendly_fire;

    PlatformConfig pcfg = {
        .window_w = 1280, .window_h = 720,
        .vsync = true, .fullscreen = false,
        .title = (args.mode == LAUNCH_HOST)   ? "Soldut " SOLDUT_VERSION_STRING " — host"
               : (args.mode == LAUNCH_CLIENT) ? "Soldut " SOLDUT_VERSION_STRING " — client"
                                              : "Soldut " SOLDUT_VERSION_STRING,
    };
    if (!platform_init(&pcfg)) {
        game_shutdown(&game); log_shutdown(); return EXIT_FAILURE;
    }

    LobbyUIState ui = (LobbyUIState){0};
    lobby_ui_init(&ui);
    if (args.name[0]) snprintf(ui.player_name, sizeof ui.player_name, "%s", args.name);

    Renderer rd;
    renderer_init(&rd, GetScreenWidth(), GetScreenHeight(), (Vec2){640, 360});

    /* Initial mode: title (unless CLI shortcut). */
    if (args.skip_title) {
        if (args.mode == LAUNCH_HOST) {
            /* test-play forces offline-solo so we don't bind a UDP port
             * the user's running game might already be using. */
            bool offline = (args.test_play_lvl[0] != 0);
            if (!bootstrap_host(&game, &args, offline)) {
                game_shutdown(&game); log_shutdown(); return EXIT_FAILURE;
            }
            game.mode = MODE_LOBBY;
            if (args.test_play_lvl[0]) {
                /* Arm the round immediately — test-play wants to skip
                 * the lobby UX and drop the player into the map. */
                lobby_auto_start_arm(&game.lobby, 1.0f);
            }
        } else if (args.mode == LAUNCH_CLIENT) {
            if (!bootstrap_client(&game, &args)) {
                game_shutdown(&game); log_shutdown(); return EXIT_FAILURE;
            }
        }
    } else {
        game.mode = MODE_TITLE;
    }

    PlatformFrame pf = {0};
    double accum = 0.0;
    double last  = GetTime();

    while (!WindowShouldClose() && game.mode != MODE_QUIT) {
        double now = GetTime();
        double dt  = now - last;
        last = now;
        if (dt > MAX_FRAME_DT) dt = MAX_FRAME_DT;
        accum += dt;

        platform_begin_frame(&pf);

        /* Pump network FIRST so inbound inputs / snapshots / lobby
         * messages are visible before this frame's sim ticks. */
        if (game.net.role != NET_ROLE_OFFLINE) {
            net_poll(&game.net, &game, dt);
        }

        /* Per-mode update + render. */
        switch (game.mode) {

        case MODE_TITLE: {
            BeginDrawing();
            title_screen_run(&ui, &game, pf.render_w, pf.render_h);
            EndDrawing();
            if (ui.request_quit)             { game.mode = MODE_QUIT; }
            else if (ui.request_single_player) {
                ui.request_single_player = false;
                snprintf(args.name, sizeof args.name, "%s", ui.player_name);
                if (bootstrap_host(&game, &args, /*offline*/true)) {
                    /* Auto-start with a short countdown. */
                    lobby_auto_start_arm(&game.lobby, 1.0f);
                    game.mode = MODE_LOBBY;
                }
            }
            else if (ui.request_host) {
                ui.request_host = false;
                snprintf(args.name, sizeof args.name, "%s", ui.player_name);
                if (bootstrap_host(&game, &args, /*offline*/false)) {
                    game.mode = MODE_LOBBY;
                }
            }
            else if (ui.request_browse) {
                ui.request_browse = false;
                /* Open a transient discovery socket so we can scan. */
                if (game.net.role == NET_ROLE_OFFLINE) {
                    net_init();
                    net_discovery_open(&game.net);
                }
                game.mode = MODE_BROWSER;
            }
            break;
        }

        case MODE_BROWSER: {
            BeginDrawing();
            browser_screen_run(&ui, &game, pf.render_w, pf.render_h);
            EndDrawing();
            net_poll(&game.net, &game, dt);   /* drain discovery replies */
            if (ui.request_connect) {
                ui.request_connect = false;
                snprintf(args.name, sizeof args.name, "%s", ui.player_name);
                snprintf(args.host, sizeof args.host, "%s", game.pending_host);
                args.port = game.pending_port;
                if (bootstrap_client(&game, &args)) {
                    /* INITIAL_STATE already moved us to MODE_LOBBY. */
                } else {
                    game.mode = MODE_TITLE;
                }
            }
            break;
        }

        case MODE_CONNECT: {
            BeginDrawing();
            connect_screen_run(&ui, &game, pf.render_w, pf.render_h);
            EndDrawing();
            if (ui.request_connect) {
                ui.request_connect = false;
                snprintf(args.name, sizeof args.name, "%s", ui.player_name);
                snprintf(args.host, sizeof args.host, "%s", game.pending_host);
                args.port = game.pending_port;
                if (bootstrap_client(&game, &args)) {
                    /* INITIAL_STATE already moved us to MODE_LOBBY. */
                } else {
                    game.mode = MODE_TITLE;
                }
            }
            break;
        }

        case MODE_LOBBY: {
            /* Host: tick the match flow controller. */
            if (game.net.role == NET_ROLE_SERVER || game.offline_solo) {
                host_match_flow_step(&game, (float)dt);
            }
            /* Client: age chat + locally decay the auto-start countdown
             * so the visible "Match starts in N.Ns" ticks smoothly
             * between the host's broadcasts (which arrive every ~0.5
             * s). The host's COUNTDOWN-phase timer (match.countdown_
             * remaining) is decayed similarly so the client's UI shows
             * a consistent number even before the next match_state
             * broadcast lands. */
            if (game.net.role == NET_ROLE_CLIENT) {
                lobby_chat_age(&game.lobby, (float)dt);
                if (game.lobby.auto_start_active &&
                    game.lobby.auto_start_remaining > 0.0f)
                {
                    game.lobby.auto_start_remaining -= (float)dt;
                    if (game.lobby.auto_start_remaining < 0.0f)
                        game.lobby.auto_start_remaining = 0.0f;
                }
                if (game.match.phase == MATCH_PHASE_COUNTDOWN &&
                    game.match.countdown_remaining > 0.0f)
                {
                    game.match.countdown_remaining -= (float)dt;
                    if (game.match.countdown_remaining < 0.0f)
                        game.match.countdown_remaining = 0.0f;
                }
            }

            BeginDrawing();
            lobby_screen_run(&ui, &game, pf.render_w, pf.render_h);
            EndDrawing();

            /* Honor the lobby UI's "Leave" button. */
            if (game.mode == MODE_TITLE) {
                /* Disconnect / shut down server. */
                if (game.net.role != NET_ROLE_OFFLINE) {
                    net_close(&game.net);
                    net_shutdown();
                }
                lobby_clear_round_mechs(&game.lobby, &game.world);
                memset(&game.lobby, 0, sizeof game.lobby);
                lobby_init(&game.lobby, game.config.auto_start_seconds);
                match_init(&game.match, game.config.mode, game.config.score_limit,
                           game.config.time_limit, game.config.friendly_fire);
                game.local_slot_id = -1;
                game.offline_solo  = false;
            }
            break;
        }

        case MODE_MATCH: {
            /* Late-bind world.local_mech_id from our lobby slot. The
             * server ships the lobby table (with each slot's mech_id)
             * just before ROUND_START; clients need to wait until the
             * matching mech actually shows up in the snapshot stream
             * before pointing local_mech_id at it. Without this the
             * camera never follows anyone and the client renders a
             * black screen. */
            if (game.world.local_mech_id < 0 && game.local_slot_id >= 0) {
                int mid = game.lobby.slots[game.local_slot_id].mech_id;
                if (mid >= 0 && mid < game.world.mech_count) {
                    game.world.local_mech_id = mid;
                    LOG_I("client: local_mech_id resolved → %d (slot %d)",
                          mid, game.local_slot_id);
                }
            }

            /* Client: locally decay match.time_remaining so the match
             * overlay's countdown ticks smoothly. The host doesn't
             * broadcast match_state during ACTIVE phase (only on
             * round transitions), so without local decay the client
             * just shows the round-start value frozen for the entire
             * match — diverging from the host's actual remaining
             * time by up to time_limit seconds. */
            if (game.net.role == NET_ROLE_CLIENT &&
                game.match.phase == MATCH_PHASE_ACTIVE &&
                game.match.time_remaining > 0.0f)
            {
                game.match.time_remaining -= (float)dt;
                if (game.match.time_remaining < 0.0f)
                    game.match.time_remaining = 0.0f;
            }

            /* Fixed-step simulation. */
            while (accum >= TICK_DT) {
                ClientInput in;
                platform_sample_input(&in);
                in.dt  = (float)TICK_DT;
                in.seq = (uint16_t)(game.world.tick + 1);

                Vec2 cursor_world = renderer_screen_to_world(&rd,
                    (Vec2){ in.aim_x, in.aim_y });
                in.aim_x = cursor_world.x;
                in.aim_y = cursor_world.y;

                if (game.net.role == NET_ROLE_CLIENT) {
                    if (game.world.local_mech_id >= 0) {
                        game.world.mechs[game.world.local_mech_id].latched_input = in;
                    }
                    simulate_step(&game.world, (float)TICK_DT);
                    /* P03: pull remote mechs back to the interpolated
                     * server position. Runs after physics so it
                     * overrides any drift from stale latched_input
                     * extrapolation. The renderer's prev-frame
                     * snapshot (taken at the top of the next
                     * simulate_step) captures this as the new
                     * "where it was last tick" anchor — yielding
                     * smooth motion across snapshot intervals.
                     *
                     * Advance the clock in double precision —
                     * (uint32_t)(1/60*1000) truncates to 16, drifts
                     * 0.67 ms/tick = 40 ms/s = client renders ~270 ms
                     * behind server after a few seconds. */
                    if (game.net.client_render_clock_armed) {
                        game.net.client_render_time_ms += TICK_DT * 1000.0;
                        /* render_time may be negative early in a
                         * connection (we init it as
                         * `first_snap.server_time - INTERP_DELAY_MS`).
                         * Clamp before casting; snapshot_interp_remotes
                         * treats render_time <= oldest_t as "clamp to
                         * oldest snapshot", which is the right thing
                         * to do when we're conceptually before any
                         * received data. */
                        double rt = game.net.client_render_time_ms;
                        uint32_t rt_u32 = (rt > 0.0) ? (uint32_t)rt : 0u;
                        snapshot_interp_remotes(&game.world, rt_u32);
                    }
                    reconcile_push_input(&game.reconcile, in);
                    net_client_send_input(&game.net, in);
                    reconcile_tick_smoothing(&game.reconcile);
                } else {
                    if (game.world.local_mech_id >= 0) {
                        game.world.mechs[game.world.local_mech_id].latched_input = in;
                    }
                    simulate_step(&game.world, (float)TICK_DT);
                }

                game.input = in;
                accum -= TICK_DT;
            }

            /* Host: drive match flow (kills → scores → end-of-round). */
            if (game.net.role == NET_ROLE_SERVER || game.offline_solo) {
                host_match_flow_step(&game, (float)dt);
            }

            /* Editor F5 test-play: per-second pelvis log so
             * tests/spawn_e2e.sh can verify the mech settled where
             * the .lvl said it should. Gated on SHOT_LOG (== `if
             * (g_shot_mode) ...`), which we toggle on at startup
             * only when --test-play is set — production play paths
             * never hit this branch. */
            if (game.match.phase == MATCH_PHASE_ACTIVE &&
                game.world.tick > 0 && (game.world.tick % 60) == 0)
            {
                int mid = game.world.local_mech_id;
                if (mid >= 0 && mid < game.world.mech_count) {
                    const Mech *m = &game.world.mechs[mid];
                    int pb = m->particle_base + PART_PELVIS;
                    SHOT_LOG("test-play: tick %llu  pelvis (%.1f, %.1f)  grounded=%d",
                             (unsigned long long)game.world.tick,
                             (double)game.world.particles.pos_x[pb],
                             (double)game.world.particles.pos_y[pb],
                             (int)m->grounded);
                }
            }

            /* Render world + HUD + (diag + match banner via draw_diag).
             * One Begin/EndDrawing pair total per frame. The local
             * mech's visual_offset comes from reconcile (smooths out
             * server snaps over ~6 frames); the host has no reconcile
             * state and passes (0,0). */
            OverlayCtx ovc = { .game = &game, .ui = &ui };
            Vec2 visual_off = (game.net.role == NET_ROLE_CLIENT)
                            ? game.reconcile.visual_offset
                            : (Vec2){ 0.0f, 0.0f };
            renderer_draw_frame(&rd, &game.world,
                                pf.render_w, pf.render_h,
                                (float)(accum / TICK_DT),
                                visual_off,
                                (Vec2){ (float)GetMouseX(), (float)GetMouseY() },
                                draw_diag, &ovc);

            if (IsKeyPressed(KEY_ESCAPE)) {
                if (game.net.role == NET_ROLE_SERVER || game.offline_solo) {
                    /* Host: end the round early. */
                    end_round(&game);
                } else {
                    /* Client: drop the connection, return to title. */
                    net_close(&game.net);
                    net_shutdown();
                    game.mode = MODE_TITLE;
                    game.offline_solo = false;
                }
            }
            break;
        }

        case MODE_SUMMARY: {
            /* Host: tick the summary timer; clients receive ROUND_END
             * via net so their match.phase == SUMMARY automatically. */
            if (game.net.role == NET_ROLE_SERVER || game.offline_solo) {
                host_match_flow_step(&game, (float)dt);
            } else if (game.net.role == NET_ROLE_CLIENT) {
                /* Decay the visible countdown locally. */
                if (game.match.summary_remaining > 0.0f) {
                    game.match.summary_remaining -= (float)dt;
                    if (game.match.summary_remaining < 0.0f)
                        game.match.summary_remaining = 0.0f;
                }
            }

            /* Draw world (frozen) + summary overlay in a single
             * Begin/EndDrawing pair via the renderer's overlay
             * callback. Two pairs per frame double-swap and read as
             * flicker. */
            OverlayCtx ovc = { .game = &game, .ui = &ui };
            renderer_draw_frame(&rd, &game.world,
                                pf.render_w, pf.render_h,
                                0.0f, (Vec2){ 0.0f, 0.0f },
                                (Vec2){ (float)GetMouseX(), (float)GetMouseY() },
                                draw_summary_overlay, &ovc);

            if (game.mode == MODE_TITLE) {
                /* User clicked Leave. Tear down. */
                if (game.net.role != NET_ROLE_OFFLINE) {
                    net_close(&game.net);
                    net_shutdown();
                }
                lobby_clear_round_mechs(&game.lobby, &game.world);
                memset(&game.lobby, 0, sizeof game.lobby);
                lobby_init(&game.lobby, game.config.auto_start_seconds);
                match_init(&game.match, game.config.mode, game.config.score_limit,
                           game.config.time_limit, game.config.friendly_fire);
                game.local_slot_id = -1;
                game.offline_solo  = false;
            }
            break;
        }

        default:
            BeginDrawing();
            ClearBackground(BLACK);
            EndDrawing();
            break;
        }
    }

    LOG_I("soldut shutting down (ran %llu sim ticks)",
          (unsigned long long)game.world.tick);

    if (game.net.role != NET_ROLE_OFFLINE) {
        net_close(&game.net);
        net_shutdown();
    }

    log_shutdown();
    _exit(EXIT_SUCCESS);
}
