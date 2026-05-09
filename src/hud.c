#include "hud.h"

#include "ctf.h"
#include "log.h"
#include "match.h"
#include "mech.h"
#include "platform.h"
#include "weapons.h"

#include "../third_party/raylib/src/raylib.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* P13 — HUD atlas. Single shared texture; lazy-loaded on first hud_draw.
 * Until P15/P16 ships `assets/ui/hud.png` the atlas stays unloaded and
 * every sprite-aware draw falls back to its M4 primitive form. The
 * atlas layout (per `documents/m5/08-rendering.md` §"HUD final art") is
 * 256x256 with weapon / killflag / crosshair / bar pieces packed via
 * the table below. */
static Texture2D g_hud_atlas = {0};
static bool      g_hud_atlas_attempted = false;

static void hud_atlas_load_once(void) {
    if (g_hud_atlas_attempted) return;
    g_hud_atlas_attempted = true;
    const char *path = "assets/ui/hud.png";
    if (!FileExists(path)) {
        LOG_I("hud: %s missing; bars + crosshair use primitive fallback", path);
        return;
    }
    Texture2D t = LoadTexture(path);
    if (t.id == 0) { LOG_W("hud: LoadTexture(%s) failed", path); return; }
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    g_hud_atlas = t;
    LOG_I("hud: atlas loaded (%dx%d)", t.width, t.height);
}

void hud_atlas_unload(void) {
    if (g_hud_atlas.id != 0) UnloadTexture(g_hud_atlas);
    g_hud_atlas = (Texture2D){0};
    g_hud_atlas_attempted = false;
}

/* Sub-rect table — single shared 256x256 atlas. Each rect is sized to
 * what the asset spec calls for; if the atlas fails to load, callers
 * fall through to flat-fill / line-crosshair. The fixed origin (0,0)
 * for crosshair sprites means we offset by (-w/2, -h/2) at draw time. */
typedef struct { Rectangle src; } HudSrc;
static const HudSrc g_hud_src_crosshair    = { { 16,  16, 24, 24 } };
/* Bar outer/fill sub-rects are reserved for an atlas-aware bar style
 * once `assets/ui/hud.png` ships at P15/P16; current draw_bar_v2
 * paints primitives regardless of atlas, since the spec's outline +
 * tick layout is the same shape either way. */

/* Weapon icons start at y=128, 16 px each, in WeaponId order. The
 * atlas index is `g_hud_src_weapon_icon_x0 + 16 * id` modulo 16 across
 * the row, wrapping to the next row. We hash the slot by id directly
 * for the placeholder and the real atlas honors the same packing. */
static Rectangle hud_src_weapon_icon(int weapon_id) {
    int col = (weapon_id & 0x0F);
    int row = (weapon_id >> 4) & 0x0F;
    return (Rectangle){ (float)(col * 16), (float)(128 + row * 16), 16, 16 };
}

/* Kill-flag icons sit on the row below weapons (y=160). 16x16 each. */
static Rectangle hud_src_killflag_icon(int slot) {
    return (Rectangle){ (float)(slot * 16), 160.0f, 16, 16 };
}

/* P13 — Atlas-aware bar. Outline + dark background fill + fg fill +
 * tick marks every 10% (legibility at distance). The four pieces all
 * fall back to flat DrawRectangle when the atlas isn't loaded — same
 * shape, slightly different texture pass. Spec:
 * `documents/m5/08-rendering.md` §"Bar style". */
static void draw_bar_v2(int x, int y, int w, int h, float v, Color fg) {
    /* 1px outline so the bar reads against any background. */
    DrawRectangleLines(x - 1, y - 1, w + 2, h + 2, (Color){0, 0, 0, 200});
    /* Dark background fill. */
    DrawRectangle(x, y, w, h, (Color){10, 12, 16, 200});
    /* Foreground fill. */
    int fill = (int)(v * (float)w);
    if (fill < 0) fill = 0;
    if (fill > w) fill = w;
    if (fill > 0) DrawRectangle(x, y, fill, h, fg);
    /* Tick marks every 10% — 1-px dark separators that read at distance. */
    for (int i = 1; i < 10; ++i) {
        DrawRectangle(x + (w * i / 10), y, 1, h, (Color){0, 0, 0, 100});
    }
}

