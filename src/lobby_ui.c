#include "lobby_ui.h"

#include "prefs.h"

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

void lobby_ui_reset_session(LobbyUIState *L) {
    if (!L) return;
    /* wan-fixes-7 — flip the "pushed to server" tracker so the next
     * lobby entry re-publishes our cached draft to the new server.
     *
     * Critically, DO NOT reset `lobby_loadout_synced` here: that flag
     * gates the "snap UI from server defaults" branch in
     * sync_loadout_from_server. If we cleared it, the next session
     * would overwrite the user's cached chassis / weapon / armor
     * picks with the new server's defaults BEFORE pushing, defeating
     * the entire fix. Leaving it set keeps the UI authoritative on
     * reconnect (push wins); a first-ever launch still has it at 0
     * from lobby_ui_init's memset and gets the normal sync-then-push
     * flow.
     *
     * Preserves: player_name, chassis/primary/secondary/armor/jet,
     * team, connect_addr. */
    L->lobby_loadout_pushed  = 0;
    L->setup_initialized     = false;
    /* Modal / transient state — drop anything that wouldn't make sense
     * to carry into the next session. */
    L->kick_target_slot      = -1;
    L->ban_target_slot       = -1;
    L->host_starting         = false;
    L->host_starting_t0      = 0.0;
    L->host_starting_status[0] = '\0';
    /* wan-fixes-11 — match-loading overlay state. Always clear on
     * session reset so the next host/connect doesn't carry over
     * stale "Loading match..." UX from the previous session. */
    L->match_loading         = false;
    L->match_loading_t0      = 0.0;
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
            { "F2",                    "Mute audio (paired-process tests)" },
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
    /* wan-fixes-9 — while the dedicated-child spawn is in flight,
     * both buttons are non-interactive (the overlay grays the screen
     * and the host_starting guard in main.c ignores request_start_host
     * anyway). Back is gated too because flipping g->mode to
     * MODE_TITLE mid-spawn would orphan the child handle in
     * s_host_start. The visual "lock" comes from the overlay + the
     * button-bg dim below. */
    bool locked = L->host_starting;
    int bw = S(220), bh = S(48);
    int by = y + S(8);
    int bx_left  = panel_x;
    int bx_right = panel_x + panel_w - bw;
    if (ui_button(&L->ui, (Rectangle){bx_left, by, bw, bh}, "Back", !locked)) {
        L->setup_initialized = false;     /* re-seed next time */
        g->mode = MODE_TITLE;
    }
    /* The accent-color button on the right communicates "primary action". */
    Rectangle start_r = (Rectangle){bx_right, by, bw, bh};
    bool start_hover = !locked && ui_point_in_rect(L->ui.mouse, start_r);
    Color start_bg = locked       ? (Color){40, 90, 60, 255}
                   : start_hover  ? (Color){80, 200, 120, 255}
                                  : (Color){60, 160, 90, 255};
    DrawRectangleRec(start_r, start_bg);
    DrawRectangleLinesEx(start_r, L->ui.scale, L->ui.panel_edge);
    int sl_w = ui_measure(&L->ui, "Start Hosting", 20);
    ui_draw_text(&L->ui, "Start Hosting",
                 (int)(start_r.x + (start_r.width - sl_w) * 0.5f),
                 (int)(start_r.y + (start_r.height - 20*L->ui.scale) * 0.5f),
                 20, locked ? (Color){12, 22, 14, 160}
                            : (Color){12, 22, 14, 255});
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

/* wan-fixes-9 — overlay rendered on top of host_setup_screen_run when
 * the dedicated-child spawn + connect-poll is in flight. Two layers:
 *   1. Full-screen translucent dark rect that drops the underlying
 *      widgets to ~30% perceived brightness (the "locked-in" cue).
 *   2. Centered panel with the rotating status message + an
 *      indeterminate progress bar (a fixed-width segment that sweeps
 *      left-to-right and wraps; same affordance as Windows
 *      "marquee"-style progress controls). */
void host_setup_screen_draw_overlay(LobbyUIState *L, int sw, int sh) {
    if (!L) return;

    /* Layer 1 — full-screen scrim. 60% alpha black gets us a clear
     * "modal blocked" feel without making the underlying widgets
     * unreadable (designers sometimes want to read the chosen mode /
     * map while the bar is spinning so they know what's being
     * started). */
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 153});

    /* Layer 2 — panel. Sized so the indeterminate bar reads at any
     * window scale without crowding the status text. */
    int panel_w = S(520);
    int panel_h = S(140);
    int panel_x = (sw - panel_w) / 2;
    int panel_y = (sh - panel_h) / 2;
    Rectangle panel = (Rectangle){panel_x, panel_y, panel_w, panel_h};
    DrawRectangleRec(panel, (Color){16, 22, 32, 240});
    DrawRectangleLinesEx(panel, L->ui.scale * 2.0f,
                         (Color){80, 120, 180, 255});

    /* Headline. */
    int title_size = 22;
    const char *title = "Starting server...";
    int tw = ui_measure(&L->ui, title, title_size);
    ui_draw_text(&L->ui, title,
                 panel_x + (panel_w - tw) / 2,
                 panel_y + S(16),
                 title_size, (Color){220, 230, 240, 255});

    /* Status sub-line — host_starting_status is set by main.c to
     * "Spawning dedicated server...", then "Waiting for server...",
     * then "Connecting...". An empty buffer falls back to a generic
     * line. */
    int sub_size = 16;
    const char *sub = (L->host_starting_status[0] != 0)
                         ? L->host_starting_status
                         : "Working...";
    int sw_  = ui_measure(&L->ui, sub, sub_size);
    ui_draw_text(&L->ui, sub,
                 panel_x + (panel_w - sw_) / 2,
                 panel_y + S(50),
                 sub_size, (Color){160, 180, 200, 255});

    /* Indeterminate progress bar. Track is full-width inside the
     * panel; the moving "thumb" is 30% of the track width and sweeps
     * left → right → left. Period 1.6 s — slow enough to look
     * intentional, fast enough that the user knows the process isn't
     * frozen. */
    int track_h     = S(10);
    int track_inset = S(24);
    int track_x     = panel_x + track_inset;
    int track_y     = panel_y + panel_h - S(34);
    int track_w     = panel_w - 2 * track_inset;
    Rectangle track = (Rectangle){track_x, track_y, track_w, track_h};
    DrawRectangleRec(track, (Color){32, 40, 52, 255});
    DrawRectangleLinesEx(track, 1.0f, (Color){60, 80, 110, 255});

    double now = GetTime();
    double elapsed = now - L->host_starting_t0;
    if (elapsed < 0) elapsed = 0;
    double period = 1.6;
    double t_norm = (elapsed - period * (int)(elapsed / period)) / period;
    /* Triangle wave 0→1→0 so the thumb bounces rather than wraps —
     * looks less like a CSS loader, more like a heartbeat. */
    double tri = (t_norm < 0.5) ? (t_norm * 2.0) : (2.0 - t_norm * 2.0);
    int thumb_w = (int)(track_w * 0.30f);
    int thumb_x = track_x + (int)((track_w - thumb_w) * tri);
    DrawRectangle(thumb_x, track_y, thumb_w, track_h,
                  (Color){80, 200, 120, 255});

    /* Elapsed-time label so the user can tell roughly how long they've
     * been waiting (and whether it's getting close to the 5 s
     * timeout). */
    char et[32];
    snprintf(et, sizeof et, "%.1fs", elapsed);
    int et_size = 12;
    int etw = ui_measure(&L->ui, et, et_size);
    ui_draw_text(&L->ui, et,
                 panel_x + panel_w - etw - S(12),
                 panel_y + panel_h - S(16),
                 et_size, (Color){120, 140, 160, 200});
}

