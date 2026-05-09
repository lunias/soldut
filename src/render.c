#include "render.h"

#include "decal.h"
#include "hud.h"
#include "level.h"
#include "log.h"
#include "map_kit.h"
#include "match.h"
#include "mech.h"
#include "mech_sprites.h"
#include "particle.h"
#include "pickup.h"
#include "projectile.h"
#include "weapon_sprites.h"
#include "weapons.h"

#include <math.h>

/* M5 P13 — halftone post-process render target + shader.
 *
 * Render flow when both are loaded:
 *   1. World draw -> RenderTexture2D `g_post_target`
 *   2. Backbuffer: blit `g_post_target` through `g_halftone_post` shader
 *   3. HUD on top, no shader
 *
 * Both fall back gracefully:
 *   - Missing shader file → g_halftone_loaded stays false, world draws
 *     directly to the backbuffer (M4 shape).
 *   - LoadShader returning the default shader (compile failure) → the
 *     uniform locations come back -1 and we treat it as "not loaded".
 *
 * The RT is recreated on backbuffer-size change (window resize); locations
 * are looked up once at first use. */
#define HALFTONE_DENSITY 0.30f

static Shader          g_halftone_post = {0};
static int             g_halftone_loc_resolution = -1;
static int             g_halftone_loc_density    = -1;
static bool            g_halftone_loaded         = false;
static bool            g_halftone_load_attempted = false;

static RenderTexture2D g_post_target = {0};
static int             g_post_target_w = 0;
static int             g_post_target_h = 0;

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
    g_halftone_loaded         = true;
    LOG_I("halftone: shader loaded; density=%.2f", (double)HALFTONE_DENSITY);
}

static bool ensure_post_target(int sw, int sh) {
    halftone_load_once();
    if (!g_halftone_loaded) return false;
    if (sw <= 0 || sh <= 0) return false;
    if (g_post_target.id != 0 && g_post_target_w == sw && g_post_target_h == sh) {
        return true;
    }
    if (g_post_target.id != 0) UnloadRenderTexture(g_post_target);
    g_post_target = LoadRenderTexture(sw, sh);
    if (g_post_target.id == 0) {
        LOG_E("halftone: LoadRenderTexture(%d,%d) failed; disabling post", sw, sh);
        g_halftone_loaded = false;
        return false;
    }
    /* Bilinear here would mush the per-pixel halftone screen — keep
     * point-sample so the dither reads sharp. */
    SetTextureFilter(g_post_target.texture, TEXTURE_FILTER_POINT);
    g_post_target_w = sw;
    g_post_target_h = sh;
    return true;
}

void renderer_post_shutdown(void) {
    if (g_post_target.id != 0) {
        UnloadRenderTexture(g_post_target);
        g_post_target = (RenderTexture2D){0};
    }
    if (g_halftone_loaded) {
        UnloadShader(g_halftone_post);
        g_halftone_post = (Shader){0};
        g_halftone_loaded = false;
    }
    g_halftone_load_attempted = false;
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
}

Vec2 renderer_screen_to_world(const Renderer *r, Vec2 screen) {
    Vector2 w = GetScreenToWorld2D((Vector2){ screen.x, screen.y }, r->camera);
    return (Vec2){ w.x, w.y };
}

/* Smoothed follow + screen-shake. Camera target gravitates toward the
 * local mech, with a small lookahead toward the cursor. */