/* P13 — Atlas-aware crosshair. Tints by bink (`red` > 0.5, pale-cyan
 * otherwise) per the spec. Falls back to the M4 line cross when the
 * atlas isn't loaded — but the bink-driven gap + the new tint colors
 * apply to both paths so player feedback is consistent. */
static void draw_crosshair(Vec2 c, float bink_total) {
    float gap = 6.0f + bink_total * 12.0f;
    Color col = (bink_total > 0.5f)
              ? (Color){255,  80,  80, 255}    /* red — very-shaky */
              : (Color){200, 240, 255, 255};   /* pale cyan otherwise */

    if (g_hud_atlas.id != 0) {
        Rectangle src = g_hud_src_crosshair.src;
        float w = 24.0f + gap;
        float h = 24.0f + gap;
        DrawTexturePro(g_hud_atlas, src,
            (Rectangle){ c.x, c.y, w, h },
            (Vector2){ w * 0.5f, h * 0.5f }, 0.0f, col);
        return;
    }
    /* Fallback: M4 line cross with the new gap + tint. */
    float arm = 8.0f;
    DrawLineEx((Vector2){c.x - gap - arm, c.y}, (Vector2){c.x - gap, c.y}, 1.5f, col);
    DrawLineEx((Vector2){c.x + gap, c.y}, (Vector2){c.x + gap + arm, c.y}, 1.5f, col);
    DrawLineEx((Vector2){c.x, c.y - gap - arm}, (Vector2){c.x, c.y - gap}, 1.5f, col);
    DrawLineEx((Vector2){c.x, c.y + gap}, (Vector2){c.x, c.y + gap + arm}, 1.5f, col);
    DrawCircleV(c, 1.5f, col);
}

/* Draw a 16x16 weapon icon at (x, y) tinted by `tint`. Falls back to
 * a small color swatch (per-weapon hashed) when the atlas isn't
 * loaded so kill-feed entries stay visually distinct. */
static void hud_draw_weapon_icon(int weapon_id, int x, int y, Color tint) {
    if (g_hud_atlas.id != 0) {
        DrawTexturePro(g_hud_atlas, hud_src_weapon_icon(weapon_id),
            (Rectangle){ (float)x, (float)y, 16, 16 },
            (Vector2){0, 0}, 0.0f, tint);
        return;
    }
    /* No-atlas fallback: a small filled square so the row reads as
     * "this is a weapon glyph slot". The hash gives a stable color
     * per weapon_id so the player can tell weapons apart. */
    uint32_t h = (uint32_t)weapon_id * 2654435761u;
    Color swatch = {
        (uint8_t)(120 + (h & 0x7F)),
        (uint8_t)(120 + ((h >> 8) & 0x7F)),
        (uint8_t)(120 + ((h >> 16) & 0x7F)),
        tint.a,
    };
    DrawRectangle(x, y, 14, 14, swatch);
    DrawRectangleLines(x - 1, y - 1, 16, 16, (Color){0, 0, 0, 200});
}

/* P13 — kill-flag pictograms. Slot order matches the bit shift in
 * `KillFlag` (HEADSHOT=0, GIB=1, OVERKILL=2, RAGDOLL=3, SUICIDE=4). */
static void hud_draw_killflag_icon(int slot, int x, int y, Color tint) {
    if (g_hud_atlas.id != 0) {
        DrawTexturePro(g_hud_atlas, hud_src_killflag_icon(slot),
            (Rectangle){ (float)x, (float)y, 16, 16 },
            (Vector2){0, 0}, 0.0f, tint);
        return;
    }
    /* No-atlas fallback: short text label per slot. */
    static const char *labels[] = { "HS", "GIB", "OK", "RAG", "SUI" };
    const char *s = (slot >= 0 && slot < 5) ? labels[slot] : "?";
    DrawTextEx(ui_font_for(UI_FONT_MONO), s,
               (Vector2){ (float)x, (float)y }, 14.0f, 1.0f, tint);
}