/* wan-fixes-11 — driven each frame from main.c's MODE_LOBBY and
 * MODE_MATCH render branches. Decides whether the "Loading match…"
 * overlay should be visible right now. Latches the t0 timestamp on
 * the rising edge so the elapsed-time pill counts from when the user
 * first saw the overlay. */
void lobby_ui_update_match_loading(LobbyUIState *L, Game *g) {
    if (!L || !g) return;
    bool active = false;
    /* Last sliver of countdown — start_round is firing on the host
     * right about now, ROUND_START is in flight. */
    if (g->match.phase == MATCH_PHASE_COUNTDOWN &&
        g->match.countdown_remaining > 0.0f &&
        g->match.countdown_remaining < 1.0f)
    {
        active = true;
    }
    /* We've entered MODE_MATCH but the first snapshot hasn't
     * resolved our local mech yet — render is about to draw an empty
     * world. */
    if (g->mode == MODE_MATCH && g->world.local_mech_id < 0) {
        active = true;
    }
    /* Rising edge → latch t0. Cleared on falling edge. */
    if (active && !L->match_loading) {
        L->match_loading_t0 = GetTime();
    }
    L->match_loading = active;
}

void match_loading_overlay_draw(LobbyUIState *L, Game *g, int sw, int sh) {
    if (!L || !L->match_loading) return;

    /* Layer 1 — scrim. Heavier than the host-setup overlay (200 vs
     * 153) because the user is meant to read the panel, not the
     * lobby underneath. */
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 200});

    int panel_w = S(560);
    int panel_h = S(180);
    int panel_x = (sw - panel_w) / 2;
    int panel_y = (sh - panel_h) / 2;
    Rectangle panel = (Rectangle){panel_x, panel_y, panel_w, panel_h};
    DrawRectangleRec(panel, (Color){16, 22, 32, 240});
    DrawRectangleLinesEx(panel, L->ui.scale * 2.0f,
                         (Color){80, 160, 220, 255});

    /* Headline — round number when relevant, generic otherwise. */
    char title[64];
    if (g->match.rounds_per_match > 1) {
        snprintf(title, sizeof title, "Round %d / %d",
                 g->match.rounds_played + 1, g->match.rounds_per_match);
    } else {
        snprintf(title, sizeof title, "Loading match");
    }
    int title_size = 24;
    int tw = ui_measure(&L->ui, title, title_size);
    ui_draw_text(&L->ui, title,
                 panel_x + (panel_w - tw) / 2,
                 panel_y + S(18),
                 title_size, (Color){230, 240, 250, 255});

    /* Map name — comes from the runtime registry when populated, the
     * static map table otherwise. */
    char map_line[80];
    const MapDef *md = map_def(g->match.map_id);
    /* `display_name` is a char array (always-addressable) — check its
     * first byte for "populated" rather than the array address, which
     * GCC -Werror=address flags as a constant comparison. */
    const char *map_name = (md && md->display_name[0]) ? md->display_name : "?";
    const char *mode_name = match_mode_name((MatchModeId)g->match.mode);
    /* ASCII separator — U+00B7 falls back to `?` in the body font. */
    snprintf(map_line, sizeof map_line, "%s - %s",
             map_name, mode_name ? mode_name : "?");
    int map_size = 16;
    int mw = ui_measure(&L->ui, map_line, map_size);
    ui_draw_text(&L->ui, map_line,
                 panel_x + (panel_w - mw) / 2,
                 panel_y + S(56),
                 map_size, (Color){180, 200, 220, 255});

    /* Status sub-line — what's blocking the world from rendering. */
    const char *status;
    if (g->mode == MODE_MATCH && g->world.local_mech_id < 0) {
        status = "Waiting for first snapshot...";
    } else if (g->match.phase == MATCH_PHASE_COUNTDOWN) {
        status = "Building map...";
    } else {
        status = "Loading match...";
    }
    int sub_size = 16;
    int sw_  = ui_measure(&L->ui, status, sub_size);
    ui_draw_text(&L->ui, status,
                 panel_x + (panel_w - sw_) / 2,
                 panel_y + S(82),
                 sub_size, (Color){150, 170, 190, 255});

    /* Indeterminate bar (same triangle-wave thumb as the host-setup
     * overlay so the two screens feel like one family). */
    int track_h     = S(10);
    int track_inset = S(28);
    int track_x     = panel_x + track_inset;
    int track_y     = panel_y + panel_h - S(38);
    int track_w     = panel_w - 2 * track_inset;
    Rectangle track = (Rectangle){track_x, track_y, track_w, track_h};
    DrawRectangleRec(track, (Color){32, 40, 52, 255});
    DrawRectangleLinesEx(track, 1.0f, (Color){60, 80, 110, 255});

    double elapsed = GetTime() - L->match_loading_t0;
    if (elapsed < 0) elapsed = 0;
    double period = 1.4;
    double t_norm = (elapsed - period * (int)(elapsed / period)) / period;
    double tri = (t_norm < 0.5) ? (t_norm * 2.0) : (2.0 - t_norm * 2.0);
    int thumb_w = (int)(track_w * 0.30f);
    int thumb_x = track_x + (int)((track_w - thumb_w) * tri);
    DrawRectangle(thumb_x, track_y, thumb_w, track_h,
                  (Color){80, 180, 240, 255});

    /* Elapsed seconds pill — same affordance as the host-setup
     * overlay so users learn it once and recognize it. */
    char et[32];
    snprintf(et, sizeof et, "%.1fs", elapsed);
    int et_size = 12;
    int etw = ui_measure(&L->ui, et, et_size);
    ui_draw_text(&L->ui, et,
                 panel_x + panel_w - etw - S(14),
                 panel_y + panel_h - S(18),
                 et_size, (Color){120, 140, 160, 200});
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
        /* wan-fixes-8 — persist the address as part of the saved
         * prefs so the next launch's Direct Connect screen pre-fills
         * what the user typed. Also captures player_name in case the
         * user edited it on the title screen this session. */
        lobby_ui_save_prefs(L);
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

