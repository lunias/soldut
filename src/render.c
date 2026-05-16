#include "render.h"

#include "atmosphere.h"
#include "audio.h"
#include "decal.h"
#include "hud.h"
#include "level.h"
#include "log.h"
#include "map_kit.h"
#include "match.h"
#include "mech.h"
#include "mech_jet_fx.h"
#include "mech_sprites.h"
#include "particle.h"
#include "pickup.h"
#include "profile.h"
#include "projectile.h"
#include "weapon_sprites.h"
#include "weapons.h"

#include <math.h>
#include <stdio.h>

/* M5 P13 + M6 P03 — halftone post-process pass over a capped internal RT.
 *
 * Render flow (post-M6 P03):
 *   1. World draw      -> RenderTexture2D `g_internal_target`  (internal res)
 *   2. Halftone pass   -> RenderTexture2D `g_post_target`      (internal res)
 *   3. Bilinear upscale -> backbuffer, aspect-preserving letterbox
 *   4. HUD on top, no shader, at window resolution
 *
 * Internal == window when `internal_h = 0` (shotmode + 1080p windowed)
 * — the bilinear blit at step 3 becomes a 1:1 copy and the pipeline
 * is pixel-byte-identical to the pre-P03 shape.
 *
 * M6 P06 attempted to collapse steps 2 + 3 into a single backbuffer
 * pass (the halftone shader applied DURING the upscale) — saves one
 * full-screen `DrawTexturePro` per frame on paper, but the WSL bench
 * showed PROF_DRAW_POST simply absorbed what PROF_DRAW_BLIT was
 * paying (the WSLg compositor stall in PROF_PRESENT is the actual
 * cost, ~15.5 ms / frame at 3440×1440 — not the draw-call boundary).
 * Net frame win 0.3 ms (within noise), so we kept the two-pass shape
 * for clarity. Findings recorded in documents/m6/perf-baseline.md.
 *
 * Fallbacks:
 *   - Missing shader file → g_halftone_loaded stays false. World still
 *     draws to g_internal_target at internal res; step 2 is skipped and
 *     step 3 bilinears the internal_target directly to the backbuffer.
 *   - LoadShader returning the default shader (compile failure) → the
 *     uniform locations come back -1 and we treat it as "not loaded". */
#define HALFTONE_DENSITY 0.30f

static Shader          g_halftone_post = {0};
static int             g_halftone_loc_resolution = -1;
static int             g_halftone_loc_density    = -1;
/* M6 P02 — heat-shimmer uniforms. Cached at load time; re-cached after
 * hot-reload via the same lazy halftone_load_once path. -1 = "not in
 * shader" (graceful skip — pre-M6-P02 shaders work unchanged). */
static int             g_halftone_loc_hot_zones  = -1;
static int             g_halftone_loc_jet_time   = -1;
static int             g_halftone_loc_hot_zone_count = -1;
/* M6 P09 — atmospherics uniforms. Same -1 graceful-skip pattern: an
 * older shader file missing these uniforms still loads, the locations
 * stay -1, and the SetShaderValue calls are skipped. */
static int             g_halftone_loc_fog_density   = -1;
static int             g_halftone_loc_fog_color     = -1;
static int             g_halftone_loc_fog_zones     = -1;
static int             g_halftone_loc_fog_zone_count= -1;
static int             g_halftone_loc_vignette      = -1;
static int             g_halftone_loc_atmos_time    = -1;
/* M6 P09 (post-user-feedback) — snow + rain scene treatment uniforms.
 * Same -1 graceful-skip pattern. */
static int             g_halftone_loc_snow_intensity = -1;
static int             g_halftone_loc_rain_intensity = -1;
static bool            g_halftone_loaded         = false;
static bool            g_halftone_load_attempted = false;

/* World render target — receives every world-space draw at internal
 * resolution. Point-filtered so the halftone pass samples crisp source
 * pixels. */
static RenderTexture2D g_internal_target = {0};
/* Post-process render target — receives the halftone pass output, also
 * at internal resolution. The backbuffer blit at present-time samples
 * THIS texture bilinearly when upscaling to the window. */
static RenderTexture2D g_post_target = {0};
static int             g_internal_target_w = 0;
static int             g_internal_target_h = 0;

static void halftone_load_once(void) {
    if (g_halftone_load_attempted) return;
    g_halftone_load_attempted = true;
    const char *path = "assets/shaders/halftone_post.fs.glsl";
    if (!FileExists(path)) {
        LOG_I("halftone: shader file missing (%s); post-process disabled", path);
        return;
    }
    Shader s = LoadShader(NULL, path);
    /* raylib falls back to its default shader on compile-failure, but
     * that shader doesn't carry our uniforms — GetShaderLocation returns
     * -1 if a name isn't present. Treat either condition as "not loaded"
     * so we don't burn cycles drawing through a no-op pass. */
    int loc_res = GetShaderLocation(s, "resolution");
    int loc_den = GetShaderLocation(s, "halftone_density");
    if (loc_res < 0 && loc_den < 0) {
        LOG_W("halftone: shader at %s loaded but uniforms missing; disabling", path);
        UnloadShader(s);
        return;
    }
    g_halftone_post           = s;
    g_halftone_loc_resolution = loc_res;
    g_halftone_loc_density    = loc_den;
    /* M6 P02 — shimmer uniforms. Missing locations are fine (a stale
     * shader file falls back to halftone-only). */
    g_halftone_loc_hot_zones      = GetShaderLocation(s, "jet_hot_zones");
    g_halftone_loc_jet_time       = GetShaderLocation(s, "jet_time");
    g_halftone_loc_hot_zone_count = GetShaderLocation(s, "jet_hot_zone_count");
    /* M6 P09 — atmosphere uniforms. Missing locations are fine —
     * see the comment at the static declarations. */
    g_halftone_loc_fog_density    = GetShaderLocation(s, "fog_density");
    g_halftone_loc_fog_color      = GetShaderLocation(s, "fog_color");
    g_halftone_loc_fog_zones      = GetShaderLocation(s, "fog_zones");
    g_halftone_loc_fog_zone_count = GetShaderLocation(s, "fog_zone_count");
    g_halftone_loc_vignette       = GetShaderLocation(s, "vignette_strength");
    g_halftone_loc_atmos_time     = GetShaderLocation(s, "atmos_time");
    g_halftone_loc_snow_intensity = GetShaderLocation(s, "snow_intensity");
    g_halftone_loc_rain_intensity = GetShaderLocation(s, "rain_intensity");
    g_halftone_loaded             = true;
    LOG_I("halftone: shader loaded; density=%.2f shimmer=%s atmos=%s",
          (double)HALFTONE_DENSITY,
          (g_halftone_loc_hot_zones >= 0 ? "ok" : "missing"),
          (g_halftone_loc_fog_density >= 0 ? "ok" : "missing"));
}

/* M6 P03 — Ensure both internal RTs exist at (iw, ih). Returns:
 *   true if g_internal_target is ready (with or without the halftone pass).
 *   false if iw/ih are invalid or RT allocation failed — caller must skip
 *         the world+post pipeline entirely.
 *
 * `g_post_target` only matters when the halftone shader loaded. If the
 * shader file is missing, the caller blits g_internal_target straight
 * to the backbuffer. */
static bool ensure_internal_targets(int iw, int ih) {
    halftone_load_once();
    if (iw <= 0 || ih <= 0) return false;
    if (g_internal_target.id != 0 && g_post_target.id != 0 &&
        g_internal_target_w == iw && g_internal_target_h == ih) {
        return true;
    }
    if (g_internal_target.id != 0 && g_post_target.id != 0 &&
        g_internal_target_w == iw && g_internal_target_h == ih &&
        !g_halftone_loaded) {
        return true;
    }
    /* Size mismatch or first call: re-allocate both. */
    if (g_internal_target.id != 0) {
        UnloadRenderTexture(g_internal_target);
        g_internal_target = (RenderTexture2D){0};
    }
    if (g_post_target.id != 0) {
        UnloadRenderTexture(g_post_target);
        g_post_target = (RenderTexture2D){0};
    }
    g_internal_target = LoadRenderTexture(iw, ih);
    if (g_internal_target.id == 0) {
        LOG_E("renderer: internal RT LoadRenderTexture(%d,%d) failed", iw, ih);
        return false;
    }
    /* Point-filter so the halftone pass samples crisp source pixels.
     * The backbuffer-stage blit overrides the filter on g_post_target
     * to BILINEAR for the upscale; the internal_target stays POINT. */
    SetTextureFilter(g_internal_target.texture, TEXTURE_FILTER_POINT);

    if (g_halftone_loaded) {
        g_post_target = LoadRenderTexture(iw, ih);
        if (g_post_target.id == 0) {
            LOG_E("halftone: post RT LoadRenderTexture(%d,%d) failed; disabling post", iw, ih);
            g_halftone_loaded = false;
        } else {
            SetTextureFilter(g_post_target.texture, TEXTURE_FILTER_POINT);
        }
    }
    g_internal_target_w = iw;
    g_internal_target_h = ih;
    LOG_I("renderer: internal targets sized %dx%d (halftone=%s)",
          iw, ih, g_halftone_loaded ? "on" : "off");
    return true;
}

void renderer_post_shutdown(void) {
    if (g_post_target.id != 0) {
        UnloadRenderTexture(g_post_target);
        g_post_target = (RenderTexture2D){0};
    }
    if (g_internal_target.id != 0) {
        UnloadRenderTexture(g_internal_target);
        g_internal_target = (RenderTexture2D){0};
    }
    g_internal_target_w = 0;
    g_internal_target_h = 0;
    if (g_halftone_loaded) {
        UnloadShader(g_halftone_post);
        g_halftone_post = (Shader){0};
        g_halftone_loaded = false;
    }
    g_halftone_load_attempted = false;
    g_halftone_loc_resolution     = -1;
    g_halftone_loc_density        = -1;
    g_halftone_loc_hot_zones      = -1;
    g_halftone_loc_jet_time       = -1;
    g_halftone_loc_hot_zone_count = -1;
}

void renderer_init(Renderer *r, int sw, int sh, Vec2 follow) {
    r->camera.offset   = (Vector2){ sw * 0.5f, sh * 0.5f };
    r->camera.target   = follow;
    r->camera.rotation = 0.0f;
    r->camera.zoom     = 1.4f;
    r->shake_phase     = 0.0f;
    r->last_cursor_screen = (Vec2){ sw * 0.5f, sh * 0.5f };
    r->last_cursor_world  = follow;
    r->cam_dt_override    = 0.0f;
    /* M6 P03 — identity transform until the first renderer_draw_frame
     * writes the real letterbox numbers in. Stays the identity for
     * shotmode + 1080p windowed (internal == window). */
    r->blit_scale = 1.0f;
    r->blit_dx    = 0.0f;
    r->blit_dy    = 0.0f;
    r->shake_scale = 1.0f;  /* M6 P10 — main.c overrides from prefs. */
}

Vec2 renderer_screen_to_world(const Renderer *r, Vec2 screen) {
    /* M6 P03 — `screen` arrives in window pixels (raw GetMousePosition
     * from main.c / shotmode). The camera operates in internal-RT
     * pixels because update_camera writes
     * `r->camera.offset = internal_w/2, internal_h/2`. Apply the
     * inverse of the backbuffer letterbox transform to land in
     * internal coords before handing to raylib's
     * GetScreenToWorld2D. With blit_scale=1 and dx=dy=0 (shotmode +
     * 1080p windowed) this is the identity. */
    float scale = (r->blit_scale > 0.0f) ? r->blit_scale : 1.0f;
    Vector2 internal_px = {
        (screen.x - r->blit_dx) / scale,
        (screen.y - r->blit_dy) / scale,
    };
    Vector2 w = GetScreenToWorld2D(internal_px, r->camera);
    return (Vec2){ w.x, w.y };
}

/* Smoothed follow + screen-shake. Camera target gravitates toward the
 * local mech, with a small lookahead toward the cursor. */
