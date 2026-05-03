#include "render.h"

#include "decal.h"
#include "hud.h"
#include "level.h"
#include "mech.h"
#include "particle.h"

#include <math.h>

void renderer_init(Renderer *r, int sw, int sh, Vec2 follow) {
    r->camera.offset   = (Vector2){ sw * 0.5f, sh * 0.5f };
    r->camera.target   = follow;
    r->camera.rotation = 0.0f;
    r->camera.zoom     = 1.4f;
    r->shake_phase     = 0.0f;
    r->last_cursor_screen = (Vec2){ sw * 0.5f, sh * 0.5f };
    r->last_cursor_world  = follow;
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
            if (L->tiles[y * L->width + x] != TILE_SOLID) continue;
            Color c = ((x + y) & 1) ? floor_a : floor_b;
            DrawRectangle((int)((float)x * ts), (int)((float)y * ts),
                          (int)ts, (int)ts, c);
            DrawRectangleLines((int)((float)x * ts), (int)((float)y * ts),
                               (int)ts, (int)ts, edge);
        }
    }
}

static void draw_bone(const ParticlePool *p, int a, int b, float thick, Color c) {
    Vector2 va = { p->pos_x[a], p->pos_y[a] };
    Vector2 vb = { p->pos_x[b], p->pos_y[b] };
    DrawLineEx(va, vb, thick, c);
}

/* Polygonal mech rendering — flat-shaded plates per
 * [06-rendering-audio.md]. M1 uses thick capsule lines as plates;
 * baked sprites land at M5 (asset pass). */
static void draw_mech(const ParticlePool *p, const Mech *m) {
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

    /* Back leg first, then front, head last — see [06-rendering-audio.md]. */
    Color leg_back  = (Color){ body.r - 20, body.g - 20, body.b - 20, 255 };
    Color leg_front = body;
    int back_leg_hip   = m->facing_left ? PART_R_HIP   : PART_L_HIP;
    int back_leg_knee  = m->facing_left ? PART_R_KNEE  : PART_L_KNEE;
    int back_leg_foot  = m->facing_left ? PART_R_FOOT  : PART_L_FOOT;
    int front_leg_hip  = m->facing_left ? PART_L_HIP   : PART_R_HIP;
    int front_leg_knee = m->facing_left ? PART_L_KNEE  : PART_R_KNEE;
    int front_leg_foot = m->facing_left ? PART_L_FOOT  : PART_R_FOOT;

    draw_bone(p, b + back_leg_hip,   b + back_leg_knee,  6.0f, leg_back);
    draw_bone(p, b + back_leg_knee,  b + back_leg_foot,  5.5f, leg_back);

    /* Torso — chest to pelvis as a thick beam plus shoulder span. */
    draw_bone(p, idx_chest, idx_pelvis, 13.0f, body);
    DrawCircleV((Vector2){ p->pos_x[idx_pelvis], p->pos_y[idx_pelvis] }, 6.0f, body);
    /* Hip plate. */
    draw_bone(p, b + PART_L_HIP, b + PART_R_HIP, 10.0f, body);
    /* Shoulder plate. */
    draw_bone(p, b + PART_L_SHOULDER, b + PART_R_SHOULDER, 10.0f, body);

    /* Front leg. */
    draw_bone(p, b + front_leg_hip,  b + front_leg_knee,  6.5f, leg_front);
    draw_bone(p, b + front_leg_knee, b + front_leg_foot,  6.0f, leg_front);

    /* Back arm (the off-hand). */
    int back_sho = m->facing_left ? PART_R_SHOULDER : PART_L_SHOULDER;
    int back_elb = m->facing_left ? PART_R_ELBOW    : PART_L_ELBOW;
    int back_hnd = m->facing_left ? PART_R_HAND     : PART_L_HAND;
    int frnt_sho = m->facing_left ? PART_L_SHOULDER : PART_R_SHOULDER;
    int frnt_elb = m->facing_left ? PART_L_ELBOW    : PART_R_ELBOW;
    int frnt_hnd = m->facing_left ? PART_L_HAND     : PART_R_HAND;

    /* Skip rendering bones whose distance constraints are gone — that's
     * how a dismembered limb appears: the upstream constraint is dead
     * but the limb particles continue to integrate freely. We render
     * them anyway (the limb is still in the world); the visual cue is
     * that they no longer track the body. */
    Color arm_back = (Color){body.r - 30, body.g - 30, body.b - 30, 255};
    draw_bone(p, b + back_sho, b + back_elb, 5.0f, arm_back);
    draw_bone(p, b + back_elb, b + back_hnd, 4.5f, arm_back);

    /* Head + neck. */
    draw_bone(p, idx_chest, idx_neck, 7.0f, body);
    DrawCircleV((Vector2){ p->pos_x[idx_head], p->pos_y[idx_head] }, 9.0f, body);
    DrawCircleLines((int)p->pos_x[idx_head], (int)p->pos_y[idx_head], 9.0f, edge);

    /* Front (rifle) arm. */
    Color arm_front = body;
    draw_bone(p, b + frnt_sho, b + frnt_elb, 5.5f, arm_front);
    draw_bone(p, b + frnt_elb, b + frnt_hnd, 5.0f, arm_front);

    /* Rifle: a small line from R_HAND in aim direction. */
    if (m->alive) {
        Vector2 rh = { p->pos_x[b + PART_R_HAND], p->pos_y[b + PART_R_HAND] };
        Vector2 sh = { p->pos_x[b + PART_R_SHOULDER], p->pos_y[b + PART_R_SHOULDER] };
        Vector2 d = { rh.x - sh.x, rh.y - sh.y };
        float L = sqrtf(d.x * d.x + d.y * d.y);
        if (L > 1.0f) { d.x /= L; d.y /= L; }
        Vector2 muzzle = { rh.x + d.x * 22.0f, rh.y + d.y * 22.0f };
        DrawLineEx(rh, muzzle, 3.0f, (Color){50, 60, 80, 255});
    }
}

/* ---- Frame --------------------------------------------------------- */

void renderer_draw_frame(Renderer *r, World *w, int sw, int sh,
                         float alpha, Vec2 cursor_screen,
                         RendererOverlayFn overlay_cb, void *overlay_user) {
    (void)alpha;
    update_camera(r, w, sw, sh, GetFrameTime() > 0 ? GetFrameTime() : 1.0f / 60.0f);

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
            for (int i = 0; i < w->mech_count; ++i) {
                draw_mech(&w->particles, &w->mechs[i]);
            }
            fx_draw(&w->fx);
        EndMode2D();

        hud_draw(w, sw, sh, cursor_screen);
        if (overlay_cb) overlay_cb(overlay_user, sw, sh);
    EndDrawing();
}
