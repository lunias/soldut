#include "render.h"

#include "decal.h"
#include "hud.h"
#include "level.h"
#include "mech.h"
#include "particle.h"
#include "pickup.h"
#include "projectile.h"

#include <math.h>

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

static void draw_level(const Level *L) {
    const float ts = (float)L->tile_size;
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

    /* P02: temporary polygon renderer. Draws each free polygon as a
     * filled triangle plus an edge outline so the slope test bed (in
     * level_build_tutorial) shows up in shot tests. The proper art
     * pass (sprite atlas + halftone) lands at P13. */
    Color poly_solid = (Color){ 50, 70, 100, 255 };
    Color poly_ice   = (Color){180, 220, 240, 255 };
    Color poly_dead  = (Color){140,  40,  40, 255 };
    Color poly_one   = (Color){ 90, 110,  60, 255 };
    Color poly_back  = (Color){ 28,  32,  40, 200 };
    Color poly_edge  = (Color){180, 200, 230, 255 };
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *poly = &L->polys[i];
        Color fill;
        switch ((PolyKind)poly->kind) {
            case POLY_KIND_ICE:        fill = poly_ice;   break;
            case POLY_KIND_DEADLY:     fill = poly_dead;  break;
            case POLY_KIND_ONE_WAY:    fill = poly_one;   break;
            case POLY_KIND_BACKGROUND: fill = poly_back;  break;
            case POLY_KIND_SOLID:
            default:                   fill = poly_solid; break;
        }
        Vector2 v0 = { (float)poly->v_x[0], (float)poly->v_y[0] };
        Vector2 v1 = { (float)poly->v_x[1], (float)poly->v_y[1] };
        Vector2 v2 = { (float)poly->v_x[2], (float)poly->v_y[2] };
        /* DrawTriangle is CCW-only; our authoring is screen-CW so flip
         * the order to keep the fill visible. */
        DrawTriangle(v0, v2, v1, fill);
        DrawLineEx(v0, v1, 2.0f, poly_edge);
        DrawLineEx(v1, v2, 2.0f, poly_edge);
        DrawLineEx(v2, v0, 2.0f, poly_edge);
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

/* Polygonal mech rendering — flat-shaded plates per
 * [06-rendering-audio.md]. M1 uses thick capsule lines as plates;
 * baked sprites land at M5 (asset pass). The Level pointer is only
 * used to clamp arm/rifle bones that would otherwise visibly cross a
 * thin solid (e.g., aiming through the col-55 wall). The
 * ConstraintPool lets us skip drawing bones whose distance constraint
 * has been deactivated by dismemberment so the corpse doesn't trail a
 * stretched bone across the level.
 *
 * P03: `alpha` = sub-tick interp factor (in [0,1)) lerps between the
 * physics state at the start of the most-recent simulate tick and the
 * latest result. `visual_offset` is added to every drawn particle —
 * used by the local mech to smooth out reconciliation snaps over
 * ~6 frames. Pass {0,0} for non-local mechs. */
static void draw_mech(const ParticlePool *p, const ConstraintPool *cp,
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

    /* Rifle: a small line from R_HAND in aim direction, stopped at
     * the first solid tile so the barrel doesn't draw through walls. */
    if (m->alive) {
        Vec2 rh = particle_render_pos(p, b + PART_R_HAND, alpha);
        Vec2 sh = particle_render_pos(p, b + PART_R_SHOULDER, alpha);
        rh.x += visual_offset.x; rh.y += visual_offset.y;
        sh.x += visual_offset.x; sh.y += visual_offset.y;
        float dx = rh.x - sh.x, dy = rh.y - sh.y;
        float dl = sqrtf(dx * dx + dy * dy);
        if (dl > 1.0f) { dx /= dl; dy /= dl; }
        Vec2 muzzle = { rh.x + dx * 22.0f, rh.y + dy * 22.0f };
        draw_bone_clamped(L, rh, muzzle, 3.0f, (Color){50, 60, 80, 255});
    }
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
     * BeginMode2D. */
    decal_flush_pending();

    BeginDrawing();
        ClearBackground((Color){12, 14, 18, 255});

        BeginMode2D(r->camera);
            draw_level(&w->level);
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
            }
            projectile_draw(&w->projectiles, alpha);
            fx_draw(&w->fx, alpha);
        EndMode2D();

        hud_draw(w, sw, sh, cursor_screen);
        if (overlay_cb) overlay_cb(overlay_user, sw, sh);
    EndDrawing();
}
