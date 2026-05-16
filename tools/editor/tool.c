#include "tool.h"

#include "log.h"
#include "palette.h"

#include "raylib.h"
#include "stb_ds.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void tool_ctx_init(ToolCtx *c) {
    memset(c, 0, sizeof(*c));
    c->kind         = TOOL_TILE;
    c->tile_id      = 0;
    c->tile_flags   = TILE_F_SOLID;
    c->poly_kind    = POLY_KIND_SOLID;
    c->poly_preset  = PRESET_COUNT;             /* PRESET_COUNT = "freehand, no preset" */
    c->spawn_team   = 0;
    c->spawn_lane_hint = 0;
    c->pickup_variant  = PICK_HEALTH_S;
    c->ambi_kind    = AMBI_WIND;
    /* M6 P09 — defaults match the pre-P09 hardcoded ambi values so an
     * unmodified placement produces identical bytes. */
    c->ambi_strength = 0.5f;
    c->ambi_dir_deg  = 0.0f;     /* east; +1 in dir_x_q */
    c->deco_layer   = 1;
    c->deco_scale       = 1.0f;
    c->deco_rot_deg     = 0.0f;
    c->deco_flipped_x   = false;
    c->deco_additive    = false;
    c->flag_team    = 1;        /* P07 — start on RED so first click drops Red */
}

const char *tool_name(ToolKind k) {
    switch (k) {
        case TOOL_TILE:   return "Tile";
        case TOOL_POLY:   return "Polygon";
        case TOOL_SPAWN:  return "Spawn";
        case TOOL_PICKUP: return "Pickup";
        case TOOL_AMBI:   return "Ambient";
        case TOOL_DECO:   return "Deco";
        case TOOL_FLAG:   return "Flag";
        case TOOL_META:   return "Meta";
        default:          return "?";
    }
}

/* ---- Tile tool ---------------------------------------------------- */

static void tile_paint(EditorDoc *d, UndoStack *u, int tx, int ty,
                       LvlTile new_tile) {
    if (tx < 0 || tx >= d->width || ty < 0 || ty >= d->height) return;
    LvlTile *cur = &d->tiles[ty * d->width + tx];
    if (cur->id == new_tile.id && cur->flags == new_tile.flags) return;
    LvlTile before = *cur;
    *cur = new_tile;
    d->dirty = true;
    undo_record_tile(u, tx, ty, before, new_tile);
}

static void tile_press(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    int tx = wx / d->tile_size;
    int ty = wy / d->tile_size;
    LvlTile t = { .id = c->tile_id, .flags = c->tile_flags };
    undo_begin_tile_stroke(u);
    tile_paint(d, u, tx, ty, t);
}

static void tile_drag(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    int tx = wx / d->tile_size;
    int ty = wy / d->tile_size;
    LvlTile t = { .id = c->tile_id, .flags = c->tile_flags };
    tile_paint(d, u, tx, ty, t);
}

static void tile_release(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    (void)d; (void)c; (void)wx; (void)wy;
    undo_end_tile_stroke(u, d);
}

static void tile_overlay(const EditorDoc *d, const ToolCtx *c, const EditorView *v) {
    /* Hover highlight of the tile under the mouse. */
    Vector2 m = view_screen_to_world(v, GetMousePosition());
    int tx = (int)(m.x / d->tile_size);
    int ty = (int)(m.y / d->tile_size);
    if (tx < 0 || tx >= d->width || ty < 0 || ty >= d->height) return;
    Color col = (Color){200, 220, 255, 80};
    if (c->tile_flags & TILE_F_DEADLY) col = (Color){255, 80, 80, 100};
    else if (c->tile_flags & TILE_F_ICE)  col = (Color){128, 200, 255, 100};
    DrawRectangle(tx * d->tile_size, ty * d->tile_size,
                  d->tile_size, d->tile_size, col);
}

