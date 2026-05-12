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

#include "raylib.h"

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

static void push_flag(int wx, int wy, uint8_t team) {
    Level *L = &g_cooker.world.level;
    if (L->flag_count >= MAX_FLAGS) return;
    L->flags[L->flag_count++] = (LvlFlag){
        .pos_x = (int16_t)wx, .pos_y = (int16_t)wy,
        .team = team, .reserved = {0, 0, 0},
    };
}

static void push_deco(int wx, int wy, float scale, float rot_turns,
                      const char *sprite_path, uint8_t layer, uint8_t flags) {
    Level *L = &g_cooker.world.level;
    if (L->deco_count >= MAX_DECOS) return;
    L->decos[L->deco_count++] = (LvlDeco){
        .pos_x = (int16_t)wx, .pos_y = (int16_t)wy,
        .scale_q = (int16_t)(scale * 32767.0f),
        .rot_q   = (int16_t)(rot_turns * 256.0f),
        .sprite_str_idx = (uint16_t)strt_intern(sprite_path),
        .layer = layer, .flags = flags,
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
/* ============================ CATWALK ================================ */
/* ===================================================================== */
/* 120×70 TDM. Vertical layout — RED base at the bottom-left, BLUE base
 * at the top-right, connected by stacked catwalks. 60° external slide
 * slopes drop between catwalks; 45° angled overhead struts above mid;
 * 3 mid-catwalk jetpack alcoves; 2 WIND zones at the top. */

static void build_catwalk(void) {
    cook_reset(120, 70);
    Level *L = &g_cooker.world.level;
    const int W = L->width, H = L->height;
    const uint16_t SOLID = TILE_F_SOLID;

    /* Floor + outer ceiling band. */
    tile_fill(0, H - 4, W, H, SOLID);
    tile_fill(0, 0, W, 2, SOLID);

    /* Outer walls. */
    tile_fill(0,     2, 2,     H - 4, SOLID);
    tile_fill(W - 2, 2, W,     H - 4, SOLID);

    /* RED spawn alcove (bottom-left, edge archetype). Cavity at floor
     * level, capped above so jets won't pop the player out the roof. */
    tile_fill(2, H - 9, 14, H - 4, TILE_F_EMPTY);
    tile_fill(2, H - 10, 14, H - 9, SOLID);

    /* BLUE spawn alcove (top-right, jetpack archetype). Cavity 6 tiles
     * tall × 12 wide, with its own platform floor at row 11 + roof at
     * row 5. */
    tile_fill(W - 14, 6, W - 2, 11, TILE_F_EMPTY);
    tile_fill(W - 14, 11, W - 2, 12, SOLID);
    tile_fill(W - 14,  5, W - 2,  6, SOLID);

    /* Catwalks — 4 layers + bottom floor.
     *
     *   row    span
     *   ─────────────────────────────
     *   20-21  cw4 — top connector x=40..80
     *   32-33  cw3 — left x=20..50, right x=70..100 (split)
     *   44-45  cw2 — left x=15..50, right x=70..105 (split)
     *   56-57  cw1 — long central x=25..95 */
    tile_fill(40, H - 50, 80,  H - 49, SOLID);
    tile_fill(20, H - 38, 50,  H - 37, SOLID);
    tile_fill(70, H - 38, 100, H - 37, SOLID);
    tile_fill(15, H - 26, 50,  H - 25, SOLID);
    tile_fill(70, H - 26, 105, H - 25, SOLID);
    tile_fill(25, H - 14, 95,  H - 13, SOLID);

    /* Jetpack alcoves above central catwalks (3). 3-wide × 3-deep cavities
     * cut into the side walls — only reachable by jet from below. */
    tile_fill(2, H - 43, 8, H - 40, TILE_F_EMPTY);    /* above cw3 left */
    tile_fill(2, H - 44, 8, H - 43, SOLID);
    tile_fill(W - 8, H - 43, W - 2, H - 40, TILE_F_EMPTY);     /* above cw3 right */
    tile_fill(W - 8, H - 44, W - 2, H - 43, SOLID);
    tile_fill(58, H - 56, 62, H - 53, TILE_F_EMPTY);  /* above cw4 (overhead) */
    tile_fill(58, H - 57, 62, H - 56, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y = t2w(H - 4);
    const int cw1_top = t2w(H - 13);
    const int cw3_top = t2w(H - 37);
    const int cw4_top = t2w(H - 49);
    const int red_plat_y  = t2w(H - 9);
    const int blue_plat_y = t2w(11);

    /* 60° slide slopes — drop from cw3 endpoints to cw1 (centre catwalk).
     * tan(60°) ≈ 1.73 → ~5 tile rise per 3 tile run. */
    push_tri(POLY_KIND_SOLID,
             t2w(50), cw3_top, t2w(55), cw1_top, t2w(50), cw1_top);
    push_tri(POLY_KIND_SOLID,
             t2w(65), cw1_top, t2w(70), cw3_top, t2w(70), cw1_top);
    /* 60° drops from cw4 to cw3. */
    push_tri(POLY_KIND_SOLID,
             t2w(40), cw4_top, t2w(45), cw3_top, t2w(40), cw3_top);
    push_tri(POLY_KIND_SOLID,
             t2w(75), cw3_top, t2w(80), cw4_top, t2w(80), cw3_top);

    /* 45° angled overhead struts — 5 small triangles hanging from the
     * top ceiling band. Each is a right-triangle with the hypotenuse
     * acting as a slide-jet redirect surface. */
    {
        const int cy = t2w(2);
        push_tri(POLY_KIND_SOLID, t2w(20), cy, t2w(26), cy, t2w(20), cy + t2w(3));
        push_tri(POLY_KIND_SOLID, t2w(45), cy, t2w(51), cy, t2w(45), cy + t2w(3));
        push_tri(POLY_KIND_SOLID, t2w(70), cy, t2w(76), cy, t2w(76), cy + t2w(3));
        push_tri(POLY_KIND_SOLID, t2w(95), cy, t2w(101),cy, t2w(101),cy + t2w(3));
        push_tri(POLY_KIND_SOLID, t2w(58), cy, t2w(64), cy, t2w(58), cy + t2w(3));
    }

    /* WIND ambient zones at top (sideways push on the highest catwalk). */
    push_ambi(t2w(38), t2w(H - 56), t2w(20), t2w(8),
              AMBI_WIND, 0.18f, +1.0f, 0.0f);
    push_ambi(t2w(20), t2w(H - 44), t2w(20), t2w(6),
              AMBI_WIND, 0.15f, -1.0f, 0.0f);

    /* ---- Spawns — 8 RED on bottom, 8 BLUE on top. ---- */
    const int red_floor_y = floor_y - 40;
    const int red_plat    = red_plat_y  - 40;
    const int blue_plat   = blue_plat_y - 40;
    push_spawn(t2w(3),  red_floor_y, 1, 1, 0);
    push_spawn(t2w(6),  red_floor_y, 1, 1, 1);
    push_spawn(t2w(9),  red_floor_y, 1, 1, 2);
    push_spawn(t2w(12), red_floor_y, 1, 1, 3);
    push_spawn(t2w(20), red_floor_y, 1, 1, 4);
    push_spawn(t2w(28), red_floor_y, 1, 1, 5);
    push_spawn(t2w(40), red_floor_y, 1, 1, 6);
    push_spawn(t2w(60), red_floor_y, 1, 1, 7);

    push_spawn(t2w(W -  3), blue_plat, 2, 1, 8);
    push_spawn(t2w(W -  6), blue_plat, 2, 1, 9);
    push_spawn(t2w(W -  9), blue_plat, 2, 1, 10);
    push_spawn(t2w(W - 12), blue_plat, 2, 1, 11);
    push_spawn(t2w(60),     t2w(H - 49) - 40, 2, 1, 12);    /* cw4 top */
    push_spawn(t2w(35),     t2w(H - 37) - 40, 2, 1, 13);
    push_spawn(t2w(85),     t2w(H - 37) - 40, 2, 1, 14);
    push_spawn(t2w(W - 30), blue_plat, 2, 1, 15);

    /* ---- Pickups (≈20 per brief). ---- */
    const int floor_pick = floor_y - 16;
    const int cw1_pick   = cw1_top - 16;
    const int cw2_pick   = t2w(H - 25) - 16;
    const int cw3_pick   = cw3_top - 16;
    const int cw4_pick   = cw4_top - 16;
    const int red_pick   = red_plat_y  - 16;
    const int blue_pick  = blue_plat_y - 16;

    /* Red base alcove. */
    push_pickup(t2w(3),  red_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(6),  red_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(9),  red_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(11), red_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(13), red_pick, PICKUP_AMMO_PRIMARY, 0);
    /* Blue base alcove. */
    push_pickup(t2w(W -  3), blue_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  6), blue_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  9), blue_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W - 11), blue_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 13), blue_pick, PICKUP_AMMO_PRIMARY, 0);
    /* Mid-catwalk jetpack alcoves: JET_FUEL, ARMOR light, POWERUP berserk. */
    push_pickup(t2w(5),     t2w(H - 41) - 16, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(W - 5), t2w(H - 41) - 16, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(60),    t2w(H - 54) - 16, PICKUP_POWERUP, POWERUP_BERSERK);
    /* Open catwalks: HEALTH_SMALL ×4 + JET_FUEL ×3. */
    push_pickup(t2w(35), cw3_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(85), cw3_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(30), cw2_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(90), cw2_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(40), cw1_pick, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(60), cw1_pick, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(80), cw1_pick, PICKUP_JET_FUEL, 0);
    /* Highest catwalk: Rail Cannon (exposed). */
    push_pickup(t2w(60), cw4_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
    /* Lowest gap mid-floor: Mass Driver (suicide pickup — you have to
     * jet back up after grabbing). */
    push_pickup(t2w(60), floor_pick, PICKUP_WEAPON, WEAPON_MASS_DRIVER);

    set_meta("Catwalk",
             "Vertical TDM. Slide catwalks; grapple-friendly struts.",
             "exterior",
             (uint16_t)(1u << MATCH_MODE_TDM));

    (void)red_plat;
}