static void update_camera(Renderer *r, World *w, int sw, int sh, float dt) {
    Vec2 focus;
    if (w->local_mech_id >= 0) {
        /* M6 — follow the PELVIS, not the chest. Pelvis is the
         * authoritative anchor for the procedural pose; chest is a
         * pose-output that shifts when the player crouches (chest
         * pulls in toward pelvis) or goes prone (chest extends
         * forward along the ground). Following the chest in those
         * poses makes the camera appear to drift backward / sideways
         * even though the mech's physical position hasn't changed —
         * pelvis is the right reference. */
        const Mech *lm = &w->mechs[w->local_mech_id];
        int p_idx = lm->particle_base + PART_PELVIS;
        focus = (Vec2){
            w->particles.pos_x[p_idx],
            w->particles.pos_y[p_idx],
        };
    } else {
        focus = (Vec2){0, 0};
    }
    /* Lookahead toward the cursor. */
    Vec2 cursor = r->last_cursor_world;
    Vec2 look_dir = { cursor.x - focus.x, cursor.y - focus.y };
    float L = sqrtf(look_dir.x * look_dir.x + look_dir.y * look_dir.y);
    if (L > 1.0f) {
        float lookahead = 80.0f;
        focus.x += (look_dir.x / L) * lookahead * 0.4f;
        focus.y += (look_dir.y / L) * lookahead * 0.4f;
    }

    /* Smooth-follow toward focus. */
    Vec2 t = r->camera.target;
    float k = 1.0f - powf(0.001f, dt);   /* frame-rate-independent lerp */
    t.x += (focus.x - t.x) * k * 6.0f;
    t.y += (focus.y - t.y) * k * 6.0f;
    /* Clamp to level bounds (with a small dead-zone at edges). */
    float halfw = (float)sw / (2.0f * r->camera.zoom);
    float halfh = (float)sh / (2.0f * r->camera.zoom);
    float minx = halfw, miny = halfh;
    float maxx = level_width_px (&w->level) - halfw;
    float maxy = level_height_px(&w->level) - halfh;
    if (t.x < minx) t.x = minx;
    if (t.x > maxx) t.x = maxx;
    if (t.y < miny) t.y = miny;
    if (t.y > maxy) t.y = maxy;
    r->camera.target = t;
    r->camera.offset = (Vector2){ sw * 0.5f, sh * 0.5f };

    /* Screen shake — low-frequency sine, amplitude proportional to
     * intensity. The intensity decays inside simulate(). M6 P10:
     * `shake_scale` multiplies amp + rotation so the user (or the
     * --shake-scale CLI) can dial total intensity. 0 disables shake;
     * 1.0 is the baseline; values above 1 amplify big events too. */
    r->shake_phase += dt * 35.0f;
    float scale = r->shake_scale;
    float amp = w->shake_intensity * 6.0f * scale;
    r->camera.target.x += sinf(r->shake_phase * 1.7f) * amp;
    r->camera.target.y += cosf(r->shake_phase * 2.1f) * amp;
    r->camera.rotation  = sinf(r->shake_phase * 1.3f) * w->shake_intensity * 1.2f * scale;
}

/* ---- Drawing helpers ---------------------------------------------- */

/* M6 P09 — Paint the per-flag overlay on top of a freshly-drawn tile.
 * `mat` comes from atmosphere_tile_material(t->flags) — its pattern
 * field picks the overlay style.
 *
 * Cost budget: 2-4 DrawLineEx per ICE/ONE_WAY tile, 4-6 short lines
 * per DEADLY tile, zero for SOLID/BACKGROUND. Worst case (110×60
 * Concourse fully filled with DEADLY tiles, which never happens) is
 * ~6600 × 6 ≈ 40k DrawLineEx; raylib batches these, and the per-frame
 * cost on a 1080p internal RT measures sub-ms in the perf overlay. */
static void draw_tile_overlay(float wx, float wy, float ts,
                              TileMaterial mat, double time) {
    switch (mat.pattern) {
        case TILE_PAT_ICE_GLINT: {
            float t = (float)time;
            float a = 0.5f + 0.5f * sinf(t * 1.5f + (wx + wy) * 0.04f);
            Color hi = (Color){ mat.accent.r, mat.accent.g, mat.accent.b,
                                (unsigned char)((float)mat.accent.a * a) };
            Vector2 p0 = { wx + ts * 0.2f, wy + ts * 0.2f };
            Vector2 p1 = { wx + ts * 0.8f, wy + ts * 0.4f };
            DrawLineEx(p0, p1, 1.5f, hi);
            break;
        }
        case TILE_PAT_DEADLY_HATCH: {
            /* 45° amber-red stripes across the tile. Each tile draws
             * 2 short stripes that visually clip themselves to the
             * tile body (the stripe goes corner-to-near-corner, well
             * inside ts). NO scissor — each BeginScissorMode call
             * forces a GPU batch flush, and the original "scissor per
             * stripe" path produced 5-10 flushes per DEADLY tile per
             * frame, which lagged the editor's F5 test-play on a map
             * with ~20 DEADLY tiles (200+ flushes/frame). The stripes
             * fit within the tile body anyway, so clipping is moot. */
            Color hatch = mat.accent;
            float pad = ts * 0.15f;
            float a = ts - pad * 2.0f;
            Vector2 p0a = { wx + pad,         wy + pad };
            Vector2 p1a = { wx + pad + a,     wy + pad + a };
            Vector2 p0b = { wx + pad,         wy + pad + a * 0.5f };
            Vector2 p1b = { wx + pad + a * 0.5f, wy + pad + a };
            DrawLineEx(p0a, p1a, 2.0f, hatch);
            DrawLineEx(p0b, p1b, 2.0f, hatch);
            break;
        }
        case TILE_PAT_ONE_WAY_CHEVRON: {
            /* '^' centered on the tile, pointing up. */
            Color chev = mat.accent;
            Vector2 tip = { wx + ts * 0.5f, wy + ts * 0.25f };
            Vector2 l   = { wx + ts * 0.30f, wy + ts * 0.55f };
            Vector2 r   = { wx + ts * 0.70f, wy + ts * 0.55f };
            DrawLineEx(l, tip, 2.0f, chev);
            DrawLineEx(r, tip, 2.0f, chev);
            break;
        }
        case TILE_PAT_BACKGROUND_ALPHA:
            /* Alpha is folded into mat.base.a; no extra overlay. */
            break;
        case TILE_PAT_NONE:
        default:
            break;
    }
}

/* M6 P09 — Tile rendering. Replaces the P13 path: each tile now picks
 * its base color from the atmosphere theme palette (via the per-flag
 * material) so ICE/DEADLY/ONE_WAY/BACKGROUND tiles read as
 * categorically different from a SOLID tile even in the no-atlas
 * fallback. Atlas path multiplies the material base into the atlas
 * sub-rect tint (designer art still wins, but the per-flag color
 * still announces the material).
 *
 * BACKGROUND tiles are PURELY decorative — physics already skips
 * them per the §5.4 wiring; they paint with alpha so the player can
 * see through. */
static void draw_level_tiles(const Level *L) {
    const float ts = (float)L->tile_size;
    Texture2D atlas = g_map_kit.tiles;
    bool has_atlas = (atlas.id != 0);
    double time = GetTime();

    if (has_atlas) {
        const float src_tile_px = 32.0f;
        for (int y = 0; y < L->height; ++y) {
            for (int x = 0; x < L->width; ++x) {
                const LvlTile *t = &L->tiles[y * L->width + x];
                if (!(t->flags & TILE_F_SOLID)) continue;
                TileMaterial mat = atmosphere_tile_material(t->flags);
                int aid = (int)t->id;
                int ax  = (aid % 8) * (int)src_tile_px;
                int ay  = (aid / 8) * (int)src_tile_px;
                DrawTexturePro(atlas,
                    (Rectangle){ (float)ax, (float)ay, src_tile_px, src_tile_px },
                    (Rectangle){ (float)x * ts, (float)y * ts, ts, ts },
                    (Vector2){0, 0}, 0.0f, mat.base);
                draw_tile_overlay((float)x * ts, (float)y * ts, ts, mat, time);
            }
        }
        return;
    }

    /* Fallback: theme-tinted 2-tone checkerboard with per-flag
     * overlays. The 2-tone variation stays so within-flag visual
     * texture survives even without an atlas. */
    for (int y = 0; y < L->height; ++y) {
        for (int x = 0; x < L->width; ++x) {
            const LvlTile *t = &L->tiles[y * L->width + x];
            if (!(t->flags & TILE_F_SOLID)) continue;
            TileMaterial mat = atmosphere_tile_material(t->flags);
            Color a = mat.base;
            /* Variant tone: darken by 20% on odd cells for the within-
             * flag checkerboard texture. */
            Color b = (Color){
                (unsigned char)((int)a.r * 80 / 100),
                (unsigned char)((int)a.g * 80 / 100),
                (unsigned char)((int)a.b * 80 / 100),
                a.a,
            };
            Color fill = ((x + y) & 1) ? a : b;
            Color edge = (Color){
                (unsigned char)((int)a.r * 130 / 100 > 255 ? 255 : (int)a.r * 130 / 100),
                (unsigned char)((int)a.g * 130 / 100 > 255 ? 255 : (int)a.g * 130 / 100),
                (unsigned char)((int)a.b * 130 / 100 > 255 ? 255 : (int)a.b * 130 / 100),
                a.a,
            };
            DrawRectangle((int)((float)x * ts), (int)((float)y * ts),
                          (int)ts, (int)ts, fill);
            DrawRectangleLines((int)((float)x * ts), (int)((float)y * ts),
                               (int)ts, (int)ts, edge);
            draw_tile_overlay((float)x * ts, (float)y * ts, ts, mat, time);
        }
    }
}

/* P13 / M6 P09 — Free polygon rendering. Per-kind fill colors come
 * from the atmosphere theme palette via
 * `atmosphere_tile_material(kind_to_flag)`, so a NEON map paints a
 * magenta DEADLY poly + a cyan ICE poly that match the tile vocabulary.
 * The DEADLY color is now {200,60,60} red (was {80,200,80} green —
 * which clashed with the editor's red and the thumbnail painter's red).
 * BACKGROUND polygons are skipped here; `draw_polys_background` paints
 * them in a separate pass after the mechs so they sit visually in front
 * of the playfield. */
static void draw_polys(const Level *L) {
    double time = GetTime();
    /* Translate poly kind → tile flag bit so the same material table
     * picks the fill color for both tile and poly geometry. */
    static const uint16_t kind_to_flag[] = {
        [POLY_KIND_SOLID]      = TILE_F_SOLID,
        [POLY_KIND_ICE]        = TILE_F_SOLID | TILE_F_ICE,
        [POLY_KIND_DEADLY]     = TILE_F_SOLID | TILE_F_DEADLY,
        [POLY_KIND_ONE_WAY]    = TILE_F_SOLID | TILE_F_ONE_WAY,
        [POLY_KIND_BACKGROUND] = TILE_F_SOLID | TILE_F_BACKGROUND,
    };
    Color poly_edge = (Color){180, 200, 230, 255 };
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *poly = &L->polys[i];
        if ((PolyKind)poly->kind == POLY_KIND_BACKGROUND) continue;
        uint16_t pk = (uint16_t)poly->kind;
        uint16_t flag = (pk < sizeof(kind_to_flag)/sizeof(kind_to_flag[0]))
                      ? kind_to_flag[pk] : TILE_F_SOLID;
        TileMaterial mat = atmosphere_tile_material(flag);
        Color fill = mat.base;
        /* ONE_WAY gets a translucent fill so the chevron stripe reads. */
        if ((PolyKind)poly->kind == POLY_KIND_ONE_WAY) fill.a = 200;
        Vector2 v0 = { (float)poly->v_x[0], (float)poly->v_y[0] };
        Vector2 v1 = { (float)poly->v_x[1], (float)poly->v_y[1] };
        Vector2 v2 = { (float)poly->v_x[2], (float)poly->v_y[2] };
        /* DrawTriangle is CCW-only; our authoring is screen-CW so flip
         * the vertex order to keep the fill visible. */
        DrawTriangle(v0, v2, v1, fill);
        DrawLineEx(v0, v1, 2.0f, poly_edge);
        DrawLineEx(v1, v2, 2.0f, poly_edge);
        DrawLineEx(v2, v0, 2.0f, poly_edge);
        /* M6 P09 — per-kind overlay (chevron / glint / hatch). */
        atmosphere_draw_poly_overlay(poly, time);
    }
}

/* BACKGROUND polygons — drawn after mechs at alpha 0.6 so they sit in
 * front of the playfield as decorative silhouettes. Per the spec, this
 * is the rendering layer for "things behind but in front of mechs"
 * (window grilles, distant railings). */
static void draw_polys_background(const Level *L) {
    Color poly_back = (Color){ 28, 32, 40, 153 };  /* alpha ~0.6 */
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *poly = &L->polys[i];
        if ((PolyKind)poly->kind != POLY_KIND_BACKGROUND) continue;
        Vector2 v0 = { (float)poly->v_x[0], (float)poly->v_y[0] };
        Vector2 v1 = { (float)poly->v_x[1], (float)poly->v_y[1] };
        Vector2 v2 = { (float)poly->v_x[2], (float)poly->v_y[2] };
        DrawTriangle(v0, v2, v1, poly_back);
    }
}

/* Compatibility wrapper kept for the existing call site below — fires
 * tiles + non-background polygons in the M4-equivalent slot. */
static void draw_level(const Level *L) {
    draw_level_tiles(L);
    draw_polys(L);
}

