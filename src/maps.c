#include "maps.h"

#include "arena.h"
#include "level.h"
#include "level_io.h"
#include "log.h"
#include "match.h"
#include "mech.h"     /* ArmorId values for default-map pickup variants */

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
    [MAP_CROSSFIRE]  = { MAP_CROSSFIRE,  "crossfire",
                         "Crossfire",
                         "Symmetric CTF arena. Two team bases, central run.",
                         140, 42 },
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

/* Allocate a fixed-size pickup array on first use; subsequent calls
 * append (no-op if the cap is reached). Code-built maps hand-author
 * spawner positions so the user has something to grab in normal play
 * before P17 ships authored .lvl maps. Capacity 16 covers what fits
 * comfortably on the small M4 maps. */
#define MAP_PICKUP_FALLBACK_CAP 16
static void add_pickup_to_map(Level *L, Arena *arena, int wx, int wy,
                              uint8_t kind, uint8_t variant) {
    if (!L->pickups) {
        L->pickups = (LvlPickup *)arena_alloc(arena,
            sizeof(LvlPickup) * MAP_PICKUP_FALLBACK_CAP);
        if (!L->pickups) return;
        L->pickup_count = 0;
    }
    if (L->pickup_count >= MAP_PICKUP_FALLBACK_CAP) return;
    L->pickups[L->pickup_count++] = (LvlPickup){
        .pos_x      = (int16_t)wx,
        .pos_y      = (int16_t)wy,
        .category   = kind,
        .variant    = variant,
        .respawn_ms = 0,           /* use kind default */
        .flags      = 0,
        .reserved   = 0,
    };
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

    /* Pickups so normal play (no editor-authored .lvl) has something
     * to grab. P17 replaces this with authored placements; for now we
     * scatter four representative kinds along the natural traffic
     * paths. Tile-coord helpers: floor row is `height-4`, spawn
     * platform top sits at row `height-10`, dummy platform top at
     * row `height-8`. */
    int floor_y = (L->height - 4) * 32 - 16;
    int spawn_top_y = (L->height - 10) * 32 - 16;
    int dummy_top_y = (L->height - 8)  * 32 - 16;
    add_pickup_to_map(L, arena,  17 * 32, spawn_top_y, PICKUP_HEALTH,        HEALTH_MEDIUM);
    add_pickup_to_map(L, arena,  30 * 32, floor_y,     PICKUP_POWERUP,       POWERUP_INVISIBILITY);
    add_pickup_to_map(L, arena,  45 * 32, floor_y,     PICKUP_AMMO_PRIMARY,  0);
    add_pickup_to_map(L, arena,  75 * 32, dummy_top_y, PICKUP_POWERUP,       POWERUP_BERSERK);
    add_pickup_to_map(L, arena,  90 * 32, floor_y,     PICKUP_ARMOR,         ARMOR_LIGHT);

    L->meta.mode_mask = (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM));
    LOG_I("map: foundry built (%dx%d, %d pickups, mode_mask=0x%x)",
          L->width, L->height, L->pickup_count, (unsigned)L->meta.mode_mask);
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
    L->meta.mode_mask = (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM));
    LOG_I("map: slipstream built (%dx%d, mode_mask=0x%x)",
          L->width, L->height, (unsigned)L->meta.mode_mask);
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
    L->meta.mode_mask = (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM));
    LOG_I("map: reactor built (%dx%d, mode_mask=0x%x)",
          L->width, L->height, (unsigned)L->meta.mode_mask);
}

/* ---- MAP_CROSSFIRE (M5 P07) ---------------------------------------- */
/* The CTF map. Symmetric 140×42 layout — two team bases on far left and
 * far right with a small back wall, an elevated forward platform per
 * team, and a central depression with cover columns to funnel fights.
 *
 * The flag at each base sits on top of the rear elevated platform,
 * accessible only from the front (so a defender can sit between flag
 * and approach). Spawn lanes are biased to each team's back area so
 * the carrier has a legitimate run home.
 *
 * mode_mask carries FFA|TDM|CTF so the rotation can mix it in with
 * non-CTF rounds. */
