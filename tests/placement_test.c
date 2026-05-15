/*
 * placement_test — load every shipped .lvl under assets/maps/ and run
 * the placement validator. Asserts zero issues per file.
 *
 * Build: make test-map-placement
 * Run:   ./build/placement_test
 *
 * Print every issue with enough context to diff against cook_maps.c
 * (entity kind + index + world-space xy + tile/poly index). Returns
 * non-zero exit status when any file has any issue.
 *
 * Skips fixture files used by other harnesses (ctf_test.lvl,
 * grapple_test.lvl) — those are throwaway maps that don't ship in
 * the lobby rotation. The list of "shipped" maps is the same one
 * cook_maps emits.
 */

#define _POSIX_C_SOURCE 200809L

#include "../src/arena.h"
#include "../src/level_io.h"
#include "../src/log.h"
#include "../src/placement_validate.h"
#include "../src/world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kShipped[] = {
    "foundry", "slipstream", "reactor", "concourse",
    "catwalk", "aurora", "crossfire", "citadel",
};

#define PLACEMENT_ISSUE_CAP 256

static int validate_one(const char *short_name, Arena *level_arena) {
    arena_reset(level_arena);

    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", short_name);

    World w;
    memset(&w, 0, sizeof w);
    LvlResult r = level_load(&w, level_arena, path);
    if (r != LVL_OK) {
        fprintf(stderr, "FAIL: %s: level_load → %s\n",
                short_name, level_io_result_str(r));
        return 1;
    }

    PlacementIssue issues[PLACEMENT_ISSUE_CAP];
    int n = placement_validate(&w.level, issues, PLACEMENT_ISSUE_CAP);

    if (n == 0) {
        fprintf(stdout, "OK   %-10s — %d pickup / %d spawn / %d flag clean\n",
                short_name,
                w.level.pickup_count, w.level.spawn_count, w.level.flag_count);
        return 0;
    }

    fprintf(stderr, "FAIL %s — %d issue(s):\n", short_name, n);
    for (int i = 0; i < n; ++i) {
        const PlacementIssue *iss = &issues[i];
        int tx = (w.level.tile_size > 0) ? iss->x / w.level.tile_size : -1;
        int ty = (w.level.tile_size > 0) ? iss->y / w.level.tile_size : -1;
        fprintf(stderr,
                "  %-7s %-13s entity=%s idx=%-2d pos=(%d, %d) tile=(%d, %d) detail=%d\n",
                "issue:",
                placement_issue_kind_str(iss->kind),
                placement_entity_kind_str(iss->entity),
                iss->index, iss->x, iss->y, tx, ty, iss->detail);
    }
    return 1;
}

int main(void) {
    log_init(NULL);

    Arena level_arena;
    arena_init(&level_arena, 4u * 1024u * 1024u, "placement_test");

    int fail = 0;
    int n_maps = (int)(sizeof kShipped / sizeof kShipped[0]);
    for (int i = 0; i < n_maps; ++i) {
        fail |= validate_one(kShipped[i], &level_arena);
    }

    if (fail) {
        fprintf(stderr, "\nplacement_test: FAIL — one or more maps had issues\n");
        return 1;
    }
    fprintf(stdout, "\nplacement_test: all %d maps clean\n", n_maps);
    return 0;
}