/* ---- Kill feed ----------------------------------------------------- */

/* P13 — Kill feed with weapon-icon + flag-icon variants from the HUD
 * atlas. Layout per row (left → right):
 *   [shooter name]  [16x16 weapon icon]  [victim name]  [16x16 flag icons]
 * Each row's alpha decays linearly to zero over ~6 s. When the atlas
 * isn't loaded the icons fall back to color swatches / short labels so
 * the row stays parseable. The text uses the new VG5000 display font
 * via UI_FONT_DISPLAY for the "kill feed flag chips" voice the spec
 * calls out in `documents/m5/11-art-direction.md` §"Fonts". */
static void draw_kill_feed(const World *w, int screen_w) {
    int n = (w->killfeed_count < KILLFEED_CAPACITY)
            ? w->killfeed_count : KILLFEED_CAPACITY;
    int row_h = 22;
    int x = screen_w - 420;
    int y = 12;
    Font name_font = ui_font_for(UI_FONT_BODY);
    Font flag_font = ui_font_for(UI_FONT_DISPLAY);
    for (int i = 0; i < n; ++i) {
        /* Walk newest → oldest. */
        int slot = (w->killfeed_count - 1 - i) % KILLFEED_CAPACITY;
        if (slot < 0) slot += KILLFEED_CAPACITY;
        const KillFeedEntry *k = &w->killfeed[slot];
        if (k->age >= 6.0f) continue;
        unsigned char a = (unsigned char)((1.0f - (k->age / 6.0f)) * 235.0f);
        Color row_bg = (Color){ 0, 0, 0, (unsigned char)(a / 3) };
        DrawRectangle(x - 6, y - 2, 420, row_h, row_bg);

        char shooter[32], victim[32];
        if (k->killer_mech_id < 0) snprintf(shooter, sizeof shooter, "world");
        else snprintf(shooter, sizeof shooter, "mech#%d", k->killer_mech_id);
        snprintf(victim, sizeof victim, "mech#%d", k->victim_mech_id);

        Color text_col = (Color){240, 230, 220, a};
        Color icon_tint = (Color){255, 255, 255, a};

        int cx = x;
        if (!(k->flags & KILLFLAG_SUICIDE)) {
            DrawTextEx(name_font, shooter, (Vector2){(float)cx, (float)y},
                       16.0f, 1.0f, text_col);
            cx += (int)MeasureTextEx(name_font, shooter, 16.0f, 1.0f).x + 8;
            hud_draw_weapon_icon(k->weapon_id, cx, y + 2, icon_tint);
            cx += 22;
            DrawTextEx(name_font, victim, (Vector2){(float)cx, (float)y},
                       16.0f, 1.0f, text_col);
            cx += (int)MeasureTextEx(name_font, victim, 16.0f, 1.0f).x + 8;
        } else {
            /* Suicide: weapon icon → "[victim]" — no shooter chip. */
            hud_draw_weapon_icon(k->weapon_id, cx, y + 2, icon_tint);
            cx += 22;
            DrawTextEx(name_font, victim, (Vector2){(float)cx, (float)y},
                       16.0f, 1.0f, text_col);
            cx += (int)MeasureTextEx(name_font, victim, 16.0f, 1.0f).x + 8;
        }

        /* Flag icons in bit order, drawn as 16x16 atlas sub-rects (or
         * short labels in the fallback). */
        static const struct { uint32_t bit; int slot; } flag_slots[] = {
            { KILLFLAG_HEADSHOT, 0 },
            { KILLFLAG_GIB,      1 },
            { KILLFLAG_OVERKILL, 2 },
            { KILLFLAG_RAGDOLL,  3 },
            { KILLFLAG_SUICIDE,  4 },
        };
        for (size_t fi = 0; fi < sizeof flag_slots / sizeof flag_slots[0]; ++fi) {
            if (!(k->flags & flag_slots[fi].bit)) continue;
            hud_draw_killflag_icon(flag_slots[fi].slot, cx, y + 2, icon_tint);
            cx += 18;
        }
        (void)flag_font;  /* reserved for the M6 HUD-final pass */

        y += row_h;
    }
}

