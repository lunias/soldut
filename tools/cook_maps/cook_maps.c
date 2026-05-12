/*
 * cook_maps — P17 one-shot exporter.
 *
 * Synthesizes four authored .lvl files programmatically:
 *   assets/maps/foundry.lvl     (100×40, FFA|TDM, tutorial-scale)
 *   assets/maps/slipstream.lvl  (100×50, FFA|TDM, vertical + caves)
 *   assets/maps/reactor.lvl     (110×42, FFA|TDM, bowl + central pillar)
 *   assets/maps/concourse.lvl   (100×60, FFA|TDM, atrium + gallery)
 *
 * Per documents/m5/07-maps.md §"Per-map briefs", each map carries:
 *   - Tile-grid base (floor, walls, platforms, alcove cavities).
 *   - Slope vocabulary as LvlPoly triangles (30°/45°/60° per the brief).
 *   - LvlSpawn array (team-tagged for TDM-capable maps).
 *   - LvlPickup array sized per the brief's "Pickups" section.
 *   - LvlAmbi zones (WIND, FOG) where the brief calls for them.
 *   - LvlMeta with name/blurb/kit/music/ambient + mode_mask.
 *
 * Concourse is normally hand-authored in the P04 editor; this tool
 * emits a programmatic scaffold matching the brief so the build has a
 * loadable .lvl. A designer refines layout in the editor afterward.
 *
 * Build + run: `make cook-maps`.
 */

#define _POSIX_C_SOURCE 200809L

#include "../../src/arena.h"
#include "../../src/level_io.h"
#include "../../src/log.h"
#include "../../src/match.h"
#include "../../src/mech.h"
#include "../../src/weapons.h"
#include "../../src/world.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define TILE_PX 32

/* Static scratch sized for the biggest M5 map (Citadel, 200×100). */
#define MAX_TILES   (200 * 100)
#define MAX_POLYS   2048
#define MAX_SPAWNS  64
#define MAX_PICKUPS 64
#define MAX_DECOS   256
#define MAX_AMBIS   16
#define MAX_FLAGS   2
#define STRT_CAP    4096

typedef struct {
    LvlTile   tiles[MAX_TILES];
    LvlPoly   polys[MAX_POLYS];
    LvlSpawn  spawns[MAX_SPAWNS];
    LvlPickup pickups[MAX_PICKUPS];
    LvlDeco   decos[MAX_DECOS];
    LvlAmbi   ambis[MAX_AMBIS];
    LvlFlag   flags[MAX_FLAGS];
    char      strt[STRT_CAP];
    int       strt_used;

    World     world;
} Cooker;

static Cooker g_cooker;

static void cook_reset(int width, int height) {
    memset(&g_cooker, 0, sizeof g_cooker);
    Level *L              = &g_cooker.world.level;
    L->width              = width;
    L->height             = height;
    L->tile_size          = TILE_PX;
    L->gravity            = (Vec2){0.0f, 1080.0f};
    L->tiles              = g_cooker.tiles;
    L->polys              = g_cooker.polys;
    L->spawns             = g_cooker.spawns;
    L->pickups            = g_cooker.pickups;
    L->decos              = g_cooker.decos;
    L->ambis              = g_cooker.ambis;
    L->flags              = g_cooker.flags;
    g_cooker.strt[0]      = '\0';
    g_cooker.strt_used    = 1;            /* offset 0 = empty string */
    L->string_table       = g_cooker.strt;
    L->string_table_size  = g_cooker.strt_used;
}

/* Deduplicated string append. Returns the byte offset to `s` inside the
 * string-table blob (offset 0 = empty/missing). */
static int strt_intern(const char *s) {
    if (!s || !*s) return 0;
    int i = 1;                            /* offset 0 is the empty sentinel */
    while (i < g_cooker.strt_used) {
        if (strcmp(g_cooker.strt + i, s) == 0) return i;
        i += (int)strlen(g_cooker.strt + i) + 1;
    }
    size_t n = strlen(s) + 1;
    if (g_cooker.strt_used + (int)n > STRT_CAP) {
        fprintf(stderr, "cook_maps: strt overflow on %s\n", s);
        return 0;
    }
    int off = g_cooker.strt_used;
    memcpy(g_cooker.strt + off, s, n);
    g_cooker.strt_used += (int)n;
    g_cooker.world.level.string_table_size = g_cooker.strt_used;
    return off;
}

/* ---- Tile helpers --------------------------------------------------- */

static void tile_set(int tx, int ty, uint16_t flags) {
    Level *L = &g_cooker.world.level;
    if (tx < 0 || tx >= L->width || ty < 0 || ty >= L->height) return;
    L->tiles[ty * L->width + tx] = (LvlTile){ .id = 0, .flags = flags };
}

static void tile_fill(int x0, int y0, int x1, int y1, uint16_t flags) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            tile_set(x, y, flags);
}

/* ---- Polygon helpers ------------------------------------------------ */

/* Mirror of src/level.c::set_tri — vertex order is screen-clockwise
 * (y-down). Each edge's outward normal is (b-a)'s right-perpendicular
 * (dy, -dx), normalized to Q1.15. */
