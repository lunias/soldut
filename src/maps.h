#pragma once

#include "arena.h"
#include "match.h"
#include "world.h"

/*
 * maps — registry of M4 hard-coded maps.
 *
 * The proper .lvl loader + level editor lands at M5
 * (documents/07-level-design.md, documents/11-roadmap.md §M5). For M4 we
 * need the lobby/match flow to support map switching, so we ship three
 * code-built maps the lobby can rotate through.
 */

typedef enum {
    MAP_FOUNDRY = 0,         /* the existing tutorial map */
    MAP_SLIPSTREAM,          /* taller layout, more vertical jet beats */
    MAP_REACTOR,             /* central pillar, two flanking platforms */
    MAP_CROSSFIRE,           /* M5 P07 — symmetric CTF arena, two team bases */
    MAP_COUNT
} MapId;

typedef struct MapDef {
    MapId       id;
    const char *short_name;
    const char *display_name;
    const char *blurb;
    int         tile_w, tile_h;
} MapDef;

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
