/*
 * tests/spawn_test.c — unit test for src/maps.c::map_spawn_point.
 *
 * Verifies the M5 P04 fix: when a Level has authored spawn points
 * (level->spawns / spawn_count > 0, populated by level_load from a
 * .lvl SPWN section), map_spawn_point must return those coordinates,
 * not the M4 hardcoded g_*_lanes fallback. Bug surfaced because
 * F5 test-play loads a .lvl with a custom spawn but the player was
 * landing in the foundry-baseline lane instead.
 *
 * Returns 0 on all-pass, 1 on any assertion failure.
 */

#include "../src/log.h"
#include "../src/maps.h"
#include "../src/match.h"
#include "../src/world.h"

#include <stdio.h>
#include <string.h>

static int g_failed = 0;

#define ASSERT_NEAR(label, actual, expected, eps) do {                       \
    float __a = (float)(actual);                                             \
    float __e = (float)(expected);                                           \
    float __d = __a - __e; if (__d < 0) __d = -__d;                          \
    if (__d > (eps)) {                                                       \
        fprintf(stderr, "FAIL: %s: actual=%.2f expected=%.2f (delta=%.2f)\n", \
                label, __a, __e, __d);                                       \
        ++g_failed;                                                          \
    } else {                                                                 \
        fprintf(stdout, "ok:   %s: %.2f ≈ %.2f\n", label, __a, __e);         \
    }                                                                        \
} while (0)

#define ASSERT_TRUE(label, cond) do {                                        \
    if (!(cond)) {                                                           \
        fprintf(stderr, "FAIL: %s\n", label);                                \
        ++g_failed;                                                          \
    } else {                                                                 \
        fprintf(stdout, "ok:   %s\n", label);                                \
    }                                                                        \
} while (0)

/* Build a minimal Level with the supplied spawns array. The caller
 * keeps ownership of `spawns`; this function just wires the pointers. */
static void build_test_level(Level *L, LvlSpawn *spawns, int count,
                             int width_tiles, int height_tiles) {
    memset(L, 0, sizeof *L);
    L->width     = width_tiles;
    L->height    = height_tiles;
    L->tile_size = 32;
    L->spawns    = spawns;
    L->spawn_count = count;
}