/* P13 — Parallax tile across the screen with a horizontal scroll factor.
 * Drawn outside BeginMode2D, in screen space. The vertical axis isn't
 * scrolled — each layer anchors at the top of the screen at native size.
 * (When a custom map ships parallax PNGs that are taller/shorter than
 * the screen, this is the simple fallback; per-map vertical anchoring
 * is M6 polish.) Idempotent no-op when the texture isn't loaded. */
static void draw_parallax_layer(Camera2D cam, Texture2D tex,
                                float ratio, int sw, int sh)
{
    if (tex.id == 0) return;
    int tw = tex.width;
    int th = tex.height;
    if (tw <= 0 || th <= 0) return;
    (void)sh;
    /* World-space scroll, multiplied by the parallax ratio. ratio < 1
     * makes the layer drift slower than the world (depth illusion). */
    float scroll = cam.target.x * ratio;
    float mod    = fmodf(scroll, (float)tw);
    if (mod < 0.0f) mod += (float)tw;
    float x = -mod;
    while (x < (float)sw) {
        DrawTexture(tex, (int)x, 0, WHITE);
        x += (float)tw;
    }
}

static void draw_parallax_far_mid(Camera2D cam, int sw, int sh) {
    /* Far: nearly static (ratio 0.10) — sky / distant skyline. */
    draw_parallax_layer(cam, g_map_kit.parallax_far, 0.10f, sw, sh);
    /* Mid: 0.40 — buildings, hills. Drawn over far. */
    draw_parallax_layer(cam, g_map_kit.parallax_mid, 0.40f, sw, sh);
}

static void draw_parallax_near(Camera2D cam, int sw, int sh) {
    /* Near: 0.95 — background layer that scrolls almost 1:1 with the
     * camera (the "near distance" backdrop, not foreground silhouettes).
     * Drawn before the world so level geometry and mechs paint on top. */
    draw_parallax_layer(cam, g_map_kit.parallax_near, 0.95f, sw, sh);
}

/* P13 — Decoration atlas. Shared across all maps (not per-kit), loaded
 * once at startup via `decorations_atlas_load`. Until the atlas ships
 * (P15/P16), the slot stays at id=0 and the renderer draws small
 * layer-colored placeholder rectangles so designers can see where their
 * `LvlDeco` records sit in the world during test-play.
 *
 * Sub-rect lookup: there is no per-deco manifest at v1. We hash the
 * `sprite_str_idx` byte offset into a 16x16 grid of 64x64 cells across
 * the 1024x1024 atlas, so a stable string at the same offset always
 * grabs the same sub-rect. P15/P16 ships an `assets/sprites/decorations.atlas`
 * manifest and this fallback gets replaced with a real lookup. */
static Texture2D g_decorations_atlas = {0};
static bool      g_decorations_atlas_attempted = false;

static void decorations_atlas_load_once(void) {
    if (g_decorations_atlas_attempted) return;
    g_decorations_atlas_attempted = true;
    const char *path = "assets/sprites/decorations.png";
    if (!FileExists(path)) {
        LOG_I("decorations: %s missing; placeholder rectangles only", path);
        return;
    }
    Texture2D t = LoadTexture(path);
    if (t.id == 0) {
        LOG_W("decorations: LoadTexture(%s) failed", path);
        return;
    }
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    g_decorations_atlas = t;
    LOG_I("decorations: atlas loaded (%dx%d)", t.width, t.height);
}

void renderer_decorations_unload(void) {
    if (g_decorations_atlas.id != 0) UnloadTexture(g_decorations_atlas);
    g_decorations_atlas = (Texture2D){0};
    g_decorations_atlas_attempted = false;
}

/* Hash a byte offset into a 16x16 cell index. Stable across runs so a
 * given LvlDeco record always picks the same sub-rect. */
static Rectangle deco_src_rect_for(uint16_t sprite_str_idx) {
    uint32_t h = (uint32_t)sprite_str_idx * 2654435761u;
    int cell  = (int)(h >> 24) & 0xFF;        /* 0..255 — 16x16 grid */
    int cx    = cell & 0x0F;
    int cy    = (cell >> 4) & 0x0F;
    return (Rectangle){ (float)(cx * 64), (float)(cy * 64), 64.0f, 64.0f };
}

static Color deco_placeholder_color(int layer) {
    static const Color c[4] = {
        { 90, 110, 160, 180 },   /* layer 0 — far parallax tone */
        { 90, 140, 130, 200 },   /* layer 1 — mid */
        {130, 130,  90, 220 },   /* layer 2 — near foreground */
        {180,  90,  90, 230 },   /* layer 3 — foreground silhouette */
    };
    if (layer < 0 || layer > 3) return (Color){200, 200, 200, 200};
    return c[layer];
}

/* P13 — Walk `level->decos`, draw every entry whose `layer` matches.
 * Atlas-driven when present; small placeholder rectangles when not.
 * Caller is inside BeginMode2D (decorations live in world space).
 * The ADDITIVE flag wraps a single deco in BeginBlendMode pair —
 * cheap state change cost (one per ADDITIVE deco). */
enum {
    DECO_FLIPPED_X = 1u << 0,
    DECO_ADDITIVE  = 1u << 1,
};

static void draw_decorations(const Level *L, int layer) {
    if (L->deco_count == 0 || !L->decos) return;
    decorations_atlas_load_once();
    bool has_atlas = (g_decorations_atlas.id != 0);
    Color placeholder = deco_placeholder_color(layer);

    for (int i = 0; i < L->deco_count; ++i) {
        const LvlDeco *d = &L->decos[i];
        if ((int)d->layer != layer) continue;

        /* Q1.15 scale → float multiplier. Stored value 32768 = 1.0×.
         * Defensive clamp so a malformed record can't blow up draw
         * dimensions (e.g. negative scale via signed underflow). */
        float scale = (d->scale_q == 0) ? 1.0f
                                        : (float)d->scale_q / 32768.0f;
        if (scale <  0.05f) scale = 1.0f;
        if (scale > 16.0f)  scale = 16.0f;

        /* rot_q is 1/256 turns per the .lvl spec. */
        float angle = ((float)d->rot_q / 256.0f) * 360.0f;

        bool additive = (d->flags & DECO_ADDITIVE) != 0;
        if (additive) BeginBlendMode(BLEND_ADDITIVE);

        if (has_atlas) {
            Rectangle src = deco_src_rect_for(d->sprite_str_idx);
            float dw = src.width  * scale;
            float dh = src.height * scale;
            /* DrawTexturePro flipX: pass a negative source width. */
            if (d->flags & DECO_FLIPPED_X) src.width = -src.width;
            DrawTexturePro(g_decorations_atlas, src,
                (Rectangle){ (float)d->pos_x, (float)d->pos_y, dw, dh },
                /* origin = centroid so rotation pivots correctly */
                (Vector2){ dw * 0.5f, dh * 0.5f },
                angle, WHITE);
        } else {
            /* Placeholder: a small filled rectangle at the deco position.
             * Keeps deco placement visible in test-play through P14. */
            float w = 16.0f * scale;
            float h = 16.0f * scale;
            DrawRectanglePro(
                (Rectangle){ (float)d->pos_x, (float)d->pos_y, w, h },
                (Vector2){ w * 0.5f, h * 0.5f },
                angle, placeholder);
        }

        if (additive) EndBlendMode();
    }
}

/* P03: lerp between this tick's start (`render_prev_*`) and the latest
 * physics result (`pos_*`) by `alpha = accum / TICK_DT`. Decouples
 * render rate from sim rate — vsync-fast displays no longer accelerate
 * physics. Anywhere we used to read `p->pos_x[i]/p->pos_y[i]` to draw,
 * we go through this helper. */
static inline Vec2 particle_render_pos(const ParticlePool *p, int i, float alpha) {
    return (Vec2){
        p->render_prev_x[i] + (p->pos_x[i] - p->render_prev_x[i]) * alpha,
        p->render_prev_y[i] + (p->pos_y[i] - p->render_prev_y[i]) * alpha,
    };
}

/* P12 — Apply the hit-flash white-additive blend to a body tint.
 * `timer` is the per-mech `hit_flash_timer` (0..0.10 s); the blend is
 * linear from 0% (timer=0) to 100% white (timer=0.10). Alpha is
 * preserved so invisibility-powerup alpha-mod survives the flash. */
static inline Color apply_hit_flash(Color c, float timer) {
    if (timer <= 0.0f) return c;
    float f = timer / 0.10f;
    if (f > 1.0f) f = 1.0f;
    return (Color){
        (uint8_t)(c.r + ((float)(255 - c.r)) * f),
        (uint8_t)(c.g + ((float)(255 - c.g)) * f),
        (uint8_t)(c.b + ((float)(255 - c.b)) * f),
        c.a,
    };
}

static void draw_bone(const ParticlePool *p, int a, int b, float thick, Color c,
                      float alpha, Vec2 off) {
    Vec2 va = particle_render_pos(p, a, alpha);
    Vec2 vb = particle_render_pos(p, b, alpha);
    DrawLineEx((Vector2){va.x + off.x, va.y + off.y},
               (Vector2){vb.x + off.x, vb.y + off.y}, thick, c);
}

/* True if a CSTR_DISTANCE constraint between particles a and b is
 * present and still active in the pool. After a limb gets dismembered,
 * the relevant distance constraints are flagged inactive; the bones
 * connecting those particles must not be rendered or the corpse will
 * trail a stretched bone-shaped line across the level as the dead body
 * separates from the orphaned limb. */
static bool bone_constraint_active(const ConstraintPool *cp, int a, int b) {
    for (int i = 0; i < cp->count; ++i) {
        const Constraint *c = &cp->items[i];
        if (c->kind != CSTR_DISTANCE) continue;
        if ((c->a == a && c->b == b) || (c->a == b && c->b == a)) {
            return c->active != 0;
        }
    }
    /* No constraint found between this pair — render normally. The hip
     * plate, shoulder plate, etc. are constraints; the cross-body
     * bones we draw all have one. If a future bone has none, this
     * keeps it visible. */
    return true;
}

static void draw_bone_chk(const ParticlePool *p, const ConstraintPool *cp,
                          int a, int b, float thick, Color c,
                          float alpha, Vec2 off) {
    if (!bone_constraint_active(cp, a, b)) return;
    draw_bone(p, a, b, thick, c, alpha, off);
}

/* Bone draw that stops at the first solid tile. Used for the rifle
 * barrel and for arm bones that occasionally cross a thin solid when
 * the aim direction points into one — physics has already pushed each
 * particle out, but the line connecting two outside points can still
 * graze a tile corner or a 1-tile-wide pillar. */