static bool tile_key(EditorDoc *d, UndoStack *u, ToolCtx *c, int key) {
    (void)d; (void)u;
    int n;
    const TileFlagEntry *flags = palette_tile_flags(&n);
    for (int i = 0; i < n; ++i) {
        if (key == flags[i].hotkey || key == (flags[i].hotkey - 32)) {
            c->tile_flags ^= flags[i].flag;
            return true;
        }
    }
    return false;
}

/* ---- Polygon tool ------------------------------------------------- */

static void poly_press(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    /* If a preset is armed, drop it and reset. */
    if (c->poly_preset != PRESET_COUNT) {
        int tris = palette_apply_preset(d, c->poly_preset, wx, wy);
        if (tris > 0) {
            /* Record the appended polys so undo rolls them back together
             * — for v1 we record one UC_POLY_ADD per emitted triangle. */
            int n = (int)arrlen(d->polys);
            for (int i = n - tris; i < n; ++i) {
                undo_record_obj_add(u, UC_POLY_ADD, i, &d->polys[i]);
            }
            LOG_I("editor: applied preset %d at (%d, %d) → %d tri",
                  (int)c->poly_preset, wx, wy, tris);
        }
        return;
    }

    /* Free polygon: append vertex; close on second click of vertex 0. */
    if (c->poly_vertex_count >= POLY_MAX_VERTS) {
        LOG_W("editor: polygon vertex cap reached");
        return;
    }
    /* Click within 8 px of vertex 0 = close the polygon. */
    if (c->poly_vertex_count >= 3) {
        int dx = wx - c->poly_in_progress[0].x;
        int dy = wy - c->poly_in_progress[0].y;
        if (dx * dx + dy * dy < 8 * 8) {
            /* Close. */
            int n = c->poly_vertex_count;
            PolyValidate r = poly_validate(c->poly_in_progress, n);
            if (r != POLY_VALID) {
                LOG_W("editor: polygon invalid (%d) — refusing close", (int)r);
                return;
            }
            LvlPoly tris[POLY_MAX_VERTS];
            int t = poly_triangulate(c->poly_in_progress, n, c->poly_kind,
                                     tris, POLY_MAX_VERTS);
            if (t <= 0) {
                LOG_W("editor: polygon triangulation failed");
                return;
            }
            for (int i = 0; i < t; ++i) {
                arrput(d->polys, tris[i]);
                undo_record_obj_add(u, UC_POLY_ADD,
                                    (int)arrlen(d->polys) - 1,
                                    &d->polys[arrlen(d->polys) - 1]);
            }
            LOG_I("editor: polygon closed (%d verts → %d tri)", n, t);
            c->poly_vertex_count = 0;
            d->dirty = true;
            return;
        }
    }
    c->poly_in_progress[c->poly_vertex_count++] = (EditorPolyVert){ wx, wy };
}

static void poly_overlay(const EditorDoc *d, const ToolCtx *c, const EditorView *v) {
    (void)d;
    /* Cursor hint for armed preset. */
    if (c->poly_preset != PRESET_COUNT) {
        Vector2 m = view_screen_to_world(v, GetMousePosition());
        Vector2 sm = view_snap(m, 4);
        DrawRectangleLinesEx((Rectangle){sm.x - 8, sm.y - 8, 16, 16},
                             1.0f / v->cam.zoom, ORANGE);
        return;
    }
    if (c->poly_vertex_count <= 0) return;
    /* Dots on existing vertices. */
    for (int i = 0; i < c->poly_vertex_count; ++i) {
        DrawCircleV((Vector2){(float)c->poly_in_progress[i].x,
                              (float)c->poly_in_progress[i].y},
                    3.0f / v->cam.zoom, ORANGE);
    }
    /* Lines between vertices. */
    for (int i = 0; i < c->poly_vertex_count - 1; ++i) {
        DrawLineEx((Vector2){(float)c->poly_in_progress[i].x,
                             (float)c->poly_in_progress[i].y},
                   (Vector2){(float)c->poly_in_progress[i+1].x,
                             (float)c->poly_in_progress[i+1].y},
                   2.0f / v->cam.zoom, ORANGE);
    }
    /* Rubber band to mouse. */
    Vector2 m = view_screen_to_world(v, GetMousePosition());
    Vector2 sm = view_snap(m, 4);
    DrawLineEx((Vector2){(float)c->poly_in_progress[c->poly_vertex_count-1].x,
                         (float)c->poly_in_progress[c->poly_vertex_count-1].y},
               sm, 1.0f / v->cam.zoom, (Color){255, 180, 100, 200});
    /* Highlight first vertex if we'd close. */
    if (c->poly_vertex_count >= 3) {
        DrawCircleLines(c->poly_in_progress[0].x, c->poly_in_progress[0].y,
                        6.0f / v->cam.zoom, YELLOW);
    }
}

