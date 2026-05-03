#include "hud.h"

#include "mech.h"
#include "weapons.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>

static void draw_bar(int x, int y, int w, int h, float v, Color fg, Color bg) {
    DrawRectangle(x, y, w, h, bg);
    int fill = (int)(v * (float)w);
    if (fill > 0) DrawRectangle(x, y, fill, h, fg);
    DrawRectangleLines(x, y, w, h, (Color){255, 255, 255, 80});
}

static void draw_crosshair(Vec2 c, float bink) {
    float gap   = 6.0f + bink * 12.0f;
    float arm   = 8.0f;
    Color col   = (Color){255, 255, 255, 220};
    DrawLineEx((Vector2){c.x - gap - arm, c.y}, (Vector2){c.x - gap, c.y}, 1.5f, col);
    DrawLineEx((Vector2){c.x + gap, c.y}, (Vector2){c.x + gap + arm, c.y}, 1.5f, col);
    DrawLineEx((Vector2){c.x, c.y - gap - arm}, (Vector2){c.x, c.y - gap}, 1.5f, col);
    DrawLineEx((Vector2){c.x, c.y + gap}, (Vector2){c.x, c.y + gap + arm}, 1.5f, col);
    /* Center dot. */
    DrawCircleV(c, 1.5f, col);
}

void hud_draw(const World *w, int screen_w, int screen_h, Vec2 cursor) {
    if (w->local_mech_id < 0 || w->local_mech_id >= w->mech_count) return;
    const Mech *m = &w->mechs[w->local_mech_id];
    const Weapon *wp = weapon_def(m->weapon_id);

    /* Health (bottom-left). */
    int x = 16, y = screen_h - 60, bw = 240, bh = 18;
    DrawText(TextFormat("HP %d / %d", (int)m->health, (int)m->health_max),
             x, y - 22, 18, RAYWHITE);
    draw_bar(x, y, bw, bh, m->health / m->health_max,
             (Color){180, 40, 40, 230}, (Color){40, 0, 0, 200});

    /* Jet fuel — narrow vertical bar on the left. */
    int fx_ = 4, fy_ = screen_h - 280, fw = 8, fh = 200;
    float fuel_t = m->fuel_max > 0.0f ? (m->fuel / m->fuel_max) : 0.0f;
    DrawRectangle(fx_, fy_, fw, fh, (Color){0, 20, 30, 220});
    int fill = (int)(fuel_t * (float)fh);
    DrawRectangle(fx_, fy_ + (fh - fill), fw, fill, (Color){80, 220, 240, 230});

    /* Ammo + weapon (bottom-right). */
    if (wp) {
        const char *ammo_line = m->reload_timer > 0.0f ? "RELOADING…"
                                                       : TextFormat("%2d / %d", m->ammo, m->ammo_max);
        int ax = screen_w - 220;
        int ay = screen_h - 60;
        DrawText(wp->name, ax, ay - 22, 18, RAYWHITE);
        DrawText(ammo_line, ax, ay, 22, m->ammo == 0 ? RED : RAYWHITE);
    }

    /* Crosshair — bink scales with recoil_kick, decays per tick in mech_step. */
    draw_crosshair(cursor, m->recoil_kick);

    /* Kill feed (single line, top-right). */
    if (w->last_event[0] && w->last_event_time < 4.0f) {
        int wpx = MeasureText(w->last_event, 18);
        int kx = screen_w - wpx - 16;
        int ky = 16;
        unsigned char a = (unsigned char)((1.0f - (w->last_event_time / 4.0f)) * 220.0f);
        DrawText(w->last_event, kx, ky, 18, (Color){255, 230, 220, a});
    }
}
