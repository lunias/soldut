#include "hud.h"

#include "mech.h"
#include "weapons.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <string.h>

static void draw_bar(int x, int y, int w, int h, float v, Color fg, Color bg) {
    DrawRectangle(x, y, w, h, bg);
    int fill = (int)(v * (float)w);
    if (fill > 0) DrawRectangle(x, y, fill, h, fg);
    DrawRectangleLines(x, y, w, h, (Color){255, 255, 255, 80});
}

static void draw_crosshair(Vec2 c, float bink_total) {
    float gap   = 6.0f + bink_total * 12.0f;
    float arm   = 8.0f;
    Color col   = (Color){255, 255, 255, 220};
    DrawLineEx((Vector2){c.x - gap - arm, c.y}, (Vector2){c.x - gap, c.y}, 1.5f, col);
    DrawLineEx((Vector2){c.x + gap, c.y}, (Vector2){c.x + gap + arm, c.y}, 1.5f, col);
    DrawLineEx((Vector2){c.x, c.y - gap - arm}, (Vector2){c.x, c.y - gap}, 1.5f, col);
    DrawLineEx((Vector2){c.x, c.y + gap}, (Vector2){c.x, c.y + gap + arm}, 1.5f, col);
    DrawCircleV(c, 1.5f, col);
}

/* ---- Kill feed ----------------------------------------------------- */

static void draw_kill_feed(const World *w, int screen_w) {
    int n = (w->killfeed_count < KILLFEED_CAPACITY)
            ? w->killfeed_count : KILLFEED_CAPACITY;
    int x = screen_w - 380;
    int y = 12;
    for (int i = 0; i < n; ++i) {
        /* Walk newest → oldest. */
        int slot = (w->killfeed_count - 1 - i) % KILLFEED_CAPACITY;
        if (slot < 0) slot += KILLFEED_CAPACITY;
        const KillFeedEntry *k = &w->killfeed[slot];
        if (k->age >= 6.0f) continue;
        unsigned char a = (unsigned char)((1.0f - (k->age / 6.0f)) * 235.0f);
        Color row_bg = (Color){ 0, 0, 0, (unsigned char)(a / 3) };
        DrawRectangle(x - 6, y - 2, 380, 22, row_bg);

        char shooter[32], victim[32];
        if (k->killer_mech_id < 0) snprintf(shooter, sizeof shooter, "world");
        else snprintf(shooter, sizeof shooter, "mech#%d", k->killer_mech_id);
        snprintf(victim, sizeof victim, "mech#%d", k->victim_mech_id);

        const char *wname = weapon_short_name(k->weapon_id);
        char tags[48] = {0};
        if (k->flags & KILLFLAG_HEADSHOT) strcat(tags, " HEADSHOT");
        if (k->flags & KILLFLAG_GIB)      strcat(tags, " GIB");
        if (k->flags & KILLFLAG_OVERKILL) strcat(tags, " OVERKILL");
        if (k->flags & KILLFLAG_RAGDOLL)  strcat(tags, " RAGDOLL");
        if (k->flags & KILLFLAG_SUICIDE)  strcat(tags, " SUICIDE");

        char line[128];
        if (k->flags & KILLFLAG_SUICIDE) {
            snprintf(line, sizeof line, "%s [%s]%s",
                     victim, wname, tags);
        } else {
            snprintf(line, sizeof line, "%s -[%s]-> %s%s",
                     shooter, wname, victim, tags);
        }
        DrawText(line, x, y, 16, (Color){240, 230, 220, a});
        y += 22;
    }
}

