/*
 * tests/bot_nav_test.c — M6 P05 Phase 1.
 *
 * Asserts on the per-node visibility precompute that drives Phase 2's
 * cover-aware engagement positioning:
 *
 *   1. Build the nav on every shipped map → vis_edge_count > 0 on each.
 *   2. Per-map symmetry: for every (src, target) pair where
 *      `nav_node_sees(src, target)`, the inverse query
 *      `nav_node_sees(target, src)` must also report true. Visibility
 *      is symmetric by construction (LOS rays are direction-agnostic),
 *      and Phase 2's BFS queries either direction depending on which
 *      side originated the candidate set — getting this wrong gives the
 *      bot one-way "engagement" nodes from which the enemy would
 *      simultaneously be invisible to the shooter.
 *   3. Spot-check on Reactor (the worst 1v1 baseline map): there exists
 *      at least one node-pair whose visibility-mask bit is set AND a
 *      pair whose bit is clear. Catches an "all-true" or "all-false"
 *      regression in the ray-cast pass.
 *
 * Test pattern mirrors tests/mech_ik_test.c (raw assert macros, single
 * `main`, exit code reflects failure count).
 */

#include "../src/arena.h"
#include "../src/bot.h"
#include "../src/game.h"
#include "../src/level.h"
#include "../src/log.h"
#include "../src/maps.h"

#include "raylib.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed = 0;

#define ASSERT_TRUE(label, cond) do {                                    \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL: %s\n", label);                            \
        ++g_failed;                                                      \
    } else {                                                             \
        fprintf(stderr, "PASS: %s\n", label);                            \
    }                                                                    \
} while (0)

#define ASSERT_GT(label, got, want) do {                                 \
    if (!((got) > (want))) {                                             \
        fprintf(stderr, "FAIL: %s: got=%d want>%d\n",                    \
                label, (int)(got), (int)(want));                         \
        ++g_failed;                                                      \
    } else {                                                             \
        fprintf(stderr, "PASS: %s (%d > %d)\n", label, (int)(got), (int)(want)); \
    }                                                                    \
} while (0)

static void assert_symmetry_for_map(Game *g, const char *short_name) {
    map_registry_init();
    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", short_name);
    if (!map_build_from_path(&g->world, &g->level_arena, path)) {
        fprintf(stderr, "WARN: %s — .lvl not on disk; SKIP\n", short_name);
        return;
    }

    BotSystem bs;
    bot_system_init(&bs);
    int nodes = bot_system_build_nav(&bs, &g->world.level, &g->level_arena);

    char buf[128];
    snprintf(buf, sizeof buf, "%s: nav builds with > 0 nodes", short_name);
    ASSERT_GT(buf, nodes, 0);

    int edges = bot_nav_visibility_edge_count(&bs);
    snprintf(buf, sizeof buf, "%s: visibility edge count > 0", short_name);
    ASSERT_GT(buf, edges, 0);

    /* Symmetry check — exhaustive over the node set. With 32 k-NN
     * targets per node and ~150 nodes, this is ~4800 forward queries
     * each of which may scan up to 64 slots. Sub-millisecond. */
    int asym = 0;
    for (int i = 0; i < nodes; ++i) {
        for (int j = i + 1; j < nodes; ++j) {
            bool ij = bot_nav_node_sees(&bs, i, j);
            bool ji = bot_nav_node_sees(&bs, j, i);
            if (ij != ji) ++asym;
        }
    }
    snprintf(buf, sizeof buf, "%s: visibility is symmetric (asym=%d)", short_name, asym);
    if (asym != 0) {
        fprintf(stderr, "FAIL: %s (asym=%d)\n", buf, asym);
        ++g_failed;
    } else {
        fprintf(stderr, "PASS: %s\n", buf);
    }

    /* Spot-check: at least one set bit AND at least one clear bit
     * within the kNN-tested pairs. If every bit were set or every bit
     * were clear it'd indicate either no LOS check happened or every
     * ray escaped — both regressions worth catching. */
    bool any_set = false, any_clear = false;
    for (int i = 0; i < nodes && !(any_set && any_clear); ++i) {
        for (int j = i + 1; j < nodes && !(any_set && any_clear); ++j) {
            /* Only consider pairs that are in i's kNN — otherwise the
             * "false" is by construction (out of vis_targets), not a
             * tested miss. We check via bot_nav_node_sees + a
             * geometric proximity heuristic instead. */
            Vec2 a, b;
            if (!bot_nav_node_pos(&bs, i, &a)) continue;
            if (!bot_nav_node_pos(&bs, j, &b)) continue;
            float dx = b.x - a.x, dy = b.y - a.y;
            float d2 = dx*dx + dy*dy;
            if (d2 > 1200.0f * 1200.0f) continue;   /* both should be in kNN at this range */
            bool s = bot_nav_node_sees(&bs, i, j);
            if (s) any_set = true; else any_clear = true;
        }
    }
    snprintf(buf, sizeof buf, "%s: at least one visible pair", short_name);
    ASSERT_TRUE(buf, any_set);
    snprintf(buf, sizeof buf, "%s: at least one blocked pair", short_name);
    ASSERT_TRUE(buf, any_clear);

    bot_system_destroy(&bs);
}

int main(void) {
    log_init(NULL);
    SetTraceLogLevel(LOG_WARNING);

    Game g;
    if (!game_init(&g)) {
        fprintf(stderr, "FAIL: game_init failed\n");
        return 1;
    }

    /* Cycle every shipped map. The bake harness uses the same set. */
    const char *maps[] = {
        "foundry", "slipstream", "reactor", "concourse",
        "catwalk", "aurora", "crossfire", "citadel",
    };
    for (int i = 0; i < (int)(sizeof maps / sizeof maps[0]); ++i) {
        /* Each map_build resets level_arena via the arena_reset path —
         * if not done by map_build itself, the per-map BotNav allocations
         * would compound across iterations. map_build calls arena_reset
         * internally (see src/maps.c::map_build_for_descriptor). */
        assert_symmetry_for_map(&g, maps[i]);
    }

    game_shutdown(&g);

    if (g_failed > 0) {
        fprintf(stderr, "\n%d assertion(s) failed\n", g_failed);
        return 1;
    }
    fprintf(stderr, "\nbot_nav: all assertions passed\n");
    return 0;
}