static void poly_set_tri(LvlPoly *q, PolyKind kind,
                         int x0, int y0, int x1, int y1, int x2, int y2) {
    memset(q, 0, sizeof *q);
    q->v_x[0] = (int16_t)x0; q->v_y[0] = (int16_t)y0;
    q->v_x[1] = (int16_t)x1; q->v_y[1] = (int16_t)y1;
    q->v_x[2] = (int16_t)x2; q->v_y[2] = (int16_t)y2;
    q->kind   = (uint16_t)kind;
    for (int e = 0; e < 3; ++e) {
        int a = e, b = (e + 1) % 3;
        float dx = (float)(q->v_x[b] - q->v_x[a]);
        float dy = (float)(q->v_y[b] - q->v_y[a]);
        float nx =  dy, ny = -dx;
        float Lm = sqrtf(nx*nx + ny*ny);
        if (Lm > 1e-4f) { nx /= Lm; ny /= Lm; }
        else            { nx = 0.0f; ny = -1.0f; }
        int qx = (int)(nx * 32767.0f), qy = (int)(ny * 32767.0f);
        if (qx >  32767) qx =  32767;
        if (qx < -32768) qx = -32768;
        if (qy >  32767) qy =  32767;
        if (qy < -32768) qy = -32768;
        q->normal_x[e] = (int16_t)qx;
        q->normal_y[e] = (int16_t)qy;
    }
}

static void push_tri(PolyKind kind,
                     int x0, int y0, int x1, int y1, int x2, int y2) {
    Level *L = &g_cooker.world.level;
    if (L->poly_count >= MAX_POLYS) return;
    poly_set_tri(&L->polys[L->poly_count++], kind, x0, y0, x1, y1, x2, y2);
}

/* Rectangular slab as 2 triangles (covers axis-aligned cover columns +
 * angled-strut volumes). Clockwise winding so the top-edge normal points
 * up, matching the physics push-out convention. */
static void push_quad(PolyKind kind, int x0, int y0, int x1, int y1) {
    push_tri(kind, x0, y0, x1, y0, x1, y1);
    push_tri(kind, x0, y0, x1, y1, x0, y1);
}

/* ---- Record helpers ------------------------------------------------- */

static void push_spawn(int wx, int wy, uint8_t team,
                       uint8_t flags, uint8_t lane_hint) {
    Level *L = &g_cooker.world.level;
    if (L->spawn_count >= MAX_SPAWNS) return;
    L->spawns[L->spawn_count++] = (LvlSpawn){
        .pos_x = (int16_t)wx, .pos_y = (int16_t)wy,
        .team = team, .flags = flags, .lane_hint = lane_hint, .reserved = 0,
    };
}

static void push_pickup(int wx, int wy, uint8_t kind, uint8_t variant) {
    Level *L = &g_cooker.world.level;
    if (L->pickup_count >= MAX_PICKUPS) return;
    L->pickups[L->pickup_count++] = (LvlPickup){
        .pos_x = (int16_t)wx, .pos_y = (int16_t)wy,
        .category = kind, .variant = variant,
        .respawn_ms = 0, .flags = 0, .reserved = 0,
    };
}

static void push_ambi(int x, int y, int w, int h, AmbiKind kind,
                      float strength, float dir_x, float dir_y) {
    Level *L = &g_cooker.world.level;
    if (L->ambi_count >= MAX_AMBIS) return;
    int s_q  = (int)(strength * 32767.0f);
    int dx_q = (int)(dir_x    * 32767.0f);
    int dy_q = (int)(dir_y    * 32767.0f);
    L->ambis[L->ambi_count++] = (LvlAmbi){
        .rect_x = (int16_t)x, .rect_y = (int16_t)y,
        .rect_w = (int16_t)w, .rect_h = (int16_t)h,
        .kind = (uint16_t)kind, .strength_q = (int16_t)s_q,
        .dir_x_q = (int16_t)dx_q, .dir_y_q = (int16_t)dy_q,
    };
}

/* ---- META populate ---- */
static void set_meta(const char *display_name, const char *blurb,
                     const char *kit_short, uint16_t mode_mask) {
    Level *L = &g_cooker.world.level;
    char music_path[64], amb_path[64];
    snprintf(music_path, sizeof music_path, "assets/music/%s.ogg", kit_short);
    snprintf(amb_path,   sizeof amb_path,   "assets/sfx/ambient_%s.ogg", kit_short);
    L->meta = (LvlMeta){
        .name_str_idx         = (uint16_t)strt_intern(display_name),
        .blurb_str_idx        = (uint16_t)strt_intern(blurb),
        .background_str_idx   = (uint16_t)strt_intern(kit_short),
        .music_str_idx        = (uint16_t)strt_intern(music_path),
        .ambient_loop_str_idx = (uint16_t)strt_intern(amb_path),
        .reverb_amount_q      = (uint16_t)(0.20f * 65535.0f),
        .mode_mask            = mode_mask,
        .reserved             = {0},
    };
    L->string_table_size = g_cooker.strt_used;
}

/* Tile-to-world helpers. */
static inline int t2w(int t) { return t * TILE_PX; }

/* Pelvis y for a feet-on-row spawn (mech standing pose centers pelvis
 * ~40 px above the foot contact point). */
static inline int feet_on_row(int row) {
    return row * TILE_PX - 40;
}