/* Draw text inside a fixed pixel width, truncating with "..." if the
 * full string overflows. ui_draw_text doesn't clamp — long map blurbs
 * in the vote card spilled past the card edge before this. */
static void ui_draw_text_clipped(const UIContext *u, const char *text,
                                 int x, int y, int base_size, Color col,
                                 int max_width)
{
    if (!text || !*text) return;
    int full = ui_measure(u, text, base_size);
    if (full <= max_width) {
        ui_draw_text(u, text, x, y, base_size, col);
        return;
    }
    char buf[160];
    int len = (int)strlen(text);
    if (len >= (int)sizeof buf) len = (int)sizeof buf - 1;
    memcpy(buf, text, (size_t)len);
    buf[len] = '\0';
    /* Trim from the right until "<trimmed>..." fits. */
    while (len > 0) {
        buf[len] = '\0';
        char tmp[164];
        snprintf(tmp, sizeof tmp, "%s...", buf);
        if (ui_measure(u, tmp, base_size) <= max_width) {
            ui_draw_text(u, tmp, x, y, base_size, col);
            return;
        }
        len--;
    }
    /* Even "..." overflows — bail. */
}

static int next_in_cycle(int current, const int *choices, int n) {
    int idx = 0;
    for (int i = 0; i < n; ++i) if (choices[i] == current) { idx = i; break; }
    return choices[(idx + 1) % n];
}

