#include "lobby_ui.h"

#include "config.h"
#include "game.h"
#include "log.h"
#include "lobby.h"
#include "maps.h"
#include "match.h"
#include "mech.h"
#include "net.h"
#include "ui.h"
#include "version.h"
#include "weapons.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Layout-scale helper ------------------------------------------
 *
 * S(n) scales a layout dimension (px / size / spacing) by the current
 * UI scale. Wrap every hardcoded number that should grow with the
 * screen — buttons, gaps, font sizes that aren't already going through
 * ui_draw_text. (ui_draw_text + ui_button etc. already scale text
 * sizes internally.)
 *
 * Pattern: `int bw = S(320), bh = S(48);` instead of raw constants.
 */
#define S(n) ((int)((float)(n) * L->ui.scale + 0.5f))

/* ---- Small helpers ------------------------------------------------ */

static Color team_color(int team) {
    switch (team) {
        case MATCH_TEAM_RED:  return (Color){220, 80,  80,  255};
        case MATCH_TEAM_BLUE: return (Color){ 80, 140, 220, 255};
        default:              return (Color){200, 200, 200, 255};
    }
}

/* Team display is mode-aware. In FFA every slot has team=
 * MATCH_TEAM_FFA which is the same numeric value as MATCH_TEAM_RED;
 * showing "Red" there is misleading. Pass the current match mode so
 * the label reflects what the player actually is. */
static const char *team_name_for_mode(int team, int mode) {
    if (mode == MATCH_MODE_FFA) {
        return (team == MATCH_TEAM_NONE) ? "Spec" : "FFA";
    }
    switch (team) {
        case MATCH_TEAM_RED:  return "Red";
        case MATCH_TEAM_BLUE: return "Blue";
        case MATCH_TEAM_NONE: return "Spec";
        default:              return "?";
    }
}

/* Legacy two-arg name kept for paths that don't have a mode handy
 * (kill feed, summary). Defaults to TDM/CTF labelling. */
static const char *team_name(int team) {
    switch (team) {
        case MATCH_TEAM_RED:  return "Red";
        case MATCH_TEAM_BLUE: return "Blue";
        case MATCH_TEAM_NONE: return "Spec";
        default:              return "?";
    }
}

static Color team_color_for_mode(int team, int mode) {
    if (mode == MATCH_MODE_FFA) {
        /* No team colour in FFA — gray chip everywhere. */
        return (team == MATCH_TEAM_NONE)
               ? (Color){120, 120, 130, 255}
               : (Color){170, 170, 180, 255};
    }
    return team_color(team);
}

void lobby_ui_init(LobbyUIState *L) {
    if (!L) return;
    memset(L, 0, sizeof *L);
    snprintf(L->player_name, sizeof L->player_name, "player");
    L->lobby_chassis   = CHASSIS_TROOPER;
    L->lobby_primary   = WEAPON_PULSE_RIFLE;
    L->lobby_secondary = WEAPON_SIDEARM;
    L->lobby_armor     = ARMOR_LIGHT;
    L->lobby_jet       = JET_STANDARD;
    L->lobby_team      = MATCH_TEAM_FFA;
    L->browser_refresh_timer = 0.0f;
    L->kick_target_slot = -1;
    L->ban_target_slot  = -1;
    snprintf(L->connect_addr, sizeof L->connect_addr, "127.0.0.1:%u",
             (unsigned)SOLDUT_DEFAULT_PORT);
}

/* ---- Title screen ------------------------------------------------- */

void title_screen_run(LobbyUIState *L, Game *g, int sw, int sh) {
    Vec2 mouse = (Vec2){ (float)GetMouseX(), (float)GetMouseY() };
    ui_begin(&L->ui, mouse, GetFrameTime(), sh);

    ClearBackground((Color){8, 10, 14, 255});

    /* Big banner. Font sizes are BASE values; ui_draw_text multiplies
     * by ui.scale internally so they grow correctly at 4K. */
    const char *title = "SOLDUT";
    int big_base = 84;
    int tw = ui_measure(&L->ui, title, big_base);
    int big_y = sh / 6;
    ui_draw_text(&L->ui, title, (sw - tw) / 2, big_y, big_base,
                 (Color){80, 180, 255, 255});

    int sub_base = 18;
    int sub_w = ui_measure(&L->ui, "M4 — lobby & matches", sub_base);
    int sub_y = big_y + (int)((float)big_base * L->ui.scale + S(8));
    ui_draw_text(&L->ui, "M4 — lobby & matches",
                 (sw - sub_w) / 2, sub_y, sub_base,
                 (Color){160, 160, 170, 255});

    int ver_base = 14;
    ui_draw_text(&L->ui, SOLDUT_VERSION_STRING, S(12),
                 sh - S(28), ver_base, (Color){90, 100, 120, 255});

    /* Stack of buttons. */
    int bw = S(360), bh = S(56), gap = S(14);
    int by = sh / 2 - (bh * 5 + gap * 4) / 2;
    int bx = (sw - bw) / 2;

    if (ui_button(&L->ui, (Rectangle){bx, by, bw, bh}, "Single Player", true)) {
        L->request_single_player = true;
    } by += bh + gap;
    if (ui_button(&L->ui, (Rectangle){bx, by, bw, bh}, "Host Server", true)) {
        /* Goes to the host-setup screen first; the server is created
         * only after the user confirms mode/map/limits there. */
        L->setup_initialized = false;       /* re-seed defaults */
        g->mode = MODE_HOST_SETUP;
    } by += bh + gap;
    if (ui_button(&L->ui, (Rectangle){bx, by, bw, bh}, "Browse Servers (LAN)", true)) {
        L->request_browse = true;
    } by += bh + gap;
    if (ui_button(&L->ui, (Rectangle){bx, by, bw, bh}, "Direct Connect", true)) {
        g->mode = MODE_CONNECT;
    } by += bh + gap;
    if (ui_button(&L->ui, (Rectangle){bx, by, bw, bh}, "Quit", true)) {
        L->request_quit = true;
    } by += bh + gap;

    /* Display-name field at the bottom. */
    int nw = S(360), nh = S(40);
    int ny = sh - S(72);
    int nx = (sw - nw) / 2;
    ui_draw_text(&L->ui, "name", nx - S(72), ny + S(8), 16,
                 (Color){160, 160, 170, 255});
    ui_text_input(&L->ui, (Rectangle){nx, ny, nw, nh},
                  L->player_name, LOBBY_UI_NAME_BYTES,
                  UI_ID(), "player");

    /* P09 — small "Controls" link in the bottom-right corner toggles a
     * keybinds modal. The modal itself is rendered at the bottom of
     * this function so it overlays everything else. */
    Rectangle ctrl_r = (Rectangle){ sw - S(132), sh - S(40),
                                     S(116), S(28) };
    if (ui_button(&L->ui, ctrl_r, "Controls", true)) {
        L->show_keybinds = true;
    }

    if (L->show_keybinds) {
        DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 180});
        int dw = S(520), dh = S(440);
        int dx = (sw - dw) / 2, dy = (sh - dh) / 2;
        Rectangle dr = (Rectangle){dx, dy, dw, dh};
        ui_panel_default(&L->ui, dr);

        const char *hdr = "CONTROLS";
        int hw = ui_measure(&L->ui, hdr, 24);
        ui_draw_text(&L->ui, hdr, dx + (dw - hw) / 2, dy + S(18), 24,
                     (Color){240, 220, 180, 255});

        /* Two-column table of keybind rows. Mirrors the actual platform
         * layer (src/platform.c) — the canonical source of truth, since
         * code wins over docs per CLAUDE.md. RMB → BTN_FIRE_SECONDARY
         * is the new entry at M5 P09. */
        struct { const char *key, *action; } rows[] = {
            { "A / D",                 "Move left / right" },
            { "Space",                 "Jump" },
            { "W",                     "Jet" },
            { "Ctrl / S",              "Crouch (drops through one-way)" },
            { "X",                     "Prone" },
            { "Left Mouse",            "Primary fire (active slot)" },
            { "Right Mouse",           "Secondary fire — inactive slot, NEW" },
            { "Q",                     "Swap weapon (toggle slot)" },
            { "R",                     "Reload" },
            { "F",                     "Melee" },
            { "E",                     "Use / interact (Engineer pack)" },
            { "Shift",                 "Dash / burst-jet boost" },
            { "Esc",                   "Leave to title (in lobby/match)" },
        };
        int n_rows = (int)(sizeof(rows) / sizeof(rows[0]));
        int row_y = dy + S(60);
        int row_h = S(24);
        int kx    = dx + S(28);
        int ax    = dx + S(180);
        for (int i = 0; i < n_rows; ++i) {
            ui_draw_text(&L->ui, rows[i].key, kx, row_y, 16,
                         (Color){200, 220, 240, 255});
            ui_draw_text(&L->ui, rows[i].action, ax, row_y, 16,
                         (Color){170, 190, 210, 255});
            row_y += row_h;
        }

        Rectangle close_r = (Rectangle){
            dx + dw - S(140), dy + dh - S(48),
            S(120), S(36)
        };
        if (ui_button(&L->ui, close_r, "Close", true)) {
            L->show_keybinds = false;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            L->show_keybinds = false;
        }
    }

    ui_end(&L->ui);
}

/* ---- Host setup screen --------------------------------------------- */
/*
 * Pre-lobby config screen the host sees after clicking "Host Server" on
 * the title. Picks match mode, map, score limit, time limit, and
 * friendly-fire BEFORE the lobby comes up so the host doesn't have to
 * fiddle with config files for a one-off CTF round. The choices land in
 * g->config via main.c when the user clicks "Start Hosting".
 *
 * Map cycle is filtered by the current mode's bit in each map's
 * `meta.mode_mask`. The mode_mask comes from the code-built map's
 * builder fn (or the .lvl's META lump) — see src/maps.c. If the picked
 * map doesn't support the picked mode the picker auto-advances to the
 * next compatible one.
 */

