/*
 * tests/synth_map.c — synthesize a .lvl on disk for the map-share tests.
 *
 * Usage:
 *   synth_map <output_path> [<seed>]
 *
 * Builds a small synthetic level via level_save. The optional `seed`
 * parameter varies the floor row's tile id so distinct seeds yield
 * distinct CRCs.
 *
 * No raylib — only level_io + arena + a tiny World stub.
 */

#include "../src/arena.h"
#include "../src/level.h"
#include "../src/level_io.h"
#include "../src/log.h"
#include "../src/world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <output_path> [<seed>]\n", argv[0]);
        return 1;
    }
    const char *out_path = argv[1];
    unsigned seed = (argc >= 3) ? (unsigned)atoi(argv[2]) : 1u;

    Arena scratch = {0};
    arena_init(&scratch, 4 * 1024 * 1024, "scratch");

    /* Minimal World shell — only the fields level_save reads. */
    static World w;
    memset(&w, 0, sizeof(w));
    Level *L = &w.level;
    L->width     = 16;
    L->height    = 12;
    L->tile_size = 32;

    int n = L->width * L->height;
    L->tiles = (LvlTile *)arena_alloc(&scratch, sizeof(LvlTile) * (size_t)n);
    memset(L->tiles, 0, sizeof(LvlTile) * (size_t)n);

    /* Floor row + one cover column. Seed varies the floor tile id so
     * distinct seeds yield distinct CRCs. */
    for (int x = 0; x < L->width; ++x) {
        L->tiles[(L->height - 1) * L->width + x].flags = TILE_F_SOLID;
        L->tiles[(L->height - 1) * L->width + x].id    = (uint16_t)(seed & 0xffu);
    }
    L->tiles[(L->height - 4) * L->width + (L->width / 2)].flags = TILE_F_SOLID;

    L->spawns = (LvlSpawn *)arena_alloc(&scratch, sizeof(LvlSpawn) * 2);
    L->spawn_count = 2;
    L->spawns[0] = (LvlSpawn){ .pos_x = 96,  .pos_y = 320, .team = 1 };
    L->spawns[1] = (LvlSpawn){ .pos_x = 416, .pos_y = 320, .team = 2 };

    /* Stamp META with mode_mask = FFA + TDM. String-table indices
     * default to 0 (empty). */
    L->meta.mode_mask = 0x3;

    LvlResult r = level_save(&w, &scratch, out_path);
    if (r != LVL_OK) {
        fprintf(stderr, "synth_map: level_save failed: %s\n",
                level_io_result_str(r));
        return 2;
    }
    fprintf(stdout, "%s\n", out_path);
    return 0;
}
