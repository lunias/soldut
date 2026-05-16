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
#include "../../src/map_thumb.h"
#include "../../src/match.h"
#include "../../src/mech.h"
#include "../../src/placement_validate.h"
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

/* ---- META populate ----
 *
 * M6 P09 — set_meta is the legacy path that leaves atmospherics zero
 * (= theme 0 / no fog / no vignette / no weather). set_meta_atmos is
 * the per-map atmosphere recook entry point. */
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
    };
    L->string_table_size = g_cooker.strt_used;
}

/* M6 P09 — overlay an atmosphere profile on top of an already-set
 * meta. Call AFTER set_meta. RGB565 helpers + Q0.16 helpers keep the
 * recook table readable. Pass theme_id alone for "use theme defaults
 * everywhere"; pass theme_id + per-field overrides to deviate. */
static inline uint16_t rgb565(int r, int g, int b) {
    if (r <   0) r =   0;
    if (r > 255) r = 255;
    if (g <   0) g =   0;
    if (g > 255) g = 255;
    if (b <   0) b =   0;
    if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static inline uint16_t q0_16(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (uint16_t)(v * 65535.0f);
}
static void set_meta_atmos(uint16_t theme_id,
                           uint16_t fog_density_q, uint16_t fog_color_rgb565,
                           uint16_t vignette_q,
                           uint16_t weather_kind, uint16_t weather_density_q)
{
    Level *L = &g_cooker.world.level;
    L->meta.theme_id          = theme_id;
    L->meta.fog_density_q     = fog_density_q;
    L->meta.fog_color_rgb565  = fog_color_rgb565;
    L->meta.vignette_q        = vignette_q;
    L->meta.weather_kind      = weather_kind;
    L->meta.weather_density_q = weather_density_q;
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

    /* ---- Spawns — 16 total, spread along the full map width so the
     * max-min-distance picker has room to separate FFA players.
     * Hill polys live at tiles 49-54, so floor spawns stay below 46
     * and above 55 to avoid embedding.
     *
     * `plat_top` is t2w(H-9) — that's the BOTTOM edge of the
     * single-row platform tile (row H-10), not the top. Spawn at
     * `plat_top - 40` lands the pelvis 8 px above the platform TOP
     * and the foot 24 px INSIDE the platform body. Use the actual
     * top edge t2w(H-10) for the spawn calc. */
    const int spawn_plat  = t2w(H - 10) - 40;
    const int spawn_floor = floor_y     - 40;
    push_spawn(t2w(10),     spawn_plat,  1, 1, 0);
    push_spawn(t2w(W - 10), spawn_plat,  2, 1, 1);
    push_spawn(t2w(15),     spawn_plat,  1, 1, 2);
    push_spawn(t2w(W - 15), spawn_plat,  2, 1, 3);
    push_spawn(t2w(20),     spawn_plat,  1, 1, 4);
    push_spawn(t2w(W - 20), spawn_plat,  2, 1, 5);
    push_spawn(t2w(8),      spawn_floor, 1, 1, 6);
    push_spawn(t2w(W - 8),  spawn_floor, 2, 1, 7);
    push_spawn(t2w(20),     spawn_floor, 1, 1, 8);
    push_spawn(t2w(W - 20), spawn_floor, 2, 1, 9);
    push_spawn(t2w(28),     spawn_floor, 1, 1, 10);
    push_spawn(t2w(W - 28), spawn_floor, 2, 1, 11);
    push_spawn(t2w(36),     spawn_floor, 1, 1, 12);
    push_spawn(t2w(W - 36), spawn_floor, 2, 1, 13);
    push_spawn(t2w(45),     spawn_floor, 0, 1, 14);   /* center-left, FFA */
    push_spawn(t2w(W - 45), spawn_floor, 0, 1, 15);   /* center-right, FFA */

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

    /* M6 P10 §3.3 — single low-density FOG volume in the right gallery
     * so the new soft-zone bleed has somewhere to read on this
     * otherwise zone-less map. strength=0.15 maps to per-zone fog
     * density; the shader path picks this up via the existing
     * atmosphere_collect_fog_zones plumbing. */
    push_ambi(t2w(65), t2w(H - 14), t2w(30), t2w(8),
              AMBI_FOG, 0.15f, 0.0f, 0.0f);

    set_meta("Foundry",
             "Open floor with cover columns. Ground-game.",
             "foundry",
             (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM)));
    /* M6 P09 — INDUSTRIAL: brass + steel, no weather, mild vignette. */
    set_meta_atmos(/*theme*/ 6,
                   /*fog_density*/ 0,        /*fog_color*/ 0,
                   /*vignette*/    q0_16(0.18f),
                   /*weather_kind*/ 0,       /*weather_density*/ 0);
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
    /* `main_top` / `catwalk_y` are the TOP edges of their respective
     * platform tiles. The spawn-block (above) already learnt that
     * `t2w(H - 17)` is the BOTTOM of the row-(H-18) tile (cf. spawn
     * fix at `spawn_main`); the pickup block below took longer to
     * follow suit. Using the wrong row buried every pickup 16 px
     * deep in the platform body. */
    const int main_top   = t2w(H - 18);
    const int catwalk_y  = t2w(H - 32);
    const int basement_ceil = t2w(H - 11);
    (void)basement_ceil;     /* M6 slipstream-trap-fix — slide chute slopes removed; only kept for the comment. */

    /* M6 slipstream-trap-fix — basement slide chute slopes REMOVED.
     * Pre-fix, each chute slope was a 60° triangle with bottom edge
     * flush with basement_ceil (y=1248). A mech walking on basement_
     * ceil top (y=1216) right next to the chute's outer edge had its
     * body parts (pelvis y=1176, shoulders y~1142 for Heavy) inside
     * the chute interior — interior y∈[hypotenuse(x), 1248] —
     * because the slope's right edge dropped from y=1056 down to
     * y=1248 right at the carve-out boundary. The constraint solver
     * pushed the body left, but the bot/player input pushed right,
     * so the mech wedged. Reproduced visually in the user-supplied
     * screenshot. The carve-out (empty tile_fill at cols 28-36 and
     * W-36..W-28) alone is enough for the "drop to basement" effect:
     * walk off main-floor edge → fall through carve-out → land on
     * basement floor. The visual slide is gone but no chassis can
     * wedge in. */

    /* M6 slipstream-trap-fix — side overhead struts REMOVED. The
     * earlier shortening (H-32 → H-35, bottom y=480) wasn't enough:
     * Heavy head on the catwalk (foot y=576, pelvis y=536, head
     * y=474) still pokes into the strut's interior at x≈700 because
     * 474 ∈ [448 (top), 480 (bottom-right)]. To clear ALL chassis
     * heads we'd need bottom_y < 470 (1 tile shrunk further), which
     * makes the strut a barely-visible 22-px-tall sliver. Cleaner to
     * just remove. The centerline strut (below) doesn't trap because
     * the only walkable surface near it is the top beam (y=384,
     * head y=282) which is well clear of the strut interior. */

    push_tri(POLY_KIND_SOLID,
             t2w(48), t2w(H - 40),
             t2w(52), t2w(H - 40),
             t2w(50), t2w(H - 37));

    /* ---- WIND zones at slide-chute landings. ---- */
    /* Left chute landing: nudge sideways toward room 1 (negative x). */
    push_ambi(t2w(30), t2w(H - 10),
              t2w(12),  t2w(6),
              AMBI_WIND, 0.20f, -1.0f, 0.0f);
    /* Right chute landing: nudge toward room 4. */
    push_ambi(t2w(W - 42), t2w(H - 10),
              t2w(12), t2w(6),
              AMBI_WIND, 0.20f, +1.0f, 0.0f);

    /* ---- Spawns ----
     *
     * `main_top` and `catwalk_y` above are misleadingly named — they
     * use t2w(H-17) / t2w(H-31), which is the BOTTOM edge of the
     * single-row platform tiles (rows H-18 / H-32). The TOP of those
     * platforms is t2w(H-18) / t2w(H-32). A spawn at `main_top - 40`
     * lands the pelvis 8 px above the platform TOP, so the foot
     * (pelvis + 36 for Trooper, +40 for Heavy) ends up INSIDE the
     * platform body — visible as "mech stuck in the platform."
     * Compute spawn relative to the actual platform TOP. */
    const int spawn_main = t2w(H - 18) - 40;
    const int spawn_cat  = t2w(H - 32) - 40;
    push_spawn(t2w(10),     spawn_main, 1, 1, 0);
    push_spawn(t2w(W - 10), spawn_main, 2, 1, 1);
    push_spawn(t2w(15),     spawn_main, 1, 1, 2);
    push_spawn(t2w(W - 15), spawn_main, 2, 1, 3);
    push_spawn(t2w(20),     spawn_main, 1, 1, 4);
    push_spawn(t2w(W - 20), spawn_main, 2, 1, 5);
    /* Catwalk spawns moved 5 tiles inward (to 20 / W-20) — original
     * tile-15 / W-15 sat too close to the catwalk's edge polys and
     * the spawn-collision push-out would shove the mech 150+ px
     * sideways during settle. */
    push_spawn(t2w(20),     spawn_cat,  1, 1, 6);
    push_spawn(t2w(W - 20), spawn_cat,  2, 1, 7);

    /* ---- Pickups (14 — per brief) ---- */
    const int basement_pick = floor_y - 16;
    const int main_pick     = main_top - 16;
    const int catwalk_pick  = catwalk_y - 16;
    /* Top beam tile row is `H - 38` (between rows H-38 and H-37 via
     * `tile_fill(..., H-38, ..., H-37, SOLID)`). `t2w(H - 37) - 16`
     * placed the pickup mid-tile in the beam body — same off-by-one
     * as `main_top` / `catwalk_y`. */
    const int beam_pick     = t2w(H - 38) - 16;
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
    /* Top beam: WEAPON Rail Cannon (high-reward, exposed). Offset
     * three tiles left of the centerline so the pickup clears the
     * decorative centerline stalactite (cols 48..52, base y=416)
     * while still reading as the "top of the map" prize. */
    push_pickup(t2w(W / 2 - 3),  beam_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);

    set_meta("Slipstream",
             "Stacked catwalks. Vertical jet beats.",
             "maintenance",
             (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM)));
    /* M6 P09 — ICE_SHEET: snow weather at 0.30, cool blue palette. */
    set_meta_atmos(/*theme*/ 2,
                   /*fog_density*/ 0,        /*fog_color*/ 0,
                   /*vignette*/    q0_16(0.20f),
                   /*weather_kind*/ 1,       /*weather_density*/ q0_16(0.30f));
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

    /* M6 P05 — central pillar reshaped to a "hanging core." Pillar tiles
     * occupy rows 28-34 (y=896-1119) instead of 28-37 — the bottom 3
     * tiles are cut so bots (and lower-skill humans) can walk straight
     * under it at floor level via a 96-px-tall corridor. The pillar
     * still reads as the contested center: snipers on top, fighters
     * underneath. The window slit at rows 30-31 stays — same LOS line
     * for snipers, just now there's a walkable arch BELOW too. */
    tile_fill(53, H - 14, 57, H - 7, SOLID);

    /* Pillar viewport — a 2-tile-tall window 1 tile from the top
     * gives ground-level snipers a sliver of LOS across the map
     * when they line up exactly with the column. Reads visually as
     * a reactor inspection port. */
    tile_fill(53, H - 12, 57, H - 10, TILE_F_EMPTY);

    /* Two flanking platforms at mid height. */
    tile_fill(16, H - 12, 38, H - 11, SOLID);
    tile_fill(72, H - 12, 94, H - 11, SOLID);

    /* High overlooks. */
    tile_fill(22, H - 22, 32, H - 21, SOLID);
    tile_fill(78, H - 22, 88, H - 21, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y         = t2w(H - 4);
    const int bowl_low        = t2w(H - 3);              /* 1 tile below floor mark */
    const int pillar_top      = t2w(H - 14);
    const int flank_top_y     = t2w(H - 12);             /* flank platform TOP (was H-11, that was its bottom) */
    const int flank_left_end  = t2w(37) + 32;            /* x at end of left flank tile (col 37 inclusive) */
    const int flank_right_end = t2w(72);                  /* x at start of right flank tile (col 72 inclusive) */
    (void)pillar_top;

    /* Bowl floor — gentle 30° from spawn side toward the pillar base. */
    push_tri(POLY_KIND_SOLID,
             t2w(4),  floor_y,
             t2w(53), bowl_low,
             t2w(53), floor_y);
    push_tri(POLY_KIND_SOLID,
             t2w(57), bowl_low,
             t2w(W - 4), floor_y,
             t2w(57), floor_y);

    /* M6 P05 — pillar-underside overhangs removed. The 4-tile-tall 45°
     * triangles at each pillar bottom corner served as a visual flourish
     * ("alcove ceiling") but they cast a 128-px horizontal "shadow"
     * outside the pillar's tile bounds that JET reach feasibility
     * couldn't clear via straight-line ray arcs. With them gone the
     * pillar reads as a clean rectangular column and bots can JET from
     * either flank platform up to the pillar top. The pillar window
     * (tiles 53-56 × rows 30-31) still gives ground-to-ground LOS
     * through the column. */

    /* M6 bot-stuck-fix — bottom edge at floor_y - 160 (was 96). 96 px
     * was below the Heavy chassis's standing head height (102 px above
     * floor), so walking bots wedged their head into the slope's
     * underside and the constraint solver pinned them in place. 160 px
     * (5 tiles) gives every chassis ~50+ px walking clearance under the
     * ramp; the slope still reads as "flank access" (jet up to the
     * bottom edge, walk up). */
    push_tri(POLY_KIND_SOLID,
             t2w(23),         floor_y - 160,
             flank_left_end,  flank_top_y,
             flank_left_end,  floor_y - 160);
    push_tri(POLY_KIND_SOLID,
             flank_right_end, flank_top_y,
             t2w(87),         floor_y - 160,
             flank_right_end, floor_y - 160);

    /* ---- Spawns — 16 total. TDM-friendly (red on left, blue on
     * right) plus FFA-friendly central / overlook positions for the
     * max-min picker to spread out the lobby. ---- */
    const int spawn_floor = floor_y - 40;
    const int spawn_flank = t2w(H - 12) - 40;
    const int spawn_over  = t2w(H - 22) - 40;
    push_spawn(t2w(6),       spawn_floor, 1, 1, 0);
    push_spawn(t2w(W - 6),   spawn_floor, 2, 1, 1);
    push_spawn(t2w(10),      spawn_floor, 1, 1, 2);
    push_spawn(t2w(W - 10),  spawn_floor, 2, 1, 3);
    push_spawn(t2w(20),      spawn_flank, 1, 1, 4);     /* flank platform */
    push_spawn(t2w(W - 20),  spawn_flank, 2, 1, 5);
    push_spawn(t2w(28),      spawn_over,  1, 1, 6);     /* overlook */
    push_spawn(t2w(W - 28),  spawn_over,  2, 1, 7);
    push_spawn(t2w(15),      spawn_floor, 1, 1, 8);
    push_spawn(t2w(W - 15),  spawn_floor, 2, 1, 9);
    push_spawn(t2w(40),      spawn_floor, 0, 1, 10);    /* mid floor, FFA */
    push_spawn(t2w(W - 40),  spawn_floor, 0, 1, 11);
    push_spawn(t2w(48),      spawn_floor, 0, 1, 12);    /* center, FFA */
    push_spawn(t2w(W - 48),  spawn_floor, 0, 1, 13);
    push_spawn(t2w(34),      spawn_flank, 0, 1, 14);    /* flank-inner */
    push_spawn(t2w(W - 34),  spawn_flank, 0, 1, 15);

    /* ---- Pickups (16 — per brief) ---- */
    const int floor_pick = floor_y - 16;
    const int flank_pick = t2w(H - 12) - 16;
    const int over_pick  = t2w(H - 22) - 16;
    /* Pillar base, both sides: WEAPON Plasma SMG twin. */
    push_pickup(t2w(50), floor_pick, PICKUP_WEAPON, WEAPON_PLASMA_SMG);
    push_pickup(t2w(60), floor_pick, PICKUP_WEAPON, WEAPON_PLASMA_SMG);
    /* Spawn alcoves — AMMO_PRIMARY. */
    push_pickup(t2w(3),     floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 3), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    /* Pillar-underside alcoves: JET_FUEL ×2. */
    push_pickup(t2w(50), pillar_top + t2w(3),  PICKUP_JET_FUEL, 0);
    push_pickup(t2w(60), pillar_top + t2w(3),  PICKUP_JET_FUEL, 0);
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
    /* M6 P09 — RUST: oranges, embers @ 0.4, warm fog. */
    set_meta_atmos(/*theme*/ 4,
                   /*fog_density*/ q0_16(0.12f),
                   /*fog_color*/   rgb565(200, 140, 90),
                   /*vignette*/    q0_16(0.30f),
                   /*weather_kind*/ 4,       /*weather_density*/ q0_16(0.40f));

    /* M6 P10 §3.3 — ACID pool under the pillar arch. 12-tile-wide ×
     * 4-tile-deep volume sitting on the bowl floor; chest-altitude
     * lands inside the rect so mechs running the corridor take 5 HP/s
     * environmental damage. Gameplay sell of the new feathered glow +
     * a real reason to hop the bowl instead of walking through it.
     * Revert by deleting this push_ambi call (per §3.3 instructions)
     * if the bake regresses on Reactor. */
    push_ambi(t2w(49), t2w(H - 7), t2w(12), t2w(4),
              AMBI_ACID, 0.6f, 0.0f, 0.0f);

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

    /* M6 iter — open atrium redesign. The original full-height
     * partition walls at x=22/77 walled the wings off so bots (and
     * lower-skill humans) never reached the central concourse. We
     * replace them with short cover stubs (4 tiles tall, on the
     * floor) that visually separate "wing entry" from "atrium body"
     * but stay walk-around-able. The wings remain as the
     * left/right thirds of the floor; the atrium body is the wide
     * middle. */
    tile_fill(22, H - 8, 23, H - 4, SOLID);   /* left wing-side cover stub */
    tile_fill(77, H - 8, 78, H - 4, SOLID);   /* right wing-side cover stub */

    /* Upper gallery catwalks — wider, span more of the wings. The
     * brief calls for catwalks above the wings; this version makes
     * the gallery span cols 4..28 on the left, 72..96 on the right,
     * leaving a wide gap in the middle (cols 28..72) so the
     * mid-atrium air space is unobstructed. */
    tile_fill(4,  H - 30, 28, H - 29, SOLID);
    tile_fill(72, H - 30, 96, H - 29, SOLID);

    /* Concourse cover columns — 4 tile-pillars between x=30..70.
     * 2 tiles wide × 6 tiles tall from the floor. The widely-spaced
     * pillars give snipers angles but leave plenty of walkways. */
    tile_fill(34, H - 10, 36, H - 4, SOLID);
    tile_fill(47, H - 10, 49, H - 4, SOLID);
    tile_fill(58, H - 10, 60, H - 4, SOLID);
    tile_fill(70, H - 10, 72, H - 4, SOLID);

    /* Wing-floor edge alcoves — supply lockers against the outer
     * wall. 2 tiles deep × 3 tiles tall. */
    tile_fill(2, H - 7, 4, H - 4, TILE_F_EMPTY);
    tile_fill(2, H - 8, 4, H - 7, SOLID);
    tile_fill(W - 4, H - 7, W - 2, H - 4, TILE_F_EMPTY);
    tile_fill(W - 4, H - 8, W - 2, H - 7, SOLID);

    /* Upper-gallery jetpack alcoves — cut into the outer wall above
     * the gallery floor. Reachable only by jet from the gallery
     * surface. Mouth faces inward. */
    tile_fill(2, H - 33, 4, H - 30, TILE_F_EMPTY);       /* left alcove cavity */
    tile_fill(2, H - 34, 4, H - 33, SOLID);              /* alcove ceiling */
    tile_fill(W - 4, H - 33, W - 2, H - 30, TILE_F_EMPTY);
    tile_fill(W - 4, H - 34, W - 2, H - 33, SOLID);

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
    /* Gallery prizes — placed on the gallery surface itself, not in
     * the tiny outer-wall alcove cuts. (The original Y of `t2w(H-31)
     * - 16` floated the pickups ~48 px above the gallery floor;
     * `gallery_pick` lands them on the catwalk surface like the four
     * HEALTH small spawners above.) */
    push_pickup(t2w(21), gallery_pick, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(79), gallery_pick, PICKUP_POWERUP, POWERUP_INVISIBILITY);
    /* Doorways: AMMO_SECONDARY ×2 (hot-spot fights). The right pickup
     * mirrors the left one: cover stubs sit at cols 22 and 77, so the
     * atrium-side x is col 23 (left) and col 76 (right). The previous
     * `W - 23 = 77` placed the right ammo INSIDE the right wing-side
     * cover stub. */
    push_pickup(t2w(23), floor_pick, PICKUP_AMMO_SECONDARY, 0);
    push_pickup(t2w(W - 24), floor_pick, PICKUP_AMMO_SECONDARY, 0);

    set_meta("Concourse",
             "Overgrown atrium with drizzle leaking through the roof.",
             "atrium",
             (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM)));
    /* M6 P10 §3.1 — Re-themed CONCRETE → OVERGROWN to retire the
     * CONCRETE × 2 duplicate with Crossfire. RAIN @ 0.18 covers the
     * previously-unused RAIN weather mode. Sky colors override the
     * theme defaults for a covered-but-leaky atrium feel. */
    Level *Lm = &g_cooker.world.level;
    Lm->meta.sky_top_rgb565 = rgb565(110, 140, 90);
    Lm->meta.sky_bot_rgb565 = rgb565( 70, 100, 60);
    set_meta_atmos(/*theme*/ 5 /*OVERGROWN*/,
                   /*fog_density*/ q0_16(0.10f),
                   /*fog_color*/   rgb565(140, 170, 110),
                   /*vignette*/    q0_16(0.20f),
                   /*weather_kind*/ 2 /*RAIN*/,
                   /*weather_density*/ q0_16(0.18f));
}