/* Read mode_mask without building the level. Code-built maps live in
 * g_maps; we re-create a temporary world's level via map_build to peek
 * meta. Cheap (small maps) and only happens on host-setup transitions. */
static uint16_t setup_peek_mode_mask(int map_id, World *w, Arena *arena) {
    arena_reset(arena);
    map_build((MapId)map_id, w, arena);
    return w->level.meta.mode_mask;
}

static bool setup_map_supports_mode(int map_id, int mode_id,
                                    World *w, Arena *arena) {
    uint16_t mm = setup_peek_mode_mask(map_id, w, arena);
    return (mm & (1u << mode_id)) != 0;
}

static int setup_next_map_for_mode(int cur_map, int mode_id,
                                   World *w, Arena *arena) {
    /* Cycle through every entry in the runtime map registry (P08b)
     * until we find one that supports `mode_id`. Falls back to the
     * current pick if NO map supports the mode (which would be a bug
     * — there's always at least one CTF map post-P07). */
    int n = g_map_registry.count;
    if (n <= 0) return cur_map;
    for (int step = 1; step <= n; ++step) {
        int next = (cur_map + step) % n;
        if (setup_map_supports_mode(next, mode_id, w, arena)) return next;
    }
    return cur_map;
}

void host_setup_screen_run(LobbyUIState *L, Game *g, int sw, int sh) {
    Vec2 mouse = (Vec2){ (float)GetMouseX(), (float)GetMouseY() };
    ui_begin(&L->ui, mouse, GetFrameTime(), sh);

    ClearBackground((Color){8, 10, 14, 255});

    /* First-entry init: seed the draft from the loaded config so the
     * defaults are sensible (whatever soldut.cfg says, or built-ins). */
    if (!L->setup_initialized) {
        L->setup_mode         = (int)g->config.mode;
        L->setup_map_id       = (g->config.map_rotation_count > 0)
                                ? g->config.map_rotation[0] : MAP_FOUNDRY;
        L->setup_score_limit  = g->config.score_limit;
        L->setup_time_limit_s = (int)g->config.time_limit;
        L->setup_friendly_fire= g->config.friendly_fire;
        /* Fix CTF defaults that the FFA-default config wouldn't reach.
         * If the user picked CTF on entry, clamp the score limit. */
        if (L->setup_mode == MATCH_MODE_CTF && L->setup_score_limit >= 25) {
            L->setup_score_limit = FLAG_CAPTURE_DEFAULT;
        }
        /* Validate the seeded map against the seeded mode. If the
         * default cfg's map doesn't support the desired mode (e.g. user
         * has mode_rotation=ctf but map_rotation=foundry), step to a
         * compatible map. */
        if (!setup_map_supports_mode(L->setup_map_id, L->setup_mode,
                                     &g->world, &g->level_arena)) {
            L->setup_map_id = setup_next_map_for_mode(
                L->setup_map_id, L->setup_mode, &g->world, &g->level_arena);
        }
        L->setup_initialized = true;
    }

    /* Title. */
    ui_draw_text(&L->ui, "HOST SETUP", S(32), S(24), 32,
                 (Color){200, 220, 240, 255});
    ui_draw_text(&L->ui,
                 "Pick the match settings before opening the server. "
                 "All players see them in the lobby.",
                 S(32), S(70), 16, (Color){160, 170, 180, 255});

    /* Layout: vertical stack of labeled rows in a centered panel. */
    int panel_w = S(560);
    int panel_x = (sw - panel_w) / 2;
    int panel_y = S(120);
    int row_h   = S(48);
    int gap     = S(12);
    int label_w = S(160);
    int btn_w   = panel_w - label_w - S(16);

    Rectangle panel = (Rectangle){ panel_x - S(16), panel_y - S(16),
                                   panel_w + S(32),
                                   row_h * 6 + gap * 5 + S(32) };
    ui_panel_default(&L->ui, panel);

    int y = panel_y;

    /* ---- Mode picker (3-button radio) ---- */
    ui_draw_text(&L->ui, "Mode", panel_x, y + S(14), 18, (Color){200, 220, 240, 255});
    int mb_w = (btn_w - S(12)) / 3;
    Rectangle mb_r[3] = {
        (Rectangle){panel_x + label_w,                  y, mb_w, row_h},
        (Rectangle){panel_x + label_w + mb_w + S(6),    y, mb_w, row_h},
        (Rectangle){panel_x + label_w + (mb_w + S(6))*2,y, mb_w, row_h},
    };
    const char *mb_label[3] = { "FFA", "TDM", "CTF" };
    int         mb_mode [3] = { MATCH_MODE_FFA, MATCH_MODE_TDM, MATCH_MODE_CTF };
    for (int i = 0; i < 3; ++i) {
        bool active = (L->setup_mode == mb_mode[i]);
        bool hover  = ui_point_in_rect(L->ui.mouse, mb_r[i]);
        Color bg = active ? L->ui.accent
                          : hover ? L->ui.button_hover : L->ui.button_bg;
        DrawRectangleRec(mb_r[i], bg);
        DrawRectangleLinesEx(mb_r[i], L->ui.scale, L->ui.panel_edge);
        int tw = ui_measure(&L->ui, mb_label[i], 18);
        ui_draw_text(&L->ui, mb_label[i],
                     (int)(mb_r[i].x + (mb_r[i].width - tw) * 0.5f),
                     (int)(mb_r[i].y + (mb_r[i].height - 18 * L->ui.scale) * 0.5f),
                     18, active ? (Color){12, 18, 26, 255} : L->ui.text_col);
        if (hover && L->ui.mouse_pressed) {
            int new_mode = mb_mode[i];
            if (new_mode != L->setup_mode) {
                L->setup_mode = new_mode;
                /* Auto-clamp limits + auto-pick a compatible map. */
                if (L->setup_mode == MATCH_MODE_CTF &&
                    L->setup_score_limit >= 25) {
                    L->setup_score_limit = FLAG_CAPTURE_DEFAULT;
                }
                if (!setup_map_supports_mode(L->setup_map_id,
                                             L->setup_mode,
                                             &g->world, &g->level_arena)) {
                    L->setup_map_id = setup_next_map_for_mode(
                        L->setup_map_id, L->setup_mode,
                        &g->world, &g->level_arena);
                }
            }
        }
    }
    y += row_h + gap;

    /* ---- Map cycle button ---- */
    ui_draw_text(&L->ui, "Map", panel_x, y + S(14), 18, (Color){200, 220, 240, 255});
    Rectangle map_r = (Rectangle){panel_x + label_w, y, btn_w, row_h};
    if (ui_button(&L->ui, map_r,
                  TextFormat("%s  ▶", map_def(L->setup_map_id)->display_name),
                  true)) {
        L->setup_map_id = setup_next_map_for_mode(
            L->setup_map_id, L->setup_mode, &g->world, &g->level_arena);
    }
    y += row_h + gap;

    /* ---- Score limit stepper ---- */
    ui_draw_text(&L->ui, "Score limit", panel_x, y + S(14), 18,
                 (Color){200, 220, 240, 255});
    int step_w = S(48);
    int sval_w = btn_w - step_w * 2 - S(8);
    if (ui_button(&L->ui, (Rectangle){panel_x + label_w, y, step_w, row_h},
                  "−", true) && L->setup_score_limit > 1) {
        L->setup_score_limit--;
    }
    Rectangle sval_r = (Rectangle){panel_x + label_w + step_w + S(4), y,
                                    sval_w, row_h};
    DrawRectangleRec(sval_r, (Color){20, 24, 32, 255});
    DrawRectangleLinesEx(sval_r, L->ui.scale, L->ui.panel_edge);
    char sbuf[32]; snprintf(sbuf, sizeof sbuf, "%d", L->setup_score_limit);
    int sw_ = ui_measure(&L->ui, sbuf, 18);
    ui_draw_text(&L->ui, sbuf,
                 (int)(sval_r.x + (sval_r.width - sw_) * 0.5f),
                 (int)(sval_r.y + (sval_r.height - 18*L->ui.scale) * 0.5f),
                 18, L->ui.text_col);
    if (ui_button(&L->ui, (Rectangle){panel_x + label_w + step_w + sval_w + S(8),
                                       y, step_w, row_h}, "+", true)) {
        L->setup_score_limit++;
    }
    y += row_h + gap;

    /* ---- Time limit stepper (10-sec increments) ---- */
    ui_draw_text(&L->ui, "Time limit (s)", panel_x, y + S(14), 18,
                 (Color){200, 220, 240, 255});
    if (ui_button(&L->ui, (Rectangle){panel_x + label_w, y, step_w, row_h},
                  "−", true) && L->setup_time_limit_s > 10) {
        L->setup_time_limit_s -= 10;
    }
    Rectangle tval_r = (Rectangle){panel_x + label_w + step_w + S(4), y,
                                    sval_w, row_h};
    DrawRectangleRec(tval_r, (Color){20, 24, 32, 255});
    DrawRectangleLinesEx(tval_r, L->ui.scale, L->ui.panel_edge);
    char tbuf[32]; snprintf(tbuf, sizeof tbuf, "%d s", L->setup_time_limit_s);
    int tw_ = ui_measure(&L->ui, tbuf, 18);
    ui_draw_text(&L->ui, tbuf,
                 (int)(tval_r.x + (tval_r.width - tw_) * 0.5f),
                 (int)(tval_r.y + (tval_r.height - 18*L->ui.scale) * 0.5f),
                 18, L->ui.text_col);
    if (ui_button(&L->ui, (Rectangle){panel_x + label_w + step_w + sval_w + S(8),
                                       y, step_w, row_h}, "+", true)) {
        L->setup_time_limit_s += 10;
    }
    y += row_h + gap;

    /* ---- Friendly fire toggle ---- */
    ui_draw_text(&L->ui, "Friendly fire", panel_x, y + S(14), 18,
                 (Color){200, 220, 240, 255});
    Rectangle ff_r = (Rectangle){panel_x + label_w, y, btn_w, row_h};
    if (ui_button(&L->ui, ff_r,
                  L->setup_friendly_fire ? "ON" : "off",
                  true)) {
        L->setup_friendly_fire = !L->setup_friendly_fire;
    }
    y += row_h + gap;

    /* ---- Confirm / Cancel buttons ---- */
    int bw = S(220), bh = S(48);
    int by = y + S(8);
    int bx_left  = panel_x;
    int bx_right = panel_x + panel_w - bw;
    if (ui_button(&L->ui, (Rectangle){bx_left, by, bw, bh}, "Back", true)) {
        L->setup_initialized = false;     /* re-seed next time */
        g->mode = MODE_TITLE;
    }
    /* The accent-color button on the right communicates "primary action". */
    Rectangle start_r = (Rectangle){bx_right, by, bw, bh};
    bool start_hover = ui_point_in_rect(L->ui.mouse, start_r);
    Color start_bg = start_hover ? (Color){80, 200, 120, 255}
                                 : (Color){60, 160, 90, 255};
    DrawRectangleRec(start_r, start_bg);
    DrawRectangleLinesEx(start_r, L->ui.scale, L->ui.panel_edge);
    int sl_w = ui_measure(&L->ui, "Start Hosting", 20);
    ui_draw_text(&L->ui, "Start Hosting",
                 (int)(start_r.x + (start_r.width - sl_w) * 0.5f),
                 (int)(start_r.y + (start_r.height - 20*L->ui.scale) * 0.5f),
                 20, (Color){12, 22, 14, 255});
    if (start_hover && L->ui.mouse_pressed) {
        L->request_start_host = true;
    }

    /* Footer hint. */
    ui_draw_text(&L->ui,
                 "Tip: drop a soldut.cfg next to the binary to skip "
                 "this screen and pre-set everything.",
                 S(32), sh - S(28), 14, (Color){90, 100, 120, 255});

    ui_end(&L->ui);
}

