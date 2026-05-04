#include "maps.h"

#include "arena.h"
#include "level.h"
#include "level_io.h"
#include "log.h"
#include "match.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TILE_PX 32

static const MapDef g_maps[MAP_COUNT] = {
    [MAP_FOUNDRY]    = { MAP_FOUNDRY,    "foundry",
                         "Foundry",
                         "Open floor with cover columns. Ground-game.",
                         100, 40 },
    [MAP_SLIPSTREAM] = { MAP_SLIPSTREAM, "slipstream",
                         "Slipstream",
                         "Stacked catwalks. Vertical jet beats.",
                         100, 50 },
    [MAP_REACTOR]    = { MAP_REACTOR,    "reactor",
                         "Reactor",
                         "Central pillar, two flanking platforms.",
                         110, 42 },
};

const MapDef *map_def(int id) {
    if ((unsigned)id >= MAP_COUNT) return &g_maps[0];
    return &g_maps[id];
}

int map_id_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < MAP_COUNT; ++i) {
        if (strcasecmp(name, g_maps[i].short_name) == 0) return i;
        if (strcasecmp(name, g_maps[i].display_name) == 0) return i;
    }
    return -1;
}

static void set_tile(Level *l, int x, int y, TileKind k) {
    if (x < 0 || x >= l->width || y < 0 || y >= l->height) return;
    uint16_t f = (k == TILE_SOLID) ? TILE_F_SOLID : TILE_F_EMPTY;
    l->tiles[y * l->width + x] = (LvlTile){ .id = 0, .flags = f };
}

static void fill_rect(Level *l, int x0, int y0, int x1, int y1, TileKind k) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            set_tile(l, x, y, k);
}

static void map_alloc_tiles(Level *level, Arena *arena, int w, int h) {
    level->width     = w;
    level->height    = h;
    level->tile_size = TILE_PX;
    level->gravity   = (Vec2){0.0f, 1080.0f};
    int n = w * h;
    level->tiles = (LvlTile *)arena_alloc(arena, sizeof(LvlTile) * (size_t)n);
    if (!level->tiles) {
        LOG_E("map_alloc_tiles: arena out of memory (%dx%d=%zu bytes)",
              w, h, sizeof(LvlTile) * (size_t)n);
        return;
    }
    memset(level->tiles, 0, sizeof(LvlTile) * (size_t)n);
}

/* ---- MAP_FOUNDRY -------------------------------------------------- */
/* The existing tutorial map. Identical layout. */

static void build_foundry(Level *L, Arena *arena) {
    map_alloc_tiles(L, arena, 100, 40);
    /* Floor. */
    fill_rect(L, 0, L->height - 4, L->width, L->height, TILE_SOLID);
    /* Outer walls. */
    fill_rect(L, 0,            L->height - 12, 2,            L->height - 4, TILE_SOLID);
    fill_rect(L, L->width - 2, L->height - 12, L->width,     L->height - 4, TILE_SOLID);
    /* Spawn platform. */
    fill_rect(L, 12, L->height - 10, 22, L->height - 9, TILE_SOLID);
    /* Mid wall (cover column). */
    fill_rect(L, 55, L->height - 9,  56, L->height - 4, TILE_SOLID);
    /* Right-side dummy platform. */
    fill_rect(L, 70, L->height - 8,  80, L->height - 7, TILE_SOLID);
    LOG_I("map: foundry built (%dx%d)", L->width, L->height);
}

/* ---- MAP_SLIPSTREAM ----------------------------------------------- */
/* Three vertical layers — basement, main floor, catwalks. Player spawns
 * are on opposite sides; the catwalks reward jet hops. */

static void build_slipstream(Level *L, Arena *arena) {
    map_alloc_tiles(L, arena, 100, 50);
    /* Basement floor — bottom 4 rows. */
    fill_rect(L, 0, L->height - 4, L->width, L->height, TILE_SOLID);
    /* Outer walls (full vertical). */
    fill_rect(L, 0,            0, 2,        L->height - 4, TILE_SOLID);
    fill_rect(L, L->width - 2, 0, L->width, L->height - 4, TILE_SOLID);
    /* Mid floor — main combat layer. */
    fill_rect(L, 4,  L->height - 18, 36, L->height - 17, TILE_SOLID);
    fill_rect(L, 64, L->height - 18, 96, L->height - 17, TILE_SOLID);
    /* Mid-floor central island (smaller — encourages jet). */
    fill_rect(L, 46, L->height - 18, 54, L->height - 17, TILE_SOLID);
    /* Upper catwalks. */
    fill_rect(L, 8,  L->height - 32, 28, L->height - 31, TILE_SOLID);
    fill_rect(L, 72, L->height - 32, 92, L->height - 31, TILE_SOLID);
    /* Connecting beam at the top. */
    fill_rect(L, 30, L->height - 38, 70, L->height - 37, TILE_SOLID);
    /* Two cover blocks on the main floor. */
    fill_rect(L, 30, L->height - 8,  31, L->height - 4, TILE_SOLID);
    fill_rect(L, 69, L->height - 8,  70, L->height - 4, TILE_SOLID);
    LOG_I("map: slipstream built (%dx%d)", L->width, L->height);
}