/* ===================================================================== */
/* ============================ FOUNDRY ================================ */
/* ===================================================================== */
/* 100×40 tutorial map. One 30°-ish center hill, 45° spawn ramps,
 * 2 edge alcoves cut into the outer walls. */

static void build_foundry(void) {
    cook_reset(100, 40);
    Level *L = &g_cooker.world.level;
    const int W = L->width, H = L->height;
    const uint16_t SOLID = TILE_F_SOLID;

    /* Main floor — bottom 4 rows. */
    tile_fill(0, H - 4, W, H, SOLID);

    /* Outer side walls. Leave a 2-tile cavity at each wall's inner face
     * for the spawn alcoves (cut after the wall fill so the alcove sits
     * INSIDE the wall envelope). */
    tile_fill(0,     H - 14, 2,     H - 4, SOLID);
    tile_fill(W - 2, H - 14, W,     H - 4, SOLID);

    /* Spawn alcoves — edge archetype. 2 tiles deep × 3 tiles tall, mouth
     * facing the play area. The outer wall stays solid as the back; the
     * inner-face tiles are empty so the mech walks in; a 2-tile-wide
     * solid cap forms the ceiling. */
    tile_fill(2,     H - 7, 4,     H - 4, TILE_F_EMPTY);
    tile_fill(2,     H - 8, 4,     H - 7, SOLID);
    tile_fill(W - 4, H - 7, W - 2, H - 4, TILE_F_EMPTY);
    tile_fill(W - 4, H - 8, W - 2, H - 7, SOLID);

    /* Spawn platforms — same M4 footprint, top edge at row H-9. */
    tile_fill(12, H - 10, 22,     H - 9, SOLID);
    tile_fill(W - 22, H - 10, W - 12, H - 9, SOLID);

    /* Mid cover wall — 5-tile tall, 2 wide. Cover and a jet-hop target. */
    tile_fill(49, H - 9, 51, H - 4, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y   = t2w(H - 4);
    const int plat_top  = t2w(H -  9);

    /* 45° spawn ramps — 4 tiles run × 4 tiles rise from floor to
     * platform top. Left side: foot at (8, floor) up to (12, plat). */
    push_tri(POLY_KIND_SOLID,
             t2w(8),  floor_y,
             t2w(12), plat_top,
             t2w(12), floor_y);
    /* Right side mirror — foot at (W-8, floor), peak at (W-12, plat). */
    push_tri(POLY_KIND_SOLID,
             t2w(W - 12), plat_top,
             t2w(W -  8), floor_y,
             t2w(W - 12), floor_y);

    /* 30° center hill — 3 tiles wide × 1 tile tall per slope, sits on
     * either side of the cover wall so the wall stands on top of it. */
    const int hill_peak_y = t2w(H - 5);
    /* Left up-slope (peak at the cover wall). */
    push_tri(POLY_KIND_SOLID,
             t2w(46), floor_y,
             t2w(49), hill_peak_y,
             t2w(49), floor_y);
    /* Right down-slope. */
    push_tri(POLY_KIND_SOLID,
             t2w(51), hill_peak_y,
             t2w(54), floor_y,
             t2w(51), floor_y);

    /* ---- Spawns — 8 total, FFA round-robin from lane_hint. Even
     * indices left side, odd right, so successive spawns avoid telefrag. */
    const int spawn_plat = plat_top - 40;
    const int spawn_floor = floor_y - 40;
    push_spawn(t2w(15),     spawn_plat,  1, 1, 0);
    push_spawn(t2w(W - 15), spawn_plat,  2, 1, 1);
    push_spawn(t2w(18),     spawn_plat,  1, 1, 2);
    push_spawn(t2w(W - 18), spawn_plat,  2, 1, 3);
    push_spawn(t2w(25),     spawn_floor, 1, 1, 4);
    push_spawn(t2w(W - 25), spawn_floor, 2, 1, 5);
    push_spawn(t2w(35),     spawn_floor, 1, 1, 6);
    push_spawn(t2w(W - 35), spawn_floor, 2, 1, 7);

    /* ---- Pickups (12 — per brief) ---- */
    const int floor_pick = floor_y - 16;
    const int hill_pick  = hill_peak_y - 16;
    const int plat_pick  = plat_top  - 16;
    /* 4× HEALTH small (one per quadrant, two on the hill peak). */
    push_pickup(t2w(20),     floor_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(W - 20), floor_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(47),     hill_pick,  PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(53),     hill_pick,  PICKUP_HEALTH, HEALTH_SMALL);
    /* 2× HEALTH medium mid-map. */
    push_pickup(t2w(42), floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(58), floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    /* 2× AMMO_PRIMARY in spawn alcoves. */
    push_pickup(t2w(3),     floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 3), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    /* 1× ARMOR light atop the cover wall (jet hop required). */
    push_pickup(t2w(50), plat_pick - 16, PICKUP_ARMOR, ARMOR_LIGHT);
    /* 1× WEAPON Auto-Cannon left of cover wall, 1× Plasma Cannon right. */
    push_pickup(t2w(40), floor_pick, PICKUP_WEAPON, WEAPON_AUTO_CANNON);
    push_pickup(t2w(60), floor_pick, PICKUP_WEAPON, WEAPON_PLASMA_CANNON);
    /* 1× JET_FUEL on the cover-wall top alongside the armor. */
    push_pickup(t2w(50), plat_pick - 64, PICKUP_JET_FUEL, 0);

    set_meta("Foundry",
             "Open floor with cover columns. Ground-game.",
             "foundry",
             (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM)));
}

/* ===================================================================== */
/* ============================ SLIPSTREAM ============================= */
/* ===================================================================== */
/* 100×50 vertical FFA — basement / main floor / upper catwalks. 60°
 * slide chutes, ICE patches on the catwalks, angled overhead struts,
 * 4-room basement cave network, 2 upper-catwalk jetpack alcoves. */

static void build_slipstream(void) {
    cook_reset(100, 50);
    Level *L = &g_cooker.world.level;
    const int W = L->width, H = L->height;
    const uint16_t SOLID = TILE_F_SOLID;
    const uint16_t ICE   = TILE_F_SOLID | TILE_F_ICE;

    /* Basement floor — bottom 4 rows. */
    tile_fill(0, H - 4, W, H, SOLID);

    /* Outer walls full height. */
    tile_fill(0,     0, 2,     H - 4, SOLID);
    tile_fill(W - 2, 0, W,     H - 4, SOLID);

    /* Main floor (split into left + right wings; the slide chutes are
     * the gaps that drop into the basement). */
    tile_fill(4,      H - 18, 28,     H - 17, SOLID);
    tile_fill(W - 28, H - 18, W -  4, H - 17, SOLID);
    /* Central island — small, encourages jet. */
    tile_fill(46, H - 18, 54, H - 17, SOLID);

    /* Upper catwalks at row H-32. */
    tile_fill(8,      H - 32, 28,     H - 31, SOLID);
    tile_fill(W - 28, H - 32, W -  8, H - 31, SOLID);

    /* Top connecting beam. */
    tile_fill(30, H - 38, W - 30, H - 37, SOLID);

    /* Catwalk ICE patches (4-tile-wide each). */
    tile_fill(12,     H - 32, 16,     H - 31, ICE);
    tile_fill(W - 16, H - 32, W - 12, H - 31, ICE);

    /* Two cover blocks on the main floor (basement-mouth markers). */
    tile_fill(28, H - 18, 29, H - 4, SOLID);
    tile_fill(W - 29, H - 18, W - 28, H - 4, SOLID);

    /* ---- Cave network in the basement — 4 connected rooms.
     * The basement is the strip below row H-12 (between the main floor
     * and the basement floor). I cut a 1-row ceiling at H-12 so the
     * basement reads as enclosed, then carve out 4 chambers separated
     * by short cross-walls. Passages between rooms are 3 tiles tall ×
     * 2 tiles wide (already met by the open basement). */
    tile_fill(4, H - 12, W - 4, H - 11, SOLID);     /* basement ceiling */
    /* Drop the ceiling out beneath the slide chutes so the player can
     * fall through (chutes land in the basement directly). */
    tile_fill(28, H - 12, 36, H - 11, TILE_F_EMPTY);
    tile_fill(W - 36, H - 12, W - 28, H - 11, TILE_F_EMPTY);
    /* Inter-room walls (short 4-tile stubs from the basement floor). */
    tile_fill(22, H -  8, 23, H - 4, SOLID);    /* between room 1 and 2 */
    tile_fill(45, H -  8, 46, H - 4, SOLID);    /* room 2 / 3 (central) */
    tile_fill(W - 46, H - 8, W - 45, H - 4, SOLID);
    tile_fill(W - 23, H - 8, W - 22, H - 4, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y    = t2w(H -  4);
    const int main_top   = t2w(H - 17);
    const int catwalk_y  = t2w(H - 31);
    const int basement_ceil = t2w(H - 11);

    /* 60° basement slide chutes — 4 tiles tall, ~2.5 tiles wide. Left
     * chute drops from main-floor edge at (28, main_top) down to
     * (33, basement_ceil) so the surface slope is the hypotenuse. */
    push_tri(POLY_KIND_SOLID,
             t2w(28), main_top,
             t2w(36), basement_ceil,
             t2w(28), basement_ceil);
    /* Right chute mirror. */
    push_tri(POLY_KIND_SOLID,
             t2w(W - 36), basement_ceil,
             t2w(W - 28), main_top,
             t2w(W - 28), basement_ceil);

    /* Angled overhead struts above the catwalks — 45° overhang triangles
     * jutting down from the level ceiling. Three total: one center-left,
     * one center-right, one centerline. */
    push_tri(POLY_KIND_SOLID,
             t2w(18), t2w(H - 36),
             t2w(22), t2w(H - 36),
             t2w(22), t2w(H - 32));
    push_tri(POLY_KIND_SOLID,
             t2w(W - 22), t2w(H - 36),
             t2w(W - 18), t2w(H - 36),
             t2w(W - 22), t2w(H - 32));
    push_tri(POLY_KIND_SOLID,
             t2w(48), t2w(H - 40),
             t2w(52), t2w(H - 40),
             t2w(50), t2w(H - 35));

    /* ---- WIND zones at slide-chute landings. ---- */
    /* Left chute landing: nudge sideways toward room 1 (negative x). */
    push_ambi(t2w(30), t2w(H - 10),
              t2w(12),  t2w(6),
              AMBI_WIND, 0.20f, -1.0f, 0.0f);
    /* Right chute landing: nudge toward room 4. */
    push_ambi(t2w(W - 42), t2w(H - 10),
              t2w(12), t2w(6),
              AMBI_WIND, 0.20f, +1.0f, 0.0f);

    /* ---- Spawns ---- */
    const int spawn_main = main_top  - 40;
    const int spawn_cat  = catwalk_y - 40;
    push_spawn(t2w(10),     spawn_main, 1, 1, 0);
    push_spawn(t2w(W - 10), spawn_main, 2, 1, 1);
    push_spawn(t2w(15),     spawn_main, 1, 1, 2);
    push_spawn(t2w(W - 15), spawn_main, 2, 1, 3);
    push_spawn(t2w(20),     spawn_main, 1, 1, 4);
    push_spawn(t2w(W - 20), spawn_main, 2, 1, 5);
    push_spawn(t2w(15),     spawn_cat,  1, 1, 6);
    push_spawn(t2w(W - 15), spawn_cat,  2, 1, 7);

    /* ---- Pickups (14 — per brief) ---- */
    const int basement_pick = floor_y - 16;
    const int main_pick     = main_top - 16;
    const int catwalk_pick  = catwalk_y - 16;
    const int beam_pick     = t2w(H - 37) - 16;
    /* Cave network — WEAPON Mass Driver in centermost room. */
    push_pickup(t2w(50), basement_pick, PICKUP_WEAPON, WEAPON_MASS_DRIVER);
    /* Side rooms: ARMOR heavy + HEALTH large. */
    push_pickup(t2w(33), basement_pick, PICKUP_ARMOR,  ARMOR_HEAVY);
    push_pickup(t2w(W - 33), basement_pick, PICKUP_HEALTH, HEALTH_LARGE);
    /* End rooms: AMMO_PRIMARY. */
    push_pickup(t2w(12),     basement_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 12), basement_pick, PICKUP_AMMO_PRIMARY, 0);
    /* Main floor: 3× HEALTH small, 1 AMMO_PRIMARY. */
    push_pickup(t2w(15),     main_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(50),     main_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(W - 15), main_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(50),     main_pick, PICKUP_AMMO_PRIMARY, 0);
    /* Upper catwalks: JET_FUEL + POWERUP berserk in alcove-ish positions. */
    push_pickup(t2w(11),     catwalk_pick, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(W - 11), catwalk_pick, PICKUP_POWERUP, POWERUP_BERSERK);
    /* Catwalk-mid extra small healths. */
    push_pickup(t2w(20),     catwalk_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(W - 20), catwalk_pick, PICKUP_HEALTH, HEALTH_SMALL);
    /* Top beam: WEAPON Rail Cannon (high-reward, exposed). */
    push_pickup(t2w(W / 2),  beam_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);

    set_meta("Slipstream",
             "Stacked catwalks. Vertical jet beats.",
             "maintenance",
             (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM)));
}