/* ---- Browser screen ----------------------------------------------- */

typedef struct BrowserDrawCtx {
    const NetDiscoveryEntry *entries;
    int                      n;
} BrowserDrawCtx;

static void browser_row(const UIContext *u, Rectangle row, int idx,
                        bool hover, bool selected, void *user)
{
    const BrowserDrawCtx *ctx = (const BrowserDrawCtx *)user;
    if (idx >= ctx->n) return;
    const NetDiscoveryEntry *e = &ctx->entries[idx];
    Color bg = selected ? u->accent : hover ? u->button_hover : (Color){0,0,0,0};
    if (bg.a > 0) DrawRectangleRec(row, bg);
    Color tc = selected ? (Color){12, 18, 26, 255} : u->text_col;

    char addr[48];
    net_format_addr(e->addr_host, e->port, addr, sizeof addr);

    int fp = ui_font_px(u);
    int ty = (int)(row.y + (row.height - (float)fp) * 0.5f);
    int colx = (int)(12 * u->scale);
    ui_draw_text(u, e->name,  (int)row.x + colx,                ty, u->font_size, tc);
    ui_draw_text(u, addr,     (int)row.x + (int)(220 * u->scale), ty, u->font_size, tc);
    char players[16];
    snprintf(players, sizeof players, "%u/%u",
             (unsigned)e->players, (unsigned)e->max_players);
    ui_draw_text(u, players,  (int)row.x + (int)(460 * u->scale), ty, u->font_size, tc);
    ui_draw_text(u, "LAN",    (int)row.x + (int)(540 * u->scale), ty, u->font_size, tc);
}

void browser_screen_run(LobbyUIState *L, Game *g, int sw, int sh) {
    Vec2 mouse = (Vec2){ (float)GetMouseX(), (float)GetMouseY() };
    ui_begin(&L->ui, mouse, GetFrameTime(), sh);

    ClearBackground((Color){8, 10, 14, 255});
    ui_draw_text(&L->ui, "Server Browser (LAN)", S(32), S(24), 32,
                 (Color){200, 220, 240, 255});

    /* Auto-refresh discovery every 5 seconds; first refresh on entry. */
    L->browser_refresh_timer -= GetFrameTime();
    if (L->browser_refresh_timer <= 0.0f) {
        if (g->net.role == NET_ROLE_OFFLINE) {
            if (!net_init()) { L->browser_refresh_timer = 1.0f; goto draw_only; }
            net_discovery_open(&g->net);
        }
        net_discover_lan(&g->net);
        L->browser_refresh_timer = 5.0f;
    }
draw_only:;

    NetDiscoveryEntry rows[NET_MAX_DISCOVERIES];
    int n = (g->net.discovery_count < NET_MAX_DISCOVERIES) ?
            g->net.discovery_count : NET_MAX_DISCOVERIES;
    for (int i = 0; i < n; ++i) rows[i] = g->net.discoveries[i];

    Rectangle list = (Rectangle){
        S(32), S(96),
        (float)(sw - S(64)),
        (float)(sh - S(220))
    };
    BrowserDrawCtx ctx = { .entries = rows, .n = n };
    int picked = ui_list_custom(&L->ui, list, n > 0 ? n : 0, 32,
                                L->browser_selected, browser_row, &ctx);
    if (picked >= 0) L->browser_selected = picked;

    if (n == 0) {
        ui_draw_text(&L->ui, "No servers found yet — searching...",
                     (int)list.x + S(16), (int)list.y + S(16), 18,
                     (Color){160, 160, 170, 255});
    }

    /* Bottom button row. */
    int bw = S(180), bh = S(48), bgap = S(16);
    int by = sh - bh - S(24);
    int bx = S(32);
    if (ui_button(&L->ui, (Rectangle){bx, by, bw, bh}, "Refresh", true)) {
        L->browser_refresh_timer = 0.0f;
    }
    bx += bw + bgap;
    if (ui_button(&L->ui, (Rectangle){bx, by, bw, bh}, "Direct Connect", true)) {
        g->mode = MODE_CONNECT;
    }
    bx += bw + bgap;
    bool can_connect = (n > 0 && L->browser_selected >= 0 &&
                        L->browser_selected < n);
    if (ui_button(&L->ui, (Rectangle){bx, by, bw, bh}, "Connect", can_connect)) {
        const NetDiscoveryEntry *e = &rows[L->browser_selected];
        char addr[64];
        net_format_addr(e->addr_host, e->port, addr, sizeof addr);
        const char *colon = strrchr(addr, ':');
        if (colon) {
            int hl = (int)(colon - addr);
            if (hl > 0 && hl < (int)sizeof g->pending_host - 1) {
                memcpy(g->pending_host, addr, (size_t)hl);
                g->pending_host[hl] = '\0';
            }
            g->pending_port = (uint16_t)atoi(colon + 1);
        } else {
            snprintf(g->pending_host, sizeof g->pending_host, "%s", addr);
            g->pending_port = SOLDUT_DEFAULT_PORT;
        }
        L->request_connect = true;
    }
    if (ui_button(&L->ui, (Rectangle){sw - bw - S(32), by, bw, bh},
                  "Back", true)) {
        g->mode = MODE_TITLE;
    }

    ui_end(&L->ui);
}

/* ---- Connect screen ----------------------------------------------- */

void connect_screen_run(LobbyUIState *L, Game *g, int sw, int sh) {
    Vec2 mouse = (Vec2){ (float)GetMouseX(), (float)GetMouseY() };
    ui_begin(&L->ui, mouse, GetFrameTime(), sh);
    ClearBackground((Color){8, 10, 14, 255});
    ui_draw_text(&L->ui, "Direct Connect", S(32), S(24), 32,
                 (Color){200, 220, 240, 255});

    int fw = S(540), fh = S(48);
    int fx = (sw - fw) / 2;
    int fy = sh / 2 - S(60);

    ui_draw_text(&L->ui, "Server address (host:port)", fx, fy - S(32), 16,
                 (Color){160, 160, 170, 255});
    bool committed = ui_text_input(&L->ui, (Rectangle){fx, fy, fw, fh},
                                   L->connect_addr,
                                   LOBBY_UI_CONNECT_BYTES, UI_ID(),
                                   "127.0.0.1:23073");

    int by = fy + fh + S(28);
    int bw = S(240), bh = S(48);
    bool clicked = ui_button(&L->ui, (Rectangle){fx, by, bw, bh}, "Connect", true);
    if (ui_button(&L->ui, (Rectangle){fx + bw + S(20), by, bw, bh},
                  "Back", true)) {
        g->mode = MODE_TITLE;
    }

    if (committed || clicked) {
        uint32_t hh = 0; uint16_t pp = SOLDUT_DEFAULT_PORT;
        if (net_parse_addr(L->connect_addr, &hh, &pp, SOLDUT_DEFAULT_PORT)) {
            const char *colon = strrchr(L->connect_addr, ':');
            if (colon) {
                int hl = (int)(colon - L->connect_addr);
                if (hl > 0 && hl < (int)sizeof g->pending_host - 1) {
                    memcpy(g->pending_host, L->connect_addr, (size_t)hl);
                    g->pending_host[hl] = '\0';
                }
            } else {
                snprintf(g->pending_host, sizeof g->pending_host,
                         "%s", L->connect_addr);
            }
            g->pending_port = pp;
            L->request_connect = true;
        } else {
            ui_draw_text(&L->ui, "Could not parse address",
                         fx, by + S(64), 16, (Color){220, 100, 100, 255});
        }
    }

    ui_end(&L->ui);
}

/* ---- Lobby screen ------------------------------------------------- */

static const char *chassis_label(int id) {
    const Chassis *c = mech_chassis((ChassisId)id);
    return c ? c->name : "?";
}

static const char *armor_label(int id) {
    const Armor *a = armor_def(id);
    return a ? a->name : "?";
}