/* P07 — CTF: per-flag corner pip + off-screen compass arrow. The pip
 * tells you "Red flag is HOME / CARRIED / DROPPED" at a glance; the
 * arrow points toward the flag when it's off-screen, with a small
 * inset proportional to distance (further = closer to the corner).
 *
 * Layout: Red flag in the top-left, Blue flag in the top-right. The
 * pip is a 24-px square; status is encoded by fill style:
 *   HOME    — solid color
 *   CARRIED — pulsing alpha (sin-driven, 1 Hz)
 *   DROPPED — outline-only with a colored center dot (urgent)
 */
static void draw_flag_pips(const World *w, int sw, int sh) {
    if (w->flag_count == 0) return;
    const int box = 24;
    const int margin = 16;
    for (int f = 0; f < w->flag_count; ++f) {
        const Flag *fl = &w->flags[f];
        Color tc = (fl->team == MATCH_TEAM_RED)
                  ? (Color){220,  80,  80, 255}
                  : (Color){ 80, 140, 220, 255};
        int x = (fl->team == MATCH_TEAM_RED) ? margin : (sw - margin - box);
        int y = margin;
        switch ((FlagStatus)fl->status) {
            case FLAG_HOME:
                DrawRectangle(x, y, box, box, tc);
                DrawRectangleLines(x - 1, y - 1, box + 2, box + 2, BLACK);
                break;
            case FLAG_CARRIED: {
                float pulse = 0.6f + 0.4f * sinf((float)w->tick * 0.15f);
                Color cc = (Color){tc.r, tc.g, tc.b, (uint8_t)(255.0f * pulse)};
                DrawRectangle(x, y, box, box, cc);
                DrawRectangleLines(x - 1, y - 1, box + 2, box + 2, RAYWHITE);
                break;
            }
            case FLAG_DROPPED: {
                DrawRectangleLinesEx((Rectangle){(float)x, (float)y, (float)box, (float)box},
                                     2.0f, tc);
                DrawRectangle(x + box/4, y + box/4, box/2, box/2, tc);
                break;
            }
        }
        const char *label = (fl->team == MATCH_TEAM_RED) ? "RED" : "BLU";
        DrawTextEx(ui_font_for(UI_FONT_DISPLAY), label,
                   (Vector2){(float)x, (float)(y + box + 4)},
                   14.0f, 1.0f, tc);
    }
}

/* For each flag whose world-space position is outside the viewport,
 * draw a small triangle at the nearest screen edge pointing toward it.
 * Distance is encoded as inward-inset: closer flags pull the arrow
 * deeper into the screen. */