static bool poly_key(EditorDoc *d, UndoStack *u, ToolCtx *c, int key) {
    if (key == KEY_ESCAPE) {
        c->poly_vertex_count = 0;
        c->poly_preset = PRESET_COUNT;
        return true;
    }
    if (key == KEY_BACKSPACE && c->poly_vertex_count > 0) {
        c->poly_vertex_count--;
        return true;
    }
    if (key == KEY_ENTER || key == KEY_KP_ENTER) {
        /* Force-close at the current cursor position relative to first vertex. */
        if (c->poly_vertex_count >= 3) {
            /* Synthesize a click at vertex 0 to trigger close. */
            poly_press(d, u, c,
                       c->poly_in_progress[0].x,
                       c->poly_in_progress[0].y);
            return true;
        }
    }
    return false;
}

/* ---- Spawn tool --------------------------------------------------- */

static void spawn_press(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    LvlSpawn s = {0};
    s.pos_x     = (int16_t)wx;
    s.pos_y     = (int16_t)wy;
    s.team      = c->spawn_team;
    s.flags     = 1;       /* PRIMARY */
    s.lane_hint = c->spawn_lane_hint++;
    arrput(d->spawns, s);
    undo_record_obj_add(u, UC_SPAWN_ADD, (int)arrlen(d->spawns) - 1, &s);
    d->dirty = true;
}

static void spawn_overlay(const EditorDoc *d, const ToolCtx *c, const EditorView *v) {
    (void)d; (void)v;
    Color cc =
        (c->spawn_team == 1) ? (Color){240, 100, 100, 255} :
        (c->spawn_team == 2) ? (Color){100, 160, 240, 255} :
                               (Color){200, 200, 200, 255};
    Vector2 m = view_screen_to_world(v, GetMousePosition());
    Vector2 sm = view_snap(m, 4);
    DrawCircleV(sm, 6.0f / v->cam.zoom, cc);
    DrawCircleLines(sm.x, sm.y, 16.0f / v->cam.zoom, cc);
}

static bool spawn_key(EditorDoc *d, UndoStack *u, ToolCtx *c, int key) {
    (void)d; (void)u;
    if (key == KEY_ZERO || key == KEY_KP_0) { c->spawn_team = 0; return true; }
    if (key == KEY_ONE  || key == KEY_KP_1) { c->spawn_team = 1; return true; }
    if (key == KEY_TWO  || key == KEY_KP_2) { c->spawn_team = 2; return true; }
    return false;
}

/* ---- Pickup tool -------------------------------------------------- */

static void pickup_press(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    int n;
    const PickupEntry *pickups = palette_pickups(&n);
    if ((int)c->pickup_variant >= n) c->pickup_variant = (PickupVariant)0;
    const PickupEntry *e = &pickups[c->pickup_variant];
    LvlPickup p = {0};
    p.pos_x      = (int16_t)wx;
    p.pos_y      = (int16_t)wy;
    p.category   = e->category_id;
    p.variant    = e->variant_id;
    p.respawn_ms = e->respawn_ms;
    p.flags      = 0;
    arrput(d->pickups, p);
    undo_record_obj_add(u, UC_PICKUP_ADD, (int)arrlen(d->pickups) - 1, &p);
    d->dirty = true;
}

