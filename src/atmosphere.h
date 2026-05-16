#pragma once

#include "math.h"
#include "world.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * atmosphere — M6 P09. Per-map sky / fog / vignette / weather + per-flag
 * tile material lookup + ambient-zone visuals.
 *
 * Data flow:
 *   1. atmosphere_init_for_map(L) reads LvlMeta + theme table → fills
 *      g_atmosphere with the resolved Colors / floats / weather kind.
 *      Called once per map_build / map_build_from_path / map_build_for_
 *      descriptor on every runtime (host, client, dedicated, single).
 *      The dedicated server is fine — no GL calls happen here.
 *   2. atmosphere_tick(world, dt) — per-tick CPU work: spawn weather
 *      particles (screen-space), spawn ambient-zone particles (world-
 *      space). Reads g_atmosphere. No-op when no GL context (so the
 *      headless server's sim path stays unchanged — atmospherics are
 *      pure-visual).
 *   3. atmosphere_draw_sky(internal_w, internal_h) — paints the vertical
 *      gradient inside the internal RT, before the world pass clears
 *      it. Called from renderer_draw_frame.
 *   4. atmosphere_draw_ambient_zones(L, time) — paints the rect-scoped
 *      ambient-zone visuals (WIND streaks, ZERO_G motes, ACID caustic /
 *      bubbles / tint). Called from draw_world_pass inside the world
 *      camera transform.
 *   5. atmosphere_collect_fog_zones(out, max, &count) — fills the
 *      fog_zones[16] shader uniform from AMBI_FOG zones. Called from
 *      the per-frame uniform-update block in renderer_draw_frame.
 *   6. atmosphere_draw_weather(window_w, window_h) — paints weather in
 *      screen space AFTER the internal-RT upscale (sharp window
 *      pixels), BEFORE the HUD. Same layering rationale as M6 P04's
 *      damage numbers.
 *
 * Multiplayer parity: atmosphere is a pure function of LvlMeta. P08
 * already replicates LvlMeta via the map-share path, so both ends
 * compute identical visuals from identical inputs. Weather and
 * ambient particle RNG is keyed off world.tick so two clients at the
 * same tick produce the same screen-space spawn positions (within
 * driver float-order). Per the spec §11.6 — particle drift is
 * intentional decoration, not gameplay state.
 *
 * See documents/m6/09-editor-runtime-parity-and-atmospherics.md.
 */

/* ---- Themes ------------------------------------------------------- */

typedef enum {
    THEME_CONCRETE   = 0,   /* default: neutral gray industrial */
    THEME_BUNKER     = 1,   /* warm browns, low light, dust */
    THEME_ICE_SHEET  = 2,   /* cool blues, glints, snow */
    THEME_NEON       = 3,   /* purples + cyans, dim ambient */
    THEME_RUST       = 4,   /* oranges + iron, embers */
    THEME_OVERGROWN  = 5,   /* greens + earth, dust motes */
    THEME_INDUSTRIAL = 6,   /* foundry-style steel + brass */
    THEME_COUNT
} AtmosphereTheme;

/* Pulled from the FxKind enum for the per-particle dispatch but exposed
 * here so cook_maps / editor / runtime can name them without #include
 * "particle.h". The values mirror the AtmosphereTheme integers used in
 * LvlMeta.weather_kind. */
typedef enum {
    WEATHER_NONE   = 0,
    WEATHER_SNOW   = 1,
    WEATHER_RAIN   = 2,
    WEATHER_DUST   = 3,
    WEATHER_EMBERS = 4,
    WEATHER_COUNT
} WeatherKind;

typedef struct ThemePalette {
    Color  backdrop;              /* not used when sky gradient on; kept as fallback */
    Color  sky_top;               /* gradient top (used when LvlMeta.sky_top_rgb565 == 0) */
    Color  sky_bot;               /* gradient bottom */
    Color  tile_solid;            /* base tint multiplied into the atlas, or the fallback fill */
    Color  tile_ice;
    Color  tile_deadly;
    Color  tile_one_way;
    Color  tile_background;
    Color  fog_color;             /* default fog tint */
    float  vignette;              /* default vignette strength [0..1] */
    int    default_weather_kind;  /* WeatherKind; 0 = none */
    float  default_weather_density;
} ThemePalette;

extern const ThemePalette g_themes[THEME_COUNT];

/* ---- Per-tile material lookup ------------------------------------- */

typedef enum {
    TILE_PAT_NONE = 0,
    TILE_PAT_ICE_GLINT,
    TILE_PAT_DEADLY_HATCH,
    TILE_PAT_ONE_WAY_CHEVRON,
    TILE_PAT_BACKGROUND_ALPHA,
} TilePattern;