static void draw_bone_clamped(const Level *L, Vec2 a, Vec2 b, float thick, Color c) {
    float t = 1.0f;
    Vec2  end = b;
    if (level_ray_hits(L, a, b, &t)) {
        /* Pull back a hair so we don't draw on the tile edge. */
        if (t > 0.02f) t -= 0.02f; else t = 0.0f;
        end = (Vec2){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
    }
    DrawLineEx((Vector2){a.x, a.y}, (Vector2){end.x, end.y}, thick, c);
}

/* P11 — held-weapon render. Picks the effective weapon (active slot,
 * or the inactive slot for a brief flicker after BTN_FIRE_SECONDARY),
 * looks up the sprite-def, and either draws via DrawTexturePro at the
 * grip pivot OR falls back to a per-weapon-sized barrel line clamped
 * against solids. Visible muzzle flash for ~3 ticks after fire stacks
 * additively over either path.
 *
 * Plumbed at the top level (renderer_draw_frame) instead of from inside
 * draw_mech because we need `World *w` for tick + last-fired tracking
 * + local_mech_id (for invis alpha) + level (for the fallback clamp).
 *
 * Spec: documents/m5/12-rigging-and-damage.md §"Per-weapon visible art"
 * + §"BTN_FIRE_SECONDARY hand flicker". */
static void draw_held_weapon(const World *w, int mid, float alpha,
                             Vec2 visual_offset)
{
    const Mech *m = &w->mechs[mid];
    if (!m->alive) return;

    /* Effective weapon: usually the active slot, but for a short
     * window after RMB fire the inactive slot's weapon flickers in so
     * the player sees the throw/shot they just produced. */
    int eff_weapon_id = m->weapon_id;
    if (m->last_fired_slot >= 0 &&
        m->last_fired_tick != 0 &&
        (w->tick - m->last_fired_tick) < 3 &&
        m->last_fired_slot != m->active_slot) {
        eff_weapon_id = (m->last_fired_slot == 0) ? m->primary_id
                                                  : m->secondary_id;
    }

    const WeaponSpriteDef *wp  = weapon_sprite_def(eff_weapon_id);
    const Weapon          *wpn = weapon_def(eff_weapon_id);
    if (!wp || !wpn) return;

    int b = m->particle_base;
    Vec2 rh = particle_render_pos(&w->particles, b + PART_R_HAND, alpha);
    rh.x += visual_offset.x;
    rh.y += visual_offset.y;
    Vec2 aim = mech_aim_dir(w, mid);

    /* Body tint mirrors draw_mech_sprites / draw_mech_capsules so a
     * dummy's orange wash + invis alpha-mod carry through to the
     * weapon. Dead mechs already early-exit above. */
    Color tint = m->is_dummy ? (Color){200, 130,  40, 255}
                             : (Color){255, 255, 255, 255};
    if (m->powerup_invis_remaining > 0.0f) {
        tint.a = (mid == w->local_mech_id) ? (uint8_t)128 : (uint8_t)51;
    }

    if (g_weapons_atlas.id != 0) {
        /* Sprite path: rotate around the grip pivot by the aim angle.
         * Symmetric weapon design avoids the "scope flips at ±π"
         * artifact noted in 12-rigging-and-damage.md §"Plate flip past
         * 180°"; asymmetric weapons (with a top-mounted optic) can opt
         * into per-sprite flipY in a future polish pass. */
        float angle = atan2f(aim.y, aim.x) * RAD2DEG;
        DrawTexturePro(g_weapons_atlas, wp->src,
                       (Rectangle){ rh.x, rh.y, wp->draw_w, wp->draw_h },
                       wp->pivot_grip, angle, tint);
    } else {
        /* Fallback: per-weapon-sized barrel line, clamped to the first
         * solid tile crossing so the barrel doesn't draw through a
         * thin wall. Length scales with the sprite's draw_w so a Mass
         * Driver "reads" visibly longer than a Sidearm even without
         * the atlas. */
        Vec2 muzzle = {
            rh.x + aim.x * wp->draw_w * 0.7f,
            rh.y + aim.y * wp->draw_w * 0.7f,
        };
        Color line_color = (Color){ 50, 60, 80, tint.a };
        if (m->is_dummy) line_color = (Color){ 120, 80, 30, tint.a };
        draw_bone_clamped(&w->level, rh, muzzle, 3.0f, line_color);
    }

    /* Visible muzzle flash for ~3 ticks (50 ms @ 60 Hz) after fire.
     * Triggered by `last_fired_tick` rather than `fire_cooldown` so
     * the flash window is independent of weapon fire-rate (a Microgun
     * with a 25 ms cycle would otherwise never see a flash). */
    if (m->last_fired_tick != 0 && (w->tick - m->last_fired_tick) < 3) {
        Vec2 muzzle = weapon_muzzle_world(rh, aim, wp, wpn->muzzle_offset);
        float t = 1.0f - (float)(w->tick - m->last_fired_tick) / 3.0f;
        uint8_t flash_a = (uint8_t)(220.0f * t * (tint.a / 255.0f));
        BeginBlendMode(BLEND_ADDITIVE);
        DrawCircleV((Vector2){ muzzle.x, muzzle.y },
                    8.0f * (0.6f + 0.4f * t),
                    (Color){ 255, 200, 80, flash_a });
        EndBlendMode();
    }
}

/* P12 — Inverse of the decal-record transform in mech.c::mech_apply_damage.
 * Given a sprite (MSP) and i8 sprite-local coords (midpoint-relative,
 * unrotated), compute the world position the renderer should draw the
 * decal at. Reads particle_render_pos so decals lerp with the rest of
 * the body during sub-tick interp. */
static Vec2 part_local_to_world(const ParticlePool *p, const Mech *m,
                                MechSpriteId sp, int8_t lx, int8_t ly,
                                float alpha)
{
    int b = m->particle_base;
    int p_a = -1, p_b = -1;
    mech_sprite_part_endpoints(sp, &p_a, &p_b);
    Vec2 vb = particle_render_pos(p, b + p_b, alpha);
    Vec2 mid;
    float angle;
    if (p_a >= 0) {
        Vec2 va = particle_render_pos(p, b + p_a, alpha);
        mid = (Vec2){ (va.x + vb.x) * 0.5f, (va.y + vb.y) * 0.5f };
        angle = atan2f(vb.y - va.y, vb.x - va.x) - 1.5707963267948966f;
    } else {
        mid = vb;
        angle = 0.0f;
    }
    float c = cosf(angle), s = sinf(angle);
    return (Vec2){
        (float)lx * c - (float)ly * s + mid.x,
        (float)lx * s + (float)ly * c + mid.y,
    };
}

/* P12 — Composite damage decals over the just-drawn limbs. Until P13
 * ships authored decal sub-rects in the HUD atlas, render a small
 * filled circle per decal as a placeholder. The colors track the three
 * decal kinds defined in world.h. Both render paths (sprites + capsule
 * fallback) call this after their respective body draws so a hit
 * leaves a persistent visible mark either way. */
static void draw_damage_decals(const ParticlePool *p, const Mech *m,
                               float alpha, Vec2 visual_offset,
                               uint8_t base_alpha)
{
    /* Placeholder colors — replaced by HUD-atlas sub-rects at P13.
     * Brace-enclosed inner initializers (no compound-literal cast) keep
     * the array initializer constant under -Wpedantic. */
    static const Color s_decal_colors[3] = {
        [DAMAGE_DECAL_DENT]   = { 32,  36,  44, 200 },
        [DAMAGE_DECAL_SCORCH] = {  8,   8,  12, 230 },
        [DAMAGE_DECAL_GOUGE]  = { 80,  10,   8, 240 },
    };
    static const float s_decal_radius[3] = {
        [DAMAGE_DECAL_DENT]   = 2.5f,
        [DAMAGE_DECAL_SCORCH] = 3.5f,
        [DAMAGE_DECAL_GOUGE]  = 4.0f,
    };
    for (int sp = 0; sp < MECH_LIMB_DECAL_COUNT; ++sp) {
        const MechLimbDecals *ring = &m->damage_decals[sp];
        if (ring->count == 0) continue;
        int n = (ring->count < DAMAGE_DECALS_PER_LIMB)
              ? (int)ring->count : DAMAGE_DECALS_PER_LIMB;
        for (int k = 0; k < n; ++k) {
            const MechDamageDecal *d = &ring->items[k];
            uint8_t kind = d->kind;
            if (kind > DAMAGE_DECAL_GOUGE) kind = DAMAGE_DECAL_DENT;
            Color col = s_decal_colors[kind];
            col.a = (uint8_t)(((int)col.a * (int)base_alpha) / 255);
            Vec2 wp = part_local_to_world(p, m, (MechSpriteId)sp,
                                          d->local_x, d->local_y, alpha);
            wp.x += visual_offset.x;
            wp.y += visual_offset.y;
            DrawCircleV((Vector2){ wp.x, wp.y },
                        s_decal_radius[kind], col);
        }
    }
}

/* M4 capsule-line mech body — kept as the no-asset / dev fallback when
 * the chassis's sprite atlas hasn't loaded. The new sprite path
 * (`draw_mech_sprites`) is the M5 P10 default; the dispatcher in
 * `draw_mech` picks based on `g_chassis_sprites[m->chassis_id].atlas.id`.
 *
 * The Level pointer clamps arm/rifle bones that would otherwise visibly
 * cross a thin solid (e.g., aiming through the col-55 wall). The
 * ConstraintPool lets us skip drawing bones whose distance constraint
 * has been deactivated by dismemberment so the corpse doesn't trail a
 * stretched bone across the level.
 *
 * P03: `alpha` = sub-tick interp factor (in [0,1)) lerps between the
 * physics state at the start of the most-recent simulate tick and the
 * latest result. `visual_offset` is added to every drawn particle —
 * used by the local mech to smooth out reconciliation snaps over
 * ~6 frames. Pass {0,0} for non-local mechs. */
static void draw_mech_capsules(const ParticlePool *p, const ConstraintPool *cp,
                               const Mech *m, const Level *L, bool is_local,
                               float alpha, Vec2 visual_offset) {
    int b = m->particle_base;
    int idx_head    = b + PART_HEAD;
    int idx_neck    = b + PART_NECK;
    int idx_chest   = b + PART_CHEST;
    int idx_pelvis  = b + PART_PELVIS;

    /* Body color: trooper grey, dummies yellow-orange, dead = red tint. */
    Color body =  m->is_dummy
                ? (Color){200, 130,  40, 255}
                : (Color){170, 180, 200, 255};
    if (!m->alive) body = (Color){120, 50, 50, 255};
    Color edge = (Color){ 30,  40,  60, 255};

    /* P05 — invisibility powerup: alpha-mod the entire body. The local
     * mech keeps a stronger alpha (0.5) so the player can still play —
     * the wholly-faded look (alpha 0.2) is for OTHER players seeing
     * this mech. Skipped for ragdolls (corpses don't go invisible). */
    if (m->alive && m->powerup_invis_remaining > 0.0f) {
        uint8_t a = is_local ? (uint8_t)128 : (uint8_t)51;
        body.a = a;
        edge.a = a;
    }

    /* P12 — hit-flash white-additive blend over the body tint. Capsule
     * fallback applies it the same way the sprite path does so a fresh
     * checkout (no chassis atlases) still gets visible feedback on
     * every hit. */
    body = apply_hit_flash(body, m->hit_flash_timer);

    /* Back leg first, then front, head last — see [06-rendering-audio.md]. */
    Color leg_back  = (Color){ body.r - 20, body.g - 20, body.b - 20, 255 };
    Color leg_front = body;
    int back_leg_hip   = m->facing_left ? PART_R_HIP   : PART_L_HIP;
    int back_leg_knee  = m->facing_left ? PART_R_KNEE  : PART_L_KNEE;
    int back_leg_foot  = m->facing_left ? PART_R_FOOT  : PART_L_FOOT;
    int front_leg_hip  = m->facing_left ? PART_L_HIP   : PART_R_HIP;
    int front_leg_knee = m->facing_left ? PART_L_KNEE  : PART_R_KNEE;
    int front_leg_foot = m->facing_left ? PART_L_FOOT  : PART_R_FOOT;

    draw_bone_chk(p, cp, b + back_leg_hip,  b + back_leg_knee, 6.0f, leg_back, alpha, visual_offset);
    draw_bone_chk(p, cp, b + back_leg_knee, b + back_leg_foot, 5.5f, leg_back, alpha, visual_offset);

    /* Torso — chest to pelvis as a thick beam plus shoulder span. */
    draw_bone_chk(p, cp, idx_chest, idx_pelvis, 13.0f, body, alpha, visual_offset);
    {
        Vec2 pelv = particle_render_pos(p, idx_pelvis, alpha);
        DrawCircleV((Vector2){ pelv.x + visual_offset.x, pelv.y + visual_offset.y }, 6.0f, body);
    }
    /* Hip plate. */
    draw_bone_chk(p, cp, b + PART_L_HIP, b + PART_R_HIP, 10.0f, body, alpha, visual_offset);
    /* Shoulder plate. */
    draw_bone_chk(p, cp, b + PART_L_SHOULDER, b + PART_R_SHOULDER, 10.0f, body, alpha, visual_offset);

    /* Front leg. */
    draw_bone_chk(p, cp, b + front_leg_hip,  b + front_leg_knee, 6.5f, leg_front, alpha, visual_offset);
    draw_bone_chk(p, cp, b + front_leg_knee, b + front_leg_foot, 6.0f, leg_front, alpha, visual_offset);

    /* Back arm (the off-hand). */
    int back_sho = m->facing_left ? PART_R_SHOULDER : PART_L_SHOULDER;
    int back_elb = m->facing_left ? PART_R_ELBOW    : PART_L_ELBOW;
    int back_hnd = m->facing_left ? PART_R_HAND     : PART_L_HAND;
    int frnt_sho = m->facing_left ? PART_L_SHOULDER : PART_R_SHOULDER;
    int frnt_elb = m->facing_left ? PART_L_ELBOW    : PART_R_ELBOW;
    int frnt_hnd = m->facing_left ? PART_L_HAND     : PART_R_HAND;

    /* Arm bones are gated by their distance constraints — when a limb
     * is dismembered, both the upper-arm and forearm distance
     * constraints get deactivated and the bones disappear. The arm
     * particles keep integrating; they're just no longer drawn. */
    Color arm_back = (Color){body.r - 30, body.g - 30, body.b - 30, 255};
    draw_bone_chk(p, cp, b + back_sho, b + back_elb, 5.0f, arm_back, alpha, visual_offset);
    if (bone_constraint_active(cp, b + back_elb, b + back_hnd)) {
        Vec2 a = particle_render_pos(p, b + back_elb, alpha);
        Vec2 c = particle_render_pos(p, b + back_hnd, alpha);
        a.x += visual_offset.x; a.y += visual_offset.y;
        c.x += visual_offset.x; c.y += visual_offset.y;
        draw_bone_clamped(L, a, c, 4.5f, arm_back);
    }

    /* Head + neck. */
    draw_bone_chk(p, cp, idx_chest, idx_neck, 7.0f, body, alpha, visual_offset);
    {
        Vec2 head = particle_render_pos(p, idx_head, alpha);
        head.x += visual_offset.x; head.y += visual_offset.y;
        DrawCircleV((Vector2){ head.x, head.y }, 9.0f, body);
        DrawCircleLines((int)head.x, (int)head.y, 9.0f, edge);
    }

    /* Front (rifle) arm — forearm and rifle clamped against the level
     * because aim points often lie behind a thin solid. */
    Color arm_front = body;
    draw_bone_chk(p, cp, b + frnt_sho, b + frnt_elb, 5.5f, arm_front, alpha, visual_offset);
    if (bone_constraint_active(cp, b + frnt_elb, b + frnt_hnd)) {
        Vec2 a = particle_render_pos(p, b + frnt_elb, alpha);
        Vec2 c = particle_render_pos(p, b + frnt_hnd, alpha);
        a.x += visual_offset.x; a.y += visual_offset.y;
        c.x += visual_offset.x; c.y += visual_offset.y;
        draw_bone_clamped(L, a, c, 5.0f, arm_front);
    }

    /* P12 — damage decal placeholder dots, even in the capsule fallback. */
    draw_damage_decals(p, m, alpha, visual_offset, body.a);
}

/* M5 P10 — sprite-driven mech rendering. Each chassis's atlas is
 * indexed by `MechSpriteId`; the renderer walks the canonical
 * `g_render_parts` z-order table and emits one `DrawTexturePro` per
 * entry. When facing left we swap L↔R on each entry so the back-side
 * limbs (rendered first, behind the body) become the right side and
 * the front-side limbs (rendered after the body, on top) become the
 * left side. The art is authored vertically — top of source = parent
 * end, bottom = child end, which is why we subtract 90° from the bone
 * angle below.
 *
 * Single-particle anchors (entries with `part_a == -1`) draw at the
 * `part_b` particle with `angle = 0` — used for plates and hands/feet
 * that aren't bone segments. The constraint check is skipped for them
 * because the relevant dismemberment-driven inactive constraints are
 * the limb-to-body links, not links involving the plate alone.
 *
 * Body tint matches the capsule-path colors (alive, dummy-orange,
 * dead-red wash, invis alpha-mod for the local vs remote view) so a
 * mid-development build that runs Trooper as sprites and other chassis
 * as capsules reads visually consistent. */
typedef struct MechRenderPart {
    int8_t   part_a;        /* PART_* parent end, or -1 for single-particle anchor */
    int8_t   part_b;        /* PART_* child  end (the anchor when part_a == -1) */
    uint8_t  sprite_idx;    /* MechSpriteId */
} MechRenderPart;

/* Z-order: back-side limbs first (drawn behind the body when facing
 * right; the renderer swaps L↔R when facing left). Centerline pieces
 * in the middle. Front-side limbs last (drawn on top of the body).
 * Held-weapon art lives in `draw_held_weapon` (P11) and is plumbed at
 * the top level (renderer_draw_frame) so it has access to the World
 * struct for last-fired-slot / muzzle-flash tracking. */
static const MechRenderPart g_render_parts[] = {
    /* Back leg (L when facing right) */
    { PART_L_HIP,      PART_L_KNEE,     MSP_LEG_UPPER_L },
    { PART_L_KNEE,     PART_L_FOOT,     MSP_LEG_LOWER_L },
    { -1,              PART_L_FOOT,     MSP_FOOT_L      },
    /* Back arm (L when facing right) */
    { PART_L_SHOULDER, PART_L_ELBOW,    MSP_ARM_UPPER_L },
    { PART_L_ELBOW,    PART_L_HAND,     MSP_ARM_LOWER_L },
    { -1,              PART_L_HAND,     MSP_HAND_L      },
    /* Centerline body */
    { PART_CHEST,      PART_PELVIS,     MSP_TORSO       },
    { -1,              PART_PELVIS,     MSP_HIP_PLATE   },
    { -1,              PART_L_SHOULDER, MSP_SHOULDER_L  },
    { -1,              PART_R_SHOULDER, MSP_SHOULDER_R  },
    { PART_NECK,       PART_HEAD,       MSP_HEAD        },
    /* Front leg (R when facing right) */
    { PART_R_HIP,      PART_R_KNEE,     MSP_LEG_UPPER_R },
    { PART_R_KNEE,     PART_R_FOOT,     MSP_LEG_LOWER_R },
    { -1,              PART_R_FOOT,     MSP_FOOT_R      },
    /* Front arm (R when facing right) */
    { PART_R_SHOULDER, PART_R_ELBOW,    MSP_ARM_UPPER_R },
    { PART_R_ELBOW,    PART_R_HAND,     MSP_ARM_LOWER_R },
    { -1,              PART_R_HAND,     MSP_HAND_R      },
};
#define MECH_RENDER_PART_COUNT 17
_Static_assert((sizeof g_render_parts / sizeof g_render_parts[0]) ==
                   MECH_RENDER_PART_COUNT,
               "g_render_parts must have MECH_RENDER_PART_COUNT entries");

/* L↔R part-index swap for the facing-left case. PART_L_SHOULDER..
 * PART_L_HAND and PART_R_SHOULDER..PART_R_HAND are 3 apart in the
 * enum, same for PART_L_HIP..PART_L_FOOT vs PART_R_HIP..PART_R_FOOT. */
static int swap_part_lr(int part) {
    if (part >= PART_L_SHOULDER && part <= PART_L_HAND) return part + 3;
    if (part >= PART_R_SHOULDER && part <= PART_R_HAND) return part - 3;
    if (part >= PART_L_HIP      && part <= PART_L_FOOT) return part + 3;
    if (part >= PART_R_HIP      && part <= PART_R_FOOT) return part - 3;
    return part;
}

/* MSP_*_L ↔ MSP_*_R swap. The L/R pair indices aren't a simple
 * arithmetic offset (the enum starts at MSP_TORSO=0 and the L/R pairs
 * begin at MSP_SHOULDER_L=3), so an explicit switch is the clearest. */
static int swap_sprite_lr(int s) {
    switch (s) {
        case MSP_SHOULDER_L:       return MSP_SHOULDER_R;
        case MSP_SHOULDER_R:       return MSP_SHOULDER_L;
        case MSP_ARM_UPPER_L:      return MSP_ARM_UPPER_R;
        case MSP_ARM_UPPER_R:      return MSP_ARM_UPPER_L;
        case MSP_ARM_LOWER_L:      return MSP_ARM_LOWER_R;
        case MSP_ARM_LOWER_R:      return MSP_ARM_LOWER_L;
        case MSP_HAND_L:           return MSP_HAND_R;
        case MSP_HAND_R:           return MSP_HAND_L;
        case MSP_LEG_UPPER_L:      return MSP_LEG_UPPER_R;
        case MSP_LEG_UPPER_R:      return MSP_LEG_UPPER_L;
        case MSP_LEG_LOWER_L:      return MSP_LEG_LOWER_R;
        case MSP_LEG_LOWER_R:      return MSP_LEG_LOWER_L;
        case MSP_FOOT_L:           return MSP_FOOT_R;
        case MSP_FOOT_R:           return MSP_FOOT_L;
        case MSP_STUMP_SHOULDER_L: return MSP_STUMP_SHOULDER_R;
        case MSP_STUMP_SHOULDER_R: return MSP_STUMP_SHOULDER_L;
        case MSP_STUMP_HIP_L:      return MSP_STUMP_HIP_R;
        case MSP_STUMP_HIP_R:      return MSP_STUMP_HIP_L;
        default: return s;
    }
}

/* P12 — Stump caps over the parent particle of each dismembered limb.
 * Skips silently when the chassis atlas isn't loaded (capsule-fallback
 * path doesn't carry stump-cap art per the spec — the heavy initial
 * blood spray + pinned emitter sell the dismemberment without it).
 * The cap orientation reads off the body's torso (CHEST→PELVIS) so the
 * stump rotates with the body's tilt rather than the absent limb. */
static void draw_stump_caps(const ParticlePool *p, const Mech *m,
                            const MechSpriteSet *set,
                            float alpha, Vec2 visual_offset, Color tint)
{
    if (set->atlas.id == 0) return;
    if (!m->dismember_mask) return;
    int b = m->particle_base;
    Vec2 chest  = particle_render_pos(p, b + PART_CHEST,  alpha);
    Vec2 pelvis = particle_render_pos(p, b + PART_PELVIS, alpha);
    float body_angle =
        atan2f(pelvis.y - chest.y, pelvis.x - chest.x) * RAD2DEG - 90.0f;

    static const struct { uint8_t bit; int part; int sprite_id; } caps[] = {
        { LIMB_HEAD,  PART_NECK,       MSP_STUMP_NECK       },
        { LIMB_L_ARM, PART_L_SHOULDER, MSP_STUMP_SHOULDER_L },
        { LIMB_R_ARM, PART_R_SHOULDER, MSP_STUMP_SHOULDER_R },
        { LIMB_L_LEG, PART_L_HIP,      MSP_STUMP_HIP_L      },
        { LIMB_R_LEG, PART_R_HIP,      MSP_STUMP_HIP_R      },
    };
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; ++i) {
        if (!(m->dismember_mask & caps[i].bit)) continue;
        int sprite_id = caps[i].sprite_id;
        int part      = caps[i].part;
        if (m->facing_left) {
            sprite_id = swap_sprite_lr(sprite_id);
            part      = swap_part_lr(part);
        }
        const MechSpritePart *cap = &set->parts[sprite_id];
        Vec2 pos = particle_render_pos(p, b + part, alpha);
        pos.x += visual_offset.x;
        pos.y += visual_offset.y;
        DrawTexturePro(set->atlas, cap->src,
                       (Rectangle){ pos.x, pos.y, cap->draw_w, cap->draw_h },
                       cap->pivot, body_angle, tint);
    }
}