static const char *jet_label(int id) {
    const Jetpack *j = jetpack_def(id);
    return j ? j->name : "?";
}

static const char *primary_label(int id) {
    const Weapon *w = weapon_def(id);
    return w ? w->name : "?";
}

static const int g_primary_choices[]   = {
    WEAPON_PULSE_RIFLE, WEAPON_PLASMA_SMG, WEAPON_RIOT_CANNON,
    WEAPON_RAIL_CANNON, WEAPON_AUTO_CANNON, WEAPON_MASS_DRIVER,
    WEAPON_PLASMA_CANNON, WEAPON_MICROGUN
};
static const int g_secondary_choices[] = {
    WEAPON_SIDEARM, WEAPON_BURST_SMG, WEAPON_FRAG_GRENADES,
    WEAPON_MICRO_ROCKETS, WEAPON_COMBAT_KNIFE, WEAPON_GRAPPLING_HOOK
};
static const int g_armor_choices[]   = { ARMOR_NONE, ARMOR_LIGHT, ARMOR_HEAVY, ARMOR_REACTIVE };
static const int g_jet_choices[]     = { JET_NONE, JET_STANDARD, JET_BURST, JET_GLIDE_WING, JET_JUMP_JET };
static const int g_chassis_choices[] = { CHASSIS_TROOPER, CHASSIS_SCOUT, CHASSIS_HEAVY, CHASSIS_SNIPER, CHASSIS_ENGINEER };

static int next_in_cycle(int current, const int *choices, int n) {
    int idx = 0;
    for (int i = 0; i < n; ++i) if (choices[i] == current) { idx = i; break; }
    return choices[(idx + 1) % n];
}

static void sync_loadout_from_server(LobbyUIState *L, const Game *g) {
    if (g->local_slot_id < 0) return;
    const LobbySlot *me = &g->lobby.slots[g->local_slot_id];
    if (!me->in_use) return;
    /* First-entry: snap the loadout draft to whatever the server has
     * for our slot (handles re-join / mid-round join). */
    if (!L->lobby_loadout_synced) {
        L->lobby_chassis   = me->loadout.chassis_id;
        L->lobby_primary   = me->loadout.primary_id;
        L->lobby_secondary = me->loadout.secondary_id;
        L->lobby_armor     = me->loadout.armor_id;
        L->lobby_jet       = me->loadout.jetpack_id;
        L->lobby_loadout_synced = 1;
    }
    /* Team: ALWAYS sync from server so the TEAM picker reflects the
     * authoritative state. The local optimistic update in
     * apply_team_change already set L->lobby_team before the wire
     * round-trip; the server's confirmation comes back through this
     * path. If the server force-assigned (e.g., team auto-balance,
     * future kick-to-team), the picker updates to match without the
     * user having to click. */
    L->lobby_team = me->team;
}

static MechLoadout build_local_loadout(const LobbyUIState *L) {
    return (MechLoadout){
        .chassis_id   = L->lobby_chassis,
        .primary_id   = L->lobby_primary,
        .secondary_id = L->lobby_secondary,
        .armor_id     = L->lobby_armor,
        .jetpack_id   = L->lobby_jet,
    };
}

static void apply_loadout_change(LobbyUIState *L, Game *g) {
    if (g->local_slot_id < 0) return;
    MechLoadout lo = build_local_loadout(L);
    if (g->net.role == NET_ROLE_CLIENT) {
        net_client_send_loadout(&g->net, lo);
    } else {
        lobby_set_loadout(&g->lobby, g->local_slot_id, lo);
    }
}

static void apply_team_change(LobbyUIState *L, Game *g, int new_team) {
    L->lobby_team = new_team;
    if (g->local_slot_id < 0) return;
    if (g->net.role == NET_ROLE_CLIENT) {
        net_client_send_team_change(&g->net, new_team);
    } else {
        lobby_set_team(&g->lobby, g->local_slot_id, new_team);
    }
}

static void apply_ready_toggle(Game *g, bool ready) {
    if (g->local_slot_id < 0) return;
    if (g->net.role == NET_ROLE_CLIENT) {
        net_client_send_ready(&g->net, ready);
    } else {
        lobby_set_ready(&g->lobby, g->local_slot_id, ready);
    }
}

/* P09 — cast a map-vote on the local slot. Host calls the lobby state
 * directly + rebroadcasts vote state; client sends to server. */
static void apply_map_vote(Game *g, int choice) {
    if (g->local_slot_id < 0) return;
    if (choice < 0 || choice > 2) return;
    if (g->net.role == NET_ROLE_CLIENT) {
        net_client_send_map_vote(&g->net, choice);
    } else {
        lobby_vote_cast(&g->lobby, g->local_slot_id, choice);
        if (g->net.role == NET_ROLE_SERVER) {
            net_server_broadcast_vote_state(&g->net, &g->lobby);
        }
    }
}

static void apply_kick_or_ban(Game *g, int target_slot, bool ban) {
    if (target_slot < 0) return;
    if (g->net.role == NET_ROLE_CLIENT) {
        if (ban) net_client_send_ban (&g->net, target_slot);
        else     net_client_send_kick(&g->net, target_slot);
    } else {
        /* Host (or offline-solo with peers — unlikely but harmless): go
         * straight to the server-side path, no wire round-trip. */
        net_server_kick_or_ban_slot(&g->net, g, target_slot, ban);
    }
}

static void apply_chat_send(Game *g, const char *text) {
    if (!text || !*text) return;
    if (g->net.role == NET_ROLE_CLIENT) {
        net_client_send_chat(&g->net, text);
    } else {
        if (g->local_slot_id >= 0 &&
            lobby_chat_post(&g->lobby, g->local_slot_id, text, g->time_seconds))
        {
            const LobbyChatLine *line =
                &g->lobby.chat[(g->lobby.chat_count - 1) % LOBBY_CHAT_LINES];
            net_server_broadcast_chat(&g->net, line->sender_slot,
                                      line->sender_team, line->text);
        }
    }
}

/* ---- Player-list row ---- */

typedef struct PlayerListCtx {
    const LobbyState *lobby;
    const int        *order;
    int               n;
    int               mode;       /* MatchModeId — drives team chip text/colour */
    LobbyUIState     *ui_state;   /* writable — player_row sets confirm targets */
    bool              host_view;  /* true → show [Kick] [Ban] on non-host rows */
} PlayerListCtx;

static void player_row(const UIContext *u, Rectangle row, int idx,
                       bool hover, bool selected, void *user)
{
    (void)hover; (void)selected;
    PlayerListCtx *ctx = (PlayerListCtx *)user;
    if (idx >= ctx->n) return;
    int slot = ctx->order[idx];
    const LobbySlot *s = &ctx->lobby->slots[slot];
    if (!s->in_use) return;

    /* Background tint by team. In FFA mode we use a neutral gray
     * tint so every row looks the same. */
    Color tint = team_color_for_mode(s->team, ctx->mode);
    tint.a = 30;
    DrawRectangleRec(row, tint);

    int fp = ui_font_px(u);
    int ty = (int)(row.y + (row.height - (float)fp) * 0.5f);

    /* Ready dot. */
    Color rdot = s->ready ? (Color){80, 220, 120, 255}
                          : (Color){90, 90, 100, 255};
    DrawCircle((int)row.x + (int)(20 * u->scale),
               (int)(row.y + row.height * 0.5f), 7.0f * u->scale, rdot);

    char name[40];
    if (s->is_host) snprintf(name, sizeof name, "%s [HOST]", s->name);
    else            snprintf(name, sizeof name, "%s", s->name);
    ui_draw_text(u, name, (int)row.x + (int)(40 * u->scale), ty,
                 u->font_size, u->text_col);

    /* Team chip — only show colour bias for TDM/CTF; FFA gets a
     * neutral gray "FFA" chip so we don't imply a team that doesn't
     * exist. */
    Color tc = team_color_for_mode(s->team, ctx->mode);
    int chip_x = (int)row.x + (int)(240 * u->scale);
    int chip_y = (int)row.y + (int)(6 * u->scale);
    int chip_w = (int)(64 * u->scale);
    int chip_h = (int)(row.height - 12 * u->scale);
    DrawRectangle(chip_x, chip_y, chip_w, chip_h, tc);
    const char *tn = team_name_for_mode(s->team, ctx->mode);
    int tn_w = ui_measure(u, tn, 16);
    ui_draw_text(u, tn,
                 chip_x + (chip_w - tn_w) / 2,
                 chip_y + (chip_h - (int)(16 * u->scale)) / 2,
                 16, BLACK);

    /* Loadout summary. */
    char load[64];
    snprintf(load, sizeof load, "%s · %s",
             chassis_label(s->loadout.chassis_id),
             primary_label(s->loadout.primary_id));
    ui_draw_text(u, load, (int)row.x + (int)(320 * u->scale), ty,
                 u->font_size, u->text_dim);

    /* Score (kills/deaths). */
    char score[32];
    snprintf(score, sizeof score, "%d / %d", s->kills, s->deaths);
    ui_draw_text(u, score, (int)row.x + (int)(580 * u->scale), ty,
                 u->font_size, u->text_col);

    /* P09 — host controls: [Kick] [Ban] on every non-host row when the
     * local player is the host. Click stages the slot in the
     * confirmation-modal state on the lobby UI; the modal itself is
     * rendered in lobby_screen_run after the list iteration. */
    if (ctx->host_view && !s->is_host && ctx->ui_state) {
        float bw = 56.0f * u->scale;
        float bh = row.height - 12.0f * u->scale;
        float by = row.y + 6.0f * u->scale;
        float bx_ban  = row.x + row.width - bw - 8.0f * u->scale;
        float bx_kick = bx_ban - bw - 6.0f * u->scale;
        Rectangle rk = (Rectangle){ bx_kick, by, bw, bh };
        Rectangle rb = (Rectangle){ bx_ban,  by, bw, bh };

        bool hk = ui_point_in_rect(u->mouse, rk);
        DrawRectangleRec(rk, hk ? (Color){80, 70, 60, 220}
                                : (Color){45, 50, 60, 200});
        DrawRectangleLinesEx(rk, u->scale, u->panel_edge);
        int twk = ui_measure(u, "Kick", 14);
        ui_draw_text(u, "Kick",
                     (int)(rk.x + (rk.width  - twk) * 0.5f),
                     (int)(rk.y + (rk.height - 14*u->scale) * 0.5f),
                     14, u->text_col);
        if (hk && u->mouse_pressed) {
            ctx->ui_state->kick_target_slot = slot;
            ctx->ui_state->ban_target_slot  = -1;
        }

        bool hb = ui_point_in_rect(u->mouse, rb);
        DrawRectangleRec(rb, hb ? (Color){120, 50, 60, 220}
                                : (Color){70, 35, 40, 200});
        DrawRectangleLinesEx(rb, u->scale, u->panel_edge);
        int twb = ui_measure(u, "Ban", 14);
        ui_draw_text(u, "Ban",
                     (int)(rb.x + (rb.width  - twb) * 0.5f),
                     (int)(rb.y + (rb.height - 14*u->scale) * 0.5f),
                     14, u->text_col);
        if (hb && u->mouse_pressed) {
            ctx->ui_state->ban_target_slot  = slot;
            ctx->ui_state->kick_target_slot = -1;
        }
    }
}