/* ===================================================================== */
/* ============================ REACTOR ================================ */
/* ===================================================================== */
/* 110×42 contested-center map. Central pillar with bowl floor, 45°
 * pillar-underside overhangs, 45° flanking ramps, 2 spawn-side edge
 * alcoves, 2 pillar-underside slope-roof alcoves. */

static void build_reactor(void) {
    cook_reset(110, 42);
    Level *L = &g_cooker.world.level;
    const int W = L->width, H = L->height;
    const uint16_t SOLID = TILE_F_SOLID;

    /* Floor. */
    tile_fill(0, H - 4, W, H, SOLID);

    /* Outer walls — short, capped at the flanking-platform height. */
    tile_fill(0,     H - 22, 2,     H - 4, SOLID);
    tile_fill(W - 2, H - 22, W,     H - 4, SOLID);

    /* Spawn-side edge alcoves cut into the lower wall. */
    tile_fill(2, H - 8, 4, H - 4, TILE_F_EMPTY);
    tile_fill(2, H - 9, 4, H - 8, SOLID);                /* cap */
    tile_fill(W - 4, H - 8, W - 2, H - 4, TILE_F_EMPTY);
    tile_fill(W - 4, H - 9, W - 2, H - 8, SOLID);

    /* Big central pillar (the reactor core). */
    tile_fill(51, H - 16, 59, H - 4, SOLID);

    /* Two flanking platforms at mid height. */
    tile_fill(16, H - 12, 38, H - 11, SOLID);
    tile_fill(72, H - 12, 94, H - 11, SOLID);

    /* High overlooks. */
    tile_fill(22, H - 22, 32, H - 21, SOLID);
    tile_fill(78, H - 22, 88, H - 21, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y     = t2w(H - 4);
    const int bowl_low    = t2w(H - 3);                  /* 1 tile below floor mark */
    const int pillar_left  = t2w(51), pillar_right = t2w(59);
    const int pillar_top   = t2w(H - 16);
    const int flank_left_top = t2w(38);                  /* right edge of left flank */
    const int flank_right_top = t2w(72);
    const int flank_top_y     = t2w(H - 11);

    /* Bowl floor — gentle 30° from spawn side toward the pillar base.
     * Each side is a long shallow triangle. The interior side meets the
     * pillar wall; the spawn side meets the outer wall. */
    push_tri(POLY_KIND_SOLID,
             t2w(4),  floor_y,
             t2w(51), bowl_low,
             t2w(51), floor_y);
    push_tri(POLY_KIND_SOLID,
             t2w(59), bowl_low,
             t2w(W - 4), floor_y,
             t2w(59), floor_y);

    /* 45° pillar-underside overhangs — slope-roof alcove ceiling. */
    push_tri(POLY_KIND_SOLID,
             pillar_left,  pillar_top,
             pillar_left,  pillar_top + t2w(4),
             pillar_left - t2w(4), pillar_top);
    push_tri(POLY_KIND_SOLID,
             pillar_right, pillar_top,
             pillar_right + t2w(4), pillar_top,
             pillar_right, pillar_top + t2w(4));

    /* 45° flanking ramps from bowl floor up to flanking platform. */
    push_tri(POLY_KIND_SOLID,
             flank_left_top - t2w(6), floor_y,
             flank_left_top,          flank_top_y,
             flank_left_top,          floor_y);
    push_tri(POLY_KIND_SOLID,
             flank_right_top,         flank_top_y,
             flank_right_top + t2w(6), floor_y,
             flank_right_top,         floor_y);

    /* ---- Spawns — TDM-friendly (red on left, blue on right). ---- */
    const int spawn_floor = floor_y - 40;
    push_spawn(t2w(6),       spawn_floor, 1, 1, 0);
    push_spawn(t2w(W - 6),   spawn_floor, 2, 1, 1);
    push_spawn(t2w(10),      spawn_floor, 1, 1, 2);
    push_spawn(t2w(W - 10),  spawn_floor, 2, 1, 3);
    push_spawn(t2w(20),      t2w(H - 12) - 40, 1, 1, 4);     /* flank platform */
    push_spawn(t2w(W - 20),  t2w(H - 12) - 40, 2, 1, 5);
    push_spawn(t2w(28),      t2w(H - 22) - 40, 1, 1, 6);     /* overlook */
    push_spawn(t2w(W - 28),  t2w(H - 22) - 40, 2, 1, 7);

    /* ---- Pickups (16 — per brief) ---- */
    const int floor_pick = floor_y - 16;
    const int flank_pick = t2w(H - 12) - 16;
    const int over_pick  = t2w(H - 22) - 16;
    /* Pillar base, both sides: WEAPON Plasma SMG twin. */
    push_pickup(t2w(48), floor_pick, PICKUP_WEAPON, WEAPON_PLASMA_SMG);
    push_pickup(t2w(62), floor_pick, PICKUP_WEAPON, WEAPON_PLASMA_SMG);
    /* Spawn alcoves — AMMO_PRIMARY. */
    push_pickup(t2w(3),     floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 3), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    /* Pillar-underside alcoves: JET_FUEL ×2. */
    push_pickup(t2w(48), pillar_top + t2w(3),  PICKUP_JET_FUEL, 0);
    push_pickup(t2w(62), pillar_top + t2w(3),  PICKUP_JET_FUEL, 0);
    /* Floor mid: HEALTH small ×4. */
    push_pickup(t2w(15),     floor_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(W - 15), floor_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(40),     floor_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(70),     floor_pick, PICKUP_HEALTH, HEALTH_SMALL);
    /* Flanking platforms: HEALTH medium ×2 + ARMOR light ×2. */
    push_pickup(t2w(22), flank_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(88), flank_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(32), flank_pick, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(78), flank_pick, PICKUP_ARMOR, ARMOR_LIGHT);
    /* High overlooks: WEAPON Mass Driver + Rail Cannon. */
    push_pickup(t2w(28), over_pick, PICKUP_WEAPON, WEAPON_MASS_DRIVER);
    push_pickup(t2w(82), over_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);

    set_meta("Reactor",
             "Central pillar, two flanking platforms.",
             "reactor",
             (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM)));

    /* unused-loc warnings */
    (void)bowl_low;
}