void hud_draw(const World *w, int screen_w, int screen_h, Vec2 cursor) {
    if (w->local_mech_id < 0 || w->local_mech_id >= w->mech_count) return;
    const Mech *m = &w->mechs[w->local_mech_id];
    const Weapon *wp = weapon_def(m->weapon_id);
    const Armor  *ar = armor_def(m->armor_id);

    /* Health (bottom-left). */
    int x = 16, y = screen_h - 60, bw = 240, bh = 18;
    DrawText(TextFormat("HP %d / %d", (int)m->health, (int)m->health_max),
             x, y - 22, 18, RAYWHITE);
    draw_bar(x, y, bw, bh, m->health / m->health_max,
             (Color){180, 40, 40, 230}, (Color){40, 0, 0, 200});

    /* Armor bar above HP, only if any. */
    if (m->armor_hp_max > 0.0f) {
        int ay = y - 26;
        draw_bar(x, ay, bw, 8, m->armor_hp / m->armor_hp_max,
                 (Color){80, 160, 220, 230}, (Color){10, 30, 50, 200});
        DrawText(TextFormat("%s %d", ar->name, (int)m->armor_hp),
                 x + bw + 8, ay - 2, 14, (Color){180, 220, 255, 230});
    }

    /* Jet fuel — narrow vertical bar on the left. */
    int fx_ = 4, fy_ = screen_h - 280, fw = 8, fh = 200;
    float fuel_t = m->fuel_max > 0.0f ? (m->fuel / m->fuel_max) : 0.0f;
    DrawRectangle(fx_, fy_, fw, fh, (Color){0, 20, 30, 220});
    int fill = (int)(fuel_t * (float)fh);
    DrawRectangle(fx_, fy_ + (fh - fill), fw, fill, (Color){80, 220, 240, 230});

    /* Ammo + weapon (bottom-right). Show both slot and active marker. */
    int ax = screen_w - 280;
    int ay = screen_h - 60;
    if (wp) {
        const char *line = m->reload_timer > 0.0f
                           ? "RELOADING…"
                           : (wp->mag_size > 0
                              ? TextFormat("%2d / %d", m->ammo, m->ammo_max)
                              : (wp->fire_rate_sec > 0
                                 ? "READY"
                                 : "—"));
        DrawText(TextFormat("[%c] %s", m->active_slot == 0 ? '1' : '2', wp->name),
                 ax, ay - 22, 18, RAYWHITE);
        DrawText(line, ax, ay, 22, m->ammo == 0 && wp->mag_size > 0 ? RED : RAYWHITE);
    }

    /* Show the inactive slot beneath in dim gray. */
    int other_id = (m->active_slot == 0) ? m->secondary_id : m->primary_id;
    int other_ammo = (m->active_slot == 0) ? m->ammo_secondary : m->ammo_primary;
    const Weapon *ow = weapon_def(other_id);
    if (ow) {
        DrawText(TextFormat("[%c] %s  %d/%d",
                            m->active_slot == 0 ? '2' : '1',
                            ow->name, other_ammo, ow->mag_size),
                 ax, ay + 24, 14, (Color){180, 180, 180, 200});
    }

    /* Crosshair — bink scales with recoil_kick + accumulated aim_bink. */
    float bink_total = m->recoil_kick + 6.0f * (m->aim_bink < 0 ? -m->aim_bink : m->aim_bink);
    if (bink_total > 1.0f) bink_total = 1.0f;
    draw_crosshair(cursor, bink_total);

    /* P05 — active-powerup indicator (top-center). Removes the "I picked
     * something up but I don't know what's happening" ambiguity that the
     * spec's deferred-to-M6 HUD popup would otherwise solve. Show one
     * pill per active powerup with the remaining seconds; on the local
     * mech the timer is server-authoritative (or sentinel-ticked from
     * snapshot bits if we're a pure client). */
    {
        int px = screen_w / 2 - 200;
        int py = 12;
        if (m->powerup_berserk_remaining > 0.0f) {
            const char *label = TextFormat("BERSERK %.1fs", (double)m->powerup_berserk_remaining);
            int tw = MeasureText(label, 18);
            DrawRectangle(px - 8, py - 4, tw + 16, 26, (Color){80, 0, 20, 220});
            DrawText(label, px, py, 18, (Color){255, 200, 200, 240});
            px += tw + 24;
        }
        if (m->powerup_invis_remaining > 0.0f) {
            const char *label = TextFormat("INVIS %.1fs", (double)m->powerup_invis_remaining);
            int tw = MeasureText(label, 18);
            DrawRectangle(px - 8, py - 4, tw + 16, 26, (Color){0, 30, 60, 220});
            DrawText(label, px, py, 18, (Color){180, 220, 255, 240});
            px += tw + 24;
        }
        if (m->powerup_godmode_remaining > 0.0f) {
            const char *label = TextFormat("GODMODE %.1fs", (double)m->powerup_godmode_remaining);
            int tw = MeasureText(label, 18);
            DrawRectangle(px - 8, py - 4, tw + 16, 26, (Color){60, 50, 0, 220});
            DrawText(label, px, py, 18, (Color){255, 230, 160, 240});
        }
    }

    /* Kill feed (top-right). */
    draw_kill_feed(w, screen_w);

    /* Single legacy-style banner for the most recent kill (centered top).
     * Useful for shot-mode reviews where the multi-row feed is small. */
    if (w->last_event[0] && w->last_event_time < 4.0f) {
        int wpx = MeasureText(w->last_event, 18);
        int kx = (screen_w - wpx) / 2;
        int ky = screen_h - 100;
        unsigned char a = (unsigned char)((1.0f - (w->last_event_time / 4.0f)) * 220.0f);
        DrawText(w->last_event, kx, ky, 18, (Color){255, 230, 220, a});
    }
}