/* ===================================================================== */
/* ============================== AURORA =============================== */
/* ===================================================================== */
/* 160×90 open arena, TDM|FFA. Two big 30° hills, 45° central pit valley,
 * floating sky struts (grapple-only), corner mountain peaks with jetpack
 * alcoves, 1 ZERO_G zone at the very top, and ~30 POLY_KIND_BACKGROUND
 * skyline silhouettes for depth. */

static void build_aurora(void) {
    cook_reset(160, 90);
    Level *L = &g_cooker.world.level;
    const int W = L->width, H = L->height;
    const uint16_t SOLID = TILE_F_SOLID;

    /* Floor. */
    tile_fill(0, H - 4, W, H, SOLID);

    /* Outer walls — short, capped at mountain-peak height. */
    tile_fill(0,     H - 35, 2,     H - 4, SOLID);
    tile_fill(W - 2, H - 35, W,     H - 4, SOLID);

    /* Mountain peaks at the corners — tall pillars from floor to row
     * H-30, capped with a small 4×3 platform at the top + a hidden
     * alcove behind them. */
    tile_fill(2,  H - 35, 10, H - 4, SOLID);            /* west peak body */
    tile_fill(2,  H - 38, 10, H - 35, SOLID);           /* peak rise */
    tile_fill(W - 10, H - 35, W - 2, H - 4, SOLID);     /* east peak body */
    tile_fill(W - 10, H - 38, W - 2, H - 35, SOLID);

    /* Mountain-peak jetpack alcoves — cavity carved into the peak,
     * mouth facing inward (toward the map). 3 wide × 3 tall × 4 deep. */
    tile_fill( 6,  H - 38, 10, H - 35, TILE_F_EMPTY);
    tile_fill( 6,  H - 39, 10, H - 38, SOLID);
    tile_fill(W - 10, H - 38, W - 6, H - 35, TILE_F_EMPTY);
    tile_fill(W - 10, H - 39, W - 6, H - 38, SOLID);

    /* Hill-side edge alcoves cut into each mountain at floor level. */
    tile_fill(10, H - 7, 14, H - 4, TILE_F_EMPTY);
    tile_fill(10, H - 8, 14, H - 7, SOLID);
    tile_fill(W - 14, H - 7, W - 10, H - 4, TILE_F_EMPTY);
    tile_fill(W - 14, H - 8, W - 10, H - 7, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y     = t2w(H -  4);
    const int hill_peak_y = t2w(H - 11);                /* west / east hill tops */
    const int pit_low_y   = t2w(H -  4 + 2);            /* central pit basin */
    const int peak_top    = t2w(H - 35);

    /* Two big 30° hills — ~10 tiles wide each. tan(30°) ≈ 0.577 →
     * 7 tile rise / 12 tile run. */
    /* West hill — rises eastward from x=20 to x=32. */
    push_tri(POLY_KIND_SOLID,
             t2w(20), floor_y,
             t2w(32), hill_peak_y,
             t2w(32), floor_y);
    /* West hill — falls eastward into the central pit. */
    push_tri(POLY_KIND_SOLID,
             t2w(32), hill_peak_y,
             t2w(44), floor_y,
             t2w(32), floor_y);
    /* East hill — mirror. */
    push_tri(POLY_KIND_SOLID,
             t2w(W - 32), floor_y,
             t2w(W - 20), hill_peak_y,
             t2w(W - 20), floor_y);
    push_tri(POLY_KIND_SOLID,
             t2w(W - 32), hill_peak_y,
             t2w(W - 32), floor_y,
             t2w(W - 44), floor_y);
    (void)pit_low_y;

    /* 45° central pit valley — 4-tile basin at the bottom with 45°
     * slopes on each side, ~14 tiles wide each side. */
    push_tri(POLY_KIND_BACKGROUND,
             t2w(60), floor_y,
             t2w(74), floor_y + t2w(4),
             t2w(74), floor_y);
    push_tri(POLY_KIND_BACKGROUND,
             t2w(86), floor_y + t2w(4),
             t2w(100), floor_y,
             t2w(86), floor_y);

    /* 6 floating overhead struts (grapple anchors). Small horizontal
     * platforms in the sky, no surface ramps. */
    push_quad(POLY_KIND_SOLID, t2w(40), t2w(H - 50),  t2w(46), t2w(H - 49));
    push_quad(POLY_KIND_SOLID, t2w(58), t2w(H - 55),  t2w(64), t2w(H - 54));
    push_quad(POLY_KIND_SOLID, t2w(78), t2w(H - 58),  t2w(84), t2w(H - 57));
    push_quad(POLY_KIND_SOLID, t2w(96), t2w(H - 55),  t2w(102),t2w(H - 54));
    push_quad(POLY_KIND_SOLID, t2w(114),t2w(H - 50),  t2w(120),t2w(H - 49));
    push_quad(POLY_KIND_SOLID, t2w(70), t2w(H - 70),  t2w(76), t2w(H - 69));

    /* ZERO_G zone at the very top. */
    push_ambi(t2w(20), t2w(2), t2w(W - 40), t2w(15),
              AMBI_ZERO_G, 1.00f, 0.0f, 0.0f);

    /* 30+ POLY_KIND_BACKGROUND skyline silhouettes for parallax depth.
     * Two rows of small spikes at varied heights. */
    for (int i = 0; i < 18; ++i) {
        int x = 14 + i * 8;
        int top_y = t2w(H - 28 + ((i * 7) % 5));
        push_tri(POLY_KIND_BACKGROUND,
                 t2w(x),     floor_y,
                 t2w(x + 4), top_y,
                 t2w(x + 4), floor_y);
    }
    for (int i = 0; i < 14; ++i) {
        int x = 28 + i * 10;
        int top_y = t2w(H - 33 + ((i * 11) % 7));
        push_tri(POLY_KIND_BACKGROUND,
                 t2w(x),     top_y,
                 t2w(x + 6), floor_y,
                 t2w(x),     floor_y);
    }

    /* ---- Spawns — TDM split: 8 red on west, 8 blue on east. ---- */
    const int spawn_floor = floor_y - 40;
    const int peak_spawn  = peak_top - 40;
    push_spawn(t2w(12),     spawn_floor, 1, 1, 0);
    push_spawn(t2w(18),     spawn_floor, 1, 1, 1);
    push_spawn(t2w(26),     spawn_floor, 1, 1, 2);
    push_spawn(t2w(34),     spawn_floor, 1, 1, 3);
    push_spawn(t2w(42),     spawn_floor, 1, 1, 4);
    push_spawn(t2w(56),     spawn_floor, 1, 1, 5);
    push_spawn(t2w(4),      peak_spawn,  1, 1, 6);
    push_spawn(t2w(20),     t2w(H - 11) - 40, 1, 1, 7);    /* west hill summit */

    push_spawn(t2w(W - 12), spawn_floor, 2, 1, 8);
    push_spawn(t2w(W - 18), spawn_floor, 2, 1, 9);
    push_spawn(t2w(W - 26), spawn_floor, 2, 1, 10);
    push_spawn(t2w(W - 34), spawn_floor, 2, 1, 11);
    push_spawn(t2w(W - 42), spawn_floor, 2, 1, 12);
    push_spawn(t2w(W - 56), spawn_floor, 2, 1, 13);
    push_spawn(t2w(W - 4),  peak_spawn,  2, 1, 14);
    push_spawn(t2w(W - 20), t2w(H - 11) - 40, 2, 1, 15);   /* east hill summit */

    /* ---- Pickups (≈26 per brief). ---- */
    const int floor_pick = floor_y - 16;
    const int hill_pick  = hill_peak_y - 16;
    const int peak_pick  = peak_top - 16;
    const int strut_pick_a = t2w(H - 50) - 16;
    const int strut_pick_b = t2w(H - 58) - 16;

    /* Central pit floor: HEALTH small ×3 + JET_FUEL ×2 (encourage jumping in). */
    push_pickup(t2w(74), floor_pick + 64, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(80), floor_pick + 64, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(86), floor_pick + 64, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(78), floor_pick + 64, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(82), floor_pick + 64, PICKUP_JET_FUEL, 0);
    /* Hill summits (exposed): WEAPON Auto-Cannon + ARMOR heavy (west). */
    push_pickup(t2w(32), hill_pick, PICKUP_WEAPON, WEAPON_AUTO_CANNON);
    push_pickup(t2w(28), hill_pick, PICKUP_ARMOR, ARMOR_HEAVY);
    /* East hill: WEAPON Plasma Cannon + ARMOR heavy. */
    push_pickup(t2w(W - 32), hill_pick, PICKUP_WEAPON, WEAPON_PLASMA_CANNON);
    push_pickup(t2w(W - 28), hill_pick, PICKUP_ARMOR, ARMOR_HEAVY);
    /* Hill-side alcoves: AMMO ×4. */
    push_pickup(t2w(11),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(13),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 13), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 11), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    /* Sky struts (grapple-only): WEAPON Mass Driver, HEALTH small ×2. */
    push_pickup(t2w(80),  strut_pick_b, PICKUP_WEAPON, WEAPON_MASS_DRIVER);
    push_pickup(t2w(61),  strut_pick_a, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(117), strut_pick_a, PICKUP_HEALTH, HEALTH_SMALL);
    /* Mountain peaks (exposed summit): WEAPON Rail Cannon ×2. */
    push_pickup(t2w(6),     peak_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
    push_pickup(t2w(W - 6), peak_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
    /* Mountain-peak alcoves: HEALTH large ×2 + POWERUP berserk ×1. */
    push_pickup(t2w(8),     t2w(H - 37) - 16, PICKUP_HEALTH, HEALTH_LARGE);
    push_pickup(t2w(W - 8), t2w(H - 37) - 16, PICKUP_HEALTH, HEALTH_LARGE);
    push_pickup(t2w(8),     t2w(H - 37) - 16 - 16,
                PICKUP_POWERUP, POWERUP_BERSERK);
    /* Extra mid-floor health to bring count up. */
    push_pickup(t2w(48), floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W - 48), floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(60), floor_pick, PICKUP_AMMO_SECONDARY, 0);
    push_pickup(t2w(W - 60), floor_pick, PICKUP_AMMO_SECONDARY, 0);

    set_meta("Aurora",
             "Open arena. Hills + central pit, skyline at distance.",
             "aurora",
             (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM)));

    (void)strut_pick_a; (void)strut_pick_b;
}