void lobby_screen_run(LobbyUIState *L, Game *g, int sw, int sh) {
    Vec2 mouse = (Vec2){ (float)GetMouseX(), (float)GetMouseY() };
    float dt = GetFrameTime();
    ui_begin(&L->ui, mouse, dt, sh);

    ClearBackground((Color){10, 12, 16, 255});
    sync_loadout_from_server(L, g);

    /* ---- P08 — map download progress banner --------------------- *
     * Drawn first (under the title strip) when a download is in
     * flight. Width spans the screen; thin progress bar across the
     * panel. Other lobby controls remain visible/clickable but the
     * server's map-ready gate keeps the round from firing while
     * we're below 100%. */
    if (g->map_download.active) {
        int bw = sw - 2 * S(32);
        int bx = S(32);
        int by = S(50);
        int bh = S(26);
        DrawRectangle(bx, by, bw, bh, (Color){20, 32, 48, 240});
        DrawRectangleLines(bx, by, bw, bh, (Color){80, 140, 200, 255});
        int pct = map_download_progress_pct(&g->map_download);
        int fill_w = (int)((float)bw * (float)pct / 100.0f);
        if (fill_w > 0) {
            DrawRectangle(bx + 2, by + 2, fill_w - 4, bh - 4,
                          (Color){70, 130, 200, 255});
        }
        char dl_label[80];
        snprintf(dl_label, sizeof(dl_label),
                 "DOWNLOADING MAP %s — %d%%  (%u / %u bytes)",
                 g->map_download.desc.short_name, pct,
                 (unsigned)g->map_download.bytes_received,
                 (unsigned)g->map_download.total_size);
        ui_draw_text(&L->ui, dl_label,
                     bx + S(8),
                     by + (bh - 14) / 2,
                     14, (Color){220, 230, 240, 255});
    }

    /* ---- Top strip: title + MATCH panel ----
     * Visible to all players. The host gets clickable mode + map
     * buttons (only between rounds — phase == LOBBY); clients see the
     * same widgets disabled. Score/time/ff are status text for now. */
    ui_draw_text(&L->ui, "LOBBY", S(32), S(20), 28,
                 (Color){200, 220, 240, 255});

    bool is_host = (g->net.role == NET_ROLE_SERVER || g->offline_solo);
    bool can_change = is_host && (g->match.phase == MATCH_PHASE_LOBBY ||
                                   g->match.phase == MATCH_PHASE_SUMMARY);

    /* MATCH panel: a small horizontal strip of widgets right under the
     * LOBBY title. Layout: [Mode buttons] [Map cycle] [score · time · ff]. */
    int mp_y     = S(60);
    int mp_h     = S(40);
    int mp_x     = S(32);
    int mb_w     = S(58);
    int mb_gap   = S(4);
    const struct { int mode; const char *label; } modes[3] = {
        { MATCH_MODE_FFA, "FFA" },
        { MATCH_MODE_TDM, "TDM" },
        { MATCH_MODE_CTF, "CTF" },
    };
    for (int i = 0; i < 3; ++i) {
        Rectangle r = (Rectangle){mp_x + i * (mb_w + mb_gap), mp_y, mb_w, mp_h};
        bool active = ((int)g->match.mode == modes[i].mode);
        bool hover  = can_change && ui_point_in_rect(L->ui.mouse, r);
        Color bg = active ? L->ui.accent
                          : (hover ? L->ui.button_hover : L->ui.button_bg);
        DrawRectangleRec(r, bg);
        DrawRectangleLinesEx(r, L->ui.scale, L->ui.panel_edge);
        int tw = ui_measure(&L->ui, modes[i].label, 16);
        Color tc = active ? (Color){12, 18, 26, 255}
                          : can_change ? L->ui.text_col : L->ui.text_dim;
        ui_draw_text(&L->ui, modes[i].label,
                     (int)(r.x + (r.width - tw) * 0.5f),
                     (int)(r.y + (r.height - 16*L->ui.scale) * 0.5f),
                     16, tc);
        if (can_change && hover && L->ui.mouse_pressed && !active) {
            /* Host mode change. Update local match + broadcast. CTF
             * needs a compatible map; clamp score limit if jumping
             * from FFA's 25 to CTF. */
            g->match.mode = (MatchModeId)modes[i].mode;
            g->match.friendly_fire = g->config.friendly_fire ||
                                      (g->match.mode == MATCH_MODE_FFA);
            g->world.friendly_fire = g->match.friendly_fire;
            if (g->match.mode == MATCH_MODE_CTF &&
                g->match.score_limit >= 25) {
                g->match.score_limit = FLAG_CAPTURE_DEFAULT;
            }
            /* Validate map mode_mask. If incompatible, advance to a
             * supporting map. We have to peek the chosen map's META
             * which lives on the level we already built — but maps.c
             * sets meta.mode_mask at build time, so a fresh build is
             * cheap. Skip if test-play (can't change map there). */
            if (!g->test_play_lvl[0]) {
                arena_reset(&g->level_arena);
                map_build((MapId)g->match.map_id, &g->world, &g->level_arena);
                if (!(g->world.level.meta.mode_mask & (1u << g->match.mode))) {
                    int N = g_map_registry.count;
                    for (int step = 1; step <= N; ++step) {
                        int next = (g->match.map_id + step) % N;
                        arena_reset(&g->level_arena);
                        map_build((MapId)next, &g->world, &g->level_arena);
                        if (g->world.level.meta.mode_mask & (1u << g->match.mode)) {
                            g->match.map_id = next;
                            break;
                        }
                    }
                }
            }
            g->config.mode = g->match.mode;
            g->config.mode_rotation[0] = g->match.mode;
            g->config.mode_rotation_count = 1;
            g->config.map_rotation[0] = g->match.map_id;
            g->config.map_rotation_count = 1;
            /* P08 — refresh serve descriptor + push to clients so they
             * download the new map (or fall back to code-built). */
            maps_refresh_serve_info(map_def(g->match.map_id)->short_name,
                                    NULL, &g->server_map_desc,
                                    g->server_map_serve_path,
                                    sizeof(g->server_map_serve_path));
            if (g->net.role == NET_ROLE_SERVER) {
                net_server_broadcast_match_state(&g->net, &g->match);
                net_server_broadcast_map_descriptor(&g->net, &g->server_map_desc);
            }
            LOG_I("host: lobby mode → %s map → %s",
                  match_mode_name(g->match.mode),
                  map_def(g->match.map_id)->display_name);
        }
    }

    /* Map cycle button — to the right of the mode buttons. */
    int map_x = mp_x + 3 * (mb_w + mb_gap) + S(12);
    int map_w = S(180);
    Rectangle map_r = (Rectangle){map_x, mp_y, map_w, mp_h};
    char map_label[64];
    /* P08b — when the host advertises a map_id outside the client's
     * local registry (custom map only on the host side), fall through
     * to the descriptor's short_name so the lobby still shows the right
     * name. The descriptor arrives before/with ROUND_START and is the
     * source of truth on the wire. */
    const char *map_disp;
    if (g->match.map_id >= 0 && g->match.map_id < g_map_registry.count) {
        map_disp = g_map_registry.entries[g->match.map_id].display_name;
    } else if (g->pending_map.short_name_len > 0) {
        map_disp = g->pending_map.short_name;
    } else {
        map_disp = "(custom)";
    }
    snprintf(map_label, sizeof map_label, "Map: %s%s",
             map_disp,
             can_change ? "  ▶" : "");
    {
        bool hover = can_change && ui_point_in_rect(L->ui.mouse, map_r);
        Color bg = hover ? L->ui.button_hover : L->ui.button_bg;
        DrawRectangleRec(map_r, bg);
        DrawRectangleLinesEx(map_r, L->ui.scale, L->ui.panel_edge);
        Color tc = can_change ? L->ui.text_col : L->ui.text_dim;
        int tw = ui_measure(&L->ui, map_label, 16);
        ui_draw_text(&L->ui, map_label,
                     (int)(map_r.x + (map_r.width - tw) * 0.5f),
                     (int)(map_r.y + (map_r.height - 16*L->ui.scale) * 0.5f),
                     16, tc);
        if (can_change && hover && L->ui.mouse_pressed && !g->test_play_lvl[0]) {
            /* Cycle to next map that supports the current mode. P08b —
             * the registry can include user-authored maps from
             * assets/maps/, not just the four reserved indices. */
            int cur = g->match.map_id;
            int N = g_map_registry.count;
            for (int step = 1; step <= N; ++step) {
                int next = (cur + step) % N;
                arena_reset(&g->level_arena);
                map_build((MapId)next, &g->world, &g->level_arena);
                if (g->world.level.meta.mode_mask & (1u << g->match.mode)) {
                    g->match.map_id = next;
                    break;
                }
            }
            g->config.map_rotation[0] = g->match.map_id;
            g->config.map_rotation_count = 1;
            /* P08 — same refresh+broadcast as the mode-change branch. */
            maps_refresh_serve_info(map_def(g->match.map_id)->short_name,
                                    NULL, &g->server_map_desc,
                                    g->server_map_serve_path,
                                    sizeof(g->server_map_serve_path));
            if (g->net.role == NET_ROLE_SERVER) {
                net_server_broadcast_match_state(&g->net, &g->match);
                net_server_broadcast_map_descriptor(&g->net, &g->server_map_desc);
            }
            LOG_I("host: lobby map → %s",
                  map_def(g->match.map_id)->display_name);
        }
    }

    /* Status text — score / time / ff — to the right of the map. */
    char stat[96];
    snprintf(stat, sizeof stat, "score %d  ·  time %.0fs  ·  ff=%s",
             g->match.score_limit,
             (double)g->match.time_limit,
             g->match.friendly_fire ? "ON" : "off");
    int sx_text = map_x + map_w + S(16);
    int sty = mp_y + (mp_h - (int)(16*L->ui.scale + 0.5f)) / 2;
    ui_draw_text(&L->ui, stat, sx_text, sty, 16,
                 (Color){180, 200, 220, 255});

    /* Auto-start countdown banner — top-right. */
    if (g->lobby.auto_start_active) {
        char cd[64];
        snprintf(cd, sizeof cd, "Match starts in %.1fs",
                 (double)g->lobby.auto_start_remaining);
        int tw = ui_measure(&L->ui, cd, 22);
        ui_draw_text(&L->ui, cd, sw - tw - S(32), S(24), 22,
                     (Color){240, 200, 100, 255});
    } else if (lobby_all_ready(&g->lobby)) {
        const char *ar = "All ready!";
        int tw = ui_measure(&L->ui, ar, 22);
        ui_draw_text(&L->ui, ar, sw - tw - S(32), S(24), 22,
                     (Color){80, 220, 120, 255});
    }

    /* ---- Layout columns ----
     * Two-column layout: a left "content" column (player list above
     * chat) and a right "loadout" column (loadout panel + ready
     * button). Buttons row sits below, full-width.
     *
     * +---------------------------+----------------+
     * | LOBBY title  + match info | countdown      |
     * +---------------------------+----------------+
     * | Player list               |                |
     * |                           | LOADOUT panel  |
     * |   (top half)              |                |
     * +---------------------------+                |
     * | Chat list                 |                |
     * |                           +----------------+
     * |   (bottom half)           |                |
     * +---------------------------+                |
     * | Chat input + Send         | Ready button   |
     * +---------------------------+----------------+
     * |                Start (host) | Leave        |
     * +-------------------------------------------+
     */
    int margin    = S(24);
    int header_h  = S(112);                 /* room for title + MATCH panel + countdown */
    int btn_row_h = S(56);                  /* bottom button row */
    int lp_w      = S(320);                 /* loadout panel width */
    int lp_x      = sw - lp_w - margin;
    int col_gap   = S(24);                  /* gap between left column and loadout */
    int left_w    = lp_x - col_gap - margin;
    int left_top  = header_h;
    int left_btm  = sh - btn_row_h - margin;     /* leaves room for Start/Leave */

    /* Player list takes the top ~45% of the left column; chat takes
     * the rest. Tuned so the chat input row + a few chat lines stay
     * visible even on small windows. */
    int list_h    = (left_btm - left_top) * 45 / 100;
    Rectangle list_r = (Rectangle){
        margin, (float)left_top,
        (float)left_w, (float)list_h
    };
    int order[MAX_LOBBY_SLOTS];
    int n = 0;
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i)
        if (g->lobby.slots[i].in_use) order[n++] = i;
    bool host_view = (g->net.role == NET_ROLE_SERVER);
    PlayerListCtx pctx = {
        .lobby = &g->lobby, .order = order, .n = n,
        .mode = (int)g->match.mode,
        .ui_state = L,
        .host_view = host_view,
    };
    ui_list_custom(&L->ui, list_r, n, 32, -1, player_row, &pctx);

    /* P09 — kick/ban confirmation modal. Rendered after the list so the
     * dim-overlay sits on top of the player rows. The modal blocks
     * interaction with the rest of the lobby while open: it consumes
     * `mouse_pressed` whether the click lands inside its buttons or
     * outside them. (Clicking outside the modal cancels.) */
    if (L->kick_target_slot >= 0 || L->ban_target_slot >= 0) {
        bool ban = (L->ban_target_slot >= 0);
        int  target = ban ? L->ban_target_slot : L->kick_target_slot;
        const LobbySlot *ts = (target >= 0 && target < MAX_LOBBY_SLOTS)
                              ? &g->lobby.slots[target] : NULL;
        if (!ts || !ts->in_use) {
            L->kick_target_slot = L->ban_target_slot = -1;
        } else {
            DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 160});
            int dw = S(420), dh = S(160);
            int dx = (sw - dw) / 2, dy = (sh - dh) / 2;
            Rectangle dr = (Rectangle){dx, dy, dw, dh};
            ui_panel_default(&L->ui, dr);
            char title[80];
            snprintf(title, sizeof title, "%s %s?",
                     ban ? "Ban"  : "Kick", ts->name);
            int tw = ui_measure(&L->ui, title, 22);
            ui_draw_text(&L->ui, title, dx + (dw - tw) / 2, dy + S(20), 22,
                         (Color){240, 220, 180, 255});
            if (ban) {
                const char *sub = "(persists across host restarts)";
                int sw2 = ui_measure(&L->ui, sub, 14);
                ui_draw_text(&L->ui, sub, dx + (dw - sw2) / 2, dy + S(54),
                             14, (Color){180, 200, 220, 255});
            }
            int bw = S(140), bh = S(40);
            int by = dy + dh - bh - S(16);
            int bx_cancel  = dx + S(20);
            int bx_confirm = dx + dw - bw - S(20);
            Rectangle rc = (Rectangle){bx_cancel,  by, bw, bh};
            Rectangle rf = (Rectangle){bx_confirm, by, bw, bh};
            bool cancel  = ui_button(&L->ui, rc, "Cancel", true);
            bool confirm = ui_button(&L->ui, rf, ban ? "Ban" : "Kick", true);
            if (cancel) {
                L->kick_target_slot = L->ban_target_slot = -1;
            } else if (confirm) {
                apply_kick_or_ban(g, target, ban);
                L->kick_target_slot = L->ban_target_slot = -1;
            }
        }
    }

    /* ---- Loadout panel (right) ---- */
    int lx = lp_x;
    int ly = left_top;
    int row_h = S(44), gap = S(8);
    /* Always reserve a row for the Team picker (visible in all modes —
     * TDM/CTF lets the player switch sides, FFA gives them a Spectator
     * toggle so they can sit a round out without leaving). */
    int rows_total = 8;     /* TEAM + chassis/primary/secondary/armor/jet + READY (taller) + small headroom */
    int panel_h = row_h * (rows_total + 1) + gap * rows_total + S(28);
    Rectangle lp = (Rectangle){lx - S(12), ly - S(12),
                               (float)(lp_w + S(24)),
                               (float)panel_h};
    ui_panel_default(&L->ui, lp);
    ui_draw_text(&L->ui, "TEAM", lx, ly - S(2), 18,
                 (Color){200, 220, 240, 255});
    ly += S(28);

    /* ---- Team picker (always shown, mode-aware) ----
     * TDM/CTF: side-by-side RED / BLUE / SPEC — the player taps the
     * one they want. The press goes through net_client_send_team_change
     * (clients) or lobby_set_team (host); the server broadcasts the
     * updated lobby_list so other peers see the change.
     * FFA: a single "Playing / Spectator" toggle — there's only one
     * team in FFA, so the meaningful axis is "in" vs "sitting out". */
    {
        bool in_match = (g->match.mode == MATCH_MODE_TDM ||
                          g->match.mode == MATCH_MODE_CTF);
        if (in_match) {
            int slot_w = (lp_w - 2 * gap) / 3;
            const struct { int team; const char *label; Color col; } tslot[3] = {
                { MATCH_TEAM_RED,  "RED",  (Color){220, 80,  80,  255} },
                { MATCH_TEAM_BLUE, "BLUE", (Color){ 80, 140, 220, 255} },
                { MATCH_TEAM_NONE, "Spec", (Color){140, 140, 150, 255} },
            };
            for (int i = 0; i < 3; ++i) {
                Rectangle r = (Rectangle){lx + i * (slot_w + gap), ly,
                                           slot_w, row_h};
                bool active = (L->lobby_team == tslot[i].team);
                bool hover  = ui_point_in_rect(L->ui.mouse, r);
                Color bg = active ? tslot[i].col
                                  : hover ? L->ui.button_hover : L->ui.button_bg;
                DrawRectangleRec(r, bg);
                DrawRectangleLinesEx(r, L->ui.scale, L->ui.panel_edge);
                int tw = ui_measure(&L->ui, tslot[i].label, 18);
                Color tc = active ? (Color){12, 18, 26, 255} : L->ui.text_col;
                ui_draw_text(&L->ui, tslot[i].label,
                             (int)(r.x + (r.width - tw) * 0.5f),
                             (int)(r.y + (r.height - 18*L->ui.scale) * 0.5f),
                             18, tc);
                if (hover && L->ui.mouse_pressed && !active) {
                    apply_team_change(L, g, tslot[i].team);
                }
            }
        } else {
            /* FFA: Playing / Spectator. */
            int slot_w = (lp_w - gap) / 2;
            bool playing = (L->lobby_team != MATCH_TEAM_NONE);
            const struct { bool play; const char *label; } cells[2] = {
                { true,  "Playing" }, { false, "Spectator" }
            };
            for (int i = 0; i < 2; ++i) {
                Rectangle r = (Rectangle){lx + i * (slot_w + gap), ly,
                                           slot_w, row_h};
                bool active = (cells[i].play == playing);
                bool hover  = ui_point_in_rect(L->ui.mouse, r);
                Color bg = active ? L->ui.accent
                                  : hover ? L->ui.button_hover : L->ui.button_bg;
                DrawRectangleRec(r, bg);
                DrawRectangleLinesEx(r, L->ui.scale, L->ui.panel_edge);
                int tw = ui_measure(&L->ui, cells[i].label, 18);
                Color tc = active ? (Color){12, 18, 26, 255} : L->ui.text_col;
                ui_draw_text(&L->ui, cells[i].label,
                             (int)(r.x + (r.width - tw) * 0.5f),
                             (int)(r.y + (r.height - 18*L->ui.scale) * 0.5f),
                             18, tc);
                if (hover && L->ui.mouse_pressed && !active) {
                    apply_team_change(L, g, cells[i].play
                                            ? MATCH_TEAM_FFA
                                            : MATCH_TEAM_NONE);
                }
            }
        }
    }
    ly += row_h + gap;

    /* ---- LOADOUT subheader ---- */
    ui_draw_text(&L->ui, "LOADOUT", lx, ly - S(2), 18,
                 (Color){200, 220, 240, 255});
    ly += S(28);

    if (ui_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                  TextFormat("Chassis: %s", chassis_label(L->lobby_chassis)),
                  true))
    {
        L->lobby_chassis = next_in_cycle(L->lobby_chassis, g_chassis_choices,
                                         (int)(sizeof g_chassis_choices / sizeof g_chassis_choices[0]));
        apply_loadout_change(L, g);
    } ly += row_h + gap;

    if (ui_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                  TextFormat("Primary: %s", primary_label(L->lobby_primary)),
                  true))
    {
        L->lobby_primary = next_in_cycle(L->lobby_primary, g_primary_choices,
                                         (int)(sizeof g_primary_choices / sizeof g_primary_choices[0]));
        apply_loadout_change(L, g);
    } ly += row_h + gap;

    if (ui_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                  TextFormat("Secondary: %s", primary_label(L->lobby_secondary)),
                  true))
    {
        L->lobby_secondary = next_in_cycle(L->lobby_secondary, g_secondary_choices,
                                           (int)(sizeof g_secondary_choices / sizeof g_secondary_choices[0]));
        apply_loadout_change(L, g);
    } ly += row_h + gap;

    if (ui_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                  TextFormat("Armor: %s", armor_label(L->lobby_armor)),
                  true))
    {
        L->lobby_armor = next_in_cycle(L->lobby_armor, g_armor_choices,
                                       (int)(sizeof g_armor_choices / sizeof g_armor_choices[0]));
        apply_loadout_change(L, g);
    } ly += row_h + gap;

    if (ui_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                  TextFormat("Jetpack: %s", jet_label(L->lobby_jet)),
                  true))
    {
        L->lobby_jet = next_in_cycle(L->lobby_jet, g_jet_choices,
                                     (int)(sizeof g_jet_choices / sizeof g_jet_choices[0]));
        apply_loadout_change(L, g);
    } ly += row_h + gap;

    /* Ready button — taller + accent color. */
    bool me_ready = (g->local_slot_id >= 0) ?
        g->lobby.slots[g->local_slot_id].ready : false;
    Rectangle rr = (Rectangle){lx, ly, lp_w, row_h + S(8)};
    bool hover = ui_point_in_rect(L->ui.mouse, rr);
    Color rb = me_ready ? (Color){40, 110, 60, 255}
                        : hover ? L->ui.button_hover : L->ui.button_bg;
    DrawRectangleRec(rr, rb);
    DrawRectangleLinesEx(rr, L->ui.scale, L->ui.panel_edge);
    const char *rl = me_ready ? "READY ✓" : "Ready up";
    int trw = ui_measure(&L->ui, rl, 20);
    int trh = (int)(20.0f * L->ui.scale + 0.5f);
    ui_draw_text(&L->ui, rl,
                 (int)(rr.x + (rr.width - (float)trw) * 0.5f),
                 (int)(rr.y + (rr.height - (float)trh) * 0.5f),
                 20, L->ui.text_col);
    if (hover && L->ui.mouse_pressed) apply_ready_toggle(g, !me_ready);

    /* ---- Chat (bottom of left column, separate from loadout) ----
     * Sits below the player list, constrained to left_w so it doesn't
     * collide with the loadout panel. Leaves room for a chat-input
     * row at the bottom. */
    int chat_input_h = S(40);
    int chat_top = left_top + list_h + S(16);
    int chat_btm = left_btm - chat_input_h - S(8);
    if (chat_btm < chat_top + S(60)) chat_btm = chat_top + S(60);  /* min height */
    Rectangle chat_r = (Rectangle){
        margin, (float)chat_top,
        (float)left_w, (float)(chat_btm - chat_top)
    };
    ui_panel_default(&L->ui, chat_r);

    int chat_n = (g->lobby.chat_count < LOBBY_CHAT_LINES) ?
                 g->lobby.chat_count : LOBBY_CHAT_LINES;
    int line_h = S(22);
    int inner_pad = S(8);
    int max_lines = (int)(chat_r.height - 2 * inner_pad) / line_h;
    if (max_lines < 1) max_lines = 1;
    int show = (chat_n < max_lines) ? chat_n : max_lines;
    int start = g->lobby.chat_count - show;
    int yy = (int)chat_r.y + inner_pad;
    for (int i = 0; i < show; ++i) {
        int idx = (start + i) % LOBBY_CHAT_LINES;
        if (idx < 0) idx += LOBBY_CHAT_LINES;
        const LobbyChatLine *line = &g->lobby.chat[idx];
        Color tc = team_color((int)line->sender_team);
        if (line->sender_slot < 0) tc = (Color){180, 200, 220, 255};
        ui_draw_text(&L->ui, line->text, (int)chat_r.x + S(12), yy, 16, tc);
        yy += line_h;
    }

    /* Chat input — directly below the chat panel, same width column. */
    int btn_w = S(96);
    Rectangle ic = (Rectangle){
        chat_r.x, chat_r.y + chat_r.height + S(8),
        (float)(chat_r.width - btn_w - S(12)), (float)chat_input_h
    };
    bool sent = ui_text_input(&L->ui, ic, L->chat_draft,
                              LOBBY_UI_CHAT_DRAFT_BYTES, UI_ID(),
                              "Press Enter to chat...");
    if (sent) {
        apply_chat_send(g, L->chat_draft);
        L->chat_draft[0] = '\0';
    }
    if (ui_button(&L->ui, (Rectangle){ic.x + ic.width + S(12), ic.y,
                                      btn_w, chat_input_h},
                  "Send", L->chat_draft[0] != '\0'))
    {
        apply_chat_send(g, L->chat_draft);
        L->chat_draft[0] = '\0';
    }

    /* ---- Bottom button row (full width, right-aligned) ----
     * Sits below the chat input row. Start (host only) and Leave. */
    int bbw = S(180), bbh = S(44);
    int bby = sh - bbh - S(12);
    int bx_leave = sw - bbw - margin;
    int bx_start = bx_leave - bbw - S(16);
    if (g->net.role == NET_ROLE_SERVER || g->offline_solo) {
        const char *sl = g->lobby.auto_start_active ? "Cancel start" : "Start now";
        if (ui_button(&L->ui, (Rectangle){bx_start, bby, bbw, bbh}, sl, true)) {
            if (g->lobby.auto_start_active) lobby_auto_start_cancel(&g->lobby);
            else                            lobby_auto_start_arm(&g->lobby, 3.0f);
        }
    }
    if (ui_button(&L->ui, (Rectangle){bx_leave, bby, bbw, bbh},
                  "Leave", true)) {
        g->mode = MODE_TITLE;
    }

    ui_end(&L->ui);
}