/* `static` dropped at M6 lobby-loadout-preview so `render_draw_mech_preview`
 * can reuse the sprite path verbatim without touching draw_mech (which
 * needs a Level for the capsule fallback's ray-clamp). */
void draw_mech_sprites(const ParticlePool *p, const ConstraintPool *cp,
                              const Mech *m, bool is_local,
                              float alpha, Vec2 visual_offset) {
    const MechSpriteSet *set = &g_chassis_sprites[m->chassis_id];
    int b = m->particle_base;

    /* Tint mirrors the capsule path so a mid-development build that
     * mixes sprite + capsule chassis reads consistent. */
    Color tint;
    if (m->is_dummy)         tint = (Color){200, 130,  40, 255};
    else if (!m->alive)      tint = (Color){200,  90,  90, 255};
    else                     tint = (Color){255, 255, 255, 255};
    if (m->alive && m->powerup_invis_remaining > 0.0f) {
        tint.a = is_local ? (uint8_t)128 : (uint8_t)51;
    }
    /* P12 — hit-flash white-additive blend. Same blend in both render
     * paths so the visible feedback is consistent across atlases vs
     * capsules. */
    tint = apply_hit_flash(tint, m->hit_flash_timer);

    for (int i = 0; i < MECH_RENDER_PART_COUNT; ++i) {
        const MechRenderPart *rp = &g_render_parts[i];
        int part_a     = rp->part_a;
        int part_b     = rp->part_b;
        int sprite_idx = rp->sprite_idx;
        if (m->facing_left) {
            if (part_a >= 0) part_a = swap_part_lr(part_a);
            part_b     = swap_part_lr(part_b);
            sprite_idx = swap_sprite_lr(sprite_idx);
        }

        /* Skip dismembered limbs at the bone-segment entries; single-
         * particle anchors (plates, hands, feet drawn at one particle)
         * always draw — the dismemberment constraint is between the
         * limb and the body, not anchored at the plate alone. */
        if (part_a >= 0 &&
            !bone_constraint_active(cp, b + part_a, b + part_b)) continue;

        Vec2 pos_b = particle_render_pos(p, b + part_b, alpha);
        float draw_x, draw_y, angle;
        float bone_len = 0.0f;
        if (part_a >= 0) {
            Vec2 pos_a = particle_render_pos(p, b + part_a, alpha);
            draw_x = (pos_a.x + pos_b.x) * 0.5f + visual_offset.x;
            draw_y = (pos_a.y + pos_b.y) * 0.5f + visual_offset.y;
            /* Sprites are authored vertically (parent end at top of
             * source). atan2 gives the bone angle CCW from +x; raylib
             * rotates CW in screen-space (+y down). Subtract 90° so
             * angle=0 corresponds to a vertical bone (parent above
             * child) which is the source authoring orientation. */
            float dx = pos_b.x - pos_a.x;
            float dy = pos_b.y - pos_a.y;
            angle = atan2f(dy, dx) * RAD2DEG - 90.0f;
            bone_len = sqrtf(dx * dx + dy * dy);
        } else {
            draw_x = pos_b.x + visual_offset.x;
            draw_y = pos_b.y + visual_offset.y;
            angle = 0.0f;
        }

        const MechSpritePart *sp = &set->parts[sprite_idx];
        /* For 2-particle bones, scale the sprite's height to match the
         * actual bone span (plus a small overlap allowance for the
         * parent-side wraparound). The s_default_parts draw_h values
         * were authored 2-5x larger than the per-chassis bone lengths
         * (e.g. leg_lower draw_h=88 vs trooper bone_shin=18); rendering
         * those raw extends the sprite past the bone endpoints, so a
         * dense AI-filled tile shows the limb clipping through the
         * floor. The overlap factor (1.20) keeps the parent-side
         * exposed-end zone visible behind the parent plate's z-order
         * while the bottom edge stays at the child particle.
         * Single-particle sprites (plates, hands, feet, stumps) keep
         * their authored draw_h since they anchor at one point. */
        float dst_w = sp->draw_w;
        float dst_h = sp->draw_h;
        Vector2 pivot = sp->pivot;
        if (part_a >= 0 && bone_len > 0.0f) {
            /* Scale the sprite so its rendered height equals the actual
             * bone span. This makes the sprite endpoints align with the
             * particle endpoints — no overhang past the parent or child
             * particle. The previous 1.20x overlap factor produced
             * visible foot-into-floor bleed when the AI atlas filled
             * the bottom of the leg_lower tile with content. */
            float scale = bone_len / sp->draw_h;
            dst_w = sp->draw_w * scale;
            dst_h = sp->draw_h * scale;
            pivot.x *= scale;
            pivot.y *= scale;
        }
        DrawTexturePro(set->atlas, sp->src,
                       (Rectangle){ draw_x, draw_y, dst_w, dst_h },
                       pivot, angle, tint);
    }

    /* P12 — Stump caps go on top of the limb sprites so they cover the
     * exposed-end pixels at the joint. Skipped when no atlas. */
    draw_stump_caps(p, m, set, alpha, visual_offset, tint);

    /* P12 — Damage decals composited on top of every limb. The decal's
     * sprite-local coords were captured at hit time relative to the
     * sprite midpoint; `part_local_to_world` re-rotates them with the
     * current bone angle so they migrate naturally with the body. */
    draw_damage_decals(p, m, alpha, visual_offset, tint.a);
}