/* ===================================================================== */
/* ============================ CROSSFIRE ============================== */
/* ===================================================================== */
/* 180×85 — the first CTF map. Mirror-symmetric: RED base on the left,
 * BLUE base on the right, wide central battleground between them. 30°
 * entry ramps to each base; 45° angled struts over mid; flank-tunnel
 * cave segments; central-high jetpack alcoves with POWERUP invisibility. */

static void build_crossfire(void) {
    cook_reset(180, 85);
    Level *L = &g_cooker.world.level;
    const int W = L->width, H = L->height;
    const uint16_t SOLID = TILE_F_SOLID;

    /* Floor + outer walls. */
    tile_fill(0, H - 4, W, H, SOLID);
    tile_fill(0,     0, 2,     H - 4, SOLID);
    tile_fill(W - 2, 0, W,     H - 4, SOLID);

    /* Top ceiling band (thinner — open sky on the sides). */
    tile_fill(0, 0, W, 2, SOLID);

    /* Base structures. RED on left x=2..30, BLUE on right x=W-30..W-2.
     * Each base has:
     *   - a flag platform at row H-14 (16 tiles up from floor)
     *   - a resupply alcove cut into the side wall
     *   - a 30° entry ramp from the central area up to the flag platform */
    const int base_w = 28;
    const int flag_plat_top_row = H - 14;
    /* RED flag platform — extends from x=2 to x=base_w. */
    tile_fill(2, flag_plat_top_row, base_w, flag_plat_top_row + 1, SOLID);
    /* RED back wall (cuts the base off from the world edge above). */
    tile_fill(2, 4, base_w, flag_plat_top_row, TILE_F_EMPTY);
    /* RED resupply alcove — cavity at floor level, ceiling at row H-7. */
    tile_fill(2, H - 7, 12, H - 4, TILE_F_EMPTY);
    tile_fill(2, H - 8, 12, H - 7, SOLID);
    /* RED base ceiling cap (so jetting up under the flag platform from
     * floor doesn't escape into the sky). */
    /* (the flag platform tile itself caps the top of the interior) */

    /* BLUE — mirror. */
    tile_fill(W - base_w, flag_plat_top_row, W - 2, flag_plat_top_row + 1, SOLID);
    tile_fill(W - base_w, 4, W - 2, flag_plat_top_row, TILE_F_EMPTY);
    tile_fill(W - 12, H - 7, W - 2, H - 4, TILE_F_EMPTY);
    tile_fill(W - 12, H - 8, W - 2, H - 7, SOLID);

    /* Central catwalks above the battleground. Two short platforms at
     * row H-22 and a higher pair at row H-32 (sky bridge alternatives). */
    tile_fill(60, H - 22, 80, H - 21, SOLID);
    tile_fill(W - 80, H - 22, W - 60, H - 21, SOLID);
    tile_fill(75, H - 32, 105, H - 31, SOLID);          /* sky bridge */

    /* Flank tunnels — covered passages below the battleground. The
     * ceiling at row H-8 cuts off the bottom 4 rows so the tunnel reads
     * as enclosed; openings at the base sides + a central exit. */
    tile_fill(40, H - 8, 60,  H - 7, SOLID);
    tile_fill(80, H - 8, 100, H - 7, SOLID);
    tile_fill(120, H - 8, 140, H - 7, SOLID);

    /* Flank-tunnel mid-alcove rooms (one per side, cut into the ceiling
     * line above the tunnel). */
    tile_fill(48, H - 11, 54, H - 8, TILE_F_EMPTY);
    tile_fill(48, H - 12, 54, H - 11, SOLID);
    tile_fill(W - 54, H - 11, W - 48, H - 8, TILE_F_EMPTY);
    tile_fill(W - 54, H - 12, W - 48, H - 11, SOLID);

    /* Central-high jetpack alcoves — above the central catwalks at row
     * H-32, mouth facing center, only reachable by jet from below. */
    tile_fill(75, H - 36, 80, H - 32, TILE_F_EMPTY);
    tile_fill(75, H - 37, 80, H - 36, SOLID);
    tile_fill(W - 80, H - 36, W - 75, H - 32, TILE_F_EMPTY);
    tile_fill(W - 80, H - 37, W - 75, H - 36, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y   = t2w(H - 4);
    const int flag_y    = t2w(flag_plat_top_row);

    /* 30° entry ramps from central area up to each base's flag platform.
     * tan(30°) ≈ 0.58 → 7 tile rise / 12 tile run. */
    push_tri(POLY_KIND_SOLID,
             t2w(base_w),       flag_y,
             t2w(base_w + 14),  floor_y,
             t2w(base_w),       floor_y);
    push_tri(POLY_KIND_SOLID,
             t2w(W - base_w - 14), floor_y,
             t2w(W - base_w),      flag_y,
             t2w(W - base_w),      floor_y);

    /* 45° angled struts above central mid (3). */
    push_tri(POLY_KIND_SOLID,
             t2w(W / 2 - 12), t2w(2),
             t2w(W / 2 -  6), t2w(2),
             t2w(W / 2 -  6), t2w(8));
    push_tri(POLY_KIND_SOLID,
             t2w(W / 2 +  6), t2w(2),
             t2w(W / 2 + 12), t2w(2),
             t2w(W / 2 +  6), t2w(8));
    push_tri(POLY_KIND_SOLID,
             t2w(W / 2 -  6), t2w(2),
             t2w(W / 2 +  6), t2w(2),
             t2w(W / 2),      t2w(8));

    /* ---- Flag records (2). Place each flag at the center of its team's
     * flag platform, slightly above the surface (16 px). */
    push_flag(t2w(15),     flag_y - 16, MATCH_TEAM_RED);
    push_flag(t2w(W - 15), flag_y - 16, MATCH_TEAM_BLUE);

    /* ---- Spawns — 8 per team, lane_hint variety. ---- */
    const int red_base_spawn  = floor_y - 40;
    const int red_flag_spawn  = flag_y - 40;
    const int blue_base_spawn = floor_y - 40;
    const int blue_flag_spawn = flag_y - 40;
    for (int i = 0; i < 4; ++i) {
        push_spawn(t2w(4 + i * 5), red_base_spawn, 1, 1, (uint8_t)i);
    }
    push_spawn(t2w(6),  red_flag_spawn, 1, 1, 4);
    push_spawn(t2w(12), red_flag_spawn, 1, 1, 5);
    push_spawn(t2w(18), red_flag_spawn, 1, 1, 6);
    push_spawn(t2w(24), red_flag_spawn, 1, 1, 7);

    for (int i = 0; i < 4; ++i) {
        push_spawn(t2w(W - 4 - i * 5), blue_base_spawn, 2, 1, (uint8_t)(8 + i));
    }
    push_spawn(t2w(W -  6), blue_flag_spawn, 2, 1, 12);
    push_spawn(t2w(W - 12), blue_flag_spawn, 2, 1, 13);
    push_spawn(t2w(W - 18), blue_flag_spawn, 2, 1, 14);
    push_spawn(t2w(W - 24), blue_flag_spawn, 2, 1, 15);

    /* ---- Pickups (≈30 per brief). ---- */
    const int floor_pick = floor_y - 16;
    const int flag_pick  = flag_y - 16;
    const int cw_pick    = t2w(H - 22) - 16;
    const int sky_pick   = t2w(H - 32) - 16;

    /* Red base resupply (in alcove): HEALTH medium ×4, AMMO ×4, ARMOR
     * light, WEAPON Pulse Rifle. */
    push_pickup(t2w(3),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(5),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(7),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(9),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(4),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(6),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(8),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(10), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(11), floor_pick, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(12), flag_pick,  PICKUP_WEAPON, WEAPON_PULSE_RIFLE);
    /* Blue base mirror. */
    push_pickup(t2w(W -  3),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  5),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  7),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  9),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  4),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W -  6),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W -  8),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 10),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 11),  floor_pick, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(W - 12),  flag_pick,  PICKUP_WEAPON, WEAPON_PULSE_RIFLE);
    /* Flank-tunnel alcoves: HEALTH small ×2, AMMO_SECONDARY ×2 per side. */
    push_pickup(t2w(51),     t2w(H - 9) - 16, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(W - 51), t2w(H - 9) - 16, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(50),     t2w(H - 9) - 16, PICKUP_AMMO_SECONDARY, 0);
    push_pickup(t2w(W - 50), t2w(H - 9) - 16, PICKUP_AMMO_SECONDARY, 0);
    /* Central battleground low: HEALTH large + JET_FUEL ×2 (exposed). */
    push_pickup(t2w(W / 2),     floor_pick, PICKUP_HEALTH, HEALTH_LARGE);
    push_pickup(t2w(W / 2 - 8), floor_pick, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(W / 2 + 8), floor_pick, PICKUP_JET_FUEL, 0);
    /* Central catwalks: Mass Driver + Rail Cannon (heavily contested). */
    push_pickup(t2w(70),  cw_pick, PICKUP_WEAPON, WEAPON_MASS_DRIVER);
    push_pickup(t2w(W - 70), cw_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
    /* Sky bridge: ARMOR heavy. */
    push_pickup(t2w(W / 2), sky_pick, PICKUP_ARMOR, ARMOR_HEAVY);
    /* Central-high jetpack alcoves: POWERUP invisibility ×2. */
    push_pickup(t2w(77),     t2w(H - 33) - 16, PICKUP_POWERUP, POWERUP_INVISIBILITY);
    push_pickup(t2w(W - 77), t2w(H - 33) - 16, PICKUP_POWERUP, POWERUP_INVISIBILITY);

    set_meta("Crossfire",
             "Mirror CTF arena. Two bases, central battleground.",
             "foundry",
             (uint16_t)((1u << MATCH_MODE_TDM) | (1u << MATCH_MODE_CTF)));
}