/* ---- MAP_REACTOR -------------------------------------------------- */
/* Central solid pillar with two flanking elevated platforms. Plays as
 * a wider "Foundry" with a strong contested midpoint. */

static void build_reactor(Level *L, Arena *arena) {
    map_alloc_tiles(L, arena, 110, 42);
    /* Floor. */
    fill_rect(L, 0, L->height - 4, L->width, L->height, TILE_SOLID);
    /* Outer walls. */
    fill_rect(L, 0,            L->height - 18, 2,        L->height - 4, TILE_SOLID);
    fill_rect(L, L->width - 2, L->height - 18, L->width, L->height - 4, TILE_SOLID);
    /* Big central pillar (the reactor core). */
    fill_rect(L, 51, L->height - 16, 59, L->height - 4,  TILE_SOLID);
    /* Two flanking platforms at mid height. */
    fill_rect(L, 16, L->height - 12, 38, L->height - 11, TILE_SOLID);
    fill_rect(L, 72, L->height - 12, 94, L->height - 11, TILE_SOLID);
    /* High overlooks. */
    fill_rect(L, 22, L->height - 22, 32, L->height - 21, TILE_SOLID);
    fill_rect(L, 78, L->height - 22, 88, L->height - 21, TILE_SOLID);
    LOG_I("map: reactor built (%dx%d)", L->width, L->height);
}

static void build_fallback(MapId id, Level *level, Arena *arena) {
    switch (id) {
        case MAP_FOUNDRY:    build_foundry(level, arena);    break;
        case MAP_SLIPSTREAM: build_slipstream(level, arena); break;
        case MAP_REACTOR:    build_reactor(level, arena);    break;
        default:             build_foundry(level, arena);    break;
    }
}

void map_build(MapId id, World *world, Arena *arena) {
    const MapDef *def = map_def(id);
    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", def->short_name);

    LvlResult r = level_load(world, arena, path);
    if (r == LVL_OK) return;

    /* P17 will produce assets/maps/<short>.lvl from the code-built maps;
     * until then a fresh checkout has no .lvl files and we always end up
     * here. Use LOG_W only when the file existed but failed validation —
     * "file not found" is the expected case at M5 ship and shouldn't
     * pollute logs. */
    if (r == LVL_ERR_FILE_NOT_FOUND) {
        LOG_I("map_build(%s): no .lvl on disk — using code-built fallback",
              def->short_name);
    } else {
        LOG_W("map_build(%s): level_load failed (%s) — using code-built fallback",
              def->short_name, level_io_result_str(r));
    }
    build_fallback(id, &world->level, arena);
}

bool map_build_from_path(World *world, Arena *arena, const char *path) {
    if (!path || !path[0]) return false;
    LvlResult r = level_load(world, arena, path);
    if (r == LVL_OK) {
        LOG_I("map_build_from_path: loaded %s", path);
        return true;
    }
    LOG_E("map_build_from_path(%s): level_load failed (%s) — falling back to Foundry",
          path, level_io_result_str(r));
    build_fallback(MAP_FOUNDRY, &world->level, arena);
    return false;
}

/* ---- Spawn-point selection --------------------------------------- */
/* Stagger horizontally so successive spawns from the same team don't
 * telefrag. We pick from a per-map lane table. Y is derived from the
 * floor-y so any map with floor at `height-4` works. */

static const int g_red_lanes [16] = { 8, 12, 16, 10, 14, 6, 18, 20, 4, 22, 24, 26, 28, 30, 32, 34 };
static const int g_blue_lanes[16] = { 92, 88, 84, 90, 86, 94, 82, 80, 96, 78, 76, 74, 72, 70, 68, 66 };
static const int g_ffa_lanes [16] = { 16, 80, 30, 70, 24, 76, 12, 88, 44, 56, 20, 84, 36, 64, 50, 60 };

Vec2 map_spawn_point(MapId id, const Level *level, int slot_index,
                     int team, MatchModeId mode)
{
    (void)id;
    const float feet_below_pelvis = 36.0f;
    const float foot_clearance    = 4.0f;
    float floor_y = (float)(level->height - 4) * (float)level->tile_size
                  - feet_below_pelvis - foot_clearance;

    /* Mode wins over team in FFA: MATCH_TEAM_FFA aliases MATCH_TEAM_RED
     * (both = 1) so a naive team-only check would jam every FFA player
     * onto the red lanes (clustered at x=8..14). FFA lanes are spread
     * across the full map width so two players spawn ~64 tiles apart
     * instead of 4. */
    const int *lanes;
    if (mode == MATCH_MODE_FFA)            lanes = g_ffa_lanes;
    else if (team == MATCH_TEAM_BLUE)      lanes = g_blue_lanes;
    else if (team == MATCH_TEAM_RED)       lanes = g_red_lanes;
    else                                    lanes = g_ffa_lanes;

    int lane_count = 16;
    int tx = lanes[slot_index % lane_count];
    if (tx < 4) tx = 4;
    if (tx >= level->width - 4) tx = level->width - 5;

    return (Vec2){ (float)tx * (float)level->tile_size + 8.0f, floor_y };
}