static void pickup_overlay(const EditorDoc *d, const ToolCtx *c, const EditorView *v) {
    (void)d;
    int n;
    palette_pickups(&n);
    if ((int)c->pickup_variant >= n) return;
    Vector2 m = view_screen_to_world(v, GetMousePosition());
    Vector2 sm = view_snap(m, 4);
    DrawCircleV(sm, 4.0f / v->cam.zoom, (Color){255, 220, 100, 255});
    DrawCircleLines(sm.x, sm.y, 12.0f / v->cam.zoom, (Color){255, 220, 100, 255});
}

static bool pickup_key(EditorDoc *d, UndoStack *u, ToolCtx *c, int key) {
    (void)d; (void)u;
    int n;
    palette_pickups(&n);
    if (key == KEY_PERIOD || key == KEY_RIGHT_BRACKET) {
        c->pickup_variant = (PickupVariant)(((int)c->pickup_variant + 1) % n);
        return true;
    }
    if (key == KEY_COMMA  || key == KEY_LEFT_BRACKET) {
        c->pickup_variant = (PickupVariant)(((int)c->pickup_variant + n - 1) % n);
        return true;
    }
    return false;
}

/* ---- Ambient tool ------------------------------------------------- */

static void ambi_press(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    (void)d; (void)u;
    c->ambi_drag_start_x = wx;
    c->ambi_drag_start_y = wy;
    c->ambi_dragging = true;
}

static void ambi_release(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    if (!c->ambi_dragging) return;
    c->ambi_dragging = false;
    int x0 = c->ambi_drag_start_x, y0 = c->ambi_drag_start_y;
    int x1 = wx, y1 = wy;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x1 - x0 < 16 || y1 - y0 < 16) return;        /* too small */
    /* M6 P09 — pull strength + direction from ToolCtx (was hardcoded). */
    float st = c->ambi_strength;
    if (st < 0.0f) st = 0.0f;
    if (st > 1.0f) st = 1.0f;
    float ang = c->ambi_dir_deg * 3.14159265f / 180.0f;
    float dx  = cosf(ang);
    float dy  = sinf(ang);
    /* Q1.15 conversion with clamp so a slider near +1 doesn't overflow
     * to -32768 via the signed-int cast. */
    int s_q  = (int)(st * 32767.0f);
    int dx_q = (int)(dx * 32767.0f);
    int dy_q = (int)(dy * 32767.0f);
    if (dx_q >  32767) dx_q =  32767;
    if (dx_q < -32768) dx_q = -32768;
    if (dy_q >  32767) dy_q =  32767;
    if (dy_q < -32768) dy_q = -32768;
    LvlAmbi a = {0};
    a.rect_x = (int16_t)x0;
    a.rect_y = (int16_t)y0;
    a.rect_w = (int16_t)(x1 - x0);
    a.rect_h = (int16_t)(y1 - y0);
    a.kind   = (uint16_t)c->ambi_kind;
    a.strength_q = (int16_t)s_q;
    a.dir_x_q    = (int16_t)dx_q;
    a.dir_y_q    = (int16_t)dy_q;
    arrput(d->ambis, a);
    undo_record_obj_add(u, UC_AMBI_ADD, (int)arrlen(d->ambis) - 1, &a);
    d->dirty = true;
}

static void ambi_overlay(const EditorDoc *d, const ToolCtx *c, const EditorView *v) {
    (void)d;
    if (!c->ambi_dragging) return;
    Vector2 m = view_screen_to_world(v, GetMousePosition());
    int x0 = c->ambi_drag_start_x, y0 = c->ambi_drag_start_y;
    int x1 = (int)m.x, y1 = (int)m.y;
    int rx = (x0 < x1) ? x0 : x1;
    int ry = (y0 < y1) ? y0 : y1;
    int rw = abs(x1 - x0), rh = abs(y1 - y0);
    DrawRectangleLinesEx((Rectangle){(float)rx, (float)ry, (float)rw, (float)rh},
                         2.0f / v->cam.zoom, GREEN);
}

/* ---- Deco tool ---------------------------------------------------- */