static void draw_flag_compass(const World *w, int sw, int sh, Camera2D cam) {
    if (w->flag_count == 0) return;
    /* Inset a bit so the arrow doesn't sit flush against the edge — the
     * pip layer takes the corners. */
    const float edge_pad   = 28.0f;
    const float arrow_len  = 14.0f;
    const float arrow_wide = 9.0f;
    const float center_x   = (float)sw * 0.5f;
    const float center_y   = (float)sh * 0.5f;
    for (int f = 0; f < w->flag_count; ++f) {
        const Flag *fl = &w->flags[f];
        Vec2 fp = ctf_flag_position(w, f);
        Vector2 sp = GetWorldToScreen2D((Vector2){ fp.x, fp.y }, cam);
        if (sp.x >= edge_pad && sp.x < (float)sw - edge_pad &&
            sp.y >= edge_pad && sp.y < (float)sh - edge_pad) continue;

        /* Direction from screen center to projected flag. Normalize to
         * pick a unit vector, then clip the line at the screen-edge
         * inset rectangle to find the arrow position. */
        float dx = sp.x - center_x;
        float dy = sp.y - center_y;
        float L  = sqrtf(dx * dx + dy * dy);
        if (L < 0.001f) continue;
        dx /= L; dy /= L;
        /* Time-to-edge along x and y; pick the smaller (the side we hit). */
        float halfw = (float)sw * 0.5f - edge_pad;
        float halfh = (float)sh * 0.5f - edge_pad;
        float tx = (dx != 0.0f) ? halfw / fabsf(dx) :  1.0e9f;
        float ty = (dy != 0.0f) ? halfh / fabsf(dy) :  1.0e9f;
        float t  = (tx < ty) ? tx : ty;
        float ax = center_x + dx * t;
        float ay = center_y + dy * t;
        /* Triangle: tip toward the flag, base perpendicular. */
        float px = -dy, py = dx;            /* perpendicular */
        Vector2 tip = (Vector2){ ax + dx * arrow_len * 0.5f,
                                 ay + dy * arrow_len * 0.5f };
        Vector2 b1  = (Vector2){ ax - dx * arrow_len * 0.5f + px * arrow_wide * 0.5f,
                                 ay - dy * arrow_len * 0.5f + py * arrow_wide * 0.5f };
        Vector2 b2  = (Vector2){ ax - dx * arrow_len * 0.5f - px * arrow_wide * 0.5f,
                                 ay - dy * arrow_len * 0.5f - py * arrow_wide * 0.5f };
        Color tc = (fl->team == MATCH_TEAM_RED)
                  ? (Color){220,  80,  80, 230}
                  : (Color){ 80, 140, 220, 230};
        /* DrawTriangle is CCW. */
        DrawTriangle(tip, b1, b2, tc);
    }
}

