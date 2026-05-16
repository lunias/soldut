#include "atmosphere.h"

#include "audio.h"
#include "level.h"   /* level_flags_at */
#include "log.h"
#include "particle.h"

#include "../third_party/raylib/src/raylib.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

Atmosphere g_atmosphere = {0};

/* ---- Theme palette table ----------------------------------------- *
 * Hand-tuned per the spec §7.1. Each row owns ALL the per-flag tints
 * so a NEON map paints a magenta DEADLY hatch instead of the
 * CONCRETE map's amber-red. The base/tile/fog colors are also used
 * by the SLOPE poly fills (atmosphere_draw_poly_overlay), so the
 * vocabulary is consistent across tile and poly geometry. */
const ThemePalette g_themes[THEME_COUNT] = {
    /* CONCRETE — gray industrial; the safe default. */
    [THEME_CONCRETE] = {
        .backdrop        = {  12,  14,  18, 255 },
        .sky_top         = {  28,  34,  44, 255 },
        .sky_bot         = {  16,  20,  28, 255 },
        .tile_solid      = { 110, 118, 130, 255 },
        .tile_ice        = { 180, 220, 240, 255 },
        .tile_deadly     = { 200,  60,  60, 255 },
        .tile_one_way    = { 160, 160, 180, 255 },
        .tile_background = {  40,  46,  56, 255 },
        .fog_color       = { 180, 190, 210, 255 },
        .vignette        = 0.15f,
        .default_weather_kind    = WEATHER_NONE,
        .default_weather_density = 0.0f,
    },
    /* BUNKER — warm browns, mid-low light, dusty. */
    [THEME_BUNKER] = {
        .backdrop        = {  18,  14,  10, 255 },
        .sky_top         = {  44,  34,  22, 255 },
        .sky_bot         = {  22,  18,  14, 255 },
        .tile_solid      = { 130, 110,  85, 255 },
        .tile_ice        = { 200, 220, 220, 255 },
        .tile_deadly     = { 220,  80,  50, 255 },
        .tile_one_way    = { 170, 150, 120, 255 },
        .tile_background = {  44,  36,  28, 255 },
        .fog_color       = { 200, 180, 150, 255 },
        .vignette        = 0.35f,
        .default_weather_kind    = WEATHER_DUST,
        .default_weather_density = 0.3f,
    },
    /* ICE_SHEET — cool blue, glints, snow. */
    [THEME_ICE_SHEET] = {
        .backdrop        = {  18,  24,  34, 255 },
        .sky_top         = { 100, 140, 180, 255 },
        .sky_bot         = {  40,  60,  90, 255 },
        .tile_solid      = { 150, 180, 210, 255 },
        .tile_ice        = { 200, 230, 250, 255 },
        .tile_deadly     = { 200,  80,  80, 255 },
        .tile_one_way    = { 180, 200, 220, 255 },
        .tile_background = {  60,  80, 110, 255 },
        .fog_color       = { 220, 235, 250, 255 },
        .vignette        = 0.20f,
        .default_weather_kind    = WEATHER_SNOW,
        .default_weather_density = 0.4f,
    },
    /* NEON — purple/cyan cyber, dim ambient. */
    [THEME_NEON] = {
        .backdrop        = {  12,   8,  20, 255 },
        .sky_top         = {  40,  20,  64, 255 },
        .sky_bot         = {  16,   8,  24, 255 },
        .tile_solid      = {  80,  60, 130, 255 },
        .tile_ice        = { 130, 230, 240, 255 },
        .tile_deadly     = { 240,  60, 200, 255 },
        .tile_one_way    = { 120, 100, 200, 255 },
        .tile_background = {  30,  20,  50, 255 },
        .fog_color       = { 130,  80, 200, 255 },
        .vignette        = 0.40f,
        .default_weather_kind    = WEATHER_NONE,
        .default_weather_density = 0.0f,
    },
    /* RUST — oranges + iron, embers. */
    [THEME_RUST] = {
        .backdrop        = {  18,  12,   8, 255 },
        .sky_top         = {  90,  50,  30, 255 },
        .sky_bot         = {  30,  16,  10, 255 },
        .tile_solid      = { 150, 100,  60, 255 },
        .tile_ice        = { 220, 220, 230, 255 },
        .tile_deadly     = { 230,  80,  40, 255 },
        .tile_one_way    = { 180, 130,  90, 255 },
        .tile_background = {  50,  28,  18, 255 },
        .fog_color       = { 200, 140,  90, 255 },
        .vignette        = 0.35f,
        .default_weather_kind    = WEATHER_EMBERS,
        .default_weather_density = 0.35f,
    },
    /* OVERGROWN — greens + earth, dust motes. */
    [THEME_OVERGROWN] = {
        .backdrop        = {  10,  18,  14, 255 },
        .sky_top         = {  60, 100,  80, 255 },
        .sky_bot         = {  20,  30,  24, 255 },
        .tile_solid      = { 100, 130,  90, 255 },
        .tile_ice        = { 200, 230, 220, 255 },
        .tile_deadly     = { 200,  80,  60, 255 },
        .tile_one_way    = { 150, 170, 130, 255 },
        .tile_background = {  34,  46,  36, 255 },
        .fog_color       = { 180, 200, 170, 255 },
        .vignette        = 0.25f,
        .default_weather_kind    = WEATHER_DUST,
        .default_weather_density = 0.2f,
    },
    /* INDUSTRIAL — Foundry / steel mill — brass + steel, warm tint. */
    [THEME_INDUSTRIAL] = {
        .backdrop        = {  16,  18,  20, 255 },
        .sky_top         = {  60,  50,  40, 255 },
        .sky_bot         = {  22,  20,  18, 255 },
        .tile_solid      = { 130, 130, 140, 255 },
        .tile_ice        = { 190, 220, 235, 255 },
        .tile_deadly     = { 210,  90,  40, 255 },
        .tile_one_way    = { 160, 150, 130, 255 },
        .tile_background = {  44,  44,  48, 255 },
        .fog_color       = { 190, 170, 140, 255 },
        .vignette        = 0.20f,
        .default_weather_kind    = WEATHER_NONE,
        .default_weather_density = 0.0f,
    },
};

