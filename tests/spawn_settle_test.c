/*
 * tests/spawn_settle_test.c — for every spawn on every shipped map,
 * create a mech at the spawn coords, run 90 sim ticks (1.5 s of
 * physics) and check that the mech ENDS UP IN A REASONABLE STATE.
 *
 * Catches bugs the static `spawn_geometry_test` misses — including:
 *   * spawns inside polygons whose tile broadphase cell doesn't list
 *     them (false negatives in the static check)
 *   * spawns where the immediate body fits but tight overhangs / wall
 *     proximity wedge the body during settle
 *   * spawns where the mech falls forever (out-of-bounds spawn y)
 *
 * Pass criteria after settle (~1.5 s):
 *   * pelvis vertical drop < 200 px  (a normal "fall onto floor" is
 *     ≤80 px since spawn_floor sits 40 px above floor and the body is
 *     ~36 px deep; >200 px means we kept falling through nothing)
 *   * pelvis horizontal drift < 80 px (push-out or stuck-bouncing)
 *   * `level_point_solid` clear at the settled pelvis
 *   * pelvis vy < 1.5 px/tick (still moving = not settled)
 *
 * Returns 0 on all-clear, 1 if any spawn fails settle.
 */

#include "../src/arena.h"
#include "../src/game.h"
#include "../src/level.h"
#include "../src/level_io.h"
#include "../src/log.h"
#include "../src/maps.h"
#include "../src/match.h"
#include "../src/mech.h"
#include "../src/pickup.h"
#include "../src/simulate.h"
#include "../src/world.h"

#include "raylib.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_DT (1.0f / 60.0f)
#define SETTLE_TICKS 90        /* 1.5 s */

static int g_failed_maps   = 0;
static int g_failed_spawns = 0;

static const char *team_name(uint8_t t) {
    switch (t) {
        case 0: return "any";
        case 1: return "red";
        case 2: return "blue";
        default: return "?";
    }
}

/* Strip every alive Mech back to count=0, reset the particle and
 * constraint pools' counts. Lets us re-spawn fresh mechs into the
 * same World between spawn checks without exhausting the constraint
 * pool (which doesn't free per-mech automatically). */
static void clear_mechs(World *w) {
    for (int i = 0; i < w->mech_count; ++i) {
        w->mechs[i].alive = false;
    }
    w->mech_count        = 0;
    w->particles.count   = 0;
    w->constraints.count = 0;
    w->local_mech_id     = -1;
    w->dummy_mech_id     = -1;
}

static bool check_spawn(Game *g, int spawn_idx) {
    const LvlSpawn *s = &g->world.level.spawns[spawn_idx];
    Vec2 spawn_pos = (Vec2){ (float)s->pos_x, (float)s->pos_y };

    clear_mechs(&g->world);

    MechLoadout lo = mech_default_loadout();
    int team = (s->team == 0) ? MATCH_TEAM_FFA : (int)s->team;
    int mid = mech_create_loadout(&g->world, lo, spawn_pos, team, false);
    if (mid < 0) {
        fprintf(stderr, "  spawn[%d]: mech_create failed\n", spawn_idx);
        return false;
    }

    /* Settle. */
    for (int t = 0; t < SETTLE_TICKS; ++t) {
        simulate_step(&g->world, TICK_DT);
    }

    /* Read final pelvis state. */
    int pelv = g->world.mechs[mid].particle_base + PART_PELVIS;
    float final_x = g->world.particles.pos_x[pelv];
    float final_y = g->world.particles.pos_y[pelv];
    float prev_x  = g->world.particles.prev_x[pelv];
    float prev_y  = g->world.particles.prev_y[pelv];
    float vx = final_x - prev_x;
    float vy = final_y - prev_y;
    float dx = final_x - spawn_pos.x;
    float dy = final_y - spawn_pos.y;
    float speed = sqrtf(vx * vx + vy * vy);

    bool inside_solid = level_point_solid(&g->world.level,
                                          (Vec2){ final_x, final_y });

    /* Verdict. */
    bool fail = false;
    const char *reason = NULL;
    if (inside_solid) {
        fail = true; reason = "INSIDE_SOLID";
    } else if (dy > 200.0f) {
        fail = true; reason = "FELL_TOO_FAR";
    } else if (fabsf(dx) > 80.0f) {
        fail = true; reason = "PUSHED_OUT";
    } else if (speed > 1.5f) {
        fail = true; reason = "NEVER_SETTLED";
    }

    if (fail) {
        fprintf(stderr,
                "  FAIL spawn[%d] team=%s lane=%u — %s "
                "(spawn=(%.0f,%.0f) settled=(%.0f,%.0f) Δ=(%.1f,%.1f) v=%.2f)\n",
                spawn_idx, team_name(s->team), s->lane_hint, reason,
                spawn_pos.x, spawn_pos.y, final_x, final_y, dx, dy, speed);
        ++g_failed_spawns;
    }
    return !fail;
}

static void check_map(Game *g, const char *map_path) {
    /* Load the map fresh. */
    arena_reset(&g->level_arena);
    LvlResult r = level_load(&g->world, &g->level_arena, map_path);
    if (r != LVL_OK) {
        fprintf(stderr, "SKIP: %s — load failed (%s)\n",
                map_path, level_io_result_str(r));
        return;
    }
    pickup_init_round(&g->world);

    const char *short_name = strrchr(map_path, '/');
    short_name = short_name ? short_name + 1 : map_path;
    int n = g->world.level.spawn_count;

    fprintf(stdout, "checking %s (%d spawns):\n", short_name, n);
    int bad_in_map = 0;
    for (int i = 0; i < n; ++i) {
        if (!check_spawn(g, i)) ++bad_in_map;
    }
    if (bad_in_map > 0) {
        fprintf(stdout, "FAIL: %s — %d / %d spawns can't settle cleanly\n\n",
                short_name, bad_in_map, n);
        ++g_failed_maps;
    } else {
        fprintf(stdout, "PASS: %s — all %d spawns settle cleanly\n\n",
                short_name, n);
    }
}

int main(void) {
    SetTraceLogLevel(LOG_WARNING);
    log_init(NULL);

    Game g;
    if (!game_init(&g)) {
        fprintf(stderr, "game_init failed\n");
        return 2;
    }
    g.world.authoritative = true;
    g.world.friendly_fire = false;
    g.match.phase = MATCH_PHASE_ACTIVE;
    g.match.time_limit = 0;
    g.match.score_limit = 99999;
    map_registry_init();

    const char *maps[] = {
        "assets/maps/foundry.lvl",
        "assets/maps/slipstream.lvl",
        "assets/maps/reactor.lvl",
        "assets/maps/concourse.lvl",
        "assets/maps/catwalk.lvl",
        "assets/maps/aurora.lvl",
        "assets/maps/crossfire.lvl",
        "assets/maps/citadel.lvl",
        NULL,
    };
    for (int i = 0; maps[i]; ++i) check_map(&g, maps[i]);

    if (g_failed_spawns > 0) {
        fprintf(stderr,
                "\nspawn_settle_test: %d spawn(s) fail settle across %d map(s)\n",
                g_failed_spawns, g_failed_maps);
        return 1;
    }
    fprintf(stdout, "\nspawn_settle_test: all spawns settle cleanly\n");
    return 0;
}