/* ===================================================================== */
/* ============================ CONCOURSE ============================== */
/* ===================================================================== */
/* 100×60 atrium. Per the brief, hand-authored in the editor — this
 * exporter ships a programmatic scaffold so the build has a loadable
 * .lvl while a designer refines the layout. Wings on each side, central
 * concourse with two 30° hills, upper gallery catwalks, 4 alcoves,
 * 2 FOG ambient zones. */

static void build_concourse(void) {
    cook_reset(100, 60);
    Level *L = &g_cooker.world.level;
    const int W = L->width, H = L->height;
    const uint16_t SOLID = TILE_F_SOLID;

    /* Floor across the full width. */
    tile_fill(0, H - 4, W, H, SOLID);

    /* Outer walls. */
    tile_fill(0,     0, 2,     H - 4, SOLID);
    tile_fill(W - 2, 0, W,     H - 4, SOLID);

    /* Wing partition walls (cut openings to the concourse). The wings
     * are cols 2..22 (left) and 78..98 (right). Partition walls at x=22
     * and x=78 run the full height, with 4-tile openings at the wing
     * exit doors (rows H-8..H-4). */
    tile_fill(22, 0, 23, H - 8, SOLID);
    tile_fill(77, 0, 78, H - 8, SOLID);

    /* Upper gallery catwalks — span the wings only, at row H-30. */
    tile_fill(2,  H - 30, 22, H - 29, SOLID);
    tile_fill(78, H - 30, 98, H - 29, SOLID);

    /* Concourse cover columns — 4 tile-pillars between x=30..70.
     * 2 tiles wide × 6 tiles tall from the floor. */
    tile_fill(30, H - 10, 32, H - 4, SOLID);
    tile_fill(43, H - 10, 45, H - 4, SOLID);
    tile_fill(55, H - 10, 57, H - 4, SOLID);
    tile_fill(68, H - 10, 70, H - 4, SOLID);

    /* Wing-floor edge alcoves (one per wing) — edge alcove archetype,
     * 2 tiles deep × 3 tiles tall against the outer wall, at floor row. */
    tile_fill(2, H - 7, 4, H - 4, TILE_F_EMPTY);
    tile_fill(2, H - 8, 4, H - 7, SOLID);
    tile_fill(W - 4, H - 7, W - 2, H - 4, TILE_F_EMPTY);
    tile_fill(W - 4, H - 8, W - 2, H - 7, SOLID);

    /* Upper-gallery jetpack alcoves — one per gallery, cut into the
     * partition wall, mouth facing the gallery interior. */
    tile_fill(20, H - 33, 22, H - 30, TILE_F_EMPTY);     /* left alcove cavity */
    tile_fill(20, H - 34, 22, H - 33, SOLID);            /* alcove ceiling */
    tile_fill(78, H - 33, 80, H - 30, TILE_F_EMPTY);     /* right alcove cavity */
    tile_fill(78, H - 34, 80, H - 33, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y = t2w(H - 4);

    /* Two long 30° hills under the concourse roof. Left hill: rises from
     * (24, floor) to peak at (35, floor-128), falls back to (46, floor).
     * Each side is a separate triangle. */
    const int hill_peak_y = t2w(H - 8);
    /* Left hill — rising slope. */
    push_tri(POLY_KIND_SOLID,
             t2w(24), floor_y,
             t2w(35), hill_peak_y,
             t2w(35), floor_y);
    /* Left hill — falling slope into the central valley. */
    push_tri(POLY_KIND_SOLID,
             t2w(35), hill_peak_y,
             t2w(42), floor_y,
             t2w(35), floor_y);
    /* Right hill mirror. */
    push_tri(POLY_KIND_SOLID,
             t2w(58), floor_y,
             t2w(65), hill_peak_y,
             t2w(65), floor_y);
    push_tri(POLY_KIND_SOLID,
             t2w(65), hill_peak_y,
             t2w(76), floor_y,
             t2w(65), floor_y);

    /* Wing-floor 30° valleys — single tile-depth shallow basins.
     * Left wing valley spans (5, floor)..(18, floor) with 1-tile dip. */
    const int valley_bottom = t2w(H - 3);
    push_tri(POLY_KIND_BACKGROUND,                       /* purely visual */
             t2w(5),  floor_y,
             t2w(12), valley_bottom,
             t2w(12), floor_y);
    push_tri(POLY_KIND_BACKGROUND,
             t2w(12), valley_bottom,
             t2w(18), floor_y,
             t2w(12), floor_y);
    push_tri(POLY_KIND_BACKGROUND,
             t2w(W - 18), floor_y,
             t2w(W - 12), valley_bottom,
             t2w(W - 12), floor_y);
    push_tri(POLY_KIND_BACKGROUND,
             t2w(W - 12), valley_bottom,
             t2w(W - 5),  floor_y,
             t2w(W - 12), floor_y);

    /* ---- FOG ambient zones in the upper gallery. ---- */
    push_ambi(t2w(2),      t2w(H - 38), t2w(20), t2w(8),
              AMBI_FOG, 0.50f, 0.0f, 0.0f);
    push_ambi(t2w(W - 22), t2w(H - 38), t2w(20), t2w(8),
              AMBI_FOG, 0.50f, 0.0f, 0.0f);

    /* ---- Spawns — 8 red on left wing, 8 blue on right wing. ---- */
    const int wing_spawn_y = floor_y - 40;
    const int gallery_y    = t2w(H - 30) - 40;
    /* RED — left wing. */
    push_spawn(t2w(4),  wing_spawn_y, 1, 1, 0);
    push_spawn(t2w(7),  wing_spawn_y, 1, 1, 1);
    push_spawn(t2w(10), wing_spawn_y, 1, 1, 2);
    push_spawn(t2w(13), wing_spawn_y, 1, 1, 3);
    push_spawn(t2w(16), wing_spawn_y, 1, 1, 4);
    push_spawn(t2w(19), wing_spawn_y, 1, 1, 5);
    push_spawn(t2w(8),  gallery_y,    1, 1, 6);
    push_spawn(t2w(14), gallery_y,    1, 1, 7);
    /* BLUE — right wing. */
    push_spawn(t2w(W -  4), wing_spawn_y, 2, 1, 8);
    push_spawn(t2w(W -  7), wing_spawn_y, 2, 1, 9);
    push_spawn(t2w(W - 10), wing_spawn_y, 2, 1, 10);
    push_spawn(t2w(W - 13), wing_spawn_y, 2, 1, 11);
    push_spawn(t2w(W - 16), wing_spawn_y, 2, 1, 12);
    push_spawn(t2w(W - 19), wing_spawn_y, 2, 1, 13);
    push_spawn(t2w(W -  8), gallery_y,    2, 1, 14);
    push_spawn(t2w(W - 14), gallery_y,    2, 1, 15);

    /* ---- Pickups (18 — per brief) ---- */
    const int floor_pick   = floor_y      - 16;
    const int gallery_pick = t2w(H - 30)  - 16;
    /* Mid: WEAPON Rail Cannon at the valley between hills. */
    push_pickup(t2w(W / 2), floor_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
    /* Wings: HEALTH medium ×2 each (in alcove + open). */
    push_pickup(t2w(3),     floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(12),    floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W - 3), floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W - 12),floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    /* Wings: AMMO_PRIMARY ×2 each. */
    push_pickup(t2w(5),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(18), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 5),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 18), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    /* Wing-basin: WEAPON Riot Cannon (left wing). */
    push_pickup(t2w(12), floor_pick, PICKUP_WEAPON, WEAPON_RIOT_CANNON);
    /* Gallery: HEALTH small ×4 (open catwalk). */
    push_pickup(t2w(6),     gallery_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(15),    gallery_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(W - 6),  gallery_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(W - 15), gallery_pick, PICKUP_HEALTH, HEALTH_SMALL);
    /* Gallery alcoves: ARMOR light + POWERUP invisibility. */
    push_pickup(t2w(21), t2w(H - 31) - 16, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(79), t2w(H - 31) - 16, PICKUP_POWERUP, POWERUP_INVISIBILITY);
    /* Doorways: AMMO_SECONDARY ×2 (hot-spot fights). */
    push_pickup(t2w(23), floor_pick, PICKUP_AMMO_SECONDARY, 0);
    push_pickup(t2w(W - 23), floor_pick, PICKUP_AMMO_SECONDARY, 0);

    set_meta("Concourse",
             "Long sightlines through the atrium. Mid-range fight.",
             "atrium",
             (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM)));
}