/* M5 P10 dispatcher. Picks the sprite path when the chassis's atlas
 * is loaded; falls back to the M4 capsule path otherwise. Held-weapon
 * art is drawn separately from `renderer_draw_frame` (P11) — it needs
 * world-level state (tick + last-fired-slot for the RMB flicker, plus
 * the muzzle-flash window) that `draw_mech` doesn't see. */
static void draw_mech(const ParticlePool *p, const ConstraintPool *cp,
                      const Mech *m, const Level *L, bool is_local,
                      float alpha, Vec2 visual_offset) {
    const MechSpriteSet *set = &g_chassis_sprites[m->chassis_id];
    if (set->atlas.id != 0) {
        draw_mech_sprites(p, cp, m, is_local, alpha, visual_offset);
    } else {
        draw_mech_capsules(p, cp, m, L, is_local, alpha, visual_offset);
    }
    (void)L;     /* used inside draw_mech_capsules; silenced when sprite path takes over */
}

/* P06 — Grapple rope. Single straight line from the firer's right hand
 * to the in-flight head (FLYING) or the anchor (ATTACHED). Drawn after
 * the mech body so the rope sits over the arm. visual_offset matches
 * the mech's reconcile-smoothing offset for the local mech.
 *
 * Called from renderer_draw_frame because it needs both ProjectilePool
 * (for FLYING head lookup) and ParticlePool (for bone-anchor
 * tracking) — draw_mech only has access to particles. */
static void draw_grapple_rope(const World *w, int mid, float alpha,
                              Vec2 visual_offset)
{
    const Mech *m = &w->mechs[mid];
    if (m->grapple.state == GRAPPLE_IDLE) return;
    int b = m->particle_base;
    Vec2 hand = particle_render_pos(&w->particles, b + PART_R_HAND, alpha);
    hand.x += visual_offset.x;
    hand.y += visual_offset.y;

    if (m->grapple.state == GRAPPLE_FLYING) {
        int head_idx = projectile_find_grapple_head(&w->projectiles, mid);
        if (head_idx < 0) return;
        const ProjectilePool *pp = &w->projectiles;
        Vec2 head_pos = {
            pp->render_prev_x[head_idx] + (pp->pos_x[head_idx] - pp->render_prev_x[head_idx]) * alpha,
            pp->render_prev_y[head_idx] + (pp->pos_y[head_idx] - pp->render_prev_y[head_idx]) * alpha,
        };
        DrawLineEx((Vector2){ hand.x, hand.y },
                   (Vector2){ head_pos.x, head_pos.y },
                   1.5f, (Color){200, 200, 80, 220});
        return;
    }

    /* GRAPPLE_ATTACHED — anchor is either a tile (anchor_pos) or a
     * bone particle on a target mech (read live so the rope tracks
     * a moving target). */
    Vec2 anchor;
    if (m->grapple.anchor_mech < 0) {
        anchor = m->grapple.anchor_pos;
    } else {
        const Mech *t = &w->mechs[m->grapple.anchor_mech];
        int part_idx = t->particle_base + m->grapple.anchor_part;
        anchor = particle_render_pos(&w->particles, part_idx, alpha);
    }
    DrawLineEx((Vector2){ hand.x, hand.y },
               (Vector2){ anchor.x, anchor.y },
               1.5f, (Color){240, 220, 100, 255});
}

/* P07 — CTF flag. Vertical staff (line) + triangular pennant. Position
 * is HOME / DROPPED / CARRIED-mech-chest depending on status. The
 * pennant gets a small sin offset on its tip while carried so it
 * "waves" — a cheap cue that this flag is currently in motion. The
 * proper art (sprite + ribbon shader) lands later; this is a
 * P02-style placeholder that reads cleanly in shot tests. */
static void draw_flag(const World *w, int f, float alpha) {
    if (f < 0 || f >= w->flag_count) return;
    const Flag *fl = &w->flags[f];
    Color tc = (fl->team == MATCH_TEAM_RED)
              ? (Color){220,  80,  80, 255}
              : (Color){ 80, 140, 220, 255};
    Color staff_c = (Color){ 80,  60,  40, 255};

    /* Resolve render position. CARRIED uses the carrier's interp-lerped
     * chest so the flag tracks the body smoothly; HOME/DROPPED pull
     * from the per-flag fields. */
    Vec2 base;
    bool wave = false;
    if (fl->status == FLAG_CARRIED &&
        fl->carrier_mech >= 0 && fl->carrier_mech < w->mech_count) {
        const Mech *cm = &w->mechs[fl->carrier_mech];
        Vec2 chest = particle_render_pos(&w->particles,
                                         cm->particle_base + PART_CHEST, alpha);
        /* Carry behind the body — slightly offset to the back side so
         * the body silhouette stays clean, raised to chest level. */
        base.x = chest.x + (cm->facing_left ?  10.0f : -10.0f);
        base.y = chest.y - 6.0f;
        wave = true;
    } else if (fl->status == FLAG_DROPPED) {
        base = fl->dropped_pos;
    } else {
        base = fl->home_pos;
    }

    /* Staff: 28-px line going up. */
    Vector2 staff_bot = (Vector2){ base.x, base.y };
    Vector2 staff_top = (Vector2){ base.x, base.y - 28.0f };
    DrawLineEx(staff_bot, staff_top, 2.0f, staff_c);

    /* Pennant: triangle from staff_top, fanning to the right (or left
     * for facing_left carrier). Tip wobbles when carried. */
    float wobble = wave ? sinf((float)w->tick * 0.3f) * 2.0f : 0.0f;
    bool flip = (fl->status == FLAG_CARRIED &&
                 fl->carrier_mech >= 0 && fl->carrier_mech < w->mech_count &&
                 w->mechs[fl->carrier_mech].facing_left);
    float dx = flip ? -16.0f : 16.0f;
    Vector2 v0 = staff_top;
    Vector2 v1 = (Vector2){ staff_top.x + dx, staff_top.y + 6.0f + wobble };
    Vector2 v2 = (Vector2){ staff_top.x,      staff_top.y + 12.0f };
    /* DrawTriangle is CCW only — choose the winding by `flip`. */
    if (flip) DrawTriangle(v0, v1, v2, tc);
    else      DrawTriangle(v0, v2, v1, tc);

    /* DROPPED highlight: a small base circle so the dropped flag reads
     * differently from a parked HOME flag. (HOME has no halo; DROPPED
     * gets one to emphasize urgency.) */
    if (fl->status == FLAG_DROPPED) {
        DrawCircleLines((int)base.x, (int)base.y, 14.0f,
                        (Color){tc.r, tc.g, tc.b, 160});
    }
}

static void draw_flags(const World *w, float alpha) {
    for (int f = 0; f < w->flag_count; ++f) draw_flag(w, f, alpha);
}

/* P05 — placeholder pickup sprite. Bobs at 0.5 Hz / ±4 px for
 * available pickups; cooldown entries draw nothing. PRACTICE_DUMMY
 * entries aren't drawn either (the dummy is a real mech, not a pickup).
 *
 * PICKUP_WEAPON renders the actual atlas sprite (variant = WeaponId) so
 * players can identify the weapon at a glance; other kinds keep the
 * colored-circle placeholder until per-kind art lands. Atlas-missing or
 * out-of-range variants fall back to the placeholder. */
static void draw_pickups(const PickupPool *pool, double now_s) {
    for (int i = 0; i < pool->count; ++i) {
        const PickupSpawner *s = &pool->items[i];
        if (s->state != PICKUP_STATE_AVAILABLE) continue;
        if (s->kind == PICKUP_PRACTICE_DUMMY)   continue;
        float bob = 4.0f * sinf((float)now_s * 3.14159f);
        float cx = s->pos.x;
        float cy = s->pos.y + bob;

        if (s->kind == PICKUP_WEAPON && g_weapons_atlas.id != 0) {
            const WeaponSpriteDef *wp = weapon_sprite_def((int)s->variant);
            if (wp) {
                /* Center the sprite on the pickup position, drawn flat
                 * (no aim rotation — the weapon is lying on the ground,
                 * not being held). Origin is sprite-center so bob lifts
                 * the silhouette uniformly. */
                Rectangle dst = { cx, cy, wp->draw_w, wp->draw_h };
                Vector2   org = { wp->draw_w * 0.5f, wp->draw_h * 0.5f };
                DrawTexturePro(g_weapons_atlas, wp->src, dst, org, 0.0f,
                               (Color){255, 255, 255, 255});
                continue;
            }
        }

        uint32_t rgba = pickup_kind_color(s->kind);
        Color c = (Color){
            (uint8_t)(rgba & 0xff),
            (uint8_t)((rgba >> 8) & 0xff),
            (uint8_t)((rgba >> 16) & 0xff),
            (uint8_t)((rgba >> 24) & 0xff),
        };
        DrawCircle((int)cx, (int)cy, 12.0f, c);
        DrawCircleLines((int)cx, (int)cy, 12.0f, (Color){20, 20, 20, 255});
    }
}

/* ---- Frame --------------------------------------------------------- */

/* World pass — every world-space draw call lives here so both the
 * post-process and the no-shader fallback emit the same scene. P13
 * threads sw/sh so the screen-space parallax + foreground silhouettes
 * see the current backbuffer size. */