/* ===================================================================== */
/* ============================ CATWALK ================================ */
/* ===================================================================== */
/* 120×70 TDM. Vertical playscape — both team bases at floor level so
 * the ground route is always available; the suspended catwalks above
 * are the optional skill-expression layer. Slide slopes, angled
 * struts, jet-only alcoves with the rare prizes (ARMOR, BERSERK,
 * Rail Cannon). Snipers + Engineers prefer the catwalks; Heavies +
 * Troopers prefer the floor.
 *
 * M6 iter — the original "no ground route between bases" design left
 * bots (and lower-skill humans) marooned in their spawn alcoves. We
 * keep the iconic vertical geometry but bring both bases to the
 * ground and add walkable ramps so the height game is a CHOICE, not
 * a requirement. */

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

    /* RED + BLUE spawn alcoves — both at floor level, mirrored. */
    tile_fill(2, H - 9, 14, H - 4, TILE_F_EMPTY);          /* RED cavity */
    tile_fill(2, H - 10, 14, H - 9, SOLID);                /* RED roof */
    tile_fill(W - 14, H - 9, W - 2, H - 4, TILE_F_EMPTY);  /* BLUE cavity */
    tile_fill(W - 14, H - 10, W - 2, H - 9, SOLID);        /* BLUE roof */

    /* Catwalks — 3 stacked layers above the ground, suspended.
     *
     *   row    span
     *   ─────────────────────────────────────────────
     *   16-17  cw3 — top connector x=40..80
     *   28-29  cw2 — split: left x=20..55, right x=65..100
     *   40-41  cw1 — long central x=20..100 (the easy access layer) */
    tile_fill(40, H - 54, 80,  H - 53, SOLID);
    tile_fill(20, H - 42, 55,  H - 41, SOLID);
    tile_fill(65, H - 42, 100, H - 41, SOLID);
    tile_fill(20, H - 30, 100, H - 29, SOLID);

    /* Jetpack alcoves cut into the outer walls above mid catwalks. */
    tile_fill(2, H - 35, 8, H - 32, TILE_F_EMPTY);    /* LEFT cw2 alcove cavity */
    tile_fill(2, H - 36, 8, H - 35, SOLID);
    tile_fill(W - 8, H - 35, W - 2, H - 32, TILE_F_EMPTY);  /* RIGHT cw2 alcove */
    tile_fill(W - 8, H - 36, W - 2, H - 35, SOLID);
    tile_fill(56, H - 60, 64, H - 57, TILE_F_EMPTY);  /* top overhead alcove */
    tile_fill(56, H - 61, 64, H - 60, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y = t2w(H - 4);
    const int cw1_top = t2w(H - 30);
    const int cw2_top = t2w(H - 42);
    const int cw3_top = t2w(H - 54);

    /* M6 bot-stuck-fix — elevated bottom raised 96 → 160 (see
     * reactor's matching note). The 96-px gap caught walking Heavy/
     * Sniper heads against the slope's underside; 160 px gives all
     * chassis ground clearance to walk spawn-to-spawn under the ramps. */
    /* Floor → cw1 left ramp: cols 15..30, top at cw1, bottom 160 px above floor. */
    push_tri(POLY_KIND_SOLID,
             t2w(15), floor_y - 160, t2w(30), cw1_top, t2w(30), floor_y - 160);
    /* Floor → cw1 right ramp (mirror). */
    push_tri(POLY_KIND_SOLID,
             t2w(W - 30), cw1_top, t2w(W - 15), floor_y - 160, t2w(W - 30), floor_y - 160);
    /* cw1 → cw2 left ramp (cols 50..55) — doesn't touch floor, keep as-is. */
    push_tri(POLY_KIND_SOLID,
             t2w(50), cw1_top, t2w(55), cw2_top, t2w(55), cw1_top);
    /* cw2 → cw3 ramp (cols 60..64) — doesn't touch floor, keep as-is. */
    push_tri(POLY_KIND_SOLID,
             t2w(60), cw2_top, t2w(64), cw3_top, t2w(64), cw2_top);

    /* Slide-slopes from cw1 back to floor — elevated 160 px above floor. */
    push_tri(POLY_KIND_SOLID,
             t2w(35), cw1_top, t2w(50), floor_y - 160, t2w(50), cw1_top);
    push_tri(POLY_KIND_SOLID,
             t2w(W - 50), cw1_top, t2w(W - 35), floor_y - 160, t2w(W - 50), floor_y - 160);

    /* 45° angled overhead struts — 4 small triangles hanging from the
     * ceiling band, each acting as a jet-redirect surface. */
    {
        const int cy = t2w(2);
        push_tri(POLY_KIND_SOLID, t2w(30), cy, t2w(36), cy, t2w(30), cy + t2w(3));
        push_tri(POLY_KIND_SOLID, t2w(48), cy, t2w(54), cy, t2w(48), cy + t2w(3));
        push_tri(POLY_KIND_SOLID, t2w(66), cy, t2w(72), cy, t2w(72), cy + t2w(3));
        push_tri(POLY_KIND_SOLID, t2w(84), cy, t2w(90), cy, t2w(90), cy + t2w(3));
    }

    /* WIND ambient zones at top — push toward the center so a sniper
     * camping the Rail Cannon spot has to fight drift. */
    push_ambi(t2w(20), t2w(H - 58), t2w(40), t2w(8),
              AMBI_WIND, 0.18f, +1.0f, 0.0f);
    push_ambi(t2w(60), t2w(H - 58), t2w(40), t2w(8),
              AMBI_WIND, 0.18f, -1.0f, 0.0f);

    /* ---- Spawns — 8 RED bottom-left, 8 BLUE bottom-right. ---- */
    const int floor_spawn_y = floor_y - 40;
    push_spawn(t2w(3),  floor_spawn_y, 1, 1, 0);
    push_spawn(t2w(6),  floor_spawn_y, 1, 1, 1);
    push_spawn(t2w(9),  floor_spawn_y, 1, 1, 2);
    push_spawn(t2w(12), floor_spawn_y, 1, 1, 3);
    push_spawn(t2w(20), floor_spawn_y, 1, 1, 4);
    push_spawn(t2w(28), floor_spawn_y, 1, 1, 5);
    push_spawn(t2w(40), floor_spawn_y, 1, 1, 6);
    push_spawn(t2w(50), floor_spawn_y, 1, 1, 7);

    push_spawn(t2w(W -  3), floor_spawn_y, 2, 1,  8);
    push_spawn(t2w(W -  6), floor_spawn_y, 2, 1,  9);
    push_spawn(t2w(W -  9), floor_spawn_y, 2, 1, 10);
    push_spawn(t2w(W - 12), floor_spawn_y, 2, 1, 11);
    push_spawn(t2w(W - 20), floor_spawn_y, 2, 1, 12);
    push_spawn(t2w(W - 28), floor_spawn_y, 2, 1, 13);
    push_spawn(t2w(W - 40), floor_spawn_y, 2, 1, 14);
    push_spawn(t2w(W - 50), floor_spawn_y, 2, 1, 15);

    /* ---- Pickups (≈22 per brief). ---- */
    const int floor_pick = floor_y - 16;
    const int cw1_pick   = cw1_top - 16;
    const int cw2_pick   = cw2_top - 16;
    const int cw3_pick   = cw3_top - 16;

    /* Red base alcove — HEALTH + AMMO supply. */
    push_pickup(t2w(3),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(6),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(9),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(12), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    /* Blue base alcove. */
    push_pickup(t2w(W -  3), floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  6), floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  9), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 12), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    /* Floor mid — Mass Driver as the "ground prize" + HEALTH small. */
    push_pickup(t2w(60), floor_pick, PICKUP_WEAPON, WEAPON_MASS_DRIVER);
    push_pickup(t2w(40), floor_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(80), floor_pick, PICKUP_HEALTH, HEALTH_SMALL);
    /* cw1 (easy access) — JET_FUEL ×2 + HEALTH ×2. */
    push_pickup(t2w(30), cw1_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(45), cw1_pick, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(75), cw1_pick, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(90), cw1_pick, PICKUP_HEALTH, HEALTH_SMALL);
    /* cw2 jetpack alcoves: ARMOR left, ARMOR right (mirror balance). */
    push_pickup(t2w(5),     t2w(H - 33) - 16, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(W - 5), t2w(H - 33) - 16, PICKUP_ARMOR, ARMOR_LIGHT);
    /* cw2 open — AMMO_SECONDARY and a Plasma Cannon mid prize. */
    push_pickup(t2w(35), cw2_pick, PICKUP_AMMO_SECONDARY, 0);
    push_pickup(t2w(85), cw2_pick, PICKUP_AMMO_SECONDARY, 0);
    push_pickup(t2w(60), cw2_pick, PICKUP_WEAPON, WEAPON_PLASMA_CANNON);
    /* cw3 (top) + overhead alcove — the rare prizes. */
    push_pickup(t2w(60), cw3_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
    push_pickup(t2w(60), t2w(H - 58) - 16, PICKUP_POWERUP, POWERUP_BERSERK);

    set_meta("Catwalk",
             "Vertical TDM. Ground game + risk/reward catwalks.",
             "exterior",
             (uint16_t)(1u << MATCH_MODE_TDM));
    /* M6 P10 §3.1 — NEON catwalks with cyberpunk rain. Light RAIN @
     * 0.10 differentiates Catwalk from Aurora (which keeps NEON +
     * DUST) and exercises the second use of the RAIN weather mode. */
    set_meta_atmos(/*theme*/ 3 /*NEON*/,
                   /*fog_density*/ q0_16(0.08f),
                   /*fog_color*/   rgb565(130, 80, 200),
                   /*vignette*/    q0_16(0.40f),
                   /*weather_kind*/ 2 /*RAIN*/,
                   /*weather_density*/ q0_16(0.10f));
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

    /* Central pit — carve the top two rows of floor (cols 74..86) so
     * the cluster of HEALTH small + JET_FUEL pickups below has a
     * physical basin to live in. Pre-fix the pit was BACKGROUND-poly
     * decoration only; the pickups sat at `floor_pick + 64` which is
     * inside the still-solid floor tile. Right endpoint is 87
     * (exclusive) so the rightmost pickup at col 86 lands in the
     * carved area. */
    tile_fill(74, H - 4, 87, H - 2, TILE_F_EMPTY);

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

    /* Cracked-edge BACKGROUND silhouettes that visually sell the
     * carved central pit. Shrunk from the old 4-tile-deep slopes to
     * 2 tiles deep so the visual matches the actual tile carve
     * above. Purely decorative — physics uses the tile carve, not
     * the polys. */
    push_tri(POLY_KIND_BACKGROUND,
             t2w(68), floor_y,
             t2w(74), floor_y + t2w(2),
             t2w(74), floor_y);
    push_tri(POLY_KIND_BACKGROUND,
             t2w(86), floor_y + t2w(2),
             t2w(92), floor_y,
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
    /* `peak_top` is the top of the peak BODY (row H-35); the rise
     * sits ABOVE it (row H-38..H-35). A spawn at `peak_top - 40`
     * lands inside the rise — buried in solid. Place the spawn 40 px
     * above the actual rise apex (row H-38) instead. */
    const int peak_spawn      = t2w(H - 38) - 40;
    /* Spawning ON the bare apex of the hill polygon was a leg-trap:
     * the apex is one mathematical point, so feet 12 px to either
     * side of the spawn x land at different y values (one on the up-
     * slope, one on the down-slope), and the leg constraint solver
     * compresses one side into the slope volume. The `LEG_COLLAPSED`
     * detector in `tests/spawn_settle_test.c` caught it. Repurpose
     * the two summit spawns as central-floor spawns instead — they
     * still give the team a "front-line" position different from the
     * back-rank floor spawns. */
    /* Floor spawns avoid the hill x-ranges (west hill: tiles 20-44,
     * east hill: tiles 116-140). Original tiles 26/34/42 sat inside
     * the west hill volume; the body samples landed inside the slope
     * polygon and the mech got pushed sideways or stuck on settle. */
    push_spawn(t2w(12),     spawn_floor,    1, 1, 0);
    push_spawn(t2w(18),     spawn_floor,    1, 1, 1);
    push_spawn(t2w(50),     spawn_floor,    1, 1, 2);   /* was 26 */
    push_spawn(t2w(56),     spawn_floor,    1, 1, 3);   /* was 34 */
    push_spawn(t2w(64),     spawn_floor,    1, 1, 4);   /* was 42 */
    push_spawn(t2w(72),     spawn_floor,    1, 1, 5);   /* was 56 */
    push_spawn(t2w(4),      peak_spawn,     1, 1, 6);   /* mountain alcove */
    push_spawn(t2w(76),     spawn_floor,    1, 1, 7);   /* was hill summit — leg-collapsed on apex */

    push_spawn(t2w(W - 12), spawn_floor,    2, 1, 8);
    push_spawn(t2w(W - 18), spawn_floor,    2, 1, 9);
    push_spawn(t2w(W - 50), spawn_floor,    2, 1, 10);  /* was W-26 */
    push_spawn(t2w(W - 56), spawn_floor,    2, 1, 11);  /* was W-34 */
    push_spawn(t2w(W - 64), spawn_floor,    2, 1, 12);  /* was W-42 */
    push_spawn(t2w(W - 72), spawn_floor,    2, 1, 13);  /* was W-56 */
    push_spawn(t2w(W - 4),  peak_spawn,     2, 1, 14);  /* mountain alcove */
    push_spawn(t2w(W - 76), spawn_floor,    2, 1, 15);  /* was hill summit — leg-collapsed on apex */

    /* ---- Pickups (≈26 per brief). ---- */
    const int floor_pick = floor_y - 16;
    const int hill_pick  = hill_peak_y - 16;
    /* `peak_top` is the JOIN between peak body and peak rise (the
     * bottom edge of the rise's tile row). The "exposed summit"
     * pickup needs to sit ABOVE the rise — use the rise's top edge
     * `t2w(H - 38)` instead. Pre-fix `peak_top - 16` was buried in
     * the rise tile body, which only worked on the west side because
     * t2w(6) happened to land inside the west alcove cavity. */
    const int peak_pick  = t2w(H - 38) - 16;
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
    /* Mountain peaks (exposed summit): WEAPON Rail Cannon ×2.
     * Outer rise ledge spans cols 2..5 (west) and W-6..W-3 (east);
     * col 4 / W-4 is the middle of each ledge. (Pre-fix used col 6
     * which is the alcove cavity on the west and the rise body on
     * the east — asymmetric, and only worked by accident on the
     * west side.) */
    push_pickup(t2w(4),     peak_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
    push_pickup(t2w(W - 4), peak_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
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
    /* M6 P09 — NEON sky + dust drift at 0.20. */
    set_meta_atmos(/*theme*/ 3,
                   /*fog_density*/ q0_16(0.05f),
                   /*fog_color*/   rgb565(150, 100, 220),
                   /*vignette*/    q0_16(0.30f),
                   /*weather_kind*/ 3,       /*weather_density*/ q0_16(0.20f));

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
    /* M6 iter — shrunk 180×85 → 140×60. The original was so wide
     * (5760 px from base to base) that bots and human runners alike
     * spent all match traversing instead of fighting; mid-map fires
     * were sustained but kills were near zero. The new size keeps
     * CTF's mirror identity (RED left, BLUE right, central
     * battleground) at a more aggressive tempo. */
    cook_reset(140, 60);
    Level *L = &g_cooker.world.level;
    const int W = L->width, H = L->height;
    const uint16_t SOLID = TILE_F_SOLID;

    /* Floor + outer walls. */
    tile_fill(0, H - 4, W, H, SOLID);
    tile_fill(0,     0, 2,     H - 4, SOLID);
    tile_fill(W - 2, 0, W,     H - 4, SOLID);

    /* Top ceiling band (thinner — open sky on the sides). */
    tile_fill(0, 0, W, 2, SOLID);

    /* Base structures. RED on left x=2..base_w, BLUE mirrored. */
    const int base_w = 22;
    const int flag_plat_top_row = H - 12;
    /* Flag platforms. */
    tile_fill(2, flag_plat_top_row, base_w, flag_plat_top_row + 1, SOLID);
    tile_fill(W - base_w, flag_plat_top_row, W - 2, flag_plat_top_row + 1, SOLID);
    /* Resupply alcoves — cavity at floor level. */
    tile_fill(2, H - 7, 10, H - 4, TILE_F_EMPTY);
    tile_fill(2, H - 8, 10, H - 7, SOLID);
    tile_fill(W - 10, H - 7, W - 2, H - 4, TILE_F_EMPTY);
    tile_fill(W - 10, H - 8, W - 2, H - 7, SOLID);

    /* Central cover columns + catwalks above the battleground. */
    const int cw_row = H - 18;
    tile_fill(W / 2 - 18, cw_row, W / 2 - 10, cw_row + 1, SOLID);
    tile_fill(W / 2 + 10, cw_row, W / 2 + 18, cw_row + 1, SOLID);
    /* Sky bridge across the mid at row H-26. */
    const int sky_row = H - 26;
    tile_fill(W / 2 - 12, sky_row, W / 2 + 12, sky_row + 1, SOLID);

    /* M6 P05 — flank cover stubs shortened to 2 tiles tall (was 3).
     * 3-tile (96 px) stubs had their top at y=1696 — INSIDE the
     * mech's body Y-range (head at y=1712 for a bot with pelvis at
     * the spawn floor), so bots collided and couldn't walk past.
     * 2-tile (64 px) stubs top at y=1728 leaves the mech's head
     * 16 px of clearance. Stubs still provide visual cover for
     * shooting angles without blocking ground traversal. */
    tile_fill(W / 2 - 28, H - 6, W / 2 - 24, H - 4, SOLID);
    tile_fill(W / 2 + 24, H - 6, W / 2 + 28, H - 4, SOLID);

    /* Central-high jetpack alcoves cut into the sky-bridge underside. */
    tile_fill(W / 2 - 8, H - 30, W / 2 - 4, H - 26, TILE_F_EMPTY);
    tile_fill(W / 2 - 8, H - 31, W / 2 - 4, H - 30, SOLID);
    tile_fill(W / 2 + 4, H - 30, W / 2 + 8, H - 26, TILE_F_EMPTY);
    tile_fill(W / 2 + 4, H - 31, W / 2 + 8, H - 30, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y   = t2w(H - 4);
    const int flag_y    = t2w(flag_plat_top_row);

    /* M6 bot-stuck-fix — bottom 96 → 160. 96 px caught walking Heavy
     * heads under the slope's underside. 160 px lets all chassis walk
     * the floor underneath; ramp still readable as flag-platform
     * approach (jet up to the bottom edge). */
    push_tri(POLY_KIND_SOLID,
             t2w(base_w),       flag_y,
             t2w(base_w + 10),  floor_y - 160,
             t2w(base_w),       floor_y - 160);
    push_tri(POLY_KIND_SOLID,
             t2w(W - base_w - 10), floor_y - 160,
             t2w(W - base_w),      flag_y,
             t2w(W - base_w),      floor_y - 160);

    /* 45° angled struts above central mid (3). */
    push_tri(POLY_KIND_SOLID,
             t2w(W / 2 - 10), t2w(2),
             t2w(W / 2 -  4), t2w(2),
             t2w(W / 2 -  4), t2w(8));
    push_tri(POLY_KIND_SOLID,
             t2w(W / 2 +  4), t2w(2),
             t2w(W / 2 + 10), t2w(2),
             t2w(W / 2 +  4), t2w(8));

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
    /* Flag-platform spawns must stay within the platform x-range
     * (tiles 2..base_w=22). Original tile-24 spawn lay PAST the
     * platform's right edge — the mech spawned in the air above
     * the floor and fell ~256 px before landing. */
    push_spawn(t2w(6),  red_flag_spawn, 1, 1, 4);
    push_spawn(t2w(10), red_flag_spawn, 1, 1, 5);
    push_spawn(t2w(15), red_flag_spawn, 1, 1, 6);
    push_spawn(t2w(20), red_flag_spawn, 1, 1, 7);   /* was 24 — past platform */

    for (int i = 0; i < 4; ++i) {
        push_spawn(t2w(W - 4 - i * 5), blue_base_spawn, 2, 1, (uint8_t)(8 + i));
    }
    /* Mirror of red flag-platform spawns; same fix for the blue side
     * (W-24 was past the platform's left edge). */
    push_spawn(t2w(W -  6), blue_flag_spawn, 2, 1, 12);
    push_spawn(t2w(W - 10), blue_flag_spawn, 2, 1, 13);
    push_spawn(t2w(W - 15), blue_flag_spawn, 2, 1, 14);
    push_spawn(t2w(W - 20), blue_flag_spawn, 2, 1, 15);   /* was W-24 — past platform */

    /* ---- Pickups (≈26 per brief). ---- */
    const int floor_pick = floor_y - 16;
    const int flag_pick  = flag_y - 16;
    const int cw_pick    = t2w(cw_row) - 16;
    const int sky_pick   = t2w(sky_row) - 16;

    /* Red base resupply (in alcove): HEALTH medium ×3, AMMO ×3, ARMOR
     * light, WEAPON Pulse Rifle on the flag platform. */
    push_pickup(t2w(3),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(5),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(7),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(4),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(6),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(8),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(9),  floor_pick, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(10), flag_pick,  PICKUP_WEAPON, WEAPON_PULSE_RIFLE);
    /* Blue base mirror. */
    push_pickup(t2w(W -  3),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  5),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  7),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W -  4),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W -  6),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W -  8),  floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W -  9),  floor_pick, PICKUP_ARMOR, ARMOR_LIGHT);
    push_pickup(t2w(W - 10),  flag_pick,  PICKUP_WEAPON, WEAPON_PULSE_RIFLE);
    /* Central battleground low: HEALTH large + JET_FUEL ×2. */
    push_pickup(t2w(W / 2),     floor_pick, PICKUP_HEALTH, HEALTH_LARGE);
    push_pickup(t2w(W / 2 - 6), floor_pick, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(W / 2 + 6), floor_pick, PICKUP_JET_FUEL, 0);
    /* Central catwalks: Mass Driver + Auto-Cannon. */
    push_pickup(t2w(W / 2 - 14), cw_pick, PICKUP_WEAPON, WEAPON_MASS_DRIVER);
    push_pickup(t2w(W / 2 + 14), cw_pick, PICKUP_WEAPON, WEAPON_AUTO_CANNON);
    /* Sky bridge: ARMOR heavy + Rail Cannon. */
    push_pickup(t2w(W / 2 - 4), sky_pick, PICKUP_ARMOR, ARMOR_HEAVY);
    push_pickup(t2w(W / 2 + 4), sky_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
    /* Central-high jetpack alcoves: POWERUP invisibility ×2.
     * Place INSIDE the alcove cavity at rows H-30..H-26 — `sky_pick`
     * (= 16 px above the sky bridge top) lands in the cavity-bottom
     * row. Pre-fix `t2w(H - 33) - 16` floated the pickups four rows
     * ABOVE the alcove cap, in open air far from the labelled
     * jetpack-alcove geometry. */
    push_pickup(t2w(77),     sky_pick, PICKUP_POWERUP, POWERUP_INVISIBILITY);
    push_pickup(t2w(W - 77), sky_pick, PICKUP_POWERUP, POWERUP_INVISIBILITY);

    /* M6 P10 §3.3 — two small WIND zones flanking the central catwalk,
     * each blowing outward (away from center). Adds a "navigate the
     * crosswind" decision when crossing through. Strength 0.4 is
     * gentle enough that a Trooper can fight it but a Heavy needs to
     * jet over instead of walking through. Symmetric so the CTF
     * arena's mirror property is preserved. */
    push_ambi(t2w(W / 2 - 20), t2w(H - 22), t2w(8), t2w(8),
              AMBI_WIND, 0.4f, -1.0f, 0.0f);   /* left zone blows LEFT */
    push_ambi(t2w(W / 2 + 12), t2w(H - 22), t2w(8), t2w(8),
              AMBI_WIND, 0.4f,  1.0f, 0.0f);   /* right zone blows RIGHT */

    set_meta("Crossfire",
             "Mirror CTF arena. Two bases, central battleground.",
             "foundry",
             (uint16_t)((1u << MATCH_MODE_TDM) | (1u << MATCH_MODE_CTF)));
    /* M6 P09 — CONCRETE: keep the mirror-symmetric look clean. */
    set_meta_atmos(/*theme*/ 0,
                   /*fog_density*/ 0,        /*fog_color*/ 0,
                   /*vignette*/    q0_16(0.15f),
                   /*weather_kind*/ 0,       /*weather_density*/ 0);
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
    /* M6 iter — shrunk 200×100 → 160×80. The XL CTF identity survives
     * (still our biggest map), but the plaza is now 100 tiles wide
     * instead of 140 — short enough that bots, and humans without
     * grapple, can cross in <8 s. Castles are also smaller (20 tiles
     * wide, 18 tall) and the east passage is wider (4 tiles) so the
     * flag room is visible from the plaza approach. */
    cook_reset(160, 80);
    Level *L = &g_cooker.world.level;
    const int W = L->width, H = L->height;
    const uint16_t SOLID = TILE_F_SOLID;

    /* Floor + ceiling. */
    tile_fill(0, H - 4, W, H, SOLID);
    tile_fill(0, 0, W, 2, SOLID);

    /* Plaza bowl basin — carve the top two rows of floor at cols
     * 76..83 so the Mass Driver `floor_pick + 64` placement below
     * sits inside an actual physical recess. Pre-fix the basin was
     * SOLID-poly slope overlap with the still-solid floor tiles, so
     * the basin pickup was buried unreachably in the floor. */
    tile_fill(76, H - 4, 84, H - 2, TILE_F_EMPTY);

    /* Outer walls full height. */
    tile_fill(0,     2, 2,     H - 4, SOLID);
    tile_fill(W - 2, 2, W,     H - 4, SOLID);

    /* CASTLES — 20 tiles wide × 18 tall stone blocks with carved-out
     * 2-room dungeon interiors. Smaller than the original 200×100
     * version so a flag carrier exiting via the east passage is in
     * plaza sightlines within 4 tiles. */
    const int castle_w = 20;
    /* RED castle solid envelope. */
    tile_fill(2,            H - 22, 2 + castle_w, H - 4, SOLID);
    /* RED castle interior — flag room (west) + resupply (east). */
    tile_fill(4,  H - 20, 12, H - 4, TILE_F_EMPTY);   /* flag room */
    tile_fill(14, H - 20, 22, H - 4, TILE_F_EMPTY);   /* resupply room */
    /* M6 P05 — partition's floor-level passage (cook had this as solid
     * which trapped bots in the flag room; comment said "passage"). */
    tile_fill(12, H - 8, 14, H - 4, TILE_F_EMPTY);   /* floor-level passage */
    /* Castle entrance — 4-tile-wide opening in east wall at floor. */
    tile_fill(22, H - 8, 26, H - 4, TILE_F_EMPTY);
    tile_fill(22, H - 9, 26, H - 8, SOLID);

    /* BLUE castle mirror. */
    tile_fill(W - 2 - castle_w, H - 22, W - 2, H - 4, SOLID);
    tile_fill(W - 12, H - 20, W - 4,  H - 4, TILE_F_EMPTY);
    tile_fill(W - 22, H - 20, W - 14, H - 4, TILE_F_EMPTY);
    tile_fill(W - 14, H - 8, W - 12, H - 4, TILE_F_EMPTY);  /* floor-level passage */
    tile_fill(W - 26, H - 8, W - 22, H - 4, TILE_F_EMPTY);
    tile_fill(W - 26, H - 9, W - 22, H - 8, SOLID);

    /* Castle roofs (defender vantage — walkable, accessible by jet
     * from plaza or via the inner stairs we'll add as a ramp poly). */
    tile_fill(2,            H - 22, 2 + castle_w, H - 21, SOLID);
    tile_fill(W - 2 - castle_w, H - 22, W - 2, H - 21, SOLID);

    /* Sky bridge — one mid-altitude crossing above the plaza, jet/
     * grapple only from the floor. */
    tile_fill(60, H - 40, 100, H - 39, SOLID);

    /* ---- Slope polygons ---- */
    const int floor_y    = t2w(H - 4);
    const int castle_top = t2w(H - 22);

    /* Plaza bowl visual hint — two cracked-edge BACKGROUND wedges on
     * either side of the carved basin. Pre-fix these were SOLID polys
     * that overlapped the still-solid floor tiles (no walkable slope
     * surface, and the basin pickup was buried in tile); the tile
     * carve above now provides the basin geometry, so these stay
     * purely decorative. */
    push_tri(POLY_KIND_BACKGROUND,
             t2w(70), floor_y,
             t2w(76), floor_y + t2w(2),
             t2w(76), floor_y);
    push_tri(POLY_KIND_BACKGROUND,
             t2w(84), floor_y + t2w(2),
             t2w(90), floor_y,
             t2w(84), floor_y);

    /* M6 bot-stuck-fix — castle outer slopes raised 96 → 160 to clear
     * walking Heavy/Sniper head height. Plaza floor stays continuous
     * for spawn-to-spawn traversal; climbers still JET to the slope's
     * lower lip. */
    push_tri(POLY_KIND_SOLID,
             t2w(2 + castle_w),     castle_top,
             t2w(2 + castle_w + 4), floor_y - 160,
             t2w(2 + castle_w),     floor_y - 160);
    push_tri(POLY_KIND_SOLID,
             t2w(W - 2 - castle_w - 4), floor_y - 160,
             t2w(W - 2 - castle_w),     castle_top,
             t2w(W - 2 - castle_w),     floor_y - 160);

    /* 6 grapple-anchor struts spread across the plaza at varied
     * heights — fewer than the original 12, but still enough for the
     * grapple play. */
    for (int i = 0; i < 6; ++i) {
        int x   = 50 + i * 12;
        int y_r = H - 30 - ((i * 5) % 10);
        push_quad(POLY_KIND_SOLID, t2w(x), t2w(y_r),
                                   t2w(x + 3), t2w(y_r + 1));
    }

    /* ---- Ambient zones. ---- */
    /* 2 WIND zones at sky-bridge height — push toward the center so
     * a grappler swinging between bridges has to fight drift. */
    push_ambi(t2w(50),  t2w(H - 50), t2w(20), t2w(8),
              AMBI_WIND, 0.20f, +1.0f, 0.0f);
    push_ambi(t2w(90),  t2w(H - 50), t2w(20), t2w(8),
              AMBI_WIND, 0.20f, -1.0f, 0.0f);

    /* ---- CTF flags — at the back of each castle's flag room. ---- */
    push_flag(t2w(6),     t2w(H - 6),  MATCH_TEAM_RED);
    push_flag(t2w(W - 6), t2w(H - 6),  MATCH_TEAM_BLUE);

    /* ---- Spawns — 8 per team. Some spawns INSIDE the castle near
     * the flag (defensive); some OUTSIDE the east passage (attack
     * runners ready). ---- */
    const int castle_floor = floor_y - 40;
    const int castle_roof  = castle_top - 40;
    /* RED interior + exit-ready spawns. */
    push_spawn(t2w(6),  castle_floor, 1, 1, 0);
    push_spawn(t2w(8),  castle_floor, 1, 1, 1);
    push_spawn(t2w(16), castle_floor, 1, 1, 2);
    push_spawn(t2w(20), castle_floor, 1, 1, 3);
    push_spawn(t2w(28), castle_floor, 1, 1, 4);   /* just outside east passage */
    push_spawn(t2w(32), castle_floor, 1, 1, 5);
    push_spawn(t2w(36), castle_floor, 1, 1, 6);
    push_spawn(t2w(12), castle_roof,  1, 1, 7);
    /* BLUE mirror. */
    push_spawn(t2w(W -  6),  castle_floor, 2, 1,  8);
    push_spawn(t2w(W -  8),  castle_floor, 2, 1,  9);
    push_spawn(t2w(W - 16),  castle_floor, 2, 1, 10);
    push_spawn(t2w(W - 20),  castle_floor, 2, 1, 11);
    push_spawn(t2w(W - 28),  castle_floor, 2, 1, 12);
    push_spawn(t2w(W - 32),  castle_floor, 2, 1, 13);
    push_spawn(t2w(W - 36),  castle_floor, 2, 1, 14);
    push_spawn(t2w(W - 12),  castle_roof,  2, 1, 15);

    /* ---- Pickups (≈26 — denser than before for the smaller map). ---- */
    const int floor_pick   = floor_y - 16;
    const int rampart_pick = castle_top - 16;
    const int bridge_pick  = t2w(H - 40) - 16;

    /* Red castle interior — flag room + resupply HEALTH/AMMO/ARMOR. */
    push_pickup(t2w(6),  floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(10), floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(16), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(20), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(18), floor_pick - 16, PICKUP_ARMOR, ARMOR_LIGHT);
    /* Blue castle mirror. */
    push_pickup(t2w(W -  6), floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W - 10), floor_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W - 16), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 20), floor_pick, PICKUP_AMMO_PRIMARY, 0);
    push_pickup(t2w(W - 18), floor_pick - 16, PICKUP_ARMOR, ARMOR_LIGHT);
    /* Plaza floor — HEALTH small ×3 + JET_FUEL ×2 + power weapons. */
    push_pickup(t2w(40), floor_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(W / 2), floor_pick, PICKUP_HEALTH, HEALTH_LARGE);
    push_pickup(t2w(W - 40), floor_pick, PICKUP_HEALTH, HEALTH_SMALL);
    push_pickup(t2w(50), floor_pick, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(W - 50), floor_pick, PICKUP_JET_FUEL, 0);
    push_pickup(t2w(45), floor_pick, PICKUP_WEAPON, WEAPON_AUTO_CANNON);
    push_pickup(t2w(W - 45), floor_pick, PICKUP_WEAPON, WEAPON_AUTO_CANNON);
    /* Plaza bowl basin: Mass Driver (the contested center prize). */
    push_pickup(t2w(W / 2), floor_pick + 64, PICKUP_WEAPON, WEAPON_MASS_DRIVER);
    /* Sky bridge mid: Rail Cannon (jet/grapple-only). */
    push_pickup(t2w(W / 2), bridge_pick, PICKUP_WEAPON, WEAPON_RAIL_CANNON);
    push_pickup(t2w(70), bridge_pick, PICKUP_POWERUP, POWERUP_GODMODE);
    push_pickup(t2w(W - 70), bridge_pick, PICKUP_POWERUP, POWERUP_BERSERK);
    /* Rampart catwalks: HEALTH medium ×2 + AMMO_SECONDARY ×2. */
    push_pickup(t2w(12),     rampart_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(W - 12), rampart_pick, PICKUP_HEALTH, HEALTH_MEDIUM);
    push_pickup(t2w(18),     rampart_pick, PICKUP_AMMO_SECONDARY, 0);
    push_pickup(t2w(W - 18), rampart_pick, PICKUP_AMMO_SECONDARY, 0);

    set_meta("Citadel",
             "Large CTF. Castle keeps, plaza bowl, sky bridge.",
             "citadel",
             (uint16_t)(1u << MATCH_MODE_CTF));
    /* M6 P09 — BUNKER: warm browns + dust drift at 0.30, heavy vignette. */
    set_meta_atmos(/*theme*/ 1,
                   /*fog_density*/ q0_16(0.10f),
                   /*fog_color*/   rgb565(200, 180, 150),
                   /*vignette*/    q0_16(0.40f),
                   /*weather_kind*/ 3,       /*weather_density*/ q0_16(0.30f));
}