/* ---- Summary screen ---------------------------------------------- */

void summary_screen_run(LobbyUIState *L, Game *g, int sw, int sh) {
    Vec2 mouse = (Vec2){ (float)GetMouseX(), (float)GetMouseY() };
    ui_begin(&L->ui, mouse, GetFrameTime(), sh);

    /* The world + HUD are drawn behind us by the renderer. Use a
     * near-opaque dim panel so the HP bar / weapon HUD don't bleed
     * through (alpha 200 was too thin — HUD ghosting was visible).
     * A 240 alpha leaves just enough world visible to remind the
     * player of context without distracting from the scoreboard. */
    DrawRectangle(0, 0, sw, sh, (Color){4, 6, 10, 240});

    int title_w = ui_measure(&L->ui, "ROUND OVER", 40);
    ui_draw_text(&L->ui, "ROUND OVER", (sw - title_w) / 2, S(60), 40,
                 (Color){240, 220, 180, 255});

    /* MVP — clients compute locally if mvp_slot didn't ride the wire. */
    int mvp = g->match.mvp_slot;
    if (mvp < 0) {
        int best_score = -2147483647, best_deaths = 2147483647;
        for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
            const LobbySlot *s = &g->lobby.slots[i];
            if (!s->in_use) continue;
            if (s->score > best_score ||
                (s->score == best_score && s->deaths < best_deaths))
            {
                mvp = i; best_score = s->score; best_deaths = s->deaths;
            }
        }
    }

    char sub[96];
    if (g->match.mode == MATCH_MODE_FFA) {
        if (mvp >= 0) {
            snprintf(sub, sizeof sub, "MVP: %s",
                     g->lobby.slots[mvp].name);
        } else {
            snprintf(sub, sizeof sub, "MVP: —");
        }
    } else {
        const char *winner = (g->match.winner_team == MATCH_TEAM_RED)  ? "RED wins"
                          :  (g->match.winner_team == MATCH_TEAM_BLUE) ? "BLUE wins"
                                                                       : "Draw";
        snprintf(sub, sizeof sub, "%s · R %d · B %d", winner,
                 g->match.team_score[MATCH_TEAM_RED],
                 g->match.team_score[MATCH_TEAM_BLUE]);
    }
    int sub_w = ui_measure(&L->ui, sub, 22);
    ui_draw_text(&L->ui, sub, (sw - sub_w) / 2, S(112), 22,
                 (Color){200, 220, 240, 255});

    /* Scoreboard. Shrinks to make room for the P09 map-vote panel when
     * a vote is active; otherwise expands to the full bottom margin. */
    bool vote_show = (g->lobby.vote_map_a >= 0 ||
                      g->lobby.vote_map_b >= 0 ||
                      g->lobby.vote_map_c >= 0);
    int  vote_panel_h   = S(180);
    int  vote_panel_top = sh - S(120) - vote_panel_h;
    int sboard_w = S(720);
    int sx = (sw - sboard_w) / 2, sy = S(170);
    int sboard_h = vote_show ? (vote_panel_top - sy - S(20))
                             : (sh - sy - S(120));
    if (sboard_h < S(80)) sboard_h = S(80);
    Rectangle sr = (Rectangle){sx, sy, sboard_w, sboard_h};
    ui_panel_default(&L->ui, sr);

    int hdr_y = sy + S(10);
    ui_draw_text(&L->ui, "Player",  sx + S(20),  hdr_y, 18, (Color){200, 220, 240, 255});
    ui_draw_text(&L->ui, "Team",    sx + S(280), hdr_y, 18, (Color){200, 220, 240, 255});
    ui_draw_text(&L->ui, "Kills",   sx + S(400), hdr_y, 18, (Color){200, 220, 240, 255});
    ui_draw_text(&L->ui, "Deaths",  sx + S(490), hdr_y, 18, (Color){200, 220, 240, 255});
    ui_draw_text(&L->ui, "Score",   sx + S(590), hdr_y, 18, (Color){200, 220, 240, 255});
    int rowy = sy + S(40);
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        const LobbySlot *s = &g->lobby.slots[i];
        if (!s->in_use) continue;
        Color tc = (i == mvp) ? (Color){250, 220, 130, 255}
                              : L->ui.text_col;
        ui_draw_text(&L->ui, s->name, sx + S(20), rowy, 18, tc);
        ui_draw_text(&L->ui, team_name_for_mode(s->team, (int)g->match.mode),
                     sx + S(280), rowy, 18, tc);
        ui_draw_text(&L->ui, TextFormat("%d", s->kills),  sx + S(400), rowy, 18, tc);
        ui_draw_text(&L->ui, TextFormat("%d", s->deaths), sx + S(490), rowy, 18, tc);
        ui_draw_text(&L->ui, TextFormat("%d", s->score),  sx + S(590), rowy, 18, tc);
        rowy += S(28);
    }

    /* P09 — three-card map vote panel. Drawn between the scoreboard and
     * the "Next round" line when a vote is active. Cards are side-by-
     * side; the local player's choice (if any) gets a highlighted
     * border. Click "Vote" → apply_map_vote → server tally + rebroadcast.
     * Thumbnails are placeholder gray rects — real art comes with P13. */
    if (vote_show) {
        int candidates[3] = { g->lobby.vote_map_a, g->lobby.vote_map_b,
                              g->lobby.vote_map_c };
        uint32_t masks[3] = { g->lobby.vote_mask_a, g->lobby.vote_mask_b,
                              g->lobby.vote_mask_c };
        int n_cards = 0;
        for (int i = 0; i < 3; ++i) if (candidates[i] >= 0) n_cards++;
        int card_w = S(220), card_gap = S(16);
        int total_w = n_cards * card_w + (n_cards > 1 ? (n_cards - 1) * card_gap : 0);
        int vx = (sw - total_w) / 2;

        const char *vote_hdr = "VOTE NEXT MAP";
        int hw = ui_measure(&L->ui, vote_hdr, 18);
        ui_draw_text(&L->ui, vote_hdr, (sw - hw) / 2, vote_panel_top - S(2),
                     18, (Color){240, 220, 180, 255});

        /* Find which candidate this slot voted for, if any. */
        int my_choice = -1;
        if (g->local_slot_id >= 0) {
            uint32_t my_bit = 1u << (uint32_t)g->local_slot_id;
            if      (masks[0] & my_bit) my_choice = 0;
            else if (masks[1] & my_bit) my_choice = 1;
            else if (masks[2] & my_bit) my_choice = 2;
        }

        int drawn = 0;
        for (int i = 0; i < 3; ++i) {
            if (candidates[i] < 0) continue;
            Rectangle cr = (Rectangle){
                vx + drawn * (card_w + card_gap),
                vote_panel_top + S(22),
                card_w, vote_panel_h - S(30)
            };
            bool mine = (my_choice == i);
            Color bg = mine ? (Color){52, 86, 56, 240}
                            : (Color){28, 34, 44, 240};
            DrawRectangleRec(cr, bg);
            DrawRectangleLinesEx(cr, L->ui.scale,
                                 mine ? (Color){180, 230, 130, 255}
                                      : L->ui.panel_edge);
            /* Placeholder thumbnail (real art lands at P13/P16). */
            Rectangle thr = (Rectangle){
                cr.x + S(10), cr.y + S(10),
                cr.width - 2 * S(10), S(64)
            };
            DrawRectangleRec(thr, (Color){50, 56, 70, 255});
            DrawRectangleLinesEx(thr, L->ui.scale, (Color){70, 80, 100, 255});

            const MapDef *md = map_def(candidates[i]);
            const char *name = (md && md->display_name[0]) ? md->display_name : "?";
            ui_draw_text(&L->ui, name, (int)thr.x,
                         (int)(thr.y + thr.height + S(6)),
                         16, (Color){220, 230, 240, 255});
            if (md && md->blurb[0]) {
                ui_draw_text(&L->ui, md->blurb, (int)thr.x,
                             (int)(thr.y + thr.height + S(26)),
                             12, (Color){170, 190, 210, 255});
            }

            /* Live tally — popcount of the candidate's vote mask. */
            int votes = 0;
            for (uint32_t m = masks[i]; m; m >>= 1) votes += (int)(m & 1u);
            char tally[24];
            snprintf(tally, sizeof tally, "Votes: %d", votes);
            int tw = ui_measure(&L->ui, tally, 14);
            ui_draw_text(&L->ui, tally,
                         (int)(cr.x + cr.width - tw - S(8)),
                         (int)cr.y + S(6),
                         14, mine ? (Color){240, 240, 220, 255}
                                  : L->ui.text_dim);

            /* Vote button at the bottom of the card. */
            Rectangle vbr = (Rectangle){
                cr.x + S(10),
                cr.y + cr.height - S(34),
                cr.width - 2 * S(10), S(28)
            };
            bool can_vote = (g->local_slot_id >= 0) && !mine;
            const char *vlbl = mine ? "VOTED" : "Vote";
            if (ui_button(&L->ui, vbr, vlbl, can_vote)) {
                apply_map_vote(g, i);
            }
            drawn++;
        }
    }

    /* Countdown to next lobby. */
    char cd[64];
    snprintf(cd, sizeof cd, "Next round in %.0fs",
             (double)g->match.summary_remaining);
    int cd_w = ui_measure(&L->ui, cd, 20);
    ui_draw_text(&L->ui, cd, (sw - cd_w) / 2, sh - S(80), 20,
                 (Color){180, 200, 220, 255});

    if (ui_button(&L->ui,
                  (Rectangle){sw - S(220), sh - S(60), S(180), S(44)},
                  "Leave", true))
    {
        g->mode = MODE_TITLE;
    }

    ui_end(&L->ui);
}