static void update_camera(Renderer *r, World *w, int sw, int sh, float dt) {
    Vec2 focus;
    if (w->local_mech_id >= 0) {
        focus = mech_chest_pos(w, w->local_mech_id);
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
     * intensity. The intensity decays inside simulate(). */
    r->shake_phase += dt * 35.0f;
    float amp = w->shake_intensity * 6.0f;
    r->camera.target.x += sinf(r->shake_phase * 1.7f) * amp;
    r->camera.target.y += cosf(r->shake_phase * 2.1f) * amp;
    r->camera.rotation  = sinf(r->shake_phase * 1.3f) * w->shake_intensity * 1.2f;
}

/* ---- Drawing helpers ---------------------------------------------- */

/* P13 — Tile rendering with optional kit atlas. When g_map_kit.tiles is
 * loaded, each solid tile draws a 32x32 sub-rect from the 8x8-grid atlas
 * indexed by `LvlTile.id`. When not, falls back to the M4 flat 2-tone
 * checkerboard so a fresh checkout without per-map art still reads. */
static void draw_level_tiles(const Level *L) {
    const float ts = (float)L->tile_size;
    Texture2D atlas = g_map_kit.tiles;
    bool has_atlas = (atlas.id != 0);

    if (has_atlas) {
        const float src_tile_px = 32.0f;
        for (int y = 0; y < L->height; ++y) {
            for (int x = 0; x < L->width; ++x) {
                const LvlTile *t = &L->tiles[y * L->width + x];
                if (!(t->flags & TILE_F_SOLID)) continue;
                int aid = (int)t->id;
                int ax  = (aid % 8) * (int)src_tile_px;
                int ay  = (aid / 8) * (int)src_tile_px;
                DrawTexturePro(atlas,
                    (Rectangle){ (float)ax, (float)ay, src_tile_px, src_tile_px },
                    (Rectangle){ (float)x * ts, (float)y * ts, ts, ts },
                    (Vector2){0, 0}, 0.0f, WHITE);
            }
        }
        return;
    }

    /* Fallback: M4 2-tone checkerboard with a 1-px edge. */
    Color floor_a = (Color){ 32,  38,  46, 255 };
    Color floor_b = (Color){ 24,  28,  34, 255 };
    Color edge    = (Color){ 80,  90, 110, 255 };
    for (int y = 0; y < L->height; ++y) {
        for (int x = 0; x < L->width; ++x) {
            if (!(L->tiles[y * L->width + x].flags & TILE_F_SOLID)) continue;
            Color c = ((x + y) & 1) ? floor_a : floor_b;
            DrawRectangle((int)((float)x * ts), (int)((float)y * ts),
                          (int)ts, (int)ts, c);
            DrawRectangleLines((int)((float)x * ts), (int)((float)y * ts),
                               (int)ts, (int)ts, edge);
        }
    }
}

/* P13 — Free polygon rendering, refactored out of the P02 stopgap.
 * Color-by-kind per `documents/m5/08-rendering.md` §"Free polygons".
 * BACKGROUND polygons are skipped here; `draw_polys_background` paints
 * them in a separate pass after the mechs so they sit visually in front
 * of the playfield (alpha-blended foreground silhouettes). */
static void draw_polys(const Level *L) {
    Color poly_solid = (Color){ 32,  38,  46, 255 };
    Color poly_ice   = (Color){180, 220, 240, 255 };
    Color poly_dead  = (Color){ 80, 200,  80, 255 };
    Color poly_one   = (Color){ 80,  80, 100, 200 };
    Color poly_edge  = (Color){180, 200, 230, 255 };
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *poly = &L->polys[i];
        if ((PolyKind)poly->kind == POLY_KIND_BACKGROUND) continue;
        Color fill;
        switch ((PolyKind)poly->kind) {
            case POLY_KIND_ICE:        fill = poly_ice;   break;
            case POLY_KIND_DEADLY:     fill = poly_dead;  break;
            case POLY_KIND_ONE_WAY:    fill = poly_one;   break;
            case POLY_KIND_SOLID:
            default:                   fill = poly_solid; break;
        }
        Vector2 v0 = { (float)poly->v_x[0], (float)poly->v_y[0] };
        Vector2 v1 = { (float)poly->v_x[1], (float)poly->v_y[1] };
        Vector2 v2 = { (float)poly->v_x[2], (float)poly->v_y[2] };
        /* DrawTriangle is CCW-only; our authoring is screen-CW so flip
         * the vertex order to keep the fill visible. */
        DrawTriangle(v0, v2, v1, fill);
        DrawLineEx(v0, v1, 2.0f, poly_edge);
        DrawLineEx(v1, v2, 2.0f, poly_edge);
        DrawLineEx(v2, v0, 2.0f, poly_edge);
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
    /* Near: 0.95 — foreground silhouettes that sit visually in front of
     * the action. Drawn AFTER the world but inside the post-target so
     * the halftone screen catches it too. */
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

static void draw_mech_sprites(const ParticlePool *p, const ConstraintPool *cp,
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
        if (part_a >= 0) {
            Vec2 pos_a = particle_render_pos(p, b + part_a, alpha);
            draw_x = (pos_a.x + pos_b.x) * 0.5f + visual_offset.x;
            draw_y = (pos_a.y + pos_b.y) * 0.5f + visual_offset.y;
            /* Sprites are authored vertically (parent end at top of
             * source). atan2 gives the bone angle CCW from +x; raylib
             * rotates CW in screen-space (+y down). Subtract 90° so
             * angle=0 corresponds to a vertical bone (parent above
             * child) which is the source authoring orientation. */
            angle = atan2f(pos_b.y - pos_a.y, pos_b.x - pos_a.x) * RAD2DEG
                  - 90.0f;
        } else {
            draw_x = pos_b.x + visual_offset.x;
            draw_y = pos_b.y + visual_offset.y;
            angle = 0.0f;
        }

        const MechSpritePart *sp = &set->parts[sprite_idx];
        DrawTexturePro(set->atlas, sp->src,
                       (Rectangle){ draw_x, draw_y, sp->draw_w, sp->draw_h },
                       sp->pivot, angle, tint);
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
 * available pickups; cooldown entries draw nothing. The "real" sprite
 * art lands at P13 with the atlas pipeline. PRACTICE_DUMMY entries
 * aren't drawn either (the dummy is a real mech, not a pickup). */
static void draw_pickups(const PickupPool *pool, double now_s) {
    for (int i = 0; i < pool->count; ++i) {
        const PickupSpawner *s = &pool->items[i];
        if (s->state != PICKUP_STATE_AVAILABLE) continue;
        if (s->kind == PICKUP_PRACTICE_DUMMY)   continue;
        uint32_t rgba = pickup_kind_color(s->kind);
        Color c = (Color){
            (uint8_t)(rgba & 0xff),
            (uint8_t)((rgba >> 8) & 0xff),
            (uint8_t)((rgba >> 16) & 0xff),
            (uint8_t)((rgba >> 24) & 0xff),
        };
        float bob = 4.0f * sinf((float)now_s * 3.14159f);
        DrawCircle((int)s->pos.x, (int)(s->pos.y + bob), 12.0f, c);
        DrawCircleLines((int)s->pos.x, (int)(s->pos.y + bob), 12.0f,
                        (Color){20, 20, 20, 255});
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
    /* P13 — Parallax FAR + MID first, in screen space, before any world
     * draw. No BeginMode2D — the parallax draw computes its own scroll
     * from r->camera.target and the parallax ratio. Each layer no-ops
     * if its texture isn't loaded. */
    draw_parallax_far_mid(r->camera, sw, sh);

    BeginMode2D(r->camera);
        /* P13 — Decoration layers 0 + 1 sit BEHIND the tile sprites
         * (far + mid parallax-band silhouettes anchored in world space).
         * No-op when the level has no LvlDeco records. */
        draw_decorations(&w->level, 0);
        draw_decorations(&w->level, 1);
        draw_level_tiles(&w->level);
        draw_polys(&w->level);
        /* P13 — Decoration layer 2 sits BETWEEN tiles and mechs (near
         * silhouettes that read in front of geometry but behind action). */
        draw_decorations(&w->level, 2);
        decal_draw_layer();
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

    /* P13 — Parallax NEAR last (foreground silhouettes), screen-space
     * over the world. No-op when the texture isn't loaded. */
    draw_parallax_near(r->camera, sw, sh);
}

void renderer_draw_frame(Renderer *r, World *w, int sw, int sh,
                         float alpha, Vec2 local_visual_offset,
                         Vec2 cursor_screen,
                         RendererOverlayFn overlay_cb, void *overlay_user) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    float cam_dt = (r->cam_dt_override > 0.0f)
                   ? r->cam_dt_override
                   : (GetFrameTime() > 0 ? GetFrameTime() : 1.0f / 60.0f);
    update_camera(r, w, sw, sh, cam_dt);

    /* Cache cursor positions for hud + camera lookahead. */
    r->last_cursor_screen = cursor_screen;
    r->last_cursor_world  = renderer_screen_to_world(r, cursor_screen);

    /* Bake any blood drops the latest tick added before we begin the
     * world draw — raylib disallows nesting BeginTextureMode inside
     * BeginMode2D, and `decal_flush_pending` is itself a BeginTextureMode
     * pair on the splat layer. Run it before any BeginTextureMode /
     * BeginDrawing pair we open below. */
    decal_flush_pending();

    bool use_post = ensure_post_target(sw, sh);

    if (use_post) {
        /* World → off-screen RT. ClearBackground inside BeginTextureMode
         * targets the RT, not the backbuffer — keeps the post-pass
         * blend predictable. */
        BeginTextureMode(g_post_target);
            ClearBackground((Color){12, 14, 18, 255});
            draw_world_pass(r, w, alpha, local_visual_offset, sw, sh);
        EndTextureMode();

        /* Per-frame uniforms. Resolution may have changed since last
         * frame (window resize); halftone density is constant in P13
         * but plumbed through SetShaderValue so the M6 config slider
         * has a place to land. */
        Vector2 res = { (float)sw, (float)sh };
        float   den = HALFTONE_DENSITY;
        if (g_halftone_loc_resolution >= 0)
            SetShaderValue(g_halftone_post, g_halftone_loc_resolution,
                           &res, SHADER_UNIFORM_VEC2);
        if (g_halftone_loc_density >= 0)
            SetShaderValue(g_halftone_post, g_halftone_loc_density,
                           &den, SHADER_UNIFORM_FLOAT);

        BeginDrawing();
            ClearBackground(BLACK);
            BeginShaderMode(g_halftone_post);
                /* raylib's render textures come back Y-flipped — pass
                 * a negative source-rectangle height to flip on draw.
                 * (See [06-rendering-audio.md] "Y-flip gotcha.") */
                Rectangle src = {
                    0, 0,
                    (float)g_post_target.texture.width,
                    -(float)g_post_target.texture.height
                };
                Vector2   dst = { 0, 0 };
                DrawTextureRec(g_post_target.texture, src, dst, WHITE);
            EndShaderMode();
            hud_draw(w, sw, sh, cursor_screen, r->camera);
            if (overlay_cb) overlay_cb(overlay_user, sw, sh);
        EndDrawing();
    } else {
        /* Fallback: M4 shape, no post pass. Fires on a fresh checkout
         * without `assets/shaders/halftone_post.fs.glsl`, or when
         * LoadRenderTexture failed (low-VRAM / GL-context issue). */
        BeginDrawing();
            ClearBackground((Color){12, 14, 18, 255});
            draw_world_pass(r, w, alpha, local_visual_offset, sw, sh);
            hud_draw(w, sw, sh, cursor_screen, r->camera);
            if (overlay_cb) overlay_cb(overlay_user, sw, sh);
        EndDrawing();
    }
}
