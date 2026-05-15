#pragma once

#include "arena.h"
#include "match.h"
#include "world.h"

#include <stdint.h>

/*
 * maps — runtime registry of playable maps.
 *
 * The four reserved indices (MAP_FOUNDRY..MAP_CROSSFIRE) are LOAD-BEARING
 * for `build_fallback`'s code-built switch; they map to the three M4
 * arenas plus the P07 CTF arena and seed the registry on every launch.
 *
 * P08b — `g_map_registry` is populated at startup from the four code-built
 * defaults plus every `assets/maps/<name>.lvl` file the host finds on disk
 * (display_name + mode_mask read from the file's META lump without a full
 * level_load). A designer who saves `assets/maps/my_arena.lvl` from the
 * editor sees `my_arena` in the lobby's map cycle on next launch.
 *
 * Wire format is unchanged — `MapDescriptor` carries `short_name + crc +
 * size`, and `match.map_id` is a host-local index that the client uses
 * only as a display hint (the descriptor is the truth).
 */

typedef enum {
    MAP_FOUNDRY = 0,         /* the existing tutorial map */
    MAP_SLIPSTREAM,          /* taller layout, more vertical jet beats */
    MAP_REACTOR,             /* central pillar, two flanking platforms */
    MAP_CROSSFIRE,           /* M5 P07 — symmetric CTF arena, two team bases */
    MAP_BUILTIN_COUNT,       /* = 4; reserved indices for code-built fallbacks */
} MapId;

#define MAP_REGISTRY_MAX 32  /* hard cap on total maps (builtins + custom) */

typedef struct MapDef {
    int      id;                /* index into g_map_registry.entries */
    char     short_name[24];    /* filename stem, lowercased, ASCII */
    char     display_name[32];  /* META display, or short_name title-cased */
    char     blurb[64];         /* META blurb, or "" (custom maps don't carry one) */
    int      tile_w, tile_h;    /* from .lvl header (or code-built default) */
    uint16_t mode_mask;         /* FFA=1, TDM=2, CTF=4 */
    bool     has_lvl_on_disk;   /* false → code-built fallback only */
    uint32_t file_crc;          /* 0 if no .lvl on disk */
    uint32_t file_size;         /* 0 if no .lvl on disk */
} MapDef;

typedef struct MapRegistry {
    MapDef entries[MAP_REGISTRY_MAX];
    int    count;
} MapRegistry;

/* Process-global registry. Built once via `map_registry_init` (called
 * from game_init); subsequent module calls (map_def, map_id_from_name,
 * config_pick_map, lobby UI cycle) read from this. */
extern MapRegistry g_map_registry;

/* Populate registry: 4 code-built defaults first, then scan
 * `assets/maps/<name>.lvl` and append (or override-by-short-name) every
 * file found. Idempotent — safe to call multiple times for
 * hot-reload-style "the editor just saved a file" rescans (P08b ships
 * startup-only; rescan trigger is a future task). */
void map_registry_init(void);

/* Same as map_registry_init but scans a caller-supplied directory.
 * Used by tests; production callers should use map_registry_init. */
void map_registry_init_from(const char *maps_dir);

const MapDef *map_def(int id);
int           map_id_from_name(const char *name);

/* Build the chosen map into `world->level`, allocating from `arena`.
 * The arena should be reset (level_arena) before this call so successive
 * map loads don't accumulate. M5: tries to load assets/maps/<short>.lvl
 * via level_io first; on any load failure (file missing, bad CRC, etc.)
 * falls back to the code-built layout so a fresh checkout without
 * shipped .lvl files still boots. */
void map_build(MapId id, World *world, Arena *arena);

/* Load a .lvl directly from an absolute path. Used by the editor's F5
 * test-play (`./soldut --test-play /path/to/scratch.lvl`). On failure
 * logs and falls back to MAP_FOUNDRY's code-built layout so the game
 * still boots. Returns true if the .lvl loaded cleanly. */
bool map_build_from_path(World *world, Arena *arena, const char *path);

/* World-space spawn point picker. `slot_index` is the lobby slot id
 * (0..MAX_LOBBY_SLOTS-1) so successive spawns don't telefrag. team is
 * MATCH_TEAM_RED / MATCH_TEAM_BLUE / MATCH_TEAM_NONE — used for
 * red-vs-blue side bias on TDM/CTF maps. `mode` is the match mode;
 * in FFA we ignore the team value (which aliases MATCH_TEAM_RED) and
 * pick lanes that spread players across the full map width. */
Vec2 map_spawn_point(MapId id, const Level *level, int slot_index,
                     int team, MatchModeId mode);

/* Greedy max-min-distance spawn picker. Given a list of `n_used`
 * positions already chosen for previous slots in this round, returns
 * the eligible spawn (matching team / FFA) that is farthest from any
 * of them. First call with n_used=0 returns the lowest-lane_hint
 * eligible spawn (deterministic). Used by lobby_spawn_round_mechs to
 * avoid the round-robin "you spawn next to your opponent" problem in
 * FFA where consecutive lobby slots used to pick consecutive (often
 * adjacent) authored spawns. Falls back to map_spawn_point when the
 * level has no authored spawns or no eligibles. */
Vec2 map_pick_separated_spawn(MapId id, const Level *level,
                              int team, MatchModeId mode,
                              const Vec2 *used_positions, int n_used);

/* M5 P08 — client-side map build by descriptor. Walks the same
 * resolution order as client_resolve_or_download:
 *   1. assets/maps/<short>.lvl with matching CRC → load
 *   2. download_cache/<crc>.lvl with matching CRC → load
 *   3. fall back to code-built map for the supplied MapId
 *
 * Used by ROUND_START on the client so a downloaded map actually
 * loads from cache instead of getting stomped by the MapId rotation. */
struct MapDescriptor;
void map_build_for_descriptor(World *world, Arena *arena,
                              const struct MapDescriptor *desc,
                              MapId fallback_id);

/* M5 P08 — refill the host's MapDescriptor + serve_path after a
 * map_build / map_build_from_path. Probes the .lvl on disk and reads
 * its CRC + size; on a code-built map (no .lvl on disk), zeroes the
 * descriptor so clients know not to expect a download.
 *
 * Inputs:
 *   short_name   — map short_name (e.g. "foundry"), or the basename of
 *                  test_play_lvl when in F5 test-play mode.
 *   serve_path_in — when non-NULL, the host's local path to the .lvl
 *                   (used for test-play with absolute paths). When NULL,
 *                   defaults to assets/maps/<short_name>.lvl.
 *
 * Outputs (non-NULL ptrs):
 *   *out_desc       — filled from the file header (or zeroed if missing)
 *   out_serve_path  — set to the resolved host-local .lvl path
 */
struct MapDescriptor;
void maps_refresh_serve_info(const char *short_name,
                             const char *serve_path_in,
                             struct MapDescriptor *out_desc,
                             char *out_serve_path,
                             size_t out_serve_path_cap);