/* ===================================================================== */
/* ============================== CITADEL ============================== */
/* ===================================================================== */
/* 200×100 — the largest map and the CTF-primary closer for M5. Plaza
 * bowl (30°), steep castle ramparts (60°), angled castle ceilings (45°
 * overhangs), gentle tunnel grades, 12+ grapple anchors (rendered as
 * floating struts), 4 WIND zones at plaza height, 2 ACID zones at
 * tunnel ends, 10 caves/alcoves. */

static void build_citadel(void) {
    cook_reset(200, 100);
    Level *L = &g_cooker.world.level;
    const int W = L->width, H = L->height;
    const uint16_t SOLID = TILE_F_SOLID;

    /* Floor + ceiling. */
    tile_fill(0, H - 4, W, H, SOLID);
    tile_fill(0, 0, W, 2, SOLID);

    /* Outer walls full height. */
    tile_fill(0,     2, 2,     H - 4, SOLID);
    tile_fill(W - 2, 2, W,     H - 4, SOLID);

    /* CASTLES — left and right blocks of solid stone with carved-out
     * dungeon interiors. Each castle is ~30 tiles wide × 24 tall.
     * The interior dungeon is a 3-room cave network. */
    const int castle_w = 30;
    /* RED castle solid envelope. */
    tile_fill(2,            H - 30, 2 + castle_w, H - 4, SOLID);
    /* RED castle dungeon interior — carve out 3 connected rooms. */
    tile_fill(4,  H - 28, 14, H - 4, TILE_F_EMPTY);   /* room 1: flag room (west) */
    tile_fill(16, H - 28, 24, H - 4, TILE_F_EMPTY);   /* room 2: resupply (center) */
    tile_fill(26, H - 14, 30, H - 4, TILE_F_EMPTY);   /* room 3: defender perch (east, smaller) */
    /* Inter-room walls (1-tile partitions with 3-tile passages). */
    tile_fill(14, H - 10, 16, H - 4, SOLID);          /* partition between rooms 1-2, opening above row H-10 */
    tile_fill(24, H - 14, 26, H - 4, SOLID);          /* partition rooms 2-3 */
    /* Castle entrance — opening in east wall at floor level. */
    /* (the east wall is already solid; cut a passage) */
    tile_fill(30, H - 8, 32, H - 4, TILE_F_EMPTY);    /* east-facing exit */
    tile_fill(30, H - 9, 32, H - 8, SOLID);

    /* BLUE castle — mirror. */
    tile_fill(W - 2 - castle_w, H - 30, W - 2, H - 4, SOLID);
    tile_fill(W - 14, H - 28, W - 4,  H - 4, TILE_F_EMPTY);
    tile_fill(W - 24, H - 28, W - 16, H - 4, TILE_F_EMPTY);
    tile_fill(W - 30, H - 14, W - 26, H - 4, TILE_F_EMPTY);
    tile_fill(W - 16, H - 10, W - 14, H - 4, SOLID);
    tile_fill(W - 26, H - 14, W - 24, H - 4, SOLID);
    tile_fill(W - 32, H - 8, W - 30, H - 4, TILE_F_EMPTY);
    tile_fill(W - 32, H - 9, W - 30, H - 8, SOLID);

    /* Outer wall catwalks circling each castle (defender vantage). */
    tile_fill(2,            H - 30, 2 + castle_w, H - 29, SOLID);  /* castle roof */
    tile_fill(W - 2 - castle_w, H - 30, W - 2, H - 29, SOLID);

    /* Underground tunnels — low-route between bases. Tunnel ceiling at
     * row H-10 spans the central plaza, with openings at the castle
     * exits. */
    tile_fill(2 + castle_w, H - 10, W - 2 - castle_w, H - 9, SOLID);

    /* Tunnel choke alcoves (one per side, mid-tunnel). */
    tile_fill(60, H - 13, 66, H - 10, TILE_F_EMPTY);
    tile_fill(60, H - 14, 66, H - 13, SOLID);
    tile_fill(W - 66, H - 13, W - 60, H - 10, TILE_F_EMPTY);
    tile_fill(W - 66, H - 14, W - 60, H - 13, SOLID);

    /* Sky bridges — 2 high-altitude crossings above the plaza. */
    tile_fill(70,     H - 60, 130,    H - 59, SOLID);  /* lower bridge */
    tile_fill(85,     H - 75, 115,    H - 74, SOLID);  /* upper bridge */

    /* Sky-bridge overlook alcoves on the underside (mouth facing down). */
    tile_fill(75,     H - 63, 80,     H - 60, TILE_F_EMPTY);
    tile_fill(75,     H - 64, 80,     H - 63, SOLID);
    tile_fill(120,    H - 63, 125,    H - 60, TILE_F_EMPTY);
    tile_fill(120,    H - 64, 125,    H - 63, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y    = t2w(H - 4);
    const int castle_top = t2w(H - 30);

    /* 30° plaza bowl — 60 tiles wide (x=70..130), basin 8 tiles wide at
     * the bottom (x=96..104). Slopes meet the floor at the castle exits. */
    push_tri(POLY_KIND_SOLID,
             t2w(2 + castle_w + 4), floor_y,
             t2w(96), floor_y + t2w(2),
             t2w(96), floor_y);
    push_tri(POLY_KIND_SOLID,
             t2w(104), floor_y + t2w(2),
             t2w(W - 2 - castle_w - 4), floor_y,
             t2w(104), floor_y);

    /* 60° castle ramparts — steep outer slopes on each castle's
     * approach side (facing the plaza). tan(60°) ≈ 1.73. */
    push_tri(POLY_KIND_SOLID,
             t2w(2 + castle_w),     castle_top,
             t2w(2 + castle_w + 4), floor_y,
             t2w(2 + castle_w),     floor_y);
    push_tri(POLY_KIND_SOLID,
             t2w(W - 2 - castle_w - 4), floor_y,
             t2w(W - 2 - castle_w),     castle_top,
             t2w(W - 2 - castle_w),     floor_y);

    /* 45° angled castle ceilings inside each castle's flag room — slope
     * from the back wall down to the inner partition. */
    push_tri(POLY_KIND_SOLID,
             t2w(4),  t2w(H - 28),
             t2w(4),  t2w(H - 24),
             t2w(8),  t2w(H - 28));
    push_tri(POLY_KIND_SOLID,
             t2w(W - 4), t2w(H - 28),
             t2w(W - 8), t2w(H - 28),
             t2w(W - 4), t2w(H - 24));

    /* Slope-roof nooks — formed by each angled castle ceiling above. The
     * inner triangular space is the alcove. Mark it with a pickup so it
     * shows up clearly; no extra geometry needed. */

    /* 30° tunnel grades — gentle rise/fall in the tunnel floor between
     * castle exit and tunnel choke alcove. */
    push_tri(POLY_KIND_SOLID,
             t2w(32), floor_y,
             t2w(40), floor_y - t2w(1),
             t2w(40), floor_y);
    push_tri(POLY_KIND_SOLID,
             t2w(W - 40), floor_y - t2w(1),
             t2w(W - 32), floor_y,
             t2w(W - 32), floor_y);

    /* 12+ grapple-anchor struts — small floating platforms across the
     * plaza at varied heights. */
    for (int i = 0; i < 12; ++i) {
        int x   = 70 + i * 5;
        int y_r = H - 45 - ((i * 7) % 12);
        push_quad(POLY_KIND_SOLID, t2w(x), t2w(y_r),
                                   t2w(x + 3), t2w(y_r + 1));
    }

    /* ---- Ambient zones. ---- */
    /* 4 WIND zones at plaza height (varied directions, randomize long
     * shots). */
    push_ambi(t2w(70),  t2w(H - 50), t2w(15), t2w(8),
              AMBI_WIND, 0.18f, +1.0f, 0.0f);
    push_ambi(t2w(85),  t2w(H - 55), t2w(15), t2w(8),
              AMBI_WIND, 0.18f, -1.0f, 0.0f);
    push_ambi(t2w(115), t2w(H - 55), t2w(15), t2w(8),
              AMBI_WIND, 0.18f, +1.0f, 0.0f);
    push_ambi(t2w(130), t2w(H - 50), t2w(15), t2w(8),
              AMBI_WIND, 0.18f, -1.0f, 0.0f);
    /* 2 ACID zones at tunnel ends (5 HP/s — punishment for camping). */
    push_ambi(t2w(40),     t2w(H - 9), t2w(8), t2w(5),
              AMBI_ACID, 1.00f, 0.0f, 0.0f);
    push_ambi(t2w(W - 48), t2w(H - 9), t2w(8), t2w(5),
              AMBI_ACID, 1.00f, 0.0f, 0.0f);

    /* ---- CTF flags. Place each inside its castle's flag room. ---- */
    push_flag(t2w(8),     t2w(H - 6),  MATCH_TEAM_RED);
    push_flag(t2w(W - 8), t2w(H - 6),  MATCH_TEAM_BLUE);

    /* ---- Spawns — 8 per team across multiple lanes. ---- */
    const int red_castle_floor  = floor_y - 40;
    const int red_castle_roof   = castle_top - 40;
    const int blue_castle_floor = floor_y - 40;
    const int blue_castle_roof  = castle_top - 40;
    /* RED spawns inside dungeon. */
    push_spawn(t2w(6),  red_castle_floor, 1, 1, 0);
    push_spawn(t2w(8),  red_castle_floor, 1, 1, 1);
    push_spawn(t2w(10), red_castle_floor, 1, 1, 2);
    push_spawn(t2w(18), red_castle_floor, 1, 1, 3);
    push_spawn(t2w(20), red_castle_floor, 1, 1, 4);
    push_spawn(t2w(22), red_castle_floor, 1, 1, 5);
    push_spawn(t2w(28), red_castle_floor, 1, 1, 6);
    push_spawn(t2w(15), red_castle_roof,  1, 1, 7);
    /* BLUE spawns mirror. */
    push_spawn(t2w(W -  6),  blue_castle_floor, 2, 1, 8);
    push_spawn(t2w(W -  8),  blue_castle_floor, 2, 1, 9);
    push_spawn(t2w(W - 10),  blue_castle_floor, 2, 1, 10);
    push_spawn(t2w(W - 18),  blue_castle_floor, 2, 1, 11);
    push_spawn(t2w(W - 20),  blue_castle_floor, 2, 1, 12);
    push_spawn(t2w(W - 22),  blue_castle_floor, 2, 1, 13);
    push_spawn(t2w(W - 28),  blue_castle_floor, 2, 1, 14);
    push_spawn(t2w(W - 15),  blue_castle_roof,  2, 1, 15);

    /* ---- Pickups (≈32 per brief). ---- */
    const int floor_pick = floor_y - 16;
    const int castle_pick = floor_y - 16;
    const int rampart_pick = castle_top - 16;
    const int bridge_pick  = t2w(H - 60) - 16;
    const int upper_pick   = t2w(H - 75) - 16;
    const int alcove_pick  = t2w(H - 13) - 16;

    /* Red castle cave network: HEALTH medium ×3 + AMMO ×3 + ARMOR + Pulse Rifle. */
    push_pickup(t2w(6),  castle_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(8),  castle_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(10), castle_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(18), castle_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(20), castle_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(22), castle_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(20), castle_pick - 16, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(28), castle_pick, PICKUP_WEAPON, WEAPON_PULSE_RIFLE);
    /* Blue castle mirror. */
    push_pickup(t2w(W -  6), castle_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  8), castle_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W - 10), castle_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W - 18), castle_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 20), castle_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 22), castle_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 20), castle_pick - 16, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(W - 28), castle_pick, PICKUP_WEAPON, WEAPON_PULSE_RIFLE);
    /* Castle slope-roof nooks: ARMOR reactive ×2. */
    push_pickup(t2w(6),     t2w(H - 27) - 16, PICKUP_ARMOR, ARMOR_REACTIVE);
    push_pickup(t2w(W - 6), t2w(H - 27) - 16, PICKUP_ARMOR, ARMOR_REACTIVE);
    /* Plaza basin (lowest point of bowl): Mass Driver. */
    push_pickup(t2w(W / 2), floor_pick + 64, PICKUP_WEAPON, WEAPON_MASS_DRIVER);
    /* Plaza mid: Rail Cannon + HEALTH large + JET_FUEL ×2. */
    push_pickup(t2w(W / 2),     bridge_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
    push_pickup(t2w(W / 2 - 8), floor_pick,  PICKUP_HEALTH, HEALTH_LARGE);
    push_pickup(t2w(W / 2 - 12),floor_pick,  PICKUP_JET_FUEL, 0);
    push_pickup(t2w(W / 2 + 12),floor_pick,  PICKUP_JET_FUEL, 0);
    /* Sky-bridge overlook alcoves: JET_FUEL ×2. */
    push_pickup(t2w(77),  t2w(H - 62) - 16, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(122), t2w(H - 62) - 16, PICKUP_JET_FUEL, 0);
    /* Upper sky bridge: HEALTH small ×2. */
    push_pickup(t2w(95),  upper_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(105), upper_pick, PICKUP_HEALTH, HEALTH_SMALL);
    /* Tunnel choke alcoves: POWERUP godmode ×2 (high-stakes pickup). */
    push_pickup(t2w(63),     alcove_pick, PICKUP_POWERUP, POWERUP_GODMODE);
    push_pickup(t2w(W - 63), alcove_pick, PICKUP_POWERUP, POWERUP_GODMODE);
    /* Rampart catwalks: HEALTH medium ×2. */
    push_pickup(t2w(15),     rampart_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W - 15), rampart_pick, PICKUP_HEALTH, HEALTH_MEDIUM);

    set_meta("Citadel",
             "XL CTF. Castle keeps, plaza bowl, sky bridges.",
             "citadel",
             (uint16_t)(1u << MATCH_MODE_CTF));

    (void)rampart_pick; (void)bridge_pick; (void)upper_pick; (void)alcove_pick;
}