static void deco_press(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    /* M6 P09 — pull scale, rot, flip, additive from ToolCtx (was
     * hardcoded). */
    float scale = c->deco_scale;
    if (scale < 0.05f) scale = 0.05f;
    if (scale > 16.0f) scale = 16.0f;
    int scale_q = (int)(scale * 32768.0f);
    if (scale_q < 0)      scale_q = 0;
    if (scale_q > 32767)  scale_q = 32767;
    /* rot stored as 1/256ths of a turn per .lvl spec. Normalize first. */
    float rot_turns = c->deco_rot_deg / 360.0f;
    rot_turns -= floorf(rot_turns);
    int rot_q = (int)(rot_turns * 256.0f);
    if (rot_q > 255) rot_q = 255;
    uint8_t flags = 0;
    if (c->deco_flipped_x) flags |= 0x01;   /* DECO_FLIPPED_X */
    if (c->deco_additive)  flags |= 0x02;   /* DECO_ADDITIVE */
    LvlDeco e = {0};
    e.pos_x = (int16_t)wx;
    e.pos_y = (int16_t)wy;
    e.scale_q = (int16_t)scale_q;
    e.rot_q   = (int16_t)rot_q;
    e.sprite_str_idx = c->deco_sprite_str;
    e.layer = c->deco_layer;
    e.flags = flags;
    arrput(d->decos, e);
    undo_record_obj_add(u, UC_DECO_ADD, (int)arrlen(d->decos) - 1, &e);
    d->dirty = true;
}

static void deco_overlay(const EditorDoc *d, const ToolCtx *c, const EditorView *v) {
    (void)d; (void)c;
    Vector2 m = view_screen_to_world(v, GetMousePosition());
    Vector2 sm = view_snap(m, 4);
    DrawRectangleLines((int)sm.x - 8, (int)sm.y - 8, 16, 16, (Color){255, 140, 240, 255});
}

/* ---- Flag tool (M5 P07) ------------------------------------------ */
/* Drops a CTF flag base. team auto-toggles 1 ↔ 2 between placements so
 * a designer who clicks twice drops one of each. Validation enforces
 * "0 or one of each team" via tools/editor/validate.c. */

static void flag_press(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    LvlFlag f = {0};
    f.pos_x = (int16_t)wx;
    f.pos_y = (int16_t)wy;
    f.team  = c->flag_team;
    arrput(d->flags, f);
    undo_record_obj_add(u, UC_FLAG_ADD, (int)arrlen(d->flags) - 1, &f);
    /* Auto-toggle so successive clicks place the other team. */
    c->flag_team = (c->flag_team == 1) ? 2u : 1u;
    /* Auto-set the META mode_mask CTF bit when both flags are placed.
     * Same check as runtime ctf_init_round — Red AND Blue present. */
    int red_n = 0, blue_n = 0;
    int fn = (int)arrlen(d->flags);
    for (int i = 0; i < fn; ++i) {
        if (d->flags[i].team == 1) red_n++;
        if (d->flags[i].team == 2) blue_n++;
    }
    if (red_n > 0 && blue_n > 0) {
        d->meta.mode_mask |= (uint16_t)(1u << 2);   /* MATCH_MODE_CTF = 2 */
    }
    d->dirty = true;
}

static void flag_overlay(const EditorDoc *d, const ToolCtx *c, const EditorView *v) {
    (void)d; (void)v;
    Color cc = (c->flag_team == 1) ? (Color){240, 80, 80, 255}
                                    : (Color){100, 160, 240, 255};
    Vector2 m = view_screen_to_world(v, GetMousePosition());
    Vector2 sm = view_snap(m, 4);
    /* Mirror runtime render: rectangle staff + circle pennant top. */
    DrawRectangle((int)sm.x - 6, (int)sm.y - 12, 12, 24, cc);
    DrawCircle((int)sm.x, (int)sm.y - 14, 6, cc);
    DrawCircleLines((int)sm.x, (int)sm.y, 22.0f / v->cam.zoom,
                    (Color){cc.r, cc.g, cc.b, 200});
}