static void draw_world_pass(Renderer *r, World *w, float alpha,
                            Vec2 local_visual_offset, int sw, int sh)
{
    /* P13 — Parallax FAR + MID + NEAR all draw in screen space before the
     * world. No BeginMode2D — the parallax draw computes its own scroll
     * from r->camera.target and the parallax ratio. Each layer no-ops
     * if its texture isn't loaded. M6 P10 (2026-05-16): NEAR moved from
     * post-world to pre-world. The shipped foundry parallax_near.png is
     * a dense opaque factory silhouette, not edge art — drawing it
     * after the world obscured the playable level geometry. Treat all
     * three layers as background; level tiles + polys win over parallax. */
    draw_parallax_far_mid(r->camera, sw, sh);
    draw_parallax_near(r->camera, sw, sh);

    BeginMode2D(r->camera);
        /* P13 — Decoration layers 0 + 1 sit BEHIND the tile sprites
         * (far + mid parallax-band silhouettes anchored in world space).
         * No-op when the level has no LvlDeco records. */
        draw_decorations(&w->level, 0);
        draw_decorations(&w->level, 1);
        draw_level_tiles(&w->level);
        draw_polys(&w->level);
        /* M6 P09 — Snow pile on top of solid tiles. Reads
         * g_atmosphere.snow_accum (advanced deterministically on
         * every peer by atmosphere_advance_accumulators). Self-skips
         * when accum < 0.05 so it costs nothing on no-snow maps. */
        atmosphere_draw_snow_pile(&w->level);
        /* M6 P09 — Ambient zone visuals (WIND streaks via FxPool;
         * ZERO_G + ACID tints painted here; FOG drawn as a soft volume
         * with the shader handling the per-fragment haze). Drawn before
         * decals so blood splats sit on top, and before mechs so the
         * volume reads as a backdrop. */
        atmosphere_draw_ambient_zones(&w->level, GetTime());
        /* P13 — Decoration layer 2 sits BETWEEN tiles and mechs (near
         * silhouettes that read in front of geometry but behind action). */
        draw_decorations(&w->level, 2);
        decal_draw_layer();
        /* M6 P02 — jetpack plume sprites. Drawn between the decal
         * pass and the mech loop so the mech body silhouettes
         * against the plume. */
        mech_jet_fx_draw_plumes(w, alpha);
        draw_pickups(&w->pickups, GetTime());
        for (int i = 0; i < w->mech_count; ++i) {
            /* Reconcile-smoothing offset only applies to the local
             * mech (the one whose state we predict + replay). For
             * everyone else pass {0,0} — remote mechs get smoothed
             * via the snapshot interp buffer instead (snapshot.c). */
            Vec2 off = (i == w->local_mech_id)
                     ? local_visual_offset : (Vec2){0, 0};
            bool is_local = (i == w->local_mech_id);
            draw_mech(&w->particles, &w->constraints,
                      &w->mechs[i], &w->level, is_local, alpha, off);
            /* P11 — held weapon sits on top of the front-arm
             * silhouette. Drawn after the body so the grip
             * pivot at R_HAND covers the wrist seam. */
            draw_held_weapon(w, i, alpha, off);
            /* P06 — draw the rope after the body+weapon so it
             * sits on top of the arm and launcher. */
            draw_grapple_rope(w, i, alpha, off);
        }
        projectile_draw(&w->projectiles, alpha);
        fx_draw(&w->fx, alpha);
        /* P13 — BACKGROUND polygons sit on top of mechs as alpha-blended
         * foreground silhouettes (window grilles, distant railings).
         * Per `documents/m5/08-rendering.md` §"Free polygons". */
        draw_polys_background(&w->level);
        /* P13 — Decoration layer 3 sits on top of mechs + projectiles +
         * fx + bg polys (foreground occluders). BLEND_ALPHA is the
         * default; ADDITIVE flag flips it per-deco inside draw_decorations. */
        draw_decorations(&w->level, 3);
        /* P07 — CTF flags. Drawn after mechs so a carried flag sits
         * in front of the body silhouette; before HUD so it stays
         * inside the world camera transform. */
        draw_flags(w, alpha);
    EndMode2D();
}

void renderer_draw_frame(Renderer *r, World *w,
                         int internal_w, int internal_h,
                         int window_w,   int window_h,
                         float alpha, Vec2 local_visual_offset,
                         Vec2 cursor_screen,
                         RendererOverlayFn overlay_cb, void *overlay_user) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    if (internal_w <= 0 || internal_h <= 0) {
        /* Defensive — should never happen since platform_begin_frame
         * never produces zeros. Skip the frame rather than divide-by-
         * zero in the letterbox math. */
        BeginDrawing();
            ClearBackground(BLACK);
        EndDrawing();
        return;
    }
    if (window_w <= 0) window_w = internal_w;
    if (window_h <= 0) window_h = internal_h;

    /* ---- Aspect-preserving letterbox math ------------------------- *
     * Largest uniform scale that fits the internal RT inside the
     * window. `dx/dy` centre the upscaled RT — non-zero only when the
     * internal aspect doesn't match the window aspect (which our
     * platform_begin_frame avoids by design, but a future config
     * override might trigger it). */
    float sx = (float)window_w / (float)internal_w;
    float sy = (float)window_h / (float)internal_h;
    float scale = (sx < sy) ? sx : sy;
    float dw = (float)internal_w * scale;
    float dh = (float)internal_h * scale;
    float dx = ((float)window_w - dw) * 0.5f;
    float dy = ((float)window_h - dh) * 0.5f;
    r->blit_scale = scale;
    r->blit_dx    = dx;
    r->blit_dy    = dy;

    float cam_dt = (r->cam_dt_override > 0.0f)
                   ? r->cam_dt_override
                   : (GetFrameTime() > 0 ? GetFrameTime() : 1.0f / 60.0f);
    update_camera(r, w, internal_w, internal_h, cam_dt);

    /* Cache cursor positions for HUD + camera lookahead. The conversion
     * goes window → internal (via blit_* set above) → world (via the
     * camera in internal coords). `last_cursor_screen` is stored in
     * internal coords because update_camera reads `last_cursor_world`
     * only; we keep the field name for ABI continuity. */
    Vec2 cursor_internal = {
        (cursor_screen.x - dx) / (scale > 0.0f ? scale : 1.0f),
        (cursor_screen.y - dy) / (scale > 0.0f ? scale : 1.0f),
    };
    r->last_cursor_screen = cursor_internal;
    r->last_cursor_world  = renderer_screen_to_world(r, cursor_screen);

    /* Bake any blood drops the latest tick added before we begin the
     * world draw — raylib disallows nesting BeginTextureMode inside
     * BeginMode2D, and `decal_flush_pending` is itself a BeginTextureMode
     * pair on the splat layer. Run it before any BeginTextureMode /
     * BeginDrawing pair we open below. */
    profile_zone_begin(PROF_DECAL_FLUSH);
    decal_flush_pending();
    profile_zone_end(PROF_DECAL_FLUSH);

    bool have_internal = ensure_internal_targets(internal_w, internal_h);
    if (!have_internal) {
        /* Worst-case fallback: no internal RT at all. Draw straight
         * to the backbuffer at window res. Matches the pre-P03 no-
         * shader shape. */
        BeginDrawing();
            ClearBackground((Color){12, 14, 18, 255});
            update_camera(r, w, window_w, window_h, 0.0f);
            draw_world_pass(r, w, alpha, local_visual_offset,
                            window_w, window_h);
            hud_draw(w, window_w, window_h, cursor_screen, r->camera);
            if (overlay_cb) overlay_cb(overlay_user, window_w, window_h);
            audio_draw_mute_overlay(window_w, window_h);
        EndDrawing();
        return;
    }

    /* (1) World pass → g_internal_target at internal resolution.
     * ClearBackground inside BeginTextureMode targets the RT, not the
     * backbuffer — keeps the post-pass blend predictable.
     *
     * M6 P09 — the sky gradient replaces the flat clear. When sky_top
     * == sky_bot the call collapses to a single-color fill, so legacy
     * maps with zero atmosphere data still get the hardcoded
     * {12,14,18} backdrop via the THEME_CONCRETE default. */
    profile_zone_begin(PROF_DRAW_WORLD);
    BeginTextureMode(g_internal_target);
        ClearBackground((Color){12, 14, 18, 255});
        atmosphere_draw_sky(internal_w, internal_h);
        draw_world_pass(r, w, alpha, local_visual_offset,
                        internal_w, internal_h);
    EndTextureMode();
    profile_zone_end(PROF_DRAW_WORLD);

    /* (2) Halftone pass (when the shader is loaded):
     *     g_internal_target → g_post_target at internal resolution.
     *
     * The shader sees `resolution = internal_w/h` — its dither cell
     * and shimmer frequencies stay tuned for 1080p-class densities
     * regardless of the player's monitor. Per-fragment work drops
     * from `window_w * window_h` invocations to `internal_w *
     * internal_h` — at 3440×1440 maximised that's a 44 % reduction
     * just for this pass. */
    bool drew_post = false;
    if (g_halftone_loaded && g_post_target.id != 0) {
        profile_zone_begin(PROF_DRAW_POST);
        Vector2 res = { (float)internal_w, (float)internal_h };
        float   den = HALFTONE_DENSITY;
        if (g_halftone_loc_resolution >= 0)
            SetShaderValue(g_halftone_post, g_halftone_loc_resolution,
                           &res, SHADER_UNIFORM_VEC2);
        if (g_halftone_loc_density >= 0)
            SetShaderValue(g_halftone_post, g_halftone_loc_density,
                           &den, SHADER_UNIFORM_FLOAT);

        /* M6 P02 — Heat-shimmer hot zones. Gated on
         * mech_jet_fx_any_active so the uniform set + the shader's
         * loop only fire when at least one plume is on screen.
         *
         * NOTE: hot-zone xy come from GetWorldToScreen2D against
         * r->camera, which lives in internal-pixel space — so the
         * coords are already in the same coord system the shader's
         * frag_px uses (`fragTexCoord * resolution`). No extra
         * scaling needed. */
        if (g_halftone_loc_hot_zones >= 0 && g_halftone_loc_jet_time >= 0) {
            /* Skip shimmer in shot mode for deterministic screenshots. */
            bool any = !g_shot_mode && mech_jet_fx_any_active(w);
            int  zone_count = 0;
            if (any) {
                JetHotZone zones[JET_HOT_ZONE_MAX] = {0};
                zone_count = mech_jet_fx_collect_hot_zones(
                    w, &r->camera, zones, JET_HOT_ZONE_MAX);
                if (zone_count > 0) {
                    SetShaderValueV(g_halftone_post,
                                    g_halftone_loc_hot_zones, zones,
                                    SHADER_UNIFORM_VEC4, zone_count);
                }
                float jt = (float)GetTime();
                SetShaderValue(g_halftone_post, g_halftone_loc_jet_time,
                               &jt, SHADER_UNIFORM_FLOAT);
            }
            if (g_halftone_loc_hot_zone_count >= 0) {
                SetShaderValue(g_halftone_post,
                               g_halftone_loc_hot_zone_count,
                               &zone_count, SHADER_UNIFORM_INT);
            }
        }

        /* M6 P09 — Per-map atmospherics uniforms. All zero-default
         * (theme 0 / no fog / no vignette) so a stock map costs zero
         * extra shader work past the short-circuits in the shader. */
        if (g_halftone_loc_fog_density >= 0) {
            float fd = g_atmosphere.fog_density;
            SetShaderValue(g_halftone_post, g_halftone_loc_fog_density,
                           &fd, SHADER_UNIFORM_FLOAT);
        }
        if (g_halftone_loc_fog_color >= 0) {
            Color fc = g_atmosphere.fog_color;
            float rgb[3] = { fc.r / 255.0f, fc.g / 255.0f, fc.b / 255.0f };
            SetShaderValue(g_halftone_post, g_halftone_loc_fog_color,
                           rgb, SHADER_UNIFORM_VEC3);
        }
        if (g_halftone_loc_vignette >= 0) {
            float vg = g_atmosphere.vignette;
            SetShaderValue(g_halftone_post, g_halftone_loc_vignette,
                           &vg, SHADER_UNIFORM_FLOAT);
        }
        if (g_halftone_loc_atmos_time >= 0) {
            float at = (float)GetTime();
            SetShaderValue(g_halftone_post, g_halftone_loc_atmos_time,
                           &at, SHADER_UNIFORM_FLOAT);
        }
        if (g_halftone_loc_snow_intensity >= 0) {
            float si = g_atmosphere.snow_accum;
            SetShaderValue(g_halftone_post, g_halftone_loc_snow_intensity,
                           &si, SHADER_UNIFORM_FLOAT);
        }
        if (g_halftone_loc_rain_intensity >= 0) {
            float ri = g_atmosphere.rain_wetness;
            SetShaderValue(g_halftone_post, g_halftone_loc_rain_intensity,
                           &ri, SHADER_UNIFORM_FLOAT);
        }
        /* Fog zone array: peer of jet_hot_zones. Each zone is
         * (center_screen_x, center_screen_y, radius_px, density) in
         * the same internal-RT pixel space the shimmer pass uses. */
        if (g_halftone_loc_fog_zones >= 0 && g_halftone_loc_fog_zone_count >= 0) {
            AtmosFogZone fzones[16];
            int fz_n = 0;
            if (!g_shot_mode || true) {   /* fog deterministic — runs in shotmode too */
                fz_n = atmosphere_collect_fog_zones(&w->level, r->camera,
                                                    fzones, 16);
            }
            if (fz_n > 0) {
                SetShaderValueV(g_halftone_post, g_halftone_loc_fog_zones,
                                fzones, SHADER_UNIFORM_VEC4, fz_n);
            }
            SetShaderValue(g_halftone_post, g_halftone_loc_fog_zone_count,
                           &fz_n, SHADER_UNIFORM_INT);
        }

        BeginTextureMode(g_post_target);
            ClearBackground((Color){0, 0, 0, 0});
            BeginShaderMode(g_halftone_post);
                /* raylib's render textures come back Y-flipped — pass
                 * a negative source-rectangle height to flip on read. */
                Rectangle src = {
                    0, 0,
                    (float)g_internal_target.texture.width,
                    -(float)g_internal_target.texture.height
                };
                DrawTextureRec(g_internal_target.texture, src,
                               (Vector2){0, 0}, WHITE);
            EndShaderMode();
        EndTextureMode();
        drew_post = true;
        profile_zone_end(PROF_DRAW_POST);
    }

    /* (3) Backbuffer: bilinear-upscale the chosen source RT to the
     * window, aspect-preserving letterbox. */
    Texture2D src_tex = drew_post
                      ? g_post_target.texture
                      : g_internal_target.texture;
    SetTextureFilter(src_tex, TEXTURE_FILTER_BILINEAR);

    /* HUD/world-to-screen consumer (`draw_flag_compass`) expects the
     * camera to project world coords into WINDOW pixels. Our `r->camera`
     * lives in INTERNAL pixels — derive a window-space camera by
     * pre-multiplying the zoom by the upscale and shifting the offset
     * through the letterbox transform. Identity when internal == window. */
    Camera2D hud_cam = r->camera;
    hud_cam.zoom    *= scale;
    hud_cam.offset.x = r->camera.offset.x * scale + dx;
    hud_cam.offset.y = r->camera.offset.y * scale + dy;

    BeginDrawing();
        profile_zone_begin(PROF_DRAW_BLIT);
        ClearBackground(BLACK);
        Rectangle src = {
            0, 0,
            (float)src_tex.width,
            -(float)src_tex.height
        };
        Rectangle dst = { dx, dy, dw, dh };
        DrawTexturePro(src_tex, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        profile_zone_end(PROF_DRAW_BLIT);

        /* M6 P04 — flying damage-number glyphs. Drawn AFTER the upscale
         * blit so DrawTextPro lands at sharp window pixels (drawing
         * inside the internal RT would bilinear-blur the text on
         * upscale); BEFORE the HUD so a number that spawned over a
         * HUD-occluded part of the world doesn't render on top of the
         * HP bar. Uses r->camera (internal-pixel coord system) +
         * blit_scale/dx/dy to map world → window each frame. The pass
         * is folded into PROF_DRAW_HUD's window for now — typical cost
         * is <50 µs per frame, well below adding a new ProfSection. */
        profile_zone_begin(PROF_DRAW_HUD);
        /* M6 P09 (post-user-feedback) — Weather particles now render
         * in WORLD-space inside fx_draw (called from draw_world_pass).
         * Originally they were screen-space here so they'd land at
         * sharp window pixels, but that meant they never touched the
         * world (flakes never reached the ground / never accumulated
         * the visual pile). The user asked for flakes that "look like
         * they are hitting the ground"; world-space + tile-collide is
         * what makes that read. The pixel-fidelity loss from going
         * through the internal-RT bilinear upscale is invisible at
         * the small flake sizes we use. */
        fx_draw_damage_numbers(&w->fx, r->camera,
                               r->blit_scale, r->blit_dx, r->blit_dy);

        /* HUD draws at WINDOW resolution on top — sharp text and HUD
         * geometry regardless of the internal cap. The cursor_screen
         * passed in is window coords; HUD layout uses window_w/h;
         * world-to-screen inside the HUD uses the synthesised
         * `hud_cam` so off-screen indicators (CTF compass) land at the
         * correct window position. */
        hud_draw(w, window_w, window_h, cursor_screen, hud_cam);
        profile_zone_end(PROF_DRAW_HUD);
        if (overlay_cb) {
            profile_zone_begin(PROF_DRAW_OVERLAY);
            overlay_cb(overlay_user, window_w, window_h);
            profile_zone_end(PROF_DRAW_OVERLAY);
        }
        audio_draw_mute_overlay(window_w, window_h);

        /* M6 P03 — opt-in perf overlay (shotmode `perf_overlay on` or
         * future runtime toggle). Drawn after the HUD so the FPS reads
         * sharp at window pixels and doesn't get blurred by the
         * internal-RT upscale. Off by default → existing shot tests
         * are unaffected. */
        if (g_shot_perf_overlay) {
            int fps = GetFPS();
            const char *line1 = TextFormat("FPS %d", fps);
            const char *line2 = TextFormat("internal %dx%d",
                                           internal_w, internal_h);
            const char *line3 = TextFormat("window   %dx%d",
                                           window_w, window_h);
            Color bg = (Color){0, 0, 0, 160};
            /* Tall rect — fits FPS line + window/internal lines + the
             * 14-zone breakdown column. Width tuned for "draw_overlay
             * NN.NNms (p99 NN.NN)" without truncation at 12px. */
            DrawRectangle(8, 8, 320, 80 + 14 * (int)PROF_COUNT + 6, bg);
            Color fg = (fps >= 55) ? GREEN
                     : (fps >= 30) ? YELLOW
                                   : RED;
            DrawText(line1, 14, 12, 20, fg);
            DrawText(line2, 14, 36, 14, RAYWHITE);
            DrawText(line3, 14, 54, 14, RAYWHITE);

            /* M6 P06 — per-zone breakdown column under the FPS line.
             * 14 zones × one DrawText each ≈ trivial cost; reads sharp
             * at window pixels because the perf overlay path bypasses
             * the internal-RT upscale. Skipped on the no-internal
             * fallback path (which exits early above) — that path is
             * only hit when ensure_internal_targets fails, which
             * shouldn't happen in normal play. */
            int yy = 80;
            char line[96];
            for (int z = 0; z < PROF_COUNT; ++z) {
                snprintf(line, sizeof line, "%-12s %5.2fms (p99 %5.2f)",
                    profile_zone_name((ProfSection)z),
                    (double)profile_zone_ms((ProfSection)z),
                    (double)profile_zone_p99_ms((ProfSection)z));
                Color c = (z == PROF_FRAME) ? fg : RAYWHITE;
                DrawText(line, 14, yy, 12, c);
                yy += 14;
            }
        }
        profile_zone_begin(PROF_PRESENT);
    EndDrawing();
    profile_zone_end(PROF_PRESENT);

    /* M6 P03 — once-per-second SHOT_LOG with the same numbers, gated
     * the same way as the on-screen overlay so this only fires when
     * the bench wants it. Rate-limit by frame count (~60 fr/s at 60
     * Hz; shot mode runs as fast as it can so this rolls faster). */
    if (g_shot_perf_overlay) {
        static int s_perf_log_frames = 0;
        if ((++s_perf_log_frames % 60) == 0) {
            SHOT_LOG("perf: fps=%d internal=%dx%d window=%dx%d scale=%.3f",
                     GetFPS(), internal_w, internal_h, window_w, window_h,
                     (double)scale);
        }
    }
}

