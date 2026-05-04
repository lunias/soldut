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
        L->request_host = true;
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
    if (!L->lobby_loadout_synced) {
        L->lobby_chassis   = me->loadout.chassis_id;
        L->lobby_primary   = me->loadout.primary_id;
        L->lobby_secondary = me->loadout.secondary_id;
        L->lobby_armor     = me->loadout.armor_id;
        L->lobby_jet       = me->loadout.jetpack_id;
        L->lobby_team      = me->team;
        L->lobby_loadout_synced = 1;
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
} PlayerListCtx;

static void player_row(const UIContext *u, Rectangle row, int idx,
                       bool hover, bool selected, void *user)
{
    (void)hover; (void)selected;
    const PlayerListCtx *ctx = (const PlayerListCtx *)user;
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
}

void lobby_screen_run(LobbyUIState *L, Game *g, int sw, int sh) {
    Vec2 mouse = (Vec2){ (float)GetMouseX(), (float)GetMouseY() };
    float dt = GetFrameTime();
    ui_begin(&L->ui, mouse, dt, sh);

    ClearBackground((Color){10, 12, 16, 255});
    sync_loadout_from_server(L, g);

    /* ---- Top strip: title + match info ---- */
    ui_draw_text(&L->ui, "LOBBY", S(32), S(20), 28,
                 (Color){200, 220, 240, 255});
    char modeline[96];
    snprintf(modeline, sizeof modeline,
             "%s · %s · score %d · time %.0fs · ff=%s",
             match_mode_name(g->match.mode),
             map_def(g->match.map_id)->display_name,
             g->match.score_limit,
             (double)g->match.time_limit,
             g->match.friendly_fire ? "ON" : "off");
    ui_draw_text(&L->ui, modeline, S(32), S(60), 18,
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
    int header_h  = S(96);                  /* room for title + modeline + countdown */
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
    PlayerListCtx pctx = {
        .lobby = &g->lobby, .order = order, .n = n,
        .mode = (int)g->match.mode
    };
    ui_list_custom(&L->ui, list_r, n, 32, -1, player_row, &pctx);

    /* ---- Loadout panel (right) ---- */
    int lx = lp_x;
    int ly = left_top;
    int row_h = S(44), gap = S(8);
    int rows_total = (g->match.mode == MATCH_MODE_TDM ||
                      g->match.mode == MATCH_MODE_CTF) ? 7 : 6;
    int panel_h = row_h * (rows_total + 1) + gap * rows_total + S(28);
    Rectangle lp = (Rectangle){lx - S(12), ly - S(12),
                               (float)(lp_w + S(24)),
                               (float)panel_h};
    ui_panel_default(&L->ui, lp);
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

    if (g->match.mode == MATCH_MODE_TDM || g->match.mode == MATCH_MODE_CTF) {
        if (ui_button(&L->ui, (Rectangle){lx, ly, lp_w, row_h},
                      TextFormat("Team: %s", team_name(L->lobby_team)), true))
        {
            int next = (L->lobby_team == MATCH_TEAM_RED) ? MATCH_TEAM_BLUE :
                       (L->lobby_team == MATCH_TEAM_BLUE) ? MATCH_TEAM_NONE :
                                                             MATCH_TEAM_RED;
            apply_team_change(L, g, next);
        } ly += row_h + gap;
    }

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

    /* Scoreboard. */
    int sboard_w = S(720);
    int sx = (sw - sboard_w) / 2, sy = S(170);
    Rectangle sr = (Rectangle){sx, sy, sboard_w, sh - sy - S(120)};
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