static void build_crossfire(Level *L, Arena *arena) {
    map_alloc_tiles(L, arena, 140, 42);

    int W = L->width, H = L->height;
    /* Floor across the full width. */
    fill_rect(L, 0, H - 4, W, H, TILE_SOLID);

    /* Outer side walls (full height). */
    fill_rect(L, 0,     0, 2,     H - 4, TILE_SOLID);
    fill_rect(L, W - 2, 0, W,     H - 4, TILE_SOLID);

    /* RED base (left) — back platform 12 tiles deep, raised 6 tiles. */
    fill_rect(L, 4,  H - 10, 18, H - 9,  TILE_SOLID);   /* platform top */
    fill_rect(L, 16, H - 14, 18, H - 9,  TILE_SOLID);   /* short rear wall */

    /* BLUE base (right) — mirror of red. */
    fill_rect(L, W - 18, H - 10, W - 4, H - 9,  TILE_SOLID);
    fill_rect(L, W - 18, H - 14, W - 16, H - 9, TILE_SOLID);

    /* Forward elevated platforms — per-team approach to the central
     * battleground. RED's faces right, BLUE's faces left. */
    fill_rect(L, 26, H - 8, 36, H - 7, TILE_SOLID);
    fill_rect(L, W - 36, H - 8, W - 26, H - 7, TILE_SOLID);

    /* Central cover columns + a low mid platform. The map's "name"
     * (Crossfire) refers to the central exchange where both teams'
     * forward platforms have line-of-sight. */
    int mid = W / 2;
    fill_rect(L, mid - 12, H - 6, mid - 11, H - 4, TILE_SOLID);
    fill_rect(L, mid + 11, H - 6, mid + 12, H - 4, TILE_SOLID);
    fill_rect(L, mid - 4,  H - 7, mid + 4,  H - 6, TILE_SOLID);   /* mid bridge */

    /* High overlooks — a sniper perch per team, accessible by jet. */
    fill_rect(L, 12, H - 22, 22, H - 21, TILE_SOLID);
    fill_rect(L, W - 22, H - 22, W - 12, H - 21, TILE_SOLID);

    /* ---- Authored spawns (LvlSpawn) — preferred over the
     * hardcoded g_red/g_blue_lanes by map_spawn_point's M5 path.
     * Stagger horizontally inside each team's back third. */
    int n_spawns = 8;
    L->spawns = (LvlSpawn *)arena_alloc(arena, sizeof(LvlSpawn) * (size_t)n_spawns);
    if (L->spawns) {
        L->spawn_count = n_spawns;
        const int floor_y = (H - 4) * 32;        /* feet sit on top of row H-4 */
        const int spawn_y = floor_y - 40;        /* pelvis above the floor */
        const int plat_top_y = (H - 10) * 32 - 40;
        /* RED side: two on platform, two on floor between base and mid. */
        L->spawns[0] = (LvlSpawn){ .pos_x =  6 * 32, .pos_y = (int16_t)plat_top_y, .team = 1, .flags = 1, .lane_hint = 0 };
        L->spawns[1] = (LvlSpawn){ .pos_x = 12 * 32, .pos_y = (int16_t)plat_top_y, .team = 1, .flags = 1, .lane_hint = 1 };
        L->spawns[2] = (LvlSpawn){ .pos_x = 22 * 32, .pos_y = (int16_t)spawn_y,   .team = 1, .flags = 1, .lane_hint = 2 };
        L->spawns[3] = (LvlSpawn){ .pos_x = 30 * 32, .pos_y = (int16_t)spawn_y,   .team = 1, .flags = 1, .lane_hint = 3 };
        /* BLUE side: mirror. */
        L->spawns[4] = (LvlSpawn){ .pos_x = (int16_t)((W -  6) * 32), .pos_y = (int16_t)plat_top_y, .team = 2, .flags = 1, .lane_hint = 0 };
        L->spawns[5] = (LvlSpawn){ .pos_x = (int16_t)((W - 12) * 32), .pos_y = (int16_t)plat_top_y, .team = 2, .flags = 1, .lane_hint = 1 };
        L->spawns[6] = (LvlSpawn){ .pos_x = (int16_t)((W - 22) * 32), .pos_y = (int16_t)spawn_y,   .team = 2, .flags = 1, .lane_hint = 2 };
        L->spawns[7] = (LvlSpawn){ .pos_x = (int16_t)((W - 30) * 32), .pos_y = (int16_t)spawn_y,   .team = 2, .flags = 1, .lane_hint = 3 };
    }

    /* ---- Flags — one per team, hovering at chest height above each
     * team's back platform. Touch detection in ctf_step uses
     * mech_chest_pos against flag.home_pos with a 36 px radius
     * (FLAG_TOUCH_RADIUS_PX) — so flags need to sit near where a
     * standing mech's chest would actually be. The platform's top
     * tile row is `H-10`, top edge at y = (H-10)*32 = 1024. Pelvis
     * sits at floor - 36 = 988, chest ~30 px above pelvis = 958.
     * Setting flag y to platform_top - 50 = 974 puts the flag
     * staff/pennant in chest-overlap range (dy ≈ 16 → in radius). */
    L->flags = (LvlFlag *)arena_alloc(arena, sizeof(LvlFlag) * 2);
    if (L->flags) {
        L->flag_count = 2;
        const int chest_y = (H - 10) * 32 - 50;
        L->flags[0] = (LvlFlag){
            .pos_x = (int16_t)(10 * 32),         /* RED base */
            .pos_y = (int16_t)chest_y,
            .team  = 1,
        };
        L->flags[1] = (LvlFlag){
            .pos_x = (int16_t)((W - 10) * 32),   /* BLUE base */
            .pos_y = (int16_t)chest_y,
            .team  = 2,
        };
    }

    /* ---- Pickups — Health + Ammo on each side near mid; one armor
     * pack at center for risky teamplay. */
    int floor_y = (H - 4) * 32 - 16;
    int plat_y  = (H - 8)  * 32 - 16;
    add_pickup_to_map(L, arena,  40 * 32, floor_y, PICKUP_HEALTH,       HEALTH_SMALL);
    add_pickup_to_map(L, arena, (W - 40) * 32, floor_y, PICKUP_HEALTH,  HEALTH_SMALL);
    add_pickup_to_map(L, arena,  31 * 32, plat_y,  PICKUP_AMMO_PRIMARY, 0);
    add_pickup_to_map(L, arena, (W - 31) * 32, plat_y, PICKUP_AMMO_PRIMARY, 0);
    add_pickup_to_map(L, arena,  mid * 32, floor_y, PICKUP_ARMOR,       ARMOR_LIGHT);

    L->meta.mode_mask = (uint16_t)((1u << MATCH_MODE_FFA) |
                                    (1u << MATCH_MODE_TDM) |
                                    (1u << MATCH_MODE_CTF));
    LOG_I("map: crossfire built (%dx%d, %d spawns, %d flags, %d pickups, mode_mask=0x%x)",
          W, H, L->spawn_count, L->flag_count, L->pickup_count,
          (unsigned)L->meta.mode_mask);
}

