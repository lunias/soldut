#include "atmosphere.h"

#include "audio.h"
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

/* Spawn a single weather particle of `kind`. Screen-space spawn — the
 * spawn coords are stored as pos.x = screen_x_0..1, pos.y = screen_y_0..1
 * relative coords so the renderer can scale them to the live window
 * size each frame without an init recompute on resize. */
static void spawn_weather_particle(FxPool *pool, uint8_t kind, pcg32_t *rng) {
    int idx = -1;
    /* Mirror fx_alloc's simple linear scan + extend. We use a custom
     * spawn so the particle ignores the world gravity / decal pipeline
     * the standard FX kinds care about. */
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

    /* Screen-space spawn: x ∈ [0, 1], y depends on weather kind. */
    float u = pcg32_float01(rng);

    switch (kind) {
        case FX_WEATHER_SNOW: {
            fp->pos = (Vec2){ u, -0.05f };
            float wind = (pcg32_float01(rng) - 0.5f) * 30.0f;
            fp->vel = (Vec2){ wind, 30.0f + pcg32_float01(rng) * 20.0f };
            fp->life     = 8.0f;
            fp->life_max = 8.0f;
            fp->size     = 1.5f + pcg32_float01(rng) * 1.5f;
            fp->color    = 0xF0F8FFB0u;  /* white @ 70% */
            break;
        }
        case FX_WEATHER_RAIN: {
            fp->pos = (Vec2){ u, -0.05f };
            fp->vel = (Vec2){ 80.0f, 400.0f };
            fp->life     = 3.0f;
            fp->life_max = 3.0f;
            fp->size     = 4.0f + pcg32_float01(rng) * 2.0f;  /* line length */
            fp->color    = 0xC8DCFF99u;  /* pale blue */
            break;
        }
        case FX_WEATHER_DUST: {
            fp->pos = (Vec2){ u, pcg32_float01(rng) };
            float dir = pcg32_float01(rng) * 6.2832f;
            fp->vel = (Vec2){ cosf(dir) * 5.0f, sinf(dir) * 5.0f - 2.0f };
            fp->life     = 3.0f;
            fp->life_max = 3.0f;
            fp->size     = 1.0f + pcg32_float01(rng);
            fp->color    = 0xC8B48066u;  /* warm tan @ 40% */
            break;
        }
        case FX_WEATHER_EMBER: {
            fp->pos = (Vec2){ u, 1.05f };
            fp->vel = (Vec2){ (pcg32_float01(rng) - 0.5f) * 10.0f, -15.0f };
            fp->life     = 2.5f;
            fp->life_max = 2.5f;
            fp->size     = 2.5f + pcg32_float01(rng) * 1.5f;
            fp->color    = 0xFFAA40FFu;  /* glowing red-orange */
            break;
        }
        default: fp->alive = 0; return;
    }
    fp->render_prev_pos = fp->pos;
}

/* Spawn one ambient-zone particle. `kind` is FX_AMBIENT_*; rect comes
 * from the LvlAmbi record so the particle stays inside the zone. */
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

    float minx = (float)a->rect_x;
    float miny = (float)a->rect_y;
    float maxx = minx + (float)a->rect_w;
    float maxy = miny + (float)a->rect_h;

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
    fp->render_prev_pos = fp->pos;
}

void atmosphere_tick(World *w, float dt) {
    if (!w) return;
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
                    spawn_weather_particle(&w->fx, kind, w->rng);
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

void atmosphere_draw_ambient_zones(const Level *L, double time) {
    if (!L || L->ambi_count == 0) return;
    /* Pre-frame: paint the rect-tint overlay for ZERO_G + ACID + FOG.
     * The particle layer in fx_draw paints the per-zone particles on
     * top (already running inside BeginMode2D). */
    for (int zi = 0; zi < L->ambi_count; ++zi) {
        const LvlAmbi *a = &L->ambis[zi];
        Rectangle r = (Rectangle){ (float)a->rect_x, (float)a->rect_y,
                                   (float)a->rect_w, (float)a->rect_h };
        switch ((AmbiKind)a->kind) {
            case AMBI_ZERO_G: {
                /* Faint cyan tint + edge frame to announce the volume. */
                DrawRectangleRec(r, (Color){ 120, 160, 220, 25 });
                DrawRectangleLinesEx(r, 1.0f, (Color){ 180, 220, 255, 80 });
                break;
            }
            case AMBI_ACID: {
                /* Pulsing green tint inside the rect. */
                int alpha = 30 + (int)(20.0 * sin(time * 1.5));
                if (alpha < 0) alpha = 0;
                if (alpha > 80) alpha = 80;
                Color tint = { 120, 220, 80, (unsigned char)alpha };
                DrawRectangleRec(r, tint);
                /* Caustic surface band at the top edge. Stretched
                 * scrolling sine highlight — cheap stand-in for a
                 * real caustic texture. */
                float t = (float)time;
                for (int x = 0; x < a->rect_w; x += 8) {
                    float wave = sinf((float)(a->rect_x + x) * 0.05f + t * 2.0f);
                    int   yoff = (int)(wave * 2.0f);
                    DrawRectangle(a->rect_x + x, a->rect_y + yoff,
                                  6, 4, (Color){ 180, 255, 120, 180 });
                }
                /* Edge frame so the volume reads even with low tint alpha. */
                DrawRectangleLinesEx(r, 2.0f, (Color){ 120, 220, 80, 200 });
                break;
            }
            case AMBI_FOG: {
                /* Soft volumetric hint — the real fog lives in the
                 * shader (collect_fog_zones feeds the uniform array).
                 * Here we just paint a faint disc so designers can see
                 * where their fog volumes sit during editor preview. */
                float cx = (float)a->rect_x + (float)a->rect_w * 0.5f;
                float cy = (float)a->rect_y + (float)a->rect_h * 0.5f;
                float radius = (a->rect_w < a->rect_h ? a->rect_w : a->rect_h) * 0.5f;
                Color fog = g_atmosphere.fog_color;
                Color base = (Color){ fog.r, fog.g, fog.b, 30 };
                DrawCircle((int)cx, (int)cy, radius, base);
                DrawRectangleLinesEx(r, 1.0f, (Color){ fog.r, fog.g, fog.b, 120 });
                break;
            }
            case AMBI_WIND: {
                /* Thin edge frame in the wind direction so designers
                 * can see the volume + flow direction at a glance. */
                DrawRectangleLinesEx(r, 1.0f, (Color){ 200, 220, 255, 100 });
                float cx = (float)a->rect_x + (float)a->rect_w * 0.5f;
                float cy = (float)a->rect_y + (float)a->rect_h * 0.5f;
                float dx = (float)a->dir_x_q / 32767.0f;
                float dy = (float)a->dir_y_q / 32767.0f;
                float L_arrow = 24.0f;
                Vector2 a0 = { cx, cy };
                Vector2 a1 = { cx + dx * L_arrow, cy + dy * L_arrow };
                DrawLineEx(a0, a1, 2.0f, (Color){ 230, 240, 255, 180 });
                break;
            }
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