/* ---- RGB565 helpers ---------------------------------------------- */

Color atmosphere_color_from_rgb565(uint16_t v) {
    int r5 = (v >> 11) & 0x1F;
    int g6 = (v >>  5) & 0x3F;
    int b5 =  v        & 0x1F;
    int r = (r5 << 3) | (r5 >> 2);
    int g = (g6 << 2) | (g6 >> 4);
    int b = (b5 << 3) | (b5 >> 2);
    return (Color){ (unsigned char)r, (unsigned char)g, (unsigned char)b, 255 };
}

uint16_t atmosphere_rgb565_from_color(Color c) {
    uint16_t r = (uint16_t)((c.r >> 3) & 0x1F);
    uint16_t g = (uint16_t)((c.g >> 2) & 0x3F);
    uint16_t b = (uint16_t)((c.b >> 3) & 0x1F);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Q0.16 → float (0..1). Defensive clamp guarantees a malformed value
 * doesn't blow the shader. */
static float q0_16_to_float(uint16_t q) {
    float v = (float)q / 65535.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

static const ThemePalette *resolve_theme(uint16_t theme_id) {
    if (theme_id >= THEME_COUNT) theme_id = THEME_CONCRETE;
    return &g_themes[theme_id];
}

/* ---- Tile material lookup ---------------------------------------- */

TileMaterial atmosphere_tile_material(uint16_t flags) {
    const ThemePalette *t = resolve_theme(g_atmosphere.theme_id);

    /* Priority: DEADLY > ICE > ONE_WAY > BACKGROUND > SOLID. */
    if (flags & TILE_F_DEADLY) {
        return (TileMaterial){
            .base    = t->tile_deadly,
            .accent  = (Color){ 255, 220, 120, 200 },
            .pattern = TILE_PAT_DEADLY_HATCH,
        };
    }
    if (flags & TILE_F_ICE) {
        return (TileMaterial){
            .base    = t->tile_ice,
            .accent  = (Color){ 240, 250, 255, 220 },
            .pattern = TILE_PAT_ICE_GLINT,
        };
    }
    if (flags & TILE_F_ONE_WAY) {
        return (TileMaterial){
            .base    = t->tile_one_way,
            .accent  = (Color){ 230, 230, 230, 220 },
            .pattern = TILE_PAT_ONE_WAY_CHEVRON,
        };
    }
    if (flags & TILE_F_BACKGROUND) {
        Color base = t->tile_background;
        base.a = 160;
        return (TileMaterial){
            .base    = base,
            .accent  = base,
            .pattern = TILE_PAT_BACKGROUND_ALPHA,
        };
    }
    /* Default SOLID. */
    return (TileMaterial){
        .base    = t->tile_solid,
        .accent  = (Color){ 180, 200, 230, 200 },
        .pattern = TILE_PAT_NONE,
    };
}

/* ---- Init per-map ------------------------------------------------ */

void atmosphere_init_for_map(const Level *L) {
    memset(&g_atmosphere, 0, sizeof g_atmosphere);
    if (!L) return;
    const LvlMeta *m = &L->meta;

    uint16_t theme_id = m->theme_id;
    if (theme_id >= THEME_COUNT) theme_id = THEME_CONCRETE;
    const ThemePalette *t = &g_themes[theme_id];

    g_atmosphere.theme_id = theme_id;
    g_atmosphere.sky_top  = (m->sky_top_rgb565 != 0)
                          ? atmosphere_color_from_rgb565(m->sky_top_rgb565)
                          : t->sky_top;
    g_atmosphere.sky_bot  = (m->sky_bot_rgb565 != 0)
                          ? atmosphere_color_from_rgb565(m->sky_bot_rgb565)
                          : t->sky_bot;
    g_atmosphere.fog_density = q0_16_to_float(m->fog_density_q);
    g_atmosphere.fog_color   = (m->fog_color_rgb565 != 0)
                             ? atmosphere_color_from_rgb565(m->fog_color_rgb565)
                             : t->fog_color;
    g_atmosphere.vignette    = (m->vignette_q != 0)
                             ? q0_16_to_float(m->vignette_q)
                             : t->vignette;
    g_atmosphere.sun_angle   = q0_16_to_float(m->sun_angle_q) * (float)(2.0 * M_PI);

    /* Weather: if LvlMeta is explicit (kind != 0), trust it. Otherwise
     * inherit the theme's default. density=0 with kind!=0 is still
     * "weather off" (the spawn loop early-returns). */
    if (m->weather_kind != 0) {
        g_atmosphere.weather_kind    = (uint8_t)m->weather_kind;
        g_atmosphere.weather_density = q0_16_to_float(m->weather_density_q);
    } else {
        g_atmosphere.weather_kind    = (uint8_t)t->default_weather_kind;
        g_atmosphere.weather_density = t->default_weather_density;
    }

    LOG_I("atmosphere: theme=%u sky=(%u,%u,%u→%u,%u,%u) fog=%.2f vignette=%.2f weather=%u@%.2f",
          (unsigned)theme_id,
          g_atmosphere.sky_top.r, g_atmosphere.sky_top.g, g_atmosphere.sky_top.b,
          g_atmosphere.sky_bot.r, g_atmosphere.sky_bot.g, g_atmosphere.sky_bot.b,
          (double)g_atmosphere.fog_density,
          (double)g_atmosphere.vignette,
          (unsigned)g_atmosphere.weather_kind,
          (double)g_atmosphere.weather_density);
}

/* ---- Per-tick: weather + ambient spawn --------------------------- */

/* Helper: spawn `n_frac` particles with carry-over. Returns the
 * integer count to spawn this tick. Updates the carry slot. */
static int consume_spawn(float *carry, float n_frac) {
    *carry += n_frac;
    int n = (int)(*carry);
    *carry -= (float)n;
    return n;
}

/* Spawn a single weather particle of `kind`. M6 P09 (post-user-feedback)
 * — WORLD-space spawn around the local mech's viewport so falling
 * flakes visibly land on tiles + accumulate, get pushed by AMBI_WIND
 * zones, and tile-die at the contact point. The screen-space scheme
 * the original P09 shipped with had flakes pass right through tiles
 * (no world coord, no collide) — visually nothing reached the ground.
 *
 * Spawn region: ±SPAWN_HALF_W horizontally and SPAWN_TOP_PX above
 * the local mech's pelvis. With the camera following the pelvis,
 * this is "the visible viewport plus generous overshoot so flakes
 * appear naturally from above the screen." Falls under gravity
 * (gentler than mech gravity for snow), tile-collides via
 * particle.c::fx_update's new world-space branch. */
#define SNOW_SPAWN_HALF_W   1400.0f
#define SNOW_SPAWN_TOP_PX    300.0f
#define SNOW_SPAWN_BOT_PX    500.0f
static void spawn_weather_particle(FxPool *pool, World *w,
                                   uint8_t kind, pcg32_t *rng) {
    int idx = -1;
    for (int i = 0; i < pool->count; ++i) {
        if (!pool->items[i].alive) { idx = i; break; }
    }
    if (idx < 0) {
        if (pool->count >= pool->capacity) return;
        idx = pool->count++;
    }
    FxParticle *fp = &pool->items[idx];
    memset(fp, 0, sizeof *fp);
    fp->alive = 1;
    fp->kind  = kind;
    fp->pin_mech_id = -1;

    /* Spawn anchor: local mech pelvis when known, else map center. */
    float anchor_x, anchor_y;
    int lid = w->local_mech_id;
    if (lid >= 0 && lid < w->mech_count && w->mechs[lid].alive) {
        int b = w->mechs[lid].particle_base;
        anchor_x = w->particles.pos_x[b + PART_PELVIS];
        anchor_y = w->particles.pos_y[b + PART_PELVIS];
    } else {
        anchor_x = (float)w->level.width  * (float)w->level.tile_size * 0.5f;
        anchor_y = (float)w->level.height * (float)w->level.tile_size * 0.5f;
    }

    float u01 = pcg32_float01(rng);
    float v01 = pcg32_float01(rng);
    float spawn_x = anchor_x + (u01 - 0.5f) * SNOW_SPAWN_HALF_W * 2.0f;
    float spawn_y = anchor_y - SNOW_SPAWN_TOP_PX
                  - v01 * SNOW_SPAWN_BOT_PX;        /* spawn ABOVE pelvis */

    switch (kind) {
        case FX_WEATHER_SNOW: {
            fp->pos = (Vec2){ spawn_x, spawn_y };
            float wind = (pcg32_float01(rng) - 0.5f) * 30.0f;
            fp->vel = (Vec2){ wind, 60.0f + pcg32_float01(rng) * 30.0f };
            fp->life     = 12.0f;          /* generous — fall the full screen */
            fp->life_max = 12.0f;
            fp->size     = 1.5f + pcg32_float01(rng) * 1.5f;
            fp->color    = 0xF0F8FFE0u;    /* white, high alpha */
            break;
        }
        case FX_WEATHER_RAIN: {
            fp->pos = (Vec2){ spawn_x, spawn_y };
            fp->vel = (Vec2){ 80.0f + pcg32_float01(rng) * 40.0f,
                              700.0f + pcg32_float01(rng) * 200.0f };
            fp->life     = 6.0f;
            fp->life_max = 6.0f;
            fp->size     = 6.0f + pcg32_float01(rng) * 4.0f;  /* line length */
            fp->color    = 0xC8DCFFC8u;
            break;
        }
        case FX_WEATHER_DUST: {
            /* Dust drifts; spawn anywhere in the viewport region. */
            fp->pos = (Vec2){ spawn_x, anchor_y - 200.0f
                                       + (v01 - 0.5f) * 600.0f };
            float dir = pcg32_float01(rng) * 6.2832f;
            fp->vel = (Vec2){ cosf(dir) * 8.0f, sinf(dir) * 8.0f - 5.0f };
            fp->life     = 5.0f;
            fp->life_max = 5.0f;
            fp->size     = 1.5f + pcg32_float01(rng) * 1.5f;
            fp->color    = 0xC8B480AAu;
            break;
        }
        case FX_WEATHER_EMBER: {
            /* Embers rise from below the camera. */
            fp->pos = (Vec2){ spawn_x, anchor_y + SNOW_SPAWN_TOP_PX };
            fp->vel = (Vec2){ (pcg32_float01(rng) - 0.5f) * 30.0f,
                              -40.0f - pcg32_float01(rng) * 30.0f };
            fp->life     = 4.0f;
            fp->life_max = 4.0f;
            fp->size     = 2.5f + pcg32_float01(rng) * 1.5f;
            fp->color    = 0xFFAA40FFu;
            break;
        }
        default: fp->alive = 0; return;
    }
    fp->render_prev_pos = fp->pos;
}

/* Spawn one ambient-zone particle. `kind` is FX_AMBIENT_*; rect comes
 * from the LvlAmbi record so the particle stays inside the zone.
 *
 * M6 P10 soft-zone Part B — spawn region is inset by AMBI_PARTICLE_INSET
 * (12 px) by default, with a 15 % chance per call of spawning in the
 * outer boundary ring at half alpha + half life. Halving the alpha and
 * life on edge spawns makes the spawn-density step fade smoothly into
 * the surrounding world instead of cliffing at the rect edge — pairs
 * with the feathered overlay in atmosphere_draw_ambient_zones. */
#define AMBI_PARTICLE_INSET    12.0f
#define AMBI_BOUNDARY_FRACTION  0.15f
static void spawn_ambient_particle(FxPool *pool, uint8_t kind,
                                   const LvlAmbi *a, pcg32_t *rng) {
    int idx = -1;
    for (int i = 0; i < pool->count; ++i) {
        if (!pool->items[i].alive) { idx = i; break; }
    }
    if (idx < 0) {
        if (pool->count >= pool->capacity) return;
        idx = pool->count++;
    }
    FxParticle *fp = &pool->items[idx];
    memset(fp, 0, sizeof *fp);
    fp->alive = 1;
    fp->kind  = kind;
    fp->pin_mech_id = -1;

    bool boundary = (pcg32_float01(rng) < AMBI_BOUNDARY_FRACTION);
    float inset = boundary ? 0.0f : AMBI_PARTICLE_INSET;
    float minx = (float)a->rect_x + inset;
    float miny = (float)a->rect_y + inset;
    float maxx = (float)(a->rect_x + a->rect_w) - inset;
    float maxy = (float)(a->rect_y + a->rect_h) - inset;
    /* Degenerate small zones: fall back to the raw rect so we still
     * spawn somewhere reasonable. */
    if (maxx <= minx || maxy <= miny) {
        minx = (float)a->rect_x;
        miny = (float)a->rect_y;
        maxx = minx + (float)a->rect_w;
        maxy = miny + (float)a->rect_h;
    }

    switch (kind) {
        case FX_AMBIENT_WIND_STREAK: {
            float dx = (float)a->dir_x_q / 32767.0f;
            float dy = (float)a->dir_y_q / 32767.0f;
            float st = (float)a->strength_q / 32767.0f;
            float speed = st * 200.0f + 40.0f;
            /* Spawn on the trailing edge facing AWAY from the wind. */
            float fx_x, fx_y;
            if (fabsf(dx) > fabsf(dy)) {
                fx_x = (dx > 0) ? minx : maxx;
                fx_y = miny + pcg32_float01(rng) * (maxy - miny);
            } else {
                fx_x = minx + pcg32_float01(rng) * (maxx - minx);
                fx_y = (dy > 0) ? miny : maxy;
            }
            fp->pos = (Vec2){ fx_x, fx_y };
            fp->vel = (Vec2){ dx * speed, dy * speed };
            fp->life     = 0.4f + pcg32_float01(rng) * 0.4f;
            fp->life_max = fp->life;
            fp->size     = 4.0f;
            fp->color    = 0xE6F0FFBBu;  /* soft white */
            break;
        }
        case FX_AMBIENT_ZEROG_MOTE: {
            fp->pos = (Vec2){ minx + pcg32_float01(rng) * (maxx - minx),
                              miny + pcg32_float01(rng) * (maxy - miny) };
            fp->vel = (Vec2){ (pcg32_float01(rng) - 0.5f) * 4.0f, -8.0f };
            fp->life     = 1.5f;
            fp->life_max = 1.5f;
            fp->size     = 2.0f;
            fp->color    = 0xB4C8E699u;  /* cool blue-white */
            break;
        }
        case FX_AMBIENT_ACID_BUBBLE: {
            fp->pos = (Vec2){ minx + pcg32_float01(rng) * (maxx - minx), maxy };
            fp->vel = (Vec2){ (pcg32_float01(rng) - 0.5f) * 10.0f, -12.0f };
            fp->life     = 1.0f;
            fp->life_max = 1.0f;
            fp->size     = 3.0f + pcg32_float01(rng) * 1.5f;
            fp->color    = 0x78DC50CCu;  /* glowing green */
            break;
        }
        default: fp->alive = 0; return;
    }

    if (boundary) {
        /* Half-alpha + half-life: boundary spawns fade quickly so the
         * spawn-density step at the rect edge reads as soft bleed-out
         * rather than a cliff. */
        fp->color = (fp->color & 0xFFFFFF00u) | ((fp->color & 0xFFu) >> 1);
        fp->life     *= 0.5f;
        fp->life_max *= 0.5f;
    }
    fp->render_prev_pos = fp->pos;
}

/* M6 P09 — Sim-affecting weather state. Runs on EVERY runtime
 * (dedicated server, in-process server thread, client, single-process,
 * cooker) before the IsWindowReady() gate that follows, so server-side
 * physics uses the same friction weights the client expects. Pure
 * function of (weather_kind, weather_density, dt) — both peers compute
 * identical results from the replicated LvlMeta with no wire bytes. */
#define SNOW_FULL_S       60.0f   /* secs at density=1.0 to reach full accumulation */
#define SNOW_MELT_S       30.0f   /* secs to fully melt when not snowing */
#define RAIN_FULL_S       20.0f   /* secs at density=1.0 to fully wet surfaces */
#define RAIN_DRY_S        30.0f   /* secs to fully dry */

static void atmosphere_advance_accumulators(float dt) {
    /* Snow: only the SNOW kind grows the accumulator; any other
     * weather (including NONE) drains it. Grow rate proportional to
     * current weather_density so a "light flurries" map (density 0.2)
     * takes 5x as long to fully pile up as a blizzard (density 1.0).
     * Drain is unconditional (snow eventually melts even if weather
     * stops). Both clamps protect against runaway floating point. */
    bool is_snowing = (g_atmosphere.weather_kind == WEATHER_SNOW);
    float snow_dt   = dt * g_atmosphere.weather_density / SNOW_FULL_S;
    if (is_snowing) {
        g_atmosphere.snow_accum += snow_dt;
        if (g_atmosphere.snow_accum > 1.0f) g_atmosphere.snow_accum = 1.0f;
    } else {
        g_atmosphere.snow_accum -= dt / SNOW_MELT_S;
        if (g_atmosphere.snow_accum < 0.0f) g_atmosphere.snow_accum = 0.0f;
    }

    /* Rain: same shape. RAIN weather grows wetness; absence drains. */
    bool is_raining = (g_atmosphere.weather_kind == WEATHER_RAIN);
    float rain_dt   = dt * g_atmosphere.weather_density / RAIN_FULL_S;
    if (is_raining) {
        g_atmosphere.rain_wetness += rain_dt;
        if (g_atmosphere.rain_wetness > 1.0f) g_atmosphere.rain_wetness = 1.0f;
    } else {
        g_atmosphere.rain_wetness -= dt / RAIN_DRY_S;
        if (g_atmosphere.rain_wetness < 0.0f) g_atmosphere.rain_wetness = 0.0f;
    }
}

void atmosphere_tick(World *w, float dt) {
    if (!w) return;
    /* SIM-AFFECTING — runs on the headless dedicated server too. */
    atmosphere_advance_accumulators(dt);

    /* The dedicated server / cooker has no window — skip particle work
     * to avoid burning CPU on visuals nothing reads. The sim-side effect
     * of ambient zones (WIND nudge, ZERO_G mask, ACID damage) lives in
     * physics.c / mech.c per the spec §1.3 — those run unconditionally. */
    if (!IsWindowReady()) return;

    /* ---- Weather spawn -------------------------------------------- */
    if (g_atmosphere.weather_kind != WEATHER_NONE &&
        g_atmosphere.weather_density > 0.001f)
    {
        float density = g_atmosphere.weather_density;
        /* Per-kind per-tick spawn rate from spec §8.1-§8.4. */
        float per_tick = 0.0f;
        uint8_t kind = 0;
        switch (g_atmosphere.weather_kind) {
            case WEATHER_SNOW:   per_tick = density *  8.0f; kind = FX_WEATHER_SNOW;  break;
            case WEATHER_RAIN:   per_tick = density * 16.0f; kind = FX_WEATHER_RAIN;  break;
            case WEATHER_DUST:   per_tick = density *  4.0f; kind = FX_WEATHER_DUST;  break;
            case WEATHER_EMBERS: per_tick = density *  6.0f; kind = FX_WEATHER_EMBER; break;
            default: break;
        }
        if (kind != 0 && per_tick > 0.0f) {
            /* Map FxKind back into the carry index (1..4 → 1..4). */
            int carry_idx = (int)g_atmosphere.weather_kind;
            if (carry_idx >= 0 && carry_idx < WEATHER_COUNT) {
                int n = consume_spawn(&g_atmosphere.spawn_carry[carry_idx], per_tick * dt * 60.0f);
                for (int i = 0; i < n; ++i) {
                    spawn_weather_particle(&w->fx, w, kind, w->rng);
                }
            }
        }
    }

    /* ---- Ambient zone spawn --------------------------------------- */
    /* Per-zone spawn driven by the kind. The strength_q controls WIND
     * count; ZERO_G and ACID have fixed cadences (1/tick per zone). */
    static float s_zone_carry[32] = {0};
    for (int zi = 0; zi < w->level.ambi_count && zi < 32; ++zi) {
        const LvlAmbi *a = &w->level.ambis[zi];
        switch ((AmbiKind)a->kind) {
            case AMBI_WIND: {
                float st = (float)a->strength_q / 32767.0f;
                if (st < 0.05f) st = 0.05f;
                int n = consume_spawn(&s_zone_carry[zi], st * 4.0f * dt * 60.0f);
                for (int i = 0; i < n; ++i) {
                    spawn_ambient_particle(&w->fx, FX_AMBIENT_WIND_STREAK, a, w->rng);
                }
                break;
            }
            case AMBI_ZERO_G: {
                int n = consume_spawn(&s_zone_carry[zi], dt * 60.0f);
                for (int i = 0; i < n; ++i) {
                    spawn_ambient_particle(&w->fx, FX_AMBIENT_ZEROG_MOTE, a, w->rng);
                }
                break;
            }
            case AMBI_ACID: {
                int n = consume_spawn(&s_zone_carry[zi], 1.5f * dt * 60.0f);
                for (int i = 0; i < n; ++i) {
                    spawn_ambient_particle(&w->fx, FX_AMBIENT_ACID_BUBBLE, a, w->rng);
                }
                break;
            }
            case AMBI_FOG:
            default:
                /* FOG is shader-side; no particles. */
                break;
        }
    }

    /* ---- Per-zone ambient audio gating ---------------------------- *
     * If the local mech is inside any zone of a given kind, ensure the
     * corresponding env loop is playing. Otherwise leave the existing
     * track. We don't have a true positional ambient loop API in audio.c
     * — just trigger a global one-shot when the player enters a fresh
     * zone, throttled per zone so we don't spam. */
    int local = w->local_mech_id;
    if (local >= 0 && local < w->mech_count && w->mechs[local].alive) {
        const Mech *me = &w->mechs[local];
        int b = me->particle_base;
        Vec2 chest = (Vec2){
            w->particles.pos_x[b + PART_CHEST],
            w->particles.pos_y[b + PART_CHEST],
        };
        for (int zi = 0; zi < w->level.ambi_count && zi < 32; ++zi) {
            const LvlAmbi *a = &w->level.ambis[zi];
            float minx = (float)a->rect_x, miny = (float)a->rect_y;
            float maxx = minx + (float)a->rect_w;
            float maxy = miny + (float)a->rect_h;
            bool inside = (chest.x >= minx && chest.x <= maxx &&
                           chest.y >= miny && chest.y <= maxy);
            bool was    = (g_atmosphere.zone_audio_state[zi] != 0);
            if (inside && !was) {
                /* Edge-trigger an enter cue. WIND uses existing footstep-
                 * adjacent material; ACID + ZERO_G use new env loops. */
                switch ((AmbiKind)a->kind) {
                    case AMBI_ACID:
                        audio_play_at(SFX_ENV_ACID_BUBBLE, chest); break;
                    case AMBI_ZERO_G:
                        audio_play_at(SFX_ENV_ZEROG_HUM,   chest); break;
                    default: break;
                }
            }
            g_atmosphere.zone_audio_state[zi] = inside ? 1 : 0;
        }
    } else {
        for (int zi = 0; zi < 32; ++zi) g_atmosphere.zone_audio_state[zi] = 0;
    }
}

/* ---- Render: sky ------------------------------------------------- */

void atmosphere_draw_sky(int internal_w, int internal_h) {
    if (internal_w <= 0 || internal_h <= 0) return;
    Color top = g_atmosphere.sky_top;
    Color bot = g_atmosphere.sky_bot;
    /* Hint of warm sun bloom near the top — biased by sun_angle. The
     * full computation is in the shader (per-fragment); here we just
     * pick the gradient endpoints. */
    DrawRectangleGradientV(0, 0, internal_w, internal_h, top, bot);
}

/* ---- Render: ambient zones -------------------------------------- */

static Color rgba8_to_color(uint32_t v) {
    return (Color){
        (unsigned char)((v >> 24) & 0xFF),
        (unsigned char)((v >> 16) & 0xFF),
        (unsigned char)((v >>  8) & 0xFF),
        (unsigned char)(v & 0xFF),
    };
}

/* M6 P10 — soft ambient-zone boundaries. The hard rect + outline pair
 * that shipped in P09 broke immersion in-game (the user said as much
 * verbatim: "I do not like the hard boundary for the ambient zone that
 * is rendered in the actual game"). We replace each overlay with a
 * stack of 6 concentric rounded rects whose alpha ramps to 0 over a
 * 32-px feather distance — the volume now reads as a region of
 * atmosphere instead of a painted box. WIND drops its runtime overlay
 * entirely (the streak particles are the visual). FOG keeps the disc
 * preview but loses the outline (the shader already does the soft
 * falloff). The editor's hard-edge preview lives in tools/editor/
 * render.c and is untouched — designers still need exact bounds. */
#define AMBI_FEATHER_RINGS 6
#define AMBI_FEATHER_PX    32.0f

/* Draw a feathered-edge filled overlay. Paints AMBI_FEATHER_RINGS
 * concentric rounded rects from outermost-faintest to innermost-
 * brightest. Each ring grows by t*AMBI_FEATHER_PX and dims by (1-t).
 * Drawn outer→inner so the bright inner rect overpaints the larger
 * faint rings underneath, producing a smooth radial bleed past the
 * rect edge. Skips rings whose computed alpha is 0. */
static void draw_feathered_overlay(Rectangle r, Color tint,
                                   unsigned char base_alpha) {
    for (int k = AMBI_FEATHER_RINGS - 1; k >= 0; --k) {
        float t = (float)k / (float)(AMBI_FEATHER_RINGS - 1);
        float grow = t * AMBI_FEATHER_PX;
        unsigned char ring_a =
            (unsigned char)((float)base_alpha * (1.0f - t));
        if (ring_a == 0) continue;
        Rectangle rk = {
            r.x - grow, r.y - grow,
            r.width  + 2.0f * grow,
            r.height + 2.0f * grow,
        };
        DrawRectangleRounded(rk, 0.20f, 4,
            (Color){ tint.r, tint.g, tint.b, ring_a });
    }
}

void atmosphere_draw_ambient_zones(const Level *L, double time) {
    if (!L || L->ambi_count == 0) return;
    /* Runtime overlay pass — feathered, no hard outlines. Particles
     * paint on top (already running inside BeginMode2D in render.c). */
    for (int zi = 0; zi < L->ambi_count; ++zi) {
        const LvlAmbi *a = &L->ambis[zi];
        Rectangle r = (Rectangle){ (float)a->rect_x, (float)a->rect_y,
                                   (float)a->rect_w, (float)a->rect_h };
        switch ((AmbiKind)a->kind) {
            case AMBI_ZERO_G: {
                /* Soft cyan glow bleeding ~32 px past the rect edge. */
                Color tint = { 120, 160, 220, 0 };
                draw_feathered_overlay(r, tint, 32);
                break;
            }
            case AMBI_ACID: {
                /* Pulsing green glow with the same feather bleed. */
                int pulse = 22 + (int)(14.0 * sin(time * 1.5));
                if (pulse < 8)  pulse = 8;
                if (pulse > 50) pulse = 50;
                Color tint = { 120, 220, 80, 0 };
                draw_feathered_overlay(r, tint, (unsigned char)pulse);
                /* Caustic surface band — tapered so the wave doesn't
                 * sharp-cut at the rect's left/right edges. Loop runs
                 * within [INSET, rect_w - INSET]; alpha fades in/out
                 * across the first/last 24 px of that span. */
                const float CAUSTIC_INSET = 16.0f;
                const float CAUSTIC_FADE  = 24.0f;
                float t_now = (float)time;
                int start = (int)CAUSTIC_INSET;
                int end   = a->rect_w - (int)CAUSTIC_INSET;
                if (end <= start) break;
                for (int x = start; x < end; x += 8) {
                    float left  = (float)(x - start) / CAUSTIC_FADE;
                    float right = (float)(end - x)   / CAUSTIC_FADE;
                    float edge_t = (left < right) ? left : right;
                    if (edge_t < 0.0f) edge_t = 0.0f;
                    if (edge_t > 1.0f) edge_t = 1.0f;
                    unsigned char a8 =
                        (unsigned char)(180.0f * edge_t);
                    if (a8 == 0) continue;
                    float wave = sinf((float)(a->rect_x + x) * 0.05f
                                       + t_now * 2.0f);
                    int   yoff = (int)(wave * 2.0f);
                    DrawRectangle(a->rect_x + x, a->rect_y + yoff,
                                  6, 4,
                                  (Color){ 180, 255, 120, a8 });
                }
                break;
            }
            case AMBI_FOG: {
                /* Faint designer-visible disc; the real haze lives in
                 * the halftone shader's fog-zone pass. Outline killed —
                 * the shader's exp(-r²/r²) falloff is already soft. */
                float cx = (float)a->rect_x + (float)a->rect_w * 0.5f;
                float cy = (float)a->rect_y + (float)a->rect_h * 0.5f;
                float radius = (a->rect_w < a->rect_h
                                ? a->rect_w : a->rect_h) * 0.5f;
                Color fog = g_atmosphere.fog_color;
                DrawCircle((int)cx, (int)cy, radius,
                           (Color){ fog.r, fog.g, fog.b, 18 });
                break;
            }
            case AMBI_WIND:
                /* No runtime overlay — the streak particles ARE the
                 * visual. Editor still paints rect+arrow inline (see
                 * tools/editor/render.c). */
                break;
            default: break;
        }
    }
}

/* ---- Fog zone uniform collection -------------------------------- */

int atmosphere_collect_fog_zones(const Level *L, Camera2D cam,
                                 AtmosFogZone *out, int max)
{
    if (!L || !out || max <= 0) return 0;
    int n = 0;
    for (int zi = 0; zi < L->ambi_count && n < max; ++zi) {
        const LvlAmbi *a = &L->ambis[zi];
        if ((AmbiKind)a->kind != AMBI_FOG) continue;
        /* Use the LvlAmbi strength_q as the per-zone density override.
         * Defaults to global g_atmosphere.fog_density when strength_q
         * is zero so a designer can drop a FOG zone with no slider
         * tweaking and still get a visible volume. */
        float density = (a->strength_q != 0)
                      ? (float)a->strength_q / 32767.0f
                      : g_atmosphere.fog_density;
        if (density <= 0.001f) continue;
        float cx_world = (float)a->rect_x + (float)a->rect_w * 0.5f;
        float cy_world = (float)a->rect_y + (float)a->rect_h * 0.5f;
        float radius_world = (a->rect_w < a->rect_h
                              ? (float)a->rect_w
                              : (float)a->rect_h) * 0.5f;
        Vector2 screen = GetWorldToScreen2D((Vector2){cx_world, cy_world}, cam);
        float radius_screen = radius_world * cam.zoom;
        out[n].x       = screen.x;
        out[n].y       = screen.y;
        out[n].radius  = radius_screen;
        out[n].density = density;
        ++n;
    }
    return n;
}

/* ---- Weather render (called at window resolution) --------------- */

void atmosphere_draw_weather(const FxPool *fx, int window_w, int window_h) {
    if (!fx || window_w <= 0 || window_h <= 0) return;
    /* Iterate the pool and paint screen-space weather particles. The
     * particles store normalized (0..1) screen coords in pos; we
     * scale to window pixels here. */
    for (int i = 0; i < fx->count; ++i) {
        const FxParticle *fp = &fx->items[i];
        if (!fp->alive) continue;
        if (fp->kind != FX_WEATHER_SNOW &&
            fp->kind != FX_WEATHER_RAIN &&
            fp->kind != FX_WEATHER_DUST &&
            fp->kind != FX_WEATHER_EMBER) continue;
        float sx = fp->pos.x * (float)window_w;
        float sy = fp->pos.y * (float)window_h;
        if (sx < -32 || sx > window_w + 32 ||
            sy < -32 || sy > window_h + 64) continue;
        Color col = rgba8_to_color(fp->color);
        /* Linear life fade. */
        if (fp->life_max > 0.0f) {
            float a = fp->life / fp->life_max;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            col.a = (unsigned char)((float)col.a * a);
        }
        switch (fp->kind) {
            case FX_WEATHER_SNOW: {
                DrawCircle((int)sx, (int)sy, fp->size, col);
                break;
            }
            case FX_WEATHER_RAIN: {
                Vector2 a0 = { sx, sy };
                Vector2 a1 = { sx - fp->size * 0.4f,
                               sy - fp->size * 2.0f };
                DrawLineEx(a0, a1, 1.0f, col);
                break;
            }
            case FX_WEATHER_DUST: {
                DrawCircle((int)sx, (int)sy, fp->size, col);
                break;
            }
            case FX_WEATHER_EMBER: {
                BeginBlendMode(BLEND_ADDITIVE);
                DrawCircle((int)sx, (int)sy, fp->size, col);
                EndBlendMode();
                break;
            }
            default: break;
        }
    }
}

/* ---- Poly overlay ----------------------------------------------- */

/* Per-edge midpoint helper. Used by ONE_WAY chevron placement. */
static void edge_midpoint(const LvlPoly *p, int e, float *cx, float *cy) {
    int a = e, b = (e + 1) % 3;
    *cx = (float)(p->v_x[a] + p->v_x[b]) * 0.5f;
    *cy = (float)(p->v_y[a] + p->v_y[b]) * 0.5f;
}

void atmosphere_draw_snow_pile(const Level *L) {
    if (!L || !L->tiles) return;
    float depth = g_atmosphere.snow_accum;
    if (depth < 0.05f) return;          /* nothing visible yet */
    int ts = L->tile_size;
    if (ts <= 0) return;

    /* Pile thickness scales with accumulator, capped so a fully-snowed
     * map doesn't paint a full-tile snow layer. ~25 % of the tile at max
     * — a clearly visible pile that still leaves the tile body readable. */
    float thickness = (float)ts * 0.05f + (float)ts * 0.20f * depth;
    /* Wetter snow (longer in scene) gets a slight blue tint; fresh snow
     * is bright white. Both at high alpha so the pile reads cleanly. */
    Color pile = {
        (unsigned char)(245 - 10 * depth),
        (unsigned char)(248 - 8  * depth),
        255,
        (unsigned char)(200 + 40 * depth),
    };
    /* Tiny shadow strip just below the pile gives it depth (no need
     * for a full normal-mapped fluffy texture). */
    Color shadow = { 180, 195, 215, 90 };

    for (int y = 0; y < L->height; ++y) {
        for (int x = 0; x < L->width; ++x) {
            const LvlTile *t = &L->tiles[y * L->width + x];
            if (!(t->flags & TILE_F_SOLID)) continue;
            if (t->flags & TILE_F_BACKGROUND) continue;
            /* Pile only on tiles with an empty cell above (the top
             * face). Side / bottom faces get nothing — snow can't
             * cling to a vertical wall. */
            uint16_t above = level_flags_at(L, x, y - 1);
            if ((above & TILE_F_SOLID) && !(above & TILE_F_BACKGROUND)) continue;
            int wx = x * ts;
            int wy = y * ts;
            DrawRectangle(wx, wy, ts, (int)thickness, pile);
            /* Soft shadow band underneath the pile to anchor it. */
            DrawRectangle(wx, wy + (int)thickness, ts, 2, shadow);
        }
    }
}

void atmosphere_draw_poly_overlay(const LvlPoly *poly, double time) {
    if (!poly) return;
    /* ICE glint stripe along the longest edge — animated by time. */
    if ((PolyKind)poly->kind == POLY_KIND_ICE) {
        /* Pick the longest edge as the highlight stripe. */
        int best_e = 0;
        float best_len2 = 0.0f;
        for (int e = 0; e < 3; ++e) {
            int a = e, b = (e + 1) % 3;
            float dx = (float)(poly->v_x[b] - poly->v_x[a]);
            float dy = (float)(poly->v_y[b] - poly->v_y[a]);
            float L2 = dx*dx + dy*dy;
            if (L2 > best_len2) { best_len2 = L2; best_e = e; }
        }
        int a = best_e, b = (best_e + 1) % 3;
        float glint = 0.5f + 0.5f * sinf((float)time * 1.5f);
        Color hi = (Color){ 240, 250, 255, (unsigned char)(160 * glint) };
        Vector2 v0 = { (float)poly->v_x[a], (float)poly->v_y[a] };
        Vector2 v1 = { (float)poly->v_x[b], (float)poly->v_y[b] };
        DrawLineEx(v0, v1, 2.5f, hi);
    }
    /* DEADLY hatch — short amber-red diagonals across the triangle.
     * Scissor is opened ONCE per poly so the per-stripe loop doesn't
     * force a GPU batch flush on every DrawLineEx (the per-flush
     * version showed measurable lag in the editor's F5 test-play on
     * any map with a row of DEADLY polys). */
    if ((PolyKind)poly->kind == POLY_KIND_DEADLY) {
        float minx =  1e9f, maxx = -1e9f;
        float miny =  1e9f, maxy = -1e9f;
        for (int e = 0; e < 3; ++e) {
            if (poly->v_x[e] < minx) minx = poly->v_x[e];
            if (poly->v_x[e] > maxx) maxx = poly->v_x[e];
            if (poly->v_y[e] < miny) miny = poly->v_y[e];
            if (poly->v_y[e] > maxy) maxy = poly->v_y[e];
        }
        float span = (maxx - minx) > (maxy - miny) ? (maxx - minx) : (maxy - miny);
        float step = span / 4.0f;
        if (step < 6.0f) step = 6.0f;
        Color hatch = (Color){ 255, 200, 100, 160 };
        BeginScissorMode((int)minx, (int)miny,
                         (int)(maxx - minx + 1), (int)(maxy - miny + 1));
        for (float t = -span; t <= span; t += step) {
            Vector2 a0 = { minx + t,          miny };
            Vector2 a1 = { minx + t + span,   miny + span };
            DrawLineEx(a0, a1, 1.5f, hatch);
        }
        EndScissorMode();
    }
    /* ONE_WAY chevron — paint a small '^' along the up-normal edge. */
    if ((PolyKind)poly->kind == POLY_KIND_ONE_WAY) {
        for (int e = 0; e < 3; ++e) {
            float nx = (float)poly->normal_x[e] / 32767.0f;
            float ny = (float)poly->normal_y[e] / 32767.0f;
            if (ny > -0.4f) continue;   /* not pointing up enough */
            float cx, cy;
            edge_midpoint(poly, e, &cx, &cy);
            /* Glyph arms perpendicular to normal, pointing along the
             * normal (upward). */
            float tx = -ny, ty =  nx;     /* edge tangent */
            float arm = 6.0f;
            Vector2 tip = { cx + nx * arm,             cy + ny * arm };
            Vector2 l   = { cx + tx * arm - nx * arm,  cy + ty * arm - ny * arm };
            Vector2 r   = { cx - tx * arm - nx * arm,  cy - ty * arm - ny * arm };
            Color chev = (Color){ 240, 240, 240, 220 };
            DrawLineEx(l, tip, 2.0f, chev);
            DrawLineEx(r, tip, 2.0f, chev);
        }
    }
}