/* wan-fixes-11 — step by ±N in the choice list with wraparound.
 * Used by the loadout cycle buttons: LMB sends step=+1, RMB sends
 * step=-1. Positive modulo so a single backward step on the first
 * entry lands cleanly on the last. */
static int step_in_cycle(int current, const int *choices, int n, int step) {
    if (n <= 0) return current;
    int idx = 0;
    for (int i = 0; i < n; ++i) if (choices[i] == current) { idx = i; break; }
    int next = ((idx + step) % n + n) % n;
    return choices[next];
}

static void sync_loadout_from_server(LobbyUIState *L, Game *g) {
    if (g->local_slot_id < 0) return;
    const LobbySlot *me = &g->lobby.slots[g->local_slot_id];
    if (!me->in_use) return;
    /* wan-fixes-10 — UI draft is authoritative. The previous "snap
     * from server first, push back if not yet pushed" pattern silently
     * clobbered prefs-loaded values (wan-fixes-8) because main.c
     * seeds L->lobby_* from soldut-prefs.cfg before connect, but the
     * fresh server's slot starts at defaults — the snap then
     * overwrote prefs with server defaults BEFORE the push ran. Both
     * branches then sent the SAME defaults to the server, locking the
     * user's loadout to Sidearm/Trooper regardless of prefs.
     *
     * Fix: skip the snap. L->lobby_* is set in main.c at startup from
     * prefs (with sensible lobby_ui_init defaults when no prefs file
     * exists), updated by the user's clicks during the session, and
     * preserved across disconnects via lobby_ui_reset_session (which
     * intentionally does NOT reset lobby_chassis/primary/etc.). The
     * snap was a no-op for fresh launches anyway (lobby_add_slot uses
     * mech_default_loadout, identical to lobby_ui_init's defaults),
     * and harmful for any session with custom prefs or a remembered
     * draft. We still call lobby_loadout_synced so the team auto-sync
     * branch below knows we've completed the first-sync handshake. */
    L->lobby_loadout_synced = 1;
    /* wan-fixes-7 — push the local UI draft to the server on each
     * fresh session. Without this, a re-host (parent process spawns
     * a new dedicated child) leaves the user's cached draft visible
     * in the lobby but never communicated to the new server, and
     * round_start spawns mechs from the default loadout. The
     * `lobby_loadout_pushed` flag is reset by lobby_ui_reset_session
     * in main.c's disconnect / leave paths. */
    if (!L->lobby_loadout_pushed && g->local_slot_id >= 0) {
        MechLoadout lo = (MechLoadout){
            .chassis_id   = L->lobby_chassis,
            .primary_id   = L->lobby_primary,
            .secondary_id = L->lobby_secondary,
            .armor_id     = L->lobby_armor,
            .jetpack_id   = L->lobby_jet,
        };
        if (g->net.role == NET_ROLE_CLIENT) {
            net_client_send_loadout(&g->net, lo);
        } else {
            lobby_set_loadout(&g->lobby, g->local_slot_id, lo);
        }
        /* Same for team. apply_team_change normally fires on click,
         * but a reconnect with the previously-picked team needs an
         * explicit push too. FFA's "Playing" default = MATCH_TEAM_FFA;
         * non-FFA modes (TDM/CTF) use MATCH_TEAM_RED/_BLUE. */
        if (g->net.role == NET_ROLE_CLIENT) {
            net_client_send_team_change(&g->net, L->lobby_team);
        } else {
            lobby_set_team(&g->lobby, g->local_slot_id, L->lobby_team);
        }
        L->lobby_loadout_pushed = 1;
    } else {
        /* Already pushed this session — keep team in sync from the
         * server in case the server force-assigned (auto-balance). */
        L->lobby_team = me->team;
    }
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

/* wan-fixes-8 — persist the UI draft to soldut-prefs.cfg. Called from
 * every cycle-button click handler so the user's picks survive
 * client-process restarts (not just disconnect / re-host, which
 * wan-fixes-7 already handled via the in-memory cache). Exported
 * via `lobby_ui_save_prefs` so main.c can also call it on title-
 * screen transitions to capture player_name edits. */
void lobby_ui_save_prefs(const LobbyUIState *L) {
    if (!L) return;
    UserPrefs p;
    prefs_defaults(&p);
    snprintf(p.name, sizeof p.name, "%s", L->player_name);
    p.loadout.chassis_id   = L->lobby_chassis;
    p.loadout.primary_id   = L->lobby_primary;
    p.loadout.secondary_id = L->lobby_secondary;
    p.loadout.armor_id     = L->lobby_armor;
    p.loadout.jetpack_id   = L->lobby_jet;
    p.team                 = L->lobby_team;
    snprintf(p.connect_addr, sizeof p.connect_addr, "%s", L->connect_addr);
    prefs_save(&p, PREFS_PATH);
}


static void apply_loadout_change(LobbyUIState *L, Game *g) {
    if (g->local_slot_id < 0) return;
    MechLoadout lo = build_local_loadout(L);
    if (g->net.role == NET_ROLE_CLIENT) {
        net_client_send_loadout(&g->net, lo);
    } else {
        lobby_set_loadout(&g->lobby, g->local_slot_id, lo);
    }
    lobby_ui_save_prefs(L);
}

static void apply_team_change(LobbyUIState *L, Game *g, int new_team) {
    L->lobby_team = new_team;
    if (g->local_slot_id < 0) return;
    if (g->net.role == NET_ROLE_CLIENT) {
        net_client_send_team_change(&g->net, new_team);
    } else {
        lobby_set_team(&g->lobby, g->local_slot_id, new_team);
    }
    lobby_ui_save_prefs(L);
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
     * rendered in lobby_screen_run after the list iteration.
     *
     * Sized for click-target legibility (~72×28 at 1× scale). The
     * buttons sit flush with the right edge of the row; layout above
     * leaves a comfortable ~120 px gap after the K/D score column. */
    if (ctx->host_view && !s->is_host && ctx->ui_state) {
        float bw = 72.0f * u->scale;
        float bh = row.height - 8.0f * u->scale;
        float by = row.y + 4.0f * u->scale;
        float bx_ban  = row.x + row.width - bw - 8.0f * u->scale;
        float bx_kick = bx_ban - bw - 8.0f * u->scale;
        Rectangle rk = (Rectangle){ bx_kick, by, bw, bh };
        Rectangle rb = (Rectangle){ bx_ban,  by, bw, bh };

        bool hk = ui_point_in_rect(u->mouse, rk);
        DrawRectangleRec(rk, hk ? (Color){180, 150,  90, 240}
                                : (Color){ 90,  70,  50, 220});
        DrawRectangleLinesEx(rk, u->scale, (Color){200, 170, 110, 255});
        int twk = ui_measure(u, "Kick", 16);
        ui_draw_text(u, "Kick",
                     (int)(rk.x + (rk.width  - twk) * 0.5f),
                     (int)(rk.y + (rk.height - 16*u->scale) * 0.5f),
                     16, hk ? (Color){20, 18, 12, 255} : u->text_col);
        if (hk && u->mouse_pressed) {
            ctx->ui_state->kick_target_slot = slot;
            ctx->ui_state->ban_target_slot  = -1;
        }

        bool hb = ui_point_in_rect(u->mouse, rb);
        DrawRectangleRec(rb, hb ? (Color){200,  80,  80, 240}
                                : (Color){120,  40,  50, 220});
        DrawRectangleLinesEx(rb, u->scale, (Color){220, 110, 110, 255});
        int twb = ui_measure(u, "Ban", 16);
        ui_draw_text(u, "Ban",
                     (int)(rb.x + (rb.width  - twb) * 0.5f),
                     (int)(rb.y + (rb.height - 16*u->scale) * 0.5f),
                     16, hb ? (Color){20, 12, 14, 255} : u->text_col);
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

    /* wan-fixes-6 — "host" is now also the FIRST client to connect to
     * a dedicated server (which has no in-process host player). The
     * server marks that peer's slot is_host=true in net.c on accept;
     * the lobby_list broadcast propagates it to everyone, so any
     * client can identify the host. */
    bool slot_is_host = (g->local_slot_id >= 0 &&
                         g->local_slot_id < MAX_LOBBY_SLOTS &&
                         g->lobby.slots[g->local_slot_id].in_use &&
                         g->lobby.slots[g->local_slot_id].is_host);
    bool is_host = (g->net.role == NET_ROLE_SERVER || g->offline_solo ||
                    slot_is_host);
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
            } else if (g->net.role == NET_ROLE_CLIENT && slot_is_host) {
                /* wan-fixes-6 — dedicated server flow: host is a
                 * client. Push the change over the wire; server
                 * validates + re-broadcasts MATCH_STATE so every
                 * client converges. */
                net_client_send_host_setup(&g->net,
                    (int)g->match.mode, g->match.map_id,
                    g->match.score_limit, (int)g->match.time_limit,
                    g->match.friendly_fire);
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
            } else if (g->net.role == NET_ROLE_CLIENT && slot_is_host) {
                /* wan-fixes-6 — same dedicated-host wire push as the
                 * mode-change branch. */
                net_client_send_host_setup(&g->net,
                    (int)g->match.mode, g->match.map_id,
                    g->match.score_limit, (int)g->match.time_limit,
                    g->match.friendly_fire);
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

    /* P09 — kick/ban confirmation modal: rendered LAST in
     * lobby_screen_run (see end of function) so it overlays the chat
     * panel and loadout column. The original placement here meant the
     * chat panel was painted on top of the modal's lower half,
     * obscuring the buttons. Empty placeholder kept here as a marker;
     * the actual draw is at the bottom. */

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

    /* wan-fixes-14 — once the local player clicks "READY UP", the
     * lobby treats the loadout as locked: team picker, all five
     * cycle buttons, and the loadout-driven commit path are
     * disabled until the user clicks the ready button a second
     * time to un-ready. Stops the "click ready, then keep tweaking
     * weapons" anti-pattern; round_start uses whatever the slot
     * held the moment ready latched. */
    bool me_ready = (g->local_slot_id >= 0)
                  ? g->lobby.slots[g->local_slot_id].ready
                  : false;

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
                bool hover  = !me_ready && ui_point_in_rect(L->ui.mouse, r);
                Color bg;
                if (me_ready && !active) {
                    bg = L->ui.button_disabled;
                } else if (active) {
                    bg = tslot[i].col;
                } else if (hover) {
                    bg = L->ui.button_hover;
                } else {
                    bg = L->ui.button_bg;
                }
                DrawRectangleRec(r, bg);
                DrawRectangleLinesEx(r, L->ui.scale, L->ui.panel_edge);
                int tw = ui_measure(&L->ui, tslot[i].label, 18);
                Color tc = active ? (Color){12, 18, 26, 255}
                                  : (me_ready ? L->ui.text_dim : L->ui.text_col);
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
                bool hover  = !me_ready && ui_point_in_rect(L->ui.mouse, r);
                Color bg;
                if (me_ready && !active) {
                    bg = L->ui.button_disabled;
                } else if (active) {
                    bg = L->ui.accent;
                } else if (hover) {
                    bg = L->ui.button_hover;
                } else {
                    bg = L->ui.button_bg;
                }
                DrawRectangleRec(r, bg);
                DrawRectangleLinesEx(r, L->ui.scale, L->ui.panel_edge);
                int tw = ui_measure(&L->ui, cells[i].label, 18);
                Color tc = active ? (Color){12, 18, 26, 255}
                                  : (me_ready ? L->ui.text_dim : L->ui.text_col);
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

    /* wan-fixes-11 — cycle buttons: LMB forward (+1), RMB back (-1).
     * Arrows on the button edges hint at the directional affordance;
     * the same gesture stays as the M4-era LMB-only flow for anyone
     * who never finds the right click.
     *
     * wan-fixes-14 — `enabled = !me_ready`. ui_cycle_button's
     * existing disabled branch paints the row dim + returns 0 so
     * the loadout draft stays frozen once the user commits. */
    bool can_edit = !me_ready;
    int step;
    step = ui_cycle_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                           TextFormat("Chassis: %s", chassis_label(L->lobby_chassis)),
                           can_edit);
    if (step != 0) {
        L->lobby_chassis = step_in_cycle(L->lobby_chassis, g_chassis_choices,
                                         (int)(sizeof g_chassis_choices / sizeof g_chassis_choices[0]), step);
        apply_loadout_change(L, g);
    } ly += row_h + gap;

    step = ui_cycle_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                           TextFormat("Primary: %s", primary_label(L->lobby_primary)),
                           can_edit);
    if (step != 0) {
        L->lobby_primary = step_in_cycle(L->lobby_primary, g_primary_choices,
                                         (int)(sizeof g_primary_choices / sizeof g_primary_choices[0]), step);
        apply_loadout_change(L, g);
    } ly += row_h + gap;

    step = ui_cycle_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                           TextFormat("Secondary: %s", primary_label(L->lobby_secondary)),
                           can_edit);
    if (step != 0) {
        L->lobby_secondary = step_in_cycle(L->lobby_secondary, g_secondary_choices,
                                           (int)(sizeof g_secondary_choices / sizeof g_secondary_choices[0]), step);
        apply_loadout_change(L, g);
    } ly += row_h + gap;

    step = ui_cycle_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                           TextFormat("Armor: %s", armor_label(L->lobby_armor)),
                           can_edit);
    if (step != 0) {
        L->lobby_armor = step_in_cycle(L->lobby_armor, g_armor_choices,
                                       (int)(sizeof g_armor_choices / sizeof g_armor_choices[0]), step);
        apply_loadout_change(L, g);
    } ly += row_h + gap;

    step = ui_cycle_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                           TextFormat("Jetpack: %s", jet_label(L->lobby_jet)),
                           can_edit);
    if (step != 0) {
        L->lobby_jet = step_in_cycle(L->lobby_jet, g_jet_choices,
                                     (int)(sizeof g_jet_choices / sizeof g_jet_choices[0]), step);
        apply_loadout_change(L, g);
    } ly += row_h + gap;

    /* wan-fixes-11 — Ready button visually distinct from the loadout
     * cycle row above so the player's eye lands on it as the primary
     * commit action. Gap + bright green chrome + larger font + 2-px
     * outline + subtle pulse on the not-ready idle state so it
     * reads as "press me."
     *
     * wan-fixes-14 — `me_ready` is now hoisted to the top of the
     * loadout panel so the cycle + team rows can gate on it. The
     * button itself stays clickable in both states (so the user
     * can toggle back to "not ready" and tweak loadout again). */
    /* Visual separator gap — half a row of breathing room. */
    ly += S(14);
    int rb_h = row_h + S(20);
    Rectangle rr = (Rectangle){lx, ly, lp_w, rb_h};
    bool ready_hover = ui_point_in_rect(L->ui.mouse, rr);

    /* Idle: a soft pulse on the green so the button reads as "do
     * this next." When ready, hold a darker confirmed-green and stop
     * pulsing. When hovered, jump to a brighter highlight. */
    double now = GetTime();
    float pulse = 0.5f + 0.5f * sinf((float)now * 3.6f);
    Color rb_col;
    if (me_ready) {
        rb_col = (Color){36, 120, 64, 255};
    } else if (ready_hover) {
        rb_col = (Color){100, 220, 130, 255};
    } else {
        /* Lerp between two greens by the pulse fraction. */
        unsigned char r = (unsigned char)(60 + (int)(pulse * 24));
        unsigned char G = (unsigned char)(170 + (int)(pulse * 40));
        unsigned char B = (unsigned char)(90 + (int)(pulse * 20));
        rb_col = (Color){r, G, B, 255};
    }
    DrawRectangleRec(rr, rb_col);
    /* 2 px outline picks the button out from neighboring tiles. */
    Color rb_edge = me_ready ? (Color){80, 200, 120, 255}
                             : (Color){180, 240, 200, 255};
    DrawRectangleLinesEx(rr, L->ui.scale * 2.0f, rb_edge);

    /* ASCII only — Atkinson body / VG5000 display / Steps Mono don't
     * carry U+2713 (✓), so the original `READY ✓` rendered with a
     * fallback `?` glyph (existing "Match over - back to lobby in"
     * label uses ASCII hyphen for the same reason). */
    const char *rl = me_ready ? "READY!" : "READY UP";
    int rl_font = 26;
    int trw = ui_measure(&L->ui, rl, rl_font);
    int trh = (int)((float)rl_font * L->ui.scale + 0.5f);
    Color rl_text = me_ready ? (Color){220, 250, 230, 255}
                             : (Color){14, 32, 18, 255};
    ui_draw_text(&L->ui, rl,
                 (int)(rr.x + (rr.width - (float)trw) * 0.5f),
                 (int)(rr.y + (rr.height - (float)trh) * 0.5f),
                 rl_font, rl_text);
    if (ready_hover && L->ui.mouse_pressed) apply_ready_toggle(g, !me_ready);
    ly += rb_h - row_h;       /* compensate the extra height vs the cycle rows */

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

    /* P09 — kick/ban confirmation modal. Rendered LAST so it lands on
     * top of the chat panel, loadout column, and bottom button row.
     * Distinct color theming (orange for kick, red for ban), an
     * informative subtitle, and a destructive-styled Confirm button
     * make the action unambiguous. */
    if (L->kick_target_slot >= 0 || L->ban_target_slot >= 0) {
        bool ban = (L->ban_target_slot >= 0);
        int  target = ban ? L->ban_target_slot : L->kick_target_slot;
        const LobbySlot *ts = (target >= 0 && target < MAX_LOBBY_SLOTS)
                              ? &g->lobby.slots[target] : NULL;
        if (!ts || !ts->in_use) {
            L->kick_target_slot = L->ban_target_slot = -1;
        } else {
            DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 200});

            int dw = S(520), dh = S(220);
            int dx = (sw - dw) / 2, dy = (sh - dh) / 2;

            Color accent = ban
                         ? (Color){220,  90,  90, 255}    /* red */
                         : (Color){230, 170,  90, 255};   /* orange */

            DrawRectangle(dx, dy, dw, dh, (Color){18, 22, 30, 250});
            DrawRectangle(dx, dy, dw, S(6), accent);
            DrawRectangleLinesEx((Rectangle){dx, dy, dw, dh},
                                 2.0f * L->ui.scale, accent);

            char title[80];
            snprintf(title, sizeof title, "%s %s?",
                     ban ? "Ban"  : "Kick", ts->name);
            int tsz = 26;
            int tw  = ui_measure(&L->ui, title, tsz);
            ui_draw_text(&L->ui, title, dx + (dw - tw) / 2, dy + S(22), tsz,
                         (Color){240, 235, 220, 255});

            const char *sub1 = ban
                             ? "Disconnect AND prevent reconnect."
                             : "Disconnect this player.";
            const char *sub2 = ban
                             ? "Saved to bans.txt; persists across host restarts."
                             : "They can rejoin freely.";
            int s1 = ui_measure(&L->ui, sub1, 16);
            int s2 = ui_measure(&L->ui, sub2, 14);
            ui_draw_text(&L->ui, sub1, dx + (dw - s1) / 2, dy + S(72), 16,
                         (Color){210, 220, 235, 255});
            ui_draw_text(&L->ui, sub2, dx + (dw - s2) / 2, dy + S(98), 14,
                         (Color){170, 185, 205, 230});

            int bw = S(170), bh = S(48);
            int by = dy + dh - bh - S(20);
            int bx_cancel  = dx + S(24);
            int bx_confirm = dx + dw - bw - S(24);
            Rectangle rc = (Rectangle){bx_cancel,  by, bw, bh};
            Rectangle rf = (Rectangle){bx_confirm, by, bw, bh};

            bool canc_hover = ui_point_in_rect(L->ui.mouse, rc);
            Color canc_bg = canc_hover
                          ? (Color){140, 160, 190, 255}
                          : (Color){ 95, 115, 145, 255};
            DrawRectangleRec(rc, canc_bg);
            DrawRectangleLinesEx(rc, 2.0f * L->ui.scale,
                                 (Color){170, 195, 225, 255});
            int wlab = ui_measure(&L->ui, "Cancel", 20);
            ui_draw_text(&L->ui, "Cancel",
                         (int)(rc.x + (rc.width - wlab) * 0.5f),
                         (int)(rc.y + (rc.height - 20 * L->ui.scale) * 0.5f),
                         20, RAYWHITE);
            bool cancel = canc_hover && L->ui.mouse_pressed;

            bool conf_hover = ui_point_in_rect(L->ui.mouse, rf);
            Color conf_bg = conf_hover
                          ? (Color){(uint8_t)fminf(255, accent.r + 30),
                                    (uint8_t)fminf(255, accent.g + 30),
                                    (uint8_t)fminf(255, accent.b + 30), 255}
                          : accent;
            DrawRectangleRec(rf, conf_bg);
            DrawRectangleLinesEx(rf, 2.0f * L->ui.scale,
                                 (Color){255, 255, 255, 200});
            const char *clab = ban ? "Ban" : "Kick";
            int cwid = ui_measure(&L->ui, clab, 20);
            ui_draw_text(&L->ui, clab,
                         (int)(rf.x + (rf.width - cwid) * 0.5f),
                         (int)(rf.y + (rf.height - 20 * L->ui.scale) * 0.5f),
                         20, (Color){20, 14, 10, 255});
            bool confirm = conf_hover && L->ui.mouse_pressed;

            if (cancel) {
                L->kick_target_slot = L->ban_target_slot = -1;
            } else if (confirm) {
                apply_kick_or_ban(g, target, ban);
                L->kick_target_slot = L->ban_target_slot = -1;
            }
        }
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
    /* Phase-aware: vote picker only renders in SUMMARY phase. After
     * the summary expires, the host transitions to COUNTDOWN (the
     * inter-round bridge before the next round); during that the
     * picker is gone but the scoreboard still reads. */
    bool in_summary  = (g->match.phase == MATCH_PHASE_SUMMARY);
    bool in_countdown= (g->match.phase == MATCH_PHASE_COUNTDOWN);
    bool vote_show = in_summary && (g->lobby.vote_map_a >= 0 ||
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

            /* Card content width = thumbnail width = card_w - 20.
             * Both name + blurb are clipped to fit; long blurbs
             * (e.g. Foundry's "Open floor with cover columns. Ground-
             * level chokepoints.") would otherwise spill past the
             * card edge. */
            int text_max_w = (int)thr.width;
            const MapDef *md = map_def(candidates[i]);
            const char *name = (md && md->display_name[0]) ? md->display_name : "?";
            ui_draw_text_clipped(&L->ui, name, (int)thr.x,
                                 (int)(thr.y + thr.height + S(6)),
                                 16, (Color){220, 230, 240, 255},
                                 text_max_w);
            if (md && md->blurb[0]) {
                ui_draw_text_clipped(&L->ui, md->blurb, (int)thr.x,
                                     (int)(thr.y + thr.height + S(26)),
                                     12, (Color){170, 190, 210, 255},
                                     text_max_w);
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

    /* Status line — phase-aware:
     *   SUMMARY  → "Next round in N s" using summary_remaining.
     *   COUNTDOWN→ big "Round X / Y starts in N s" using countdown_remaining
     *              (inter-round bridge — no lobby in between).
     * For the last round of a match (rounds_played + 1 ==
     * rounds_per_match), a SUMMARY → LOBBY transition fires instead;
     * the message reflects "Match over" so players know they're
     * heading back to ready-up. */
    if (in_countdown) {
        int round_n = g->match.rounds_played + 1;
        int round_total = g->match.rounds_per_match > 0
                          ? g->match.rounds_per_match : 1;
        char banner[80];
        snprintf(banner, sizeof banner,
                 "Round %d / %d starts in %.0fs",
                 round_n, round_total,
                 (double)g->match.countdown_remaining);
        int bsz = 32;
        int bw  = ui_measure(&L->ui, banner, bsz);
        ui_draw_text(&L->ui, banner, (sw - bw) / 2, sh - S(120), bsz,
                     (Color){240, 220, 140, 255});
    } else {
        const char *label = "Next round in";
        if (g->match.rounds_per_match > 0 &&
            g->match.rounds_played + 1 >= g->match.rounds_per_match)
        {
            /* ASCII-only — raylib's default font glyph table doesn't
             * cover U+2014 (em-dash); it rendered as a fallback "?" */
            label = "Match over - back to lobby in";
        }
        char cd[80];
        snprintf(cd, sizeof cd, "%s %.0fs",
                 label, (double)g->match.summary_remaining);
        int cd_w = ui_measure(&L->ui, cd, 20);
        ui_draw_text(&L->ui, cd, (sw - cd_w) / 2, sh - S(80), 20,
                     (Color){180, 200, 220, 255});
    }

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