int main(void) {
    log_init("/tmp/spawn_test.log");

    /* ---- Test 1: Custom spawn is honored over hardcoded lanes ----
     * Pre-fix, map_spawn_point ignored level->spawns and returned
     * lanes[slot_index]. With the fix, it must return the .lvl spawn. */
    {
        LvlSpawn spawns[] = {
            { .pos_x = 1234, .pos_y = 567, .team = 0, .flags = 1 },
        };
        Level L;
        build_test_level(&L, spawns, 1, 60, 40);
        Vec2 p = map_spawn_point(MAP_FOUNDRY, &L, 0,
                                 MATCH_TEAM_RED, MATCH_MODE_FFA);
        ASSERT_NEAR("FFA mode picks authored spawn x",     p.x, 1234.0f, 0.5f);
        ASSERT_NEAR("FFA mode picks authored spawn y",     p.y,  567.0f, 0.5f);
    }

    /* ---- Test 2: Multiple spawns, slot_index round-robins ---- */
    {
        LvlSpawn spawns[] = {
            { .pos_x =  100, .pos_y = 1000, .team = 0, .flags = 1 },
            { .pos_x =  300, .pos_y = 1000, .team = 0, .flags = 1 },
            { .pos_x =  500, .pos_y = 1000, .team = 0, .flags = 1 },
        };
        Level L;
        build_test_level(&L, spawns, 3, 60, 40);
        Vec2 p0 = map_spawn_point(MAP_FOUNDRY, &L, 0, MATCH_TEAM_RED, MATCH_MODE_FFA);
        Vec2 p1 = map_spawn_point(MAP_FOUNDRY, &L, 1, MATCH_TEAM_RED, MATCH_MODE_FFA);
        Vec2 p2 = map_spawn_point(MAP_FOUNDRY, &L, 2, MATCH_TEAM_RED, MATCH_MODE_FFA);
        Vec2 p3 = map_spawn_point(MAP_FOUNDRY, &L, 3, MATCH_TEAM_RED, MATCH_MODE_FFA);
        ASSERT_NEAR("slot 0 → spawns[0].x", p0.x, 100.0f, 0.5f);
        ASSERT_NEAR("slot 1 → spawns[1].x", p1.x, 300.0f, 0.5f);
        ASSERT_NEAR("slot 2 → spawns[2].x", p2.x, 500.0f, 0.5f);
        ASSERT_NEAR("slot 3 wraps → spawns[0].x", p3.x, 100.0f, 0.5f);
    }

    /* ---- Test 3: TDM honors team affinity ----
     * Two spawns: red at left, blue at right. Red player must pick
     * the red one even with slot_index=1. */
    {
        LvlSpawn spawns[] = {
            { .pos_x =  200, .pos_y = 1000, .team = 1, .flags = 1 }, /* red  */
            { .pos_x = 2200, .pos_y = 1000, .team = 2, .flags = 1 }, /* blue */
        };
        Level L;
        build_test_level(&L, spawns, 2, 80, 40);
        Vec2 red0  = map_spawn_point(MAP_FOUNDRY, &L, 0, MATCH_TEAM_RED,  MATCH_MODE_TDM);
        Vec2 red1  = map_spawn_point(MAP_FOUNDRY, &L, 1, MATCH_TEAM_RED,  MATCH_MODE_TDM);
        Vec2 blue0 = map_spawn_point(MAP_FOUNDRY, &L, 0, MATCH_TEAM_BLUE, MATCH_MODE_TDM);
        Vec2 blue1 = map_spawn_point(MAP_FOUNDRY, &L, 1, MATCH_TEAM_BLUE, MATCH_MODE_TDM);
        ASSERT_NEAR("TDM red slot 0  picks red spawn",  red0.x,  200.0f, 0.5f);
        ASSERT_NEAR("TDM red slot 1  picks red spawn",  red1.x,  200.0f, 0.5f);
        ASSERT_NEAR("TDM blue slot 0 picks blue spawn", blue0.x, 2200.0f, 0.5f);
        ASSERT_NEAR("TDM blue slot 1 picks blue spawn", blue1.x, 2200.0f, 0.5f);
    }

    /* ---- Test 4: team=0 (any) accepts both teams ---- */
    {
        LvlSpawn spawns[] = {
            { .pos_x = 800, .pos_y = 1000, .team = 0, .flags = 1 },
        };
        Level L;
        build_test_level(&L, spawns, 1, 60, 40);
        Vec2 p_red  = map_spawn_point(MAP_FOUNDRY, &L, 0, MATCH_TEAM_RED,  MATCH_MODE_TDM);
        Vec2 p_blue = map_spawn_point(MAP_FOUNDRY, &L, 0, MATCH_TEAM_BLUE, MATCH_MODE_TDM);
        ASSERT_NEAR("team=0 spawn matches red",  p_red.x,  800.0f, 0.5f);
        ASSERT_NEAR("team=0 spawn matches blue", p_blue.x, 800.0f, 0.5f);
    }

    /* ---- Test 5: Empty spawns falls back to hardcoded lanes ----
     * When level->spawn_count == 0 we should use the M4 lane tables.
     * The actual coordinates depend on the lane[] indices but the
     * y component is derived from level->height - so we assert that
     * we GET something reasonable rather than the spawn at (0,0). */
    {
        Level L;
        build_test_level(&L, NULL, 0, 100, 40);
        Vec2 p = map_spawn_point(MAP_FOUNDRY, &L, 0, MATCH_TEAM_RED, MATCH_MODE_FFA);
        ASSERT_TRUE("fallback lane returns positive x", p.x > 0.0f);
        ASSERT_TRUE("fallback lane returns floor-relative y", p.y > 0.0f && p.y < 32.0f * 40.0f);
    }

    if (g_failed > 0) {
        fprintf(stderr, "\nspawn_test: %d failures\n", g_failed);
        return 1;
    }
    fprintf(stdout, "\nspawn_test: all passed\n");
    return 0;
}
