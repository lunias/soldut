#pragma once

#include <stdint.h>

/*
 * level_io — read and write the .lvl binary format.
 *
 * Format spec: documents/m5/01-lvl-format.md.
 *
 * level_load() reads a .lvl from disk into the supplied World, using
 * `level_arena` for the file buffer + every per-section storage array.
 * The arena MUST be reset before this call; a half-loaded level cannot
 * be cleanly torn down without arena_reset (the file's string table is
 * left in place inside the arena and Level.string_table aliases into it).
 *
 * level_save() emits the World's level state as a .lvl. The editor (P04)
 * is its primary caller; the runtime loader does not call it. `scratch`
 * is used to assemble the file in memory before a single fwrite.
 */

struct World;
struct Arena;

#define LVL_MAGIC0 'S'
#define LVL_MAGIC1 'D'
#define LVL_MAGIC2 'L'
#define LVL_MAGIC3 'V'
#define LVL_VERSION_CURRENT 1u

/* 1 MB hard cap. The map-size budget in 07-level-design.md is 50–500 KB. */
#define LVL_FILE_MAX_BYTES (1u * 1024u * 1024u)

typedef enum {
    LVL_OK = 0,
    LVL_ERR_FILE_NOT_FOUND,
    LVL_ERR_TOO_LARGE,
    LVL_ERR_BAD_MAGIC,
    LVL_ERR_BAD_VERSION,
    LVL_ERR_BAD_CRC,
    LVL_ERR_BAD_DIRECTORY,
    LVL_ERR_BAD_SECTION,
    LVL_ERR_OOM,
    LVL_ERR_IO,
} LvlResult;

LvlResult level_load(struct World *world, struct Arena *level_arena,
                     const char *path);

LvlResult level_save(const struct World *world, struct Arena *scratch,
                     const char *path);

const char *level_io_result_str(LvlResult r);

/* Public CRC32 helper — table-based, polynomial 0xEDB88320. Exposed so
 * the editor / cook tool can verify a buffer without re-saving. */
uint32_t level_crc32(const uint8_t *data, int n);

struct Level;

/* Build the per-tile polygon broadphase from the level's existing
 * `polys` array. level_load() calls this internally; code-built maps
 * (src/maps.c) must call it manually after populating `polys` /
 * `poly_count` so the runtime collision pass sees the polygons. */
LvlResult level_build_poly_broadphase(struct Level *level, struct Arena *arena);