/* ===================================================================== */
/* ============================== DRIVER =============================== */
/* ===================================================================== */

static int cook_one(Arena *scratch, Arena *verify_arena,
                    const char *short_name, void (*builder)(void)) {
    builder();
    arena_reset(scratch);

    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", short_name);
    LvlResult r = level_save(&g_cooker.world, scratch, path);
    if (r != LVL_OK) {
        fprintf(stderr, "cook_maps: %s save failed: %s\n",
                short_name, level_io_result_str(r));
        return 1;
    }

    /* Round-trip verify — load it back into a fresh world and confirm
     * counts match. Catches malformed records that level_save accepts
     * but level_load rejects (e.g., a polygon vertex outside world
     * bounds slipping past validation). */
    arena_reset(verify_arena);
    World check;
    memset(&check, 0, sizeof check);
    LvlResult lr = level_load(&check, verify_arena, path);
    if (lr != LVL_OK) {
        fprintf(stderr, "cook_maps: %s round-trip load failed: %s\n",
                short_name, level_io_result_str(lr));
        return 1;
    }
    if (check.level.width  != g_cooker.world.level.width  ||
        check.level.height != g_cooker.world.level.height ||
        check.level.poly_count   != g_cooker.world.level.poly_count   ||
        check.level.spawn_count  != g_cooker.world.level.spawn_count  ||
        check.level.pickup_count != g_cooker.world.level.pickup_count ||
        check.level.ambi_count   != g_cooker.world.level.ambi_count) {
        fprintf(stderr, "cook_maps: %s round-trip count mismatch\n",
                short_name);
        return 1;
    }
    fprintf(stdout, "cook_maps: wrote %s (%dx%d, %d polys, %d spawns, "
                    "%d pickups, %d ambis) — round-trip OK\n",
            path,
            g_cooker.world.level.width,  g_cooker.world.level.height,
            g_cooker.world.level.poly_count,
            g_cooker.world.level.spawn_count,
            g_cooker.world.level.pickup_count,
            g_cooker.world.level.ambi_count);
    return 0;
}

static void ensure_dir(const char *path) {
    /* Idempotent mkdir. */
    if (mkdir(path, 0755) != 0) {
        /* errno EEXIST is fine. */
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    log_init(NULL);

    ensure_dir("assets");
    ensure_dir("assets/maps");

    Arena scratch, verify;
    arena_init(&scratch, 4u * 1024u * 1024u, "cook_scratch");
    arena_init(&verify,  4u * 1024u * 1024u, "cook_verify");

    int fail = 0;
    fail |= cook_one(&scratch, &verify, "foundry",    build_foundry);
    fail |= cook_one(&scratch, &verify, "slipstream", build_slipstream);
    fail |= cook_one(&scratch, &verify, "reactor",    build_reactor);
    fail |= cook_one(&scratch, &verify, "concourse",  build_concourse);

    if (fail) {
        fprintf(stderr, "cook_maps: one or more maps failed to write\n");
        return 1;
    }
    fprintf(stdout, "cook_maps: all 4 maps written\n");
    return 0;
}