/* ===================================================================== */
/* ============================== DRIVER =============================== */
/* ===================================================================== */

static int cook_one(Arena *scratch, Arena *verify_arena,
                    const char *short_name, void (*builder)(void)) {
    builder();

    /* Placement audit — every pickup / spawn / flag must sit in
     * reachable space. A single bad placement aborts the cook so we
     * never quietly ship a map with pickups buried in tile or polygon
     * solids. See src/placement_validate.{c,h}. */
    PlacementIssue issues[256];
    int issue_count = placement_validate(&g_cooker.world.level,
                                         issues,
                                         (int)(sizeof issues / sizeof issues[0]));
    if (issue_count > 0) {
        fprintf(stderr, "cook_maps: %s placement validation FAILED — "
                        "%d issue(s):\n",
                short_name, issue_count);
        const int tile_size = g_cooker.world.level.tile_size;
        for (int i = 0; i < issue_count; ++i) {
            const PlacementIssue *iss = &issues[i];
            int tx = (tile_size > 0) ? iss->x / tile_size : -1;
            int ty = (tile_size > 0) ? iss->y / tile_size : -1;
            fprintf(stderr,
                    "  %-13s %s idx=%-2d pos=(%d, %d) tile=(%d, %d) detail=%d\n",
                    placement_issue_kind_str(iss->kind),
                    placement_entity_kind_str(iss->entity),
                    iss->index, iss->x, iss->y, tx, ty, iss->detail);
        }
        return 1;
    }

    arena_reset(scratch);

    /* Encode the thumb PNG first so it can ride along inside the .lvl
     * as a THMB lump — clients that download this map then have the
     * preview available without a separate transfer. */
    int thumb_size = 0;
    unsigned char *thumb_bytes =
        map_thumb_encode_png(&g_cooker.world.level, &thumb_size);
    g_cooker.world.level.thumb_png_data = thumb_bytes;
    g_cooker.world.level.thumb_png_size = thumb_size;

    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", short_name);
    LvlResult r = level_save(&g_cooker.world, scratch, path);
    g_cooker.world.level.thumb_png_data = NULL;
    g_cooker.world.level.thumb_png_size = 0;
    if (r != LVL_OK) {
        if (thumb_bytes) MemFree(thumb_bytes);
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

    /* Drop a sidecar PNG so designers + asset-bundle tooling can read
     * the thumb without parsing .lvl. The lobby UI prefers the
     * sidecar; the embedded THMB lump is the fallback for downloaded
     * maps where the sidecar isn't on disk. */
    {
        char thumb_path[256];
        snprintf(thumb_path, sizeof thumb_path,
                 "assets/maps/%s_thumb.png", short_name);
        if (thumb_bytes) {
            SaveFileData(thumb_path, thumb_bytes, thumb_size);
        } else if (!map_thumb_write_png(&g_cooker.world.level, thumb_path)) {
            fprintf(stderr, "cook_maps: %s thumb write failed\n", short_name);
            MemFree(thumb_bytes);
            return 1;
        }
    }
    if (thumb_bytes) MemFree(thumb_bytes);
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