static bool flag_key(EditorDoc *d, UndoStack *u, ToolCtx *c, int key) {
    (void)d; (void)u;
    if (key == KEY_ONE || key == KEY_KP_1) { c->flag_team = 1; return true; }
    if (key == KEY_TWO || key == KEY_KP_2) { c->flag_team = 2; return true; }
    return false;
}

static int flag_nearest(const EditorDoc *d, int wx, int wy, int max_px) {
    int best = -1; long best_d2 = (long)max_px * max_px + 1;
    for (int i = 0; i < (int)arrlen(d->flags); ++i) {
        long dx = d->flags[i].pos_x - wx;
        long dy = d->flags[i].pos_y - wy;
        long dd = dx * dx + dy * dy;
        if (dd < best_d2) { best_d2 = dd; best = i; }
    }
    return best;
}

/* ---- Right-click handlers (delete-nearest / cancel) --------------- */

static int spawn_nearest(const EditorDoc *d, int wx, int wy, int max_px) {
    int best = -1; long best_d2 = (long)max_px * max_px + 1;
    for (int i = 0; i < (int)arrlen(d->spawns); ++i) {
        long dx = d->spawns[i].pos_x - wx;
        long dy = d->spawns[i].pos_y - wy;
        long dd = dx*dx + dy*dy;
        if (dd < best_d2) { best_d2 = dd; best = i; }
    }
    return best;
}
static int pickup_nearest(const EditorDoc *d, int wx, int wy, int max_px) {
    int best = -1; long best_d2 = (long)max_px * max_px + 1;
    for (int i = 0; i < (int)arrlen(d->pickups); ++i) {
        long dx = d->pickups[i].pos_x - wx;
        long dy = d->pickups[i].pos_y - wy;
        long dd = dx*dx + dy*dy;
        if (dd < best_d2) { best_d2 = dd; best = i; }
    }
    return best;
}
static int deco_nearest(const EditorDoc *d, int wx, int wy, int max_px) {
    int best = -1; long best_d2 = (long)max_px * max_px + 1;
    for (int i = 0; i < (int)arrlen(d->decos); ++i) {
        long dx = d->decos[i].pos_x - wx;
        long dy = d->decos[i].pos_y - wy;
        long dd = dx*dx + dy*dy;
        if (dd < best_d2) { best_d2 = dd; best = i; }
    }
    return best;
}
static int ambi_at(const EditorDoc *d, int wx, int wy) {
    /* Last-added wins so the most recent zone is the easiest to delete. */
    for (int i = (int)arrlen(d->ambis) - 1; i >= 0; --i) {
        const LvlAmbi *a = &d->ambis[i];
        if (wx >= a->rect_x && wx < a->rect_x + a->rect_w &&
            wy >= a->rect_y && wy < a->rect_y + a->rect_h) return i;
    }
    return -1;
}