static void build_fallback(MapId id, Level *level, Arena *arena) {
    switch (id) {
        case MAP_FOUNDRY:    build_foundry(level, arena);    break;
        case MAP_SLIPSTREAM: build_slipstream(level, arena); break;
        case MAP_REACTOR:    build_reactor(level, arena);    break;
        case MAP_CROSSFIRE:  build_crossfire(level, arena);  break;
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

    /* M5 P04+: prefer the .lvl's authored SPWN points when present.
     * Each `LvlSpawn` already specifies its world-pixel coords + team
     * affinity, so the runtime doesn't need to re-derive lanes.
     *
     * Match logic:
     *   - FFA: any spawn matches; sort by lane_hint and round-robin
     *     across slot_index.
     *   - TDM/CTF: prefer same-team or team=0 (any). If none match,
     *     fall back to the first authored spawn so the round still
     *     starts (better than the M4 hardcoded-lanes fallback). */
    if (level->spawn_count > 0 && level->spawns) {
        int n = level->spawn_count;
        int eligible[64];        /* small map cap; we have no map with >64 spawns */
        int e_count = 0;
        for (int i = 0; i < n && e_count < (int)(sizeof eligible / sizeof eligible[0]); ++i) {
            const LvlSpawn *s = &level->spawns[i];
            if (mode == MATCH_MODE_FFA ||
                s->team == 0 ||
                (int)s->team == team) {
                eligible[e_count++] = i;
            }
        }
        if (e_count == 0) {
            /* No team-matching spawn — accept the first one. */
            const LvlSpawn *s = &level->spawns[0];
            return (Vec2){ (float)s->pos_x, (float)s->pos_y };
        }
        const LvlSpawn *s = &level->spawns[eligible[slot_index % e_count]];
        return (Vec2){ (float)s->pos_x, (float)s->pos_y };
    }

    /* Pre-M5 / fallback: hardcoded per-team lane tables on top of the
     * (height - 4) floor row. */
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