/* ---- Match overlay (top-of-screen score/timer in MODE_MATCH) ----- */

void match_overlay_draw(Game *g, int sw, int sh) {
    (void)sh;
    /* Recompute scale here; we're not inside a UIContext for the
     * overlay (called from draw_diag). */
    float sc = ui_compute_scale(GetScreenHeight());
    int x = sw / 2 - (int)(220 * sc);
    int y = (int)(8 * sc);
    int w = (int)(440 * sc);
    int h = (int)(40 * sc);
    DrawRectangle(x, y, w, h, (Color){8, 12, 18, 200});
    DrawRectangleLines(x, y, w, h, (Color){70, 90, 120, 255});

    char tline[96];
    if (g->match.mode == MATCH_MODE_FFA) {
        int best = -1, best_score = -2147483647;
        for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
            if (!g->lobby.slots[i].in_use) continue;
            if (g->lobby.slots[i].score > best_score) {
                best_score = g->lobby.slots[i].score; best = i;
            }
        }
        snprintf(tline, sizeof tline,
                 "FFA · leader: %s [%d/%d] · %.0fs",
                 best >= 0 ? g->lobby.slots[best].name : "—",
                 best_score, g->match.score_limit,
                 (double)g->match.time_remaining);
    } else {
        snprintf(tline, sizeof tline,
                 "%s · R %d - B %d · /%d · %.0fs",
                 match_mode_name(g->match.mode),
                 g->match.team_score[MATCH_TEAM_RED],
                 g->match.team_score[MATCH_TEAM_BLUE],
                 g->match.score_limit,
                 (double)g->match.time_remaining);
    }
    int sz = (int)(18 * sc + 0.5f);
    Vector2 tv = MeasureTextEx(GetFontDefault(), tline, (float)sz, (float)sz / 10.0f);
    DrawTextEx(GetFontDefault(), tline,
               (Vector2){ (float)x + (w - tv.x) * 0.5f,
                          (float)y + (h - tv.y) * 0.5f },
               (float)sz, (float)sz / 10.0f,
               (Color){220, 230, 240, 255});
}
