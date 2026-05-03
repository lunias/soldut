#include "decal.h"
#include "game.h"
#include "level.h"
#include "log.h"
#include "mech.h"
#include "net.h"
#include "platform.h"
#include "reconcile.h"
#include "render.h"
#include "shotmode.h"
#include "simulate.h"
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
 * Modes:
 *   ./soldut                                 single-player tutorial (M1)
 *   ./soldut --host [PORT]                   host an authoritative server + play
 *   ./soldut --connect HOST[:PORT]           join a server as client
 *   ./soldut --shot tests/shots/foo.shot     scripted scene → PNGs
 *
 * The simulation step is the same in every mode. What varies is who
 * latches the inputs (server: from each peer; client/local: from the
 * keyboard via platform_sample_input).
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
    bool        friendly_fire;
} LaunchArgs;

static void parse_args(int argc, char **argv, LaunchArgs *out) {
    memset(out, 0, sizeof *out);
    out->port = SOLDUT_DEFAULT_PORT;
    snprintf(out->name, sizeof out->name, "player");
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0) {
            out->mode = LAUNCH_HOST;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                out->port = (uint16_t)atoi(argv[++i]);
                if (out->port == 0) out->port = SOLDUT_DEFAULT_PORT;
            }
        }
        else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            out->mode = LAUNCH_CLIENT;
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
        /* M3 loadout flags. We accept a small handful so that the local
         * mech can be spawned with any chassis/weapon combo for testing
         * without a lobby. */
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
            out->friendly_fire = true;
        }
    }
}

/* Resolve a weapon name to id by walking the table via weapon_def +
 * weapon_short_name. Returns the default if unknown. */