void hud_draw(const World *w, int screen_w, int screen_h, Vec2 cursor,
              Camera2D camera) {
    /* P13 — lazy-load HUD atlas on first draw. _attempted flag makes
     * subsequent calls a single branch. */
    hud_atlas_load_once();

    /* CTF pips + compass don't depend on a local mech (spectators / dead
     * players still want to see the score state), so draw them first. */
    draw_flag_pips   (w, screen_w, screen_h);
    draw_flag_compass(w, screen_w, screen_h, camera);

    if (w->local_mech_id < 0 || w->local_mech_id >= w->mech_count) return;
    const Mech *m = &w->mechs[w->local_mech_id];
    const Weapon *wp = weapon_def(m->weapon_id);
    const Armor  *ar = armor_def(m->armor_id);

    /* Numerics use Steps Mono (mono digits don't shimmer when health
     * changes); body text uses Atkinson. The atlas-aware bar style
     * adds ticks + outline + 1px shadow per the M5 spec. */
    Font font_mono = ui_font_for(UI_FONT_MONO);
    Font font_body = ui_font_for(UI_FONT_BODY);

    /* Health (bottom-left). */
    int x = 16, y = screen_h - 60, bw = 240, bh = 18;
    DrawTextEx(font_mono, TextFormat("HP %d / %d",
                                     (int)m->health, (int)m->health_max),
               (Vector2){(float)x, (float)(y - 22)}, 18.0f, 1.0f, RAYWHITE);
    draw_bar_v2(x, y, bw, bh, m->health / m->health_max,
                (Color){180, 40, 40, 230});

    /* Armor bar above HP, only if any. */
    if (m->armor_hp_max > 0.0f) {
        int ay = y - 26;
        draw_bar_v2(x, ay, bw, 8, m->armor_hp / m->armor_hp_max,
                    (Color){80, 160, 220, 230});
        DrawTextEx(font_body, TextFormat("%s %d", ar->name, (int)m->armor_hp),
                   (Vector2){(float)(x + bw + 8), (float)(ay - 2)},
                   14.0f, 1.0f, (Color){180, 220, 255, 230});
    }

    /* Jet fuel — narrow vertical bar on the left. */
    int fx_ = 4, fy_ = screen_h - 280, fw = 8, fh = 200;
    float fuel_t = m->fuel_max > 0.0f ? (m->fuel / m->fuel_max) : 0.0f;
    DrawRectangleLines(fx_ - 1, fy_ - 1, fw + 2, fh + 2, (Color){0, 0, 0, 200});
    DrawRectangle(fx_, fy_, fw, fh, (Color){0, 20, 30, 220});
    int fill = (int)(fuel_t * (float)fh);
    DrawRectangle(fx_, fy_ + (fh - fill), fw, fill, (Color){80, 220, 240, 230});

    /* Ammo + weapon (bottom-right). Show both slot and active marker.
     * Active weapon's icon goes immediately left of the name; the
     * inactive slot's row gets a dimmed tint. */
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
        hud_draw_weapon_icon(m->weapon_id, ax, ay - 22, RAYWHITE);
        DrawTextEx(font_body,
                   TextFormat("[%c] %s",
                              m->active_slot == 0 ? '1' : '2', wp->name),
                   (Vector2){(float)(ax + 22), (float)(ay - 22)},
                   18.0f, 1.0f, RAYWHITE);
        Color ammo_col = (m->ammo == 0 && wp->mag_size > 0)
                       ? (Color){255, 80, 80, 255} : RAYWHITE;
        DrawTextEx(font_mono, line, (Vector2){(float)ax, (float)ay},
                   22.0f, 1.0f, ammo_col);
    }

    /* Inactive slot beneath, dimmed. */
    int other_id = (m->active_slot == 0) ? m->secondary_id : m->primary_id;
    int other_ammo = (m->active_slot == 0) ? m->ammo_secondary : m->ammo_primary;
    const Weapon *ow = weapon_def(other_id);
    if (ow) {
        Color dim = {180, 180, 180, 200};
        hud_draw_weapon_icon(other_id, ax, ay + 24, dim);
        DrawTextEx(font_body,
                   TextFormat("[%c] %s  %d/%d",
                              m->active_slot == 0 ? '2' : '1',
                              ow->name, other_ammo, ow->mag_size),
                   (Vector2){(float)(ax + 22), (float)(ay + 24)},
                   14.0f, 1.0f, dim);
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
            int tw = (int)MeasureTextEx(font_body, label, 18.0f, 1.0f).x;
            DrawRectangle(px - 8, py - 4, tw + 16, 26, (Color){80, 0, 20, 220});
            DrawTextEx(font_body, label, (Vector2){(float)px, (float)py},
                       18.0f, 1.0f, (Color){255, 200, 200, 240});
            px += tw + 24;
        }
        if (m->powerup_invis_remaining > 0.0f) {
            const char *label = TextFormat("INVIS %.1fs", (double)m->powerup_invis_remaining);
            int tw = (int)MeasureTextEx(font_body, label, 18.0f, 1.0f).x;
            DrawRectangle(px - 8, py - 4, tw + 16, 26, (Color){0, 30, 60, 220});
            DrawTextEx(font_body, label, (Vector2){(float)px, (float)py},
                       18.0f, 1.0f, (Color){180, 220, 255, 240});
            px += tw + 24;
        }
        if (m->powerup_godmode_remaining > 0.0f) {
            const char *label = TextFormat("GODMODE %.1fs", (double)m->powerup_godmode_remaining);
            int tw = (int)MeasureTextEx(font_body, label, 18.0f, 1.0f).x;
            DrawRectangle(px - 8, py - 4, tw + 16, 26, (Color){60, 50, 0, 220});
            DrawTextEx(font_body, label, (Vector2){(float)px, (float)py},
                       18.0f, 1.0f, (Color){255, 230, 160, 240});
        }
    }

    /* Kill feed (top-right). */
    draw_kill_feed(w, screen_w);

    /* Single legacy-style banner for the most recent kill (centered top).
     * Useful for shot-mode reviews where the multi-row feed is small. */
    if (w->last_event[0] && w->last_event_time < 4.0f) {
        int wpx = (int)MeasureTextEx(font_body, w->last_event, 18.0f, 1.0f).x;
        int kx = (screen_w - wpx) / 2;
        int ky = screen_h - 100;
        unsigned char a = (unsigned char)((1.0f - (w->last_event_time / 4.0f)) * 220.0f);
        DrawTextEx(font_body, w->last_event,
                   (Vector2){(float)kx, (float)ky},
                   18.0f, 1.0f, (Color){255, 230, 220, a});
    }
}
