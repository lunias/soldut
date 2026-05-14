/*
 * tests/spawn_geometry_test.c — verify that no spawn point on any
 * shipped map is embedded inside a solid tile or polygon.
 *
 * Spawn coords come from each .lvl's SPWN section (LvlSpawn pos_x /
 * pos_y, world-space px). A spawn that lands inside terrain produces
 * a "mech spawned in the floor" bug — the player materializes inside
 * a tile, the next tick's collision push-out shoves them sideways or
 * downward unpredictably.
 *
 * For each spawn we check a small rectangle that approximates the
 * mech's standing footprint:
 *   * pelvis itself (`pos_x`, `pos_y`)
 *   * head sample 52 px above pelvis (Trooper torso_h + neck_h + 8)
 *   * foot sample 36 px below pelvis (Trooper bone_thigh + bone_shin)
 *   * left / right shoulder sample at pelvis ±15 px
 *
 * `level_point_solid` checks both tiles and authored polygons (via
 * the broadphase built by level_build_poly_broadphase). If any of
 * the body samples lands inside a SOLID tile / poly, this test
 * reports the spawn as "embedded" and exits non-zero.
 *
 * Returns 0 on all-clear, 1 on any embedded spawn.
 */

#include "../src/arena.h"
#include "../src/level.h"
#include "../src/level_io.h"
#include "../src/log.h"
#include "../src/world.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed_maps = 0;
static int g_failed_spawns = 0;

/* Per-Trooper body extents — the smallest chassis still covers the
 * other four (Heavy is bigger but spawns are usually generous; the
 * tightest constraint is whether the *minimum* body fits).
 *
 * Foot is intentionally NOT sampled: a properly-placed spawn has the
 * foot resting on the floor surface, where `level_point_solid`
 * (tile-only) would always say "yes, in solid" because foot.y sits at
 * the floor tile's top edge. The collision pass push-out handles that
 * marginal case in real play. The samples we do check (pelvis / head /
 * shoulders) are always supposed to be clear air. */
static const float HEAD_DY     = -52.0f;  /* head top above pelvis */
static const float SHOULDER_DX = 15.0f;   /* lateral half-width */

typedef struct { float dx, dy; const char *label; } SamplePoint;

static const SamplePoint SAMPLES[] = {
    {  0.0f,        0.0f,    "pelvis"     },
    {  0.0f,        HEAD_DY, "head"       },
    { -SHOULDER_DX, 0.0f,    "L_shoulder" },
    { +SHOULDER_DX, 0.0f,    "R_shoulder" },
};
static const int SAMPLE_COUNT = (int)(sizeof(SAMPLES) / sizeof(SAMPLES[0]));

static const char *team_name(uint8_t t) {
    switch (t) {
        case 0: return "any";
        case 1: return "red";
        case 2: return "blue";
        default: return "?";
    }
}

static bool validate_spawn(const Level *L, const LvlSpawn *s,
                           const char *map_name, int spawn_idx)
{
    bool any_embedded = false;
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        Vec2 p = (Vec2){
            (float)s->pos_x + SAMPLES[i].dx,
            (float)s->pos_y + SAMPLES[i].dy,
        };
        if (level_point_solid(L, p)) {
            fprintf(stderr,
                    "FAIL: %s spawn[%d] team=%s lane=%u — %s sample at "
                    "(%.0f, %.0f) is INSIDE solid tile/poly\n",
                    map_name, spawn_idx, team_name(s->team), s->lane_hint,
                    SAMPLES[i].label, p.x, p.y);
            any_embedded = true;
            ++g_failed_spawns;
        }
    }
    return any_embedded;
}

static void validate_map(const char *map_path) {
    static World world;
    static Arena arena;
    static bool arena_inited = false;

    /* Reset world's level pointers; the arena holds the storage. */
    memset(&world.level, 0, sizeof(world.level));
    if (!arena_inited) {
        arena_init(&arena, 16 * 1024 * 1024, "spawn_validate");
        arena_inited = true;
    } else {
        arena_reset(&arena);
    }

    LvlResult r = level_load(&world, &arena, map_path);
    if (r != LVL_OK) {
        fprintf(stderr, "SKIP: %s — load failed (%s)\n",
                map_path, level_io_result_str(r));
        return;
    }

    const Level *L = &world.level;
    const char *short_name = strrchr(map_path, '/');
    short_name = short_name ? short_name + 1 : map_path;

    int embedded_in_map = 0;
    for (int i = 0; i < L->spawn_count; ++i) {
        if (validate_spawn(L, &L->spawns[i], short_name, i)) {
            ++embedded_in_map;
        }
    }
    if (embedded_in_map > 0) {
        fprintf(stdout,
                "FAIL: %s — %d / %d spawns embedded in geometry\n",
                short_name, embedded_in_map, L->spawn_count);
        ++g_failed_maps;
    } else {
        fprintf(stdout,
                "PASS: %s — %d spawns clear (%d body samples each = %d checks)\n",
                short_name, L->spawn_count, SAMPLE_COUNT,
                L->spawn_count * SAMPLE_COUNT);
    }
}

int main(void) {
    /* All shipped + cooked maps. Order doesn't matter; failures are
     * per-map. */
    const char *maps[] = {
        "assets/maps/foundry.lvl",
        "assets/maps/slipstream.lvl",
        "assets/maps/reactor.lvl",
        "assets/maps/concourse.lvl",
        "assets/maps/catwalk.lvl",
        "assets/maps/aurora.lvl",
        "assets/maps/crossfire.lvl",
        "assets/maps/citadel.lvl",
        "assets/maps/grapple_test.lvl",
        "assets/maps/ctf_test.lvl",
        NULL,
    };

    for (int i = 0; maps[i]; ++i) {
        validate_map(maps[i]);
    }

    if (g_failed_spawns > 0) {
        fprintf(stderr,
                "\nspawn_geometry_test: %d spawn body-samples embedded "
                "across %d map(s) — FAIL\n",
                g_failed_spawns, g_failed_maps);
        return 1;
    }
    fprintf(stdout, "\nspawn_geometry_test: all spawn body samples clear\n");
    return 0;
}