static int resolve_weapon_id(const char *name, int default_id) {
    if (!name || !*name) return default_id;
    for (int i = 0; i < 32; ++i) {
        const char *n = weapon_short_name(i);
        if (!n || strcmp(n, "?") == 0) continue;
        /* Case-insensitive prefix match: "pulse" matches "Pulse Rifle". */
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
    LOG_W("resolve_armor_id: unknown '%s'", name);
    return default_id;
}

static int resolve_jetpack_id(const char *name, int default_id) {
    if (!name || !*name) return default_id;
    for (int i = 0; i < 8; ++i) {
        const Jetpack *j = jetpack_def(i);
        if (!j || !j->name) continue;
        if (strcasecmp(name, j->name) == 0) return i;
    }
    LOG_W("resolve_jetpack_id: unknown '%s'", name);
    return default_id;
}

/* Build the tutorial level + a player mech (and a dummy in offline /
 * host mode). The host's player is local mech id 0; remote clients get
 * their mechs spawned at handshake by net.c. */
static void seed_world(Game *g, LaunchMode mode, const LaunchArgs *args) {
    World *w = &g->world;

    level_build_tutorial(&w->level, &g->level_arena);

    decal_init((int)level_width_px(&w->level),
               (int)level_height_px(&w->level));

    w->friendly_fire = args ? args->friendly_fire : false;

    const float feet_below_pelvis = 36.0f;
    const float foot_clearance    = 4.0f;
    Vec2 player_spawn = { 16.0f * 32.0f + 8.0f,
                          30.0f * 32.0f - feet_below_pelvis - foot_clearance };

    if (mode == LAUNCH_CLIENT) {
        /* Pure client: the server's INITIAL_STATE will spawn the
         * mechs for us. We deliberately leave the world empty here. */
        w->camera_target = player_spawn;
        w->camera_smooth = player_spawn;
        w->camera_zoom   = 1.4f;
        w->local_mech_id = -1;
        w->dummy_mech_id = -1;
        w->authoritative = false;
        return;
    }

    /* Resolve loadout from CLI flags (or use defaults). */
    MechLoadout lo = mech_default_loadout();
    if (args) {
        lo.chassis_id   = chassis_id_from_name(args->chassis[0] ? args->chassis : "Trooper");
        lo.primary_id   = resolve_weapon_id  (args->primary,   WEAPON_PULSE_RIFLE);
        lo.secondary_id = resolve_weapon_id  (args->secondary, WEAPON_SIDEARM);
        lo.armor_id     = resolve_armor_id   (args->armor,     ARMOR_LIGHT);
        lo.jetpack_id   = resolve_jetpack_id (args->jetpack,   JET_STANDARD);
    }
    w->local_mech_id = mech_create_loadout(w, lo, player_spawn,
                                           /*team*/1, /*is_dummy*/false);

    /* Dummy: trooper, pulse rifle, no armor — punching bag. */
    Vec2 dummy_spawn = { 75.0f * 32.0f,
                         32.0f * 32.0f - feet_below_pelvis - foot_clearance };
    MechLoadout dlo = mech_default_loadout();
    dlo.armor_id = ARMOR_NONE;
    w->dummy_mech_id = mech_create_loadout(w, dlo, dummy_spawn,
                                           /*team*/2, /*is_dummy*/true);

    w->camera_target = player_spawn;
    w->camera_smooth = player_spawn;
    w->camera_zoom   = 1.4f;

    /* Single-player and host both run authoritatively. */
    w->authoritative = true;
}

static void draw_diag(void *user, int sw, int sh) {
    (void)sw;
    const Game *g = (const Game *)user;
    int fps = GetFPS();
    Color st = (fps >= 55) ? GREEN : (fps >= 30) ? YELLOW : RED;
    DrawText("soldut " SOLDUT_VERSION_STRING, 12, 10, 18, RAYWHITE);
    DrawText(TextFormat("FPS %d", fps), 12, 32, 18, st);
    DrawText(TextFormat("tick %llu  mechs %d  particles %d",
                        (unsigned long long)g->world.tick,
                        g->world.mech_count,
                        g->world.particles.count),
             12, 52, 14, LIGHTGRAY);

    const char *role = "offline";
    Color rc = GRAY;
    if (g->net.role == NET_ROLE_SERVER) { role = "host"; rc = GREEN; }
    if (g->net.role == NET_ROLE_CLIENT) { role = "client"; rc = SKYBLUE; }

    NetStats st_n; net_get_stats(&g->net, &st_n);
    DrawText(TextFormat("%s peers=%d rtt=%ums tx=%uKB rx=%uKB",
                        role, st_n.peer_count, st_n.rtt_ms_max,
                        st_n.bytes_sent / 1024u, st_n.bytes_recv / 1024u),
             12, 70, 14, rc);

    DrawText("WASD: move/jet  SPACE: jump  LMB: fire",
             12, sh - 22, 14, GRAY);
}

int main(int argc, char **argv) {
    log_init("soldut.log");
    LOG_I("soldut " SOLDUT_VERSION_STRING " (M2) starting");

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

    PlatformConfig pcfg = {
        .window_w = 1280, .window_h = 720,
        .vsync = true, .fullscreen = false,
        .title = (args.mode == LAUNCH_HOST)   ? "Soldut " SOLDUT_VERSION_STRING " — host"
               : (args.mode == LAUNCH_CLIENT) ? "Soldut " SOLDUT_VERSION_STRING " — client"
                                              : "Soldut " SOLDUT_VERSION_STRING " — M2",
    };
    if (!platform_init(&pcfg)) {
        game_shutdown(&game); log_shutdown(); return EXIT_FAILURE;
    }

    /* Network setup. seed_world depends on knowing the launch mode. */
    if (args.mode != LAUNCH_OFFLINE) {
        if (!net_init()) { game_shutdown(&game); log_shutdown(); return EXIT_FAILURE; }
    }

    if (args.mode == LAUNCH_HOST) {
        if (!net_server_start(&game.net, args.port, &game)) {
            LOG_E("host: failed to bind UDP %u", (unsigned)args.port);
            game_shutdown(&game); net_shutdown(); log_shutdown();
            return EXIT_FAILURE;
        }
        seed_world(&game, LAUNCH_HOST, &args);
        net_discovery_open(&game.net);
        LOG_I("host: ready on port %u — invite a client with `--connect <ip>:%u`",
              (unsigned)args.port, (unsigned)args.port);
    }
    else if (args.mode == LAUNCH_CLIENT) {
        seed_world(&game, LAUNCH_CLIENT, &args);
        if (!net_client_connect(&game.net, args.host, args.port,
                                args.name, &game))
        {
            LOG_E("client: failed to connect to %s:%u",
                  args.host[0] ? args.host : "?", (unsigned)args.port);
            platform_shutdown();
            game_shutdown(&game); net_shutdown(); log_shutdown();
            return EXIT_FAILURE;
        }
    }
    else {
        seed_world(&game, LAUNCH_OFFLINE, &args);
    }

    Renderer rd;
    {
        Vec2 follow = (game.world.local_mech_id >= 0)
            ? mech_chest_pos(&game.world, game.world.local_mech_id)
            : (Vec2){ (float)game.world.level.width  * 16.0f,
                      (float)game.world.level.height * 16.0f };
        renderer_init(&rd, GetScreenWidth(), GetScreenHeight(), follow);
    }

    PlatformFrame pf = {0};
    double accum = 0.0;
    double last  = GetTime();

    while (!WindowShouldClose()) {
        double now = GetTime();
        double dt  = now - last;
        last = now;
        if (dt > MAX_FRAME_DT) dt = MAX_FRAME_DT;
        accum += dt;

        platform_begin_frame(&pf);

        /* Pump network FIRST so inbound inputs (server side) and
         * snapshots (client side) are visible before this frame's
         * sim ticks run. */
        if (game.net.role != NET_ROLE_OFFLINE) {
            net_poll(&game.net, &game, dt);
        }

        /* Fixed-step simulation. */
        while (accum >= TICK_DT) {
            ClientInput in;
            platform_sample_input(&in);
            in.dt  = (float)TICK_DT;
            in.seq = (uint16_t)(game.world.tick + 1);

            /* Convert cursor screen→world via the live camera. We do
             * this on the client side so the server doesn't need a
             * camera; it just reads aim_world directly. */
            Vec2 cursor_world = renderer_screen_to_world(&rd,
                (Vec2){ in.aim_x, in.aim_y });
            in.aim_x = cursor_world.x;
            in.aim_y = cursor_world.y;

            if (game.net.role == NET_ROLE_CLIENT) {
                /* Predict locally: latch onto our mech, simulate. */
                if (game.world.local_mech_id >= 0) {
                    game.world.mechs[game.world.local_mech_id].latched_input = in;
                }
                simulate_step(&game.world, (float)TICK_DT);

                /* Push onto the unacked-input ring + ship to server. */
                reconcile_push_input(&game.reconcile, in);
                net_client_send_input(&game.net, in);
                reconcile_tick_smoothing(&game.reconcile);
            }
            else {
                /* Offline / host: latch onto local mech and run the
                 * authoritative tick. (On the host, peer inputs were
                 * latched onto their respective mechs by net_poll
                 * before this loop entered.) */
                if (game.world.local_mech_id >= 0) {
                    game.world.mechs[game.world.local_mech_id].latched_input = in;
                }
                simulate_step(&game.world, (float)TICK_DT);
            }

            game.input = in;
            accum -= TICK_DT;
        }

        renderer_draw_frame(&rd, &game.world,
                            pf.render_w, pf.render_h,
                            (float)(accum / TICK_DT),
                            (Vec2){ (float)GetMouseX(), (float)GetMouseY() },
                            draw_diag, &game);
    }

    LOG_I("soldut shutting down (ran %llu sim ticks)",
          (unsigned long long)game.world.tick);

    if (game.net.role != NET_ROLE_OFFLINE) {
        net_close(&game.net);
        net_shutdown();
    }

    /* Fast exit: only flush the log file, then hand the rest back to
     * the OS. raylib's CloseAudioDevice (miniaudio teardown) and
     * CloseWindow (glfwTerminate / Wayland connection close) can each
     * stall the process for a noticeable beat on WSLg/Linux; arena
     * destroys would also free ~60 MB. None of that is necessary —
     * the kernel reclaims memory, GL context, and audio device when
     * the process exits. We only flush our own log so the last lines
     * survive. */
    log_shutdown();
    _exit(EXIT_SUCCESS);
}