typedef struct TileMaterial {
    Color   base;       /* per-flag base color (theme-influenced) */
    Color   accent;     /* highlight / chevron / hatch */
    uint8_t pattern;    /* TilePattern */
} TileMaterial;

/* Resolve the visible material for a tile flag bitmask. Priority:
 *   DEADLY > ICE > ONE_WAY > BACKGROUND > SOLID
 * The base color is theme-tinted; the pattern is flag-derived. Safe
 * to call from headless code (returns the resolved struct by value;
 * no GL calls). */
TileMaterial atmosphere_tile_material(uint16_t flags);

/* ---- Live state --------------------------------------------------- */

/* Resolved per-map atmospherics. Refreshed by atmosphere_init_for_map.
 * Read by the render + tick paths. Single global because there's only
 * one live `Level` at a time (the dedicated server's Level lives in a
 * separate Game struct, and the in-process server thread loads its own
 * .lvl independently — but both runtimes have only one g_atmosphere at
 * any moment, mirroring the single-Level invariant). */
typedef struct Atmosphere {
    uint16_t theme_id;
    Color    sky_top;
    Color    sky_bot;
    float    fog_density;
    Color    fog_color;
    float    vignette;
    float    sun_angle;          /* radians */
    uint8_t  weather_kind;       /* WeatherKind */
    float    weather_density;
    /* RNG accumulator for tick-driven spawn rates that need a 0..1
     * "fractional particle this tick" leftover. Per-kind to avoid
     * coupling cadences. */
    float    spawn_carry[WEATHER_COUNT];
    /* Ambient zone audio handles per zone, indexed by ambi list. -1
     * when no loop currently playing. Cleared on init_for_map. */
    int8_t   zone_audio_state[32];   /* 1 = currently playing, 0 = idle */
} Atmosphere;

extern Atmosphere g_atmosphere;

/* Re-resolve from the freshly-loaded Level (its LvlMeta + theme table).
 * Call from map_build / map_build_from_path / map_build_for_descriptor
 * right after level_load + level_build_poly_broadphase. */
void atmosphere_init_for_map(const Level *L);

/* Per-tick CPU work — weather particle spawn, ambient particle spawn,
 * per-zone audio gating. Reads g_atmosphere. Skips when GL context is
 * absent (so the dedicated server / cooker / headless tests don't burn
 * cycles on render-only state). Called from simulate_step after
 * fx_update. */
void atmosphere_tick(World *w, float dt);

/* ---- Render passes ------------------------------------------------ */

/* Paint a vertical sky gradient covering the internal RT. Caller is
 * inside BeginTextureMode(g_internal_target) but BEFORE BeginMode2D.
 * Costs one DrawRectangleGradientV — negligible. Skipped (cleared to
 * theme.backdrop instead) when both sky colors are equal. */
void atmosphere_draw_sky(int internal_w, int internal_h);

/* Paint each ambient zone's visual. Caller is inside BeginMode2D
 * (zones live in world space). Per-zone dispatch on AmbiKind. WIND
 * spawns are handled in atmosphere_tick; this just renders the live
 * particles. ACID + ZERO_G get a tinted overlay rect here. */
void atmosphere_draw_ambient_zones(const Level *L, double time);

/* Collect AMBI_FOG zone rects into the fog_zones[16] shader uniform
 * format `(center_x, center_y, radius_px, density)`. center/radius
 * are converted from world to screen via the supplied camera. Returns
 * the count written (≤ max). */
typedef struct { float x, y, radius, density; } AtmosFogZone;
int atmosphere_collect_fog_zones(const Level *L, Camera2D cam,
                                 AtmosFogZone *out, int max);

/* Paint weather particles. Caller is at window-resolution OUTSIDE the
 * internal RT — drawn from renderer_draw_frame after the upscale blit
 * and before the HUD. Uses blit_scale to size the particles in window
 * pixels even when the internal RT is downscaled. */
void atmosphere_draw_weather(const FxPool *fx, int window_w, int window_h);

/* Helper — drives the chevron/hatch/glint overlays for a poly. The
 * tile path lives inside draw_level_tiles; the poly path is split out
 * so render.c can call it after each triangle fill. */
void atmosphere_draw_poly_overlay(const LvlPoly *poly, double time);

/* RGB565 → Color helpers (exported because the editor's atmospherics
 * panel needs to round-trip them for the color-picker UI). */
Color atmosphere_color_from_rgb565(uint16_t v);
uint16_t atmosphere_rgb565_from_color(Color c);