void tool_on_press_right(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy,
                         ToolKind k) {
    switch (k) {
        case TOOL_TILE: {
            int tx = wx / d->tile_size, ty = wy / d->tile_size;
            LvlTile empty = (LvlTile){0};
            undo_begin_tile_stroke(u);
            tile_paint(d, u, tx, ty, empty);
            undo_end_tile_stroke(u, d);
            break;
        }
        case TOOL_POLY:
            c->poly_vertex_count = 0;
            c->poly_preset = PRESET_COUNT;
            break;
        case TOOL_SPAWN: {
            int idx = spawn_nearest(d, wx, wy, 24);
            if (idx >= 0) {
                LvlSpawn rec = d->spawns[idx];
                arrdel(d->spawns, idx);
                undo_record_obj_del(u, UC_SPAWN_DEL, idx, &rec);
                d->dirty = true;
            }
            break;
        }
        case TOOL_PICKUP: {
            int idx = pickup_nearest(d, wx, wy, 24);
            if (idx >= 0) {
                LvlPickup rec = d->pickups[idx];
                arrdel(d->pickups, idx);
                undo_record_obj_del(u, UC_PICKUP_DEL, idx, &rec);
                d->dirty = true;
            }
            break;
        }
        case TOOL_DECO: {
            int idx = deco_nearest(d, wx, wy, 24);
            if (idx >= 0) {
                LvlDeco rec = d->decos[idx];
                arrdel(d->decos, idx);
                undo_record_obj_del(u, UC_DECO_DEL, idx, &rec);
                d->dirty = true;
            }
            break;
        }
        case TOOL_AMBI: {
            int idx = ambi_at(d, wx, wy);
            if (idx >= 0) {
                LvlAmbi rec = d->ambis[idx];
                arrdel(d->ambis, idx);
                undo_record_obj_del(u, UC_AMBI_DEL, idx, &rec);
                d->dirty = true;
            }
            break;
        }
        case TOOL_FLAG: {
            int idx = flag_nearest(d, wx, wy, 24);
            if (idx >= 0) {
                LvlFlag rec = d->flags[idx];
                arrdel(d->flags, idx);
                undo_record_obj_del(u, UC_FLAG_DEL, idx, &rec);
                /* If we dropped below the Red+Blue threshold, clear the
                 * CTF bit on mode_mask so saving doesn't ship stale
                 * info. */
                int red_n = 0, blue_n = 0;
                int fn = (int)arrlen(d->flags);
                for (int i = 0; i < fn; ++i) {
                    if (d->flags[i].team == 1) red_n++;
                    if (d->flags[i].team == 2) blue_n++;
                }
                if (red_n == 0 || blue_n == 0) {
                    d->meta.mode_mask &= (uint16_t)~(1u << 2);   /* CTF bit */
                }
                d->dirty = true;
            }
            break;
        }
        default: break;
    }
}

/* ---- Vtable ------------------------------------------------------- */

static void noop_drag   (EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    (void)d; (void)u; (void)c; (void)wx; (void)wy;
}
static void noop_release(EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy) {
    (void)d; (void)u; (void)c; (void)wx; (void)wy;
}
static bool noop_key(EditorDoc *d, UndoStack *u, ToolCtx *c, int key) {
    (void)d; (void)u; (void)c; (void)key; return false;
}
static void noop_overlay(const EditorDoc *d, const ToolCtx *c, const EditorView *v) {
    (void)d; (void)c; (void)v;
}

static const ToolVTable g_vtables[TOOL_COUNT] = {
    [TOOL_TILE] = {
        .on_press = tile_press, .on_drag = tile_drag, .on_release = tile_release,
        .draw_overlay = tile_overlay, .on_key = tile_key,
    },
    [TOOL_POLY] = {
        .on_press = poly_press, .on_drag = noop_drag, .on_release = noop_release,
        .draw_overlay = poly_overlay, .on_key = poly_key,
    },
    [TOOL_SPAWN] = {
        .on_press = spawn_press, .on_drag = noop_drag, .on_release = noop_release,
        .draw_overlay = spawn_overlay, .on_key = spawn_key,
    },
    [TOOL_PICKUP] = {
        .on_press = pickup_press, .on_drag = noop_drag, .on_release = noop_release,
        .draw_overlay = pickup_overlay, .on_key = pickup_key,
    },
    [TOOL_AMBI] = {
        .on_press = ambi_press, .on_drag = noop_drag, .on_release = ambi_release,
        .draw_overlay = ambi_overlay, .on_key = noop_key,
    },
    [TOOL_DECO] = {
        .on_press = deco_press, .on_drag = noop_drag, .on_release = noop_release,
        .draw_overlay = deco_overlay, .on_key = noop_key,
    },
    [TOOL_FLAG] = {
        .on_press = flag_press, .on_drag = noop_drag, .on_release = noop_release,
        .draw_overlay = flag_overlay, .on_key = flag_key,
    },
    [TOOL_META] = {
        .on_press = NULL,  /* meta is a modal — main.c handles it */
        .on_drag = noop_drag, .on_release = noop_release,
        .draw_overlay = noop_overlay, .on_key = noop_key,
    },
};

const ToolVTable *tool_vtables(void) { return g_vtables; }