/* ---- M6 lobby-loadout-preview --------------------------------------- */
/* Isolated mech render path used by the lobby's loadout preview modal.
 * Caller-supplied ParticlePool holds 16 PART_* slots (typically filled
 * from pose_compute output for the synthetic mech); ConstraintPool may
 * be empty (bone_constraint_active returns true for unknown pairs, so
 * all bones render). No Level is referenced — the held-weapon line
 * fallback uses an unclamped DrawLineEx instead of draw_bone_clamped. */

static void preview_draw_capsules(const ParticlePool *p,
                                  const ConstraintPool *cp,
                                  const Mech *m)
{
    int b = m->particle_base;
    Color body = (Color){170, 180, 200, 255};
    Color edge = (Color){ 30,  40,  60, 255};
    Color leg_back  = (Color){body.r - 20, body.g - 20, body.b - 20, 255};
    Color arm_back  = (Color){body.r - 30, body.g - 30, body.b - 30, 255};
    Vec2 off = (Vec2){0, 0};

    int back_hip   = m->facing_left ? PART_R_HIP   : PART_L_HIP;
    int back_knee  = m->facing_left ? PART_R_KNEE  : PART_L_KNEE;
    int back_foot  = m->facing_left ? PART_R_FOOT  : PART_L_FOOT;
    int front_hip  = m->facing_left ? PART_L_HIP   : PART_R_HIP;
    int front_knee = m->facing_left ? PART_L_KNEE  : PART_R_KNEE;
    int front_foot = m->facing_left ? PART_L_FOOT  : PART_R_FOOT;
    int back_sho   = m->facing_left ? PART_R_SHOULDER : PART_L_SHOULDER;
    int back_elb   = m->facing_left ? PART_R_ELBOW    : PART_L_ELBOW;
    int back_hnd   = m->facing_left ? PART_R_HAND     : PART_L_HAND;
    int frnt_sho   = m->facing_left ? PART_L_SHOULDER : PART_R_SHOULDER;
    int frnt_elb   = m->facing_left ? PART_L_ELBOW    : PART_R_ELBOW;
    int frnt_hnd   = m->facing_left ? PART_L_HAND     : PART_R_HAND;

    draw_bone_chk(p, cp, b + back_hip,  b + back_knee, 6.0f, leg_back, 1.0f, off);
    draw_bone_chk(p, cp, b + back_knee, b + back_foot, 5.5f, leg_back, 1.0f, off);
    draw_bone_chk(p, cp, b + PART_CHEST, b + PART_PELVIS, 13.0f, body, 1.0f, off);
    {
        Vec2 pv = particle_render_pos(p, b + PART_PELVIS, 1.0f);
        DrawCircleV((Vector2){pv.x, pv.y}, 6.0f, body);
    }
    draw_bone_chk(p, cp, b + PART_L_HIP,      b + PART_R_HIP,      10.0f, body, 1.0f, off);
    draw_bone_chk(p, cp, b + PART_L_SHOULDER, b + PART_R_SHOULDER, 10.0f, body, 1.0f, off);
    draw_bone_chk(p, cp, b + front_hip,  b + front_knee, 6.5f, body, 1.0f, off);
    draw_bone_chk(p, cp, b + front_knee, b + front_foot, 6.0f, body, 1.0f, off);
    draw_bone_chk(p, cp, b + back_sho, b + back_elb, 5.0f, arm_back, 1.0f, off);
    draw_bone_chk(p, cp, b + back_elb, b + back_hnd, 4.5f, arm_back, 1.0f, off);
    draw_bone_chk(p, cp, b + PART_CHEST, b + PART_NECK, 7.0f, body, 1.0f, off);
    {
        Vec2 head = particle_render_pos(p, b + PART_HEAD, 1.0f);
        DrawCircleV((Vector2){head.x, head.y}, 9.0f, body);
        DrawCircleLines((int)head.x, (int)head.y, 9.0f, edge);
    }
    draw_bone_chk(p, cp, b + frnt_sho, b + frnt_elb, 5.5f, body, 1.0f, off);
    draw_bone_chk(p, cp, b + frnt_elb, b + frnt_hnd, 5.0f, body, 1.0f, off);
}

void render_draw_mech_preview(const Mech *m,
                              const ParticlePool *p,
                              const ConstraintPool *cp,
                              Vec2 aim_dir)
{
    if (!m || !p || !cp) return;

    /* Body — sprite path when this chassis has a loaded atlas, else
     * the simplified capsule fallback above (no Level dependency). */
    const MechSpriteSet *set = &g_chassis_sprites[m->chassis_id];
    if (set->atlas.id != 0) {
        draw_mech_sprites(p, cp, m, /*is_local=*/true,
                          /*alpha=*/1.0f, (Vec2){0, 0});
    } else {
        preview_draw_capsules(p, cp, m);
    }

    /* Held weapon at R_HAND, rotated by aim_dir. Same look as
     * draw_held_weapon's primary branch, minus the World dependencies
     * (last_fired flicker, muzzle-flash burst, invis alpha). */
    int eff_weapon_id = m->weapon_id;
    const WeaponSpriteDef *wp  = weapon_sprite_def(eff_weapon_id);
    const Weapon          *wpn = weapon_def(eff_weapon_id);
    if (!wp || !wpn) return;

    int b = m->particle_base;
    Vec2 rh = particle_render_pos(p, b + PART_R_HAND, 1.0f);
    Color tint = (Color){255, 255, 255, 255};

    if (g_weapons_atlas.id != 0) {
        float angle = atan2f(aim_dir.y, aim_dir.x) * RAD2DEG;
        DrawTexturePro(g_weapons_atlas, wp->src,
                       (Rectangle){ rh.x, rh.y, wp->draw_w, wp->draw_h },
                       wp->pivot_grip, angle, tint);
    } else {
        Vec2 muzzle = {
            rh.x + aim_dir.x * wp->draw_w * 0.7f,
            rh.y + aim_dir.y * wp->draw_w * 0.7f,
        };
        DrawLineEx((Vector2){rh.x, rh.y}, (Vector2){muzzle.x, muzzle.y},
                   3.0f, (Color){ 50, 60, 80, 255 });
    }
}