/* ===================================================================== */
/* ============================== DRIVER =============================== */
/* ===================================================================== */

static void render_thumb(const char *short_name, const Level *L);

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

    render_thumb(short_name, &g_cooker.world.level);
    return 0;
}

static void ensure_dir(const char *path) {
    /* Idempotent mkdir. */
    if (mkdir(path, 0755) != 0) {
        /* errno EEXIST is fine. */
    }
}

/* M5 P18 — generate a 256×144 thumbnail for the lobby vote picker.
 * Quality bar is low (functional, identifiable per the prompt); polish
 * pass is M6. Renders the level into the thumb as tinted blocks for
 * solid tiles + outlined polygons + colored dots for spawns/flags.
 * raylib's Image API gives us pixel + line + rect + circle — we don't
 * have a triangle-fill primitive, so polygons render as wireframe
 * outlines. */
static void render_thumb(const char *short_name, const Level *L) {
    const int TW = 256, TH = 144;
    Image img = GenImageColor(TW, TH, (Color){20, 24, 32, 255});

    float wpx = (float)(L->width  * L->tile_size);
    float hpx = (float)(L->height * L->tile_size);
    if (wpx <= 0.0f || hpx <= 0.0f) return;
    float sx = (float)TW / wpx;
    float sy = (float)TH / hpx;

    /* Tiles — solid as 90/100/110, ICE as pale blue, DEADLY as red. */
    for (int ty = 0; ty < L->height; ++ty) {
        for (int tx = 0; tx < L->width; ++tx) {
            uint16_t f = L->tiles[ty * L->width + tx].flags;
            if (!(f & TILE_F_SOLID) && !(f & TILE_F_DEADLY)) continue;
            Color c = (Color){90, 100, 110, 255};
            if (f & TILE_F_ICE)    c = (Color){180, 220, 240, 255};
            if (f & TILE_F_DEADLY) c = (Color){200, 80, 60, 255};
            int x0 = (int)(tx        * L->tile_size * sx);
            int y0 = (int)(ty        * L->tile_size * sy);
            int x1 = (int)((tx + 1)  * L->tile_size * sx);
            int y1 = (int)((ty + 1)  * L->tile_size * sy);
            if (x1 - x0 < 1) x1 = x0 + 1;
            if (y1 - y0 < 1) y1 = y0 + 1;
            ImageDrawRectangle(&img, x0, y0, x1 - x0, y1 - y0, c);
        }
    }

    /* Polygons — wireframe outlines. raylib's Image API doesn't ship a
     * triangle fill, so designer-readable thumb is outline + label-by-
     * color. Per-kind palette mirrors the M5 spec colors. */
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *p = &L->polys[i];
        Color c;
        switch (p->kind) {
            case POLY_KIND_SOLID:      c = (Color){120, 130, 140, 255}; break;
            case POLY_KIND_ICE:        c = (Color){180, 220, 240, 255}; break;
            case POLY_KIND_DEADLY:     c = (Color){200, 80, 60, 255};   break;
            case POLY_KIND_ONE_WAY:    c = (Color){120, 120, 160, 255}; break;
            case POLY_KIND_BACKGROUND: c = (Color){60, 70, 90, 180};    break;
            default:                   c = (Color){120, 130, 140, 255}; break;
        }
        int x0 = (int)(p->v_x[0] * sx), y0 = (int)(p->v_y[0] * sy);
        int x1 = (int)(p->v_x[1] * sx), y1 = (int)(p->v_y[1] * sy);
        int x2 = (int)(p->v_x[2] * sx), y2 = (int)(p->v_y[2] * sy);
        ImageDrawLine(&img, x0, y0, x1, y1, c);
        ImageDrawLine(&img, x1, y1, x2, y2, c);
        ImageDrawLine(&img, x2, y2, x0, y0, c);
    }

    /* Spawns — small team-colored dots. */
    for (int i = 0; i < L->spawn_count; ++i) {
        const LvlSpawn *s = &L->spawns[i];
        Color c = (s->team == 1) ? (Color){220, 80, 80, 255}
                : (s->team == 2) ? (Color){ 80, 140, 220, 255}
                                 : (Color){200, 200, 80, 255};
        int x = (int)(s->pos_x * sx);
        int y = (int)(s->pos_y * sy);
        ImageDrawCircle(&img, x, y, 2, c);
    }

    /* Flags — bigger, brighter team-colored squares. */
    for (int i = 0; i < L->flag_count; ++i) {
        const LvlFlag *f = &L->flags[i];
        Color c = (f->team == 1) ? (Color){240, 50, 50, 255}
                                 : (Color){ 50, 100, 240, 255};
        int x = (int)(f->pos_x * sx);
        int y = (int)(f->pos_y * sy);
        ImageDrawRectangle(&img, x - 3, y - 3, 7, 7, c);
    }

    /* Pickups — tiny yellow dots (visual marker only; kind not encoded). */
    for (int i = 0; i < L->pickup_count; ++i) {
        const LvlPickup *p = &L->pickups[i];
        int x = (int)(p->pos_x * sx);
        int y = (int)(p->pos_y * sy);
        ImageDrawPixel(&img, x, y,     (Color){200, 200, 80, 255});
        ImageDrawPixel(&img, x + 1, y, (Color){200, 200, 80, 255});
        ImageDrawPixel(&img, x, y + 1, (Color){200, 200, 80, 255});
    }

    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s_thumb.png", short_name);
    ExportImage(img, path);
    UnloadImage(img);
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
    fail |= cook_one(&scratch, &verify, "catwalk",    build_catwalk);
    fail |= cook_one(&scratch, &verify, "aurora",     build_aurora);
    fail |= cook_one(&scratch, &verify, "crossfire",  build_crossfire);
    fail |= cook_one(&scratch, &verify, "citadel",    build_citadel);

    if (fail) {
        fprintf(stderr, "cook_maps: one or more maps failed to write\n");
        return 1;
    }
    fprintf(stdout, "cook_maps: all 8 maps written\n");
    return 0;
}
