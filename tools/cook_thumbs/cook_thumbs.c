/*
 * cook_thumbs — refresh sidecar thumbnails from existing .lvl files.
 *
 * Unlike cook_maps (which rewrites the 8 authored .lvls from
 * code-built builders), this tool ONLY reads a .lvl and writes its
 * matching `<stem>_thumb.png` next to it. Safe to run on every build:
 * `make` invokes it for every .lvl whose thumb is older than the .lvl,
 * so designers + the editor + cook_maps can all rely on the thumb
 * being current without anyone having to remember a separate step.
 *
 * Usage: `cook_thumbs <path/to/level.lvl>...`
 *
 * Returns non-zero on any failure (the Makefile rule depends on this).
 */

#define _POSIX_C_SOURCE 200809L

#include "../../src/arena.h"
#include "../../src/level_io.h"
#include "../../src/log.h"
#include "../../src/map_thumb.h"
#include "../../src/world.h"

#include "raylib.h"

#include <stdio.h>
#include <string.h>

#define SCRATCH_BYTES (4u * 1024u * 1024u)

static int do_one(const char *lvl_path, Arena *level_arena) {
    arena_reset(level_arena);

    World w;
    memset(&w, 0, sizeof w);
    LvlResult r = level_load(&w, level_arena, lvl_path);
    if (r != LVL_OK) {
        fprintf(stderr, "cook_thumbs: %s load failed: %s\n",
                lvl_path, level_io_result_str(r));
        return 1;
    }

    /* Derive sidecar path: swap trailing ".lvl" for "_thumb.png". */
    char thumb_path[512];
    size_t n = strlen(lvl_path);
    if (n < 4 || strcmp(lvl_path + n - 4, ".lvl") != 0) {
        fprintf(stderr, "cook_thumbs: %s does not end in .lvl\n", lvl_path);
        return 1;
    }
    if (n - 4 + 11 >= sizeof thumb_path) {
        fprintf(stderr, "cook_thumbs: %s path too long\n", lvl_path);
        return 1;
    }
    memcpy(thumb_path, lvl_path, n - 4);
    memcpy(thumb_path + n - 4, "_thumb.png", 11);

    if (!map_thumb_write_png(&w.level, thumb_path)) {
        fprintf(stderr, "cook_thumbs: %s thumb write failed\n", thumb_path);
        return 1;
    }

    fprintf(stdout, "cook_thumbs: %s -> %s\n", lvl_path, thumb_path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: cook_thumbs <path/to/level.lvl>...\n");
        return 2;
    }
    /* raylib's log output is noisy at TRACE/INFO and shows up in the
     * build log; mute it. Our own LOG_I lines stay since log_init
     * uses the project's logger, not raylib's. */
    SetTraceLogLevel(LOG_WARNING);
    log_init(NULL);

    Arena scratch;
    arena_init(&scratch, SCRATCH_BYTES, "cook_thumbs_scratch");

    int fail = 0;
    for (int i = 1; i < argc; ++i) {
        fail |= do_one(argv[i], &scratch);
    }
    return fail ? 1 : 0;
}
