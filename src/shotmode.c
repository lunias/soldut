#include "shotmode.h"

#include "atmosphere.h"
#include "audio.h"
#include "config.h"
#include "ctf.h"
#include "decal.h"
#include "game.h"
#include "input.h"
#include "level.h"
#include "lobby.h"
#include "lobby_ui.h"
#include "log.h"
#include "maps.h"
#include "match.h"
#include "mech.h"
#include "hud.h"
#include "map_kit.h"
#include "mech_sprites.h"
#include "net.h"
#include "weapon_sprites.h"
#include "pickup.h"
#include "platform.h"
#include "physics.h"
#include "reconcile.h"
#include "render.h"
#include "simulate.h"
#include "snapshot.h"
#include "version.h"
#include "weapons.h"

#include "../third_party/raylib/src/raylib.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#define SHOT_MKDIR(path) _mkdir(path)
#else
#define SHOT_MKDIR(path) mkdir((path), 0755)
#endif

/* ---- Script representation ---------------------------------------- */

typedef enum {
    EV_PRESS = 0,
    EV_RELEASE,
    EV_TAP,
    EV_AIM,
    EV_MOUSE,
    EV_SHOT,
    EV_END,
    EV_GIVE_INVIS,    /* P05 debug — set powerup_invis_remaining on local mech */
    EV_FLAG_CARRY,    /* P07 debug — force flag_idx (ax) into CARRIED state by local mech */
    EV_TEAM_CHANGE,   /* lobby UX test — send/apply team-change for the local slot */
    EV_KILL_PEER,     /* P07 host-only — directly mech_kill the named peer (ax = mech_id) */
    EV_ARM_CARRY,     /* P07 host-only — arm flag (ax) with carrier mech (ay) on the server */
    EV_KICK,          /* P09 host-only — kick lobby slot (ax = slot) */
    EV_BAN,           /* P09 host-only — ban lobby slot (ax = slot) */
    EV_KICK_MODAL,    /* P09 host-only — show confirmation modal for slot (ax) */
    EV_BAN_MODAL,     /* P09 host-only — show ban confirmation modal for slot (ax) */
    EV_VOTE_MAP,      /* P09 — cast a map vote (ax = choice 0/1/2) */
    EV_BOT_TEAM,      /* host-only — flip bot's team (ax = slot, ay = team) */
    EV_ADD_BOT,       /* host-only — send ADD_BOT (ax = BotTier 0..3) */
    EV_DAMAGE,        /* M6 P04 — apply damage on named mech for the
                         color-tier shot test. ax = mech_id, ay = part,
                         az = amount. Drives the same mech_apply_damage
                         path real combat uses, so the FX_DAMAGE_NUMBER
                         spawn fires from the canonical seam. */
} EventKind;

typedef struct {
    int       tick;
    EventKind kind;
    uint16_t  button;       /* PRESS/RELEASE/TAP */
    float     ax, ay;       /* AIM (world) or MOUSE (screen) */
    int       az;           /* M6 P04 EV_DAMAGE: amount (0..255) */
    char      name[64];     /* SHOT */
} Event;

typedef enum {
    AIM_NONE = 0,
    AIM_WORLD,
    AIM_SCREEN,
} AimMode;

typedef struct {
    int   t0, t1;            /* tick range, inclusive */
    float to_x, to_y;        /* target screen coords at t1 */
    float from_x, from_y;    /* runtime: snapshotted at t0 */
    bool  snapshotted;
} LerpSeg;

/* Networked-shot setup. None = legacy single-process world; Host
 * runs an authoritative server + a local lobby slot; Client connects
 * to a remote host. */
typedef enum {
    NETMODE_NONE = 0,
    NETMODE_HOST,
    NETMODE_CONNECT,
} ShotNetMode;

typedef struct {
    int      window_w, window_h;
    bool     seed_override;
    uint64_t seed_hi, seed_lo;
    char     out_dir[256];

    AimMode  initial_aim_mode;
    float    initial_ax, initial_ay;

    bool     have_spawn_at;
    float    spawn_x, spawn_y;

    bool     have_loadout;
    MechLoadout loadout;

    /* Optional .lvl path. When set, seed_world calls map_build_from_path
     * (same loader the game's --test-play uses) instead of the hardcoded
     * `level_build_tutorial`. Empty string = use tutorial. Useful for
     * grapple/swing tests that need ceilings or other geometry the
     * tutorial doesn't have. */
    char     load_lvl[256];

    /* P07 — optional code-built map override. When `map_id` >= 0,
     * seed_world calls map_build(map_id, ...) instead of tutorial /
     * load_lvl. Lets a CTF shot test pull in the in-binary Crossfire
     * arena without authoring a `.lvl` first. -1 = unset. */
    int      map_id;

    /* P07 — match mode for the local-only shot path. seed_world copies
     * this into game.match.mode and runs ctf_init_round when set to
     * CTF. Has no effect on the networked NETMODE_HOST/CONNECT paths
     * (those pick mode from config_pick_mode). */
    int      match_mode;        /* MatchModeId; -1 = leave as default */

    Event   *events;
    int      event_count, event_capacity;

    LerpSeg *lerps;
    int      lerp_count, lerp_capacity;

    /* Contact sheet — composite all shots into one PNG at end of run. */
    bool     make_contact;
    char     contact_name[64];
    int      contact_cols;
    int      contact_cell_w, contact_cell_h;

    /* M6 P03 — perf bench knobs.
     *   perf_overlay  — when true, an FPS readout is drawn on top of
     *                   every rendered frame and the per-frame SHOT_LOG
     *                   carries fps + internal/window dims. Existing
     *                   regression shot tests leave this off so their
     *                   PNGs stay byte-identical.
     *   internal_h    — explicit internal-render-target cap. Default 0
     *                   keeps the pre-P03 identity pipeline (internal ==
     *                   window) so existing shots are unchanged. Bench
     *                   scripts set it to 1080 (or wherever) to measure
     *                   the post-P03 reduced fillrate. */
    bool     perf_overlay;
    int      internal_h;

    /* M6 P07 — when true, zero out g_map_kit.parallax_*.id after world
     * build so the renderer's draw_parallax_layer early-outs. Useful
     * for movement-tuning shots where parallax obscures the mech. */
    bool     no_parallax;

    /* M6 countdown-fix — explicit pre-round countdown duration. 0 keeps
     * the shot-mode default of 1.0s (1-tick GO! visibility); set to
     * 3.0 (matches interactive default) to capture the full 3-2-1-GO
     * arc in burst shots. */
    float    countdown_seconds;

    int      end_tick;       /* -1 = derive from last event */

    /* Networked-shot config (legacy single-process when NETMODE_NONE). */
    ShotNetMode netmode;
    uint16_t    netport;          /* host: listen port; connect: target port */
    char        nethost[64];      /* connect: target host */
    char        netname[24];      /* display name */

    /* Host-side override of peer mech positions on each round start.
     * Useful for kill tests where you want a specific layout on the
     * authoritative side (the client's local spawn_at is purely
     * cosmetic — the server owns positions and will overwrite the
     * client via snapshot). */
    struct {
        int   slot;
        float x, y;
    }       peer_spawns[8];
    int     peer_spawn_count;

    /* M5 P10 — extra dummy mechs spawned alongside the player + main
     * dummy in the legacy (NETMODE_NONE) shot path. Each entry creates
     * a non-aiming dummy at the given world-space coordinates so a
     * single shot can capture multiple chassis side-by-side for
     * per-chassis distinctness verification. Capped at 6.
     * P11 — optional primary_id override so per-weapon visible-art
     * tests can spawn one chassis per weapon variant in a single shot.
     * primary_id < 0 means "use mech_default_loadout's primary". */
    struct {
        int   chassis_id;
        int   primary_id;
        float x, y;
    }       extra_chassis[6];
    int     extra_chassis_count;

    /* M6 P09 — tile painters. Applied after world build, before tick 0
     * so per-flag overlay regression tests can author a small row of
     * each tile flag (ICE / DEADLY / ONE_WAY / BACKGROUND) on top of
     * whatever map was loaded.
     *   `paint_tile <tx> <ty> <flags_hex>`
     *   `paint_rect <tx0> <ty0> <tx1> <ty1> <flags_hex>` (half-open)
     * flags_hex is the TILE_F_* bitmask in hex (e.g. 0x09 = SOLID|ONE_WAY).
     * Capped at 32 paints per script.
     *
     * `paint_ambi <kind> <wx0> <wy0> <wx1> <wy1>` appends a fresh
     * LvlAmbi rect after world build; the runtime's atmosphere_init_for_
     * map will re-read it. Caps at 8. */
#define SHOTMODE_MAX_PAINTS 32
    struct {
        bool     is_rect;
        int      tx0, ty0, tx1, ty1;
        uint16_t flags;
    }       paints[SHOTMODE_MAX_PAINTS];
    int     paint_count;
#define SHOTMODE_MAX_PAINT_AMBIS 8
    struct {
        uint16_t kind;
        int      x0, y0, x1, y1;
        float    strength, dir_x, dir_y;
    }       paint_ambis[SHOTMODE_MAX_PAINT_AMBIS];
    int     paint_ambi_count;
    /* M6 P09 — atmosphere weather override. Applied AFTER world build
     * by clobbering g_atmosphere fields. Useful for testing snow/rain
     * accumulation without authoring a dedicated .lvl. */
    bool    have_weather_override;
    int     weather_kind_override;        /* WeatherKind */
    float   weather_density_override;     /* [0..1] */
} Script;

static const struct { const char *name; uint16_t bit; } BUTTON_TABLE[] = {
    {"left",   BTN_LEFT},   {"right",  BTN_RIGHT},
    {"jump",   BTN_JUMP},   {"jet",    BTN_JET},
    {"crouch", BTN_CROUCH}, {"prone",  BTN_PRONE},
    {"fire",   BTN_FIRE},   {"reload", BTN_RELOAD},
    {"melee",  BTN_MELEE},  {"use",    BTN_USE},
    {"swap",   BTN_SWAP},   {"dash",   BTN_DASH},
    {"fire_secondary", BTN_FIRE_SECONDARY},  /* P09 — RMB one-shot */
};

static uint16_t button_bit(const char *s) {
    for (size_t i = 0; i < sizeof(BUTTON_TABLE) / sizeof(BUTTON_TABLE[0]); ++i) {
        if (strcmp(s, BUTTON_TABLE[i].name) == 0) return BUTTON_TABLE[i].bit;
    }
    return 0;
}

static int cmp_event(const void *pa, const void *pb) {
    const Event *a = (const Event *)pa, *b = (const Event *)pb;
    if (a->tick != b->tick) return a->tick - b->tick;
    /* TAP unfolds into PRESS@N + RELEASE@N+1; ordering inside a tick
     * doesn't otherwise matter since input is rebuilt fresh each tick. */
    return (int)a->kind - (int)b->kind;
}

static void script_push(Script *s, Event ev) {
    if (s->event_count == s->event_capacity) {
        int cap = s->event_capacity ? s->event_capacity * 2 : 32;
        s->events = (Event *)realloc(s->events, sizeof(Event) * (size_t)cap);
        s->event_capacity = cap;
    }
    s->events[s->event_count++] = ev;
}

/* Resolve a weapon name (substring match, case-insensitive) to an id.
 * Mirrors main.c's resolve_weapon_id but inline so shotmode.c stays
 * self-contained. Returns -1 if not found. */
static int find_weapon_id(const char *name) {
    if (!name || !*name || strcmp(name, "-") == 0) return -1;
    for (int i = 0; i < 32; ++i) {
        const char *n = weapon_short_name(i);
        if (!n || strcmp(n, "?") == 0) continue;
        size_t L = strlen(name);
        bool ok = true;
        for (size_t k = 0; k < L && n[k]; ++k) {
            char a = name[k]; if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            char b = n[k];    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) { ok = false; break; }
        }
        if (ok) return i;
    }
    return -1;
}

static int find_armor_id(const char *name) {
    if (!name || !*name || strcmp(name, "-") == 0) return -1;
    for (int i = 0; i < 8; ++i) {
        const Armor *a = armor_def(i);
        if (!a || !a->name) continue;
        if (strcasecmp(name, a->name) == 0) return i;
    }
    return -1;
}

static int find_jetpack_id(const char *name) {
    if (!name || !*name || strcmp(name, "-") == 0) return -1;
    for (int i = 0; i < 8; ++i) {
        const Jetpack *j = jetpack_def(i);
        if (!j || !j->name) continue;
        if (strcasecmp(name, j->name) == 0) return i;
    }
    return -1;
}

/* Pull one whitespace- or quote-bounded token from `s`. Advances `*s`
 * past the token + any trailing whitespace. Returns false on EOL. */
static bool next_token(char **s, char *out, int outsz) {
    char *p = *s;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (!*p) return false;
    char *e;
    if (*p == '"') {
        ++p;
        e = p;
        while (*e && *e != '"') ++e;
    } else {
        e = p;
        while (*e && !isspace((unsigned char)*e)) ++e;
    }
    int n = (int)(e - p); if (n >= outsz) n = outsz - 1;
    memcpy(out, p, (size_t)n); out[n] = 0;
    if (*e == '"') ++e;
    while (*e && isspace((unsigned char)*e)) ++e;
    *s = e;
    return true;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static bool parse_script(const char *path, Script *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_E("shotmode: cannot open %s: %s", path, strerror(errno));
        return false;
    }

    out->window_w = 1280; out->window_h = 720;
    out->seed_override = false;
    snprintf(out->out_dir, sizeof(out->out_dir), "build/shots");
    out->end_tick = -1;
    out->map_id = -1;       /* P07 — only set when `map <name>` directive parsed */
    out->match_mode = -1;   /* P07 — only set when `mode <name>` directive parsed */
    out->perf_overlay = false;  /* M6 P03 — see Shot struct comment. */
    out->internal_h   = 0;       /* M6 P03 — 0 = identity (no cap). */
    out->no_parallax  = false;  /* M6 P07 — see Shot struct comment. */
    out->countdown_seconds = 0.0f; /* M6 countdown-fix — 0 = keep shot default (1s). */
    out->paint_count = 0;       /* M6 P09 — paint_tile / paint_rect cache */
    out->paint_ambi_count = 0;  /* M6 P09 — paint_ambi cache */
    out->have_weather_override = false;

    char line[512];
    int lineno = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = trim(line);
        if (!*p || *p == '#') continue;

        char tok[64];
        int n = 0;
        if (sscanf(p, "%63s%n", tok, &n) != 1) continue;
        char *rest = trim(p + n);

        if (strcmp(tok, "window") == 0) {
            if (sscanf(rest, "%d %d", &out->window_w, &out->window_h) != 2) {
                LOG_E("shotmode: %s:%d bad 'window'", path, lineno); ok = false;
            }
        } else if (strcmp(tok, "seed") == 0) {
            unsigned long long hi, lo;
            if (sscanf(rest, "%llu %llu", &hi, &lo) != 2) {
                LOG_E("shotmode: %s:%d bad 'seed'", path, lineno); ok = false;
            } else {
                out->seed_hi = (uint64_t)hi; out->seed_lo = (uint64_t)lo;
                out->seed_override = true;
            }
        } else if (strcmp(tok, "out") == 0) {
            snprintf(out->out_dir, sizeof(out->out_dir), "%s", rest);
        } else if (strcmp(tok, "load_lvl") == 0) {
            /* `load_lvl <path>` — seed_world will load this .lvl
             * via map_build_from_path instead of the hardcoded
             * tutorial. Path can be relative to cwd. */
            snprintf(out->load_lvl, sizeof(out->load_lvl), "%s", rest);
        } else if (strcmp(tok, "map") == 0) {
            /* `map <short_name>` — load a code-built map instead of
             * the tutorial. Wins over load_lvl when both are set.
             * Useful for CTF shot tests that need Crossfire's flag
             * geometry without authoring a .lvl. */
            int mid = map_id_from_name(rest);
            if (mid < 0) {
                LOG_E("shotmode: %s:%d unknown map '%s'", path, lineno, rest);
                ok = false;
            } else {
                out->map_id = mid;
            }
        } else if (strcmp(tok, "mode") == 0) {
            /* `mode <ffa|tdm|ctf>` — overrides match.mode in
             * seed_world. Triggers ctf_init_round when mode == ctf and
             * the loaded map has 2 flag records. No-op for the
             * networked NETMODE_HOST/CONNECT paths (those pick from
             * config; soldut.cfg in the test cwd is the override
             * mechanism for those). */
            MatchModeId m = match_mode_from_name(rest);
            out->match_mode = (int)m;
        } else if (strcmp(tok, "loadout") == 0) {
            char chassis[32], primary[32], secondary[32], armor[32], jetpack[32];
            char *q = rest;
            if (!next_token(&q, chassis, sizeof chassis) ||
                !next_token(&q, primary, sizeof primary) ||
                !next_token(&q, secondary, sizeof secondary) ||
                !next_token(&q, armor, sizeof armor) ||
                !next_token(&q, jetpack, sizeof jetpack)) {
                LOG_E("shotmode: %s:%d 'loadout' needs 5 fields (use '-' to keep default)",
                      path, lineno); ok = false;
                continue;
            }
            out->loadout = mech_default_loadout();
            if (strcmp(chassis, "-") != 0)
                out->loadout.chassis_id = chassis_id_from_name(chassis);
            int wid = find_weapon_id(primary);
            if (wid >= 0) out->loadout.primary_id = wid;
            wid = find_weapon_id(secondary);
            if (wid >= 0) out->loadout.secondary_id = wid;
            int aid = find_armor_id(armor);
            if (aid >= 0) out->loadout.armor_id = aid;
            int jid = find_jetpack_id(jetpack);
            if (jid >= 0) out->loadout.jetpack_id = jid;
            out->have_loadout = true;
        } else if (strcmp(tok, "spawn_at") == 0) {
            if (sscanf(rest, "%f %f", &out->spawn_x, &out->spawn_y) != 2) {
                LOG_E("shotmode: %s:%d bad 'spawn_at'", path, lineno); ok = false;
            } else {
                out->have_spawn_at = true;
            }
        } else if (strcmp(tok, "extra_chassis") == 0) {
            char chassis[32];
            char primary[64] = {0};
            float ex, ey;
            int matched = sscanf(rest, "%31s %f %f \"%63[^\"]\"",
                                 chassis, &ex, &ey, primary);
            if (matched < 3) {
                LOG_E("shotmode: %s:%d 'extra_chassis' needs "
                      "<name> <x> <y> [\"primary weapon\"]",
                      path, lineno);
                ok = false;
                continue;
            }
            int max_extras =
                (int)(sizeof out->extra_chassis / sizeof out->extra_chassis[0]);
            if (out->extra_chassis_count >= max_extras) {
                LOG_E("shotmode: %s:%d too many extra_chassis (max %d)",
                      path, lineno, max_extras);
                ok = false;
                continue;
            }
            int n = out->extra_chassis_count++;
            out->extra_chassis[n].chassis_id =
                (int)chassis_id_from_name(chassis);
            out->extra_chassis[n].x = ex;
            out->extra_chassis[n].y = ey;
            out->extra_chassis[n].primary_id = -1;
            if (matched == 4 && primary[0] != '\0') {
                int wid = find_weapon_id(primary);
                if (wid >= 0) out->extra_chassis[n].primary_id = wid;
                else LOG_W("shotmode: %s:%d unknown weapon '%s'",
                           path, lineno, primary);
            }
        } else if (strcmp(tok, "aim") == 0) {
            if (sscanf(rest, "%f %f", &out->initial_ax, &out->initial_ay) != 2) {
                LOG_E("shotmode: %s:%d bad 'aim'", path, lineno); ok = false;
            } else {
                out->initial_aim_mode = AIM_WORLD;
            }
        } else if (strcmp(tok, "mouse") == 0) {
            if (sscanf(rest, "%f %f", &out->initial_ax, &out->initial_ay) != 2) {
                LOG_E("shotmode: %s:%d bad 'mouse'", path, lineno); ok = false;
            } else {
                out->initial_aim_mode = AIM_SCREEN;
            }
        } else if (strcmp(tok, "at") == 0) {
            int tick;
            char ev_kind[32];
            int eaten = 0;
            if (sscanf(rest, "%d %31s%n", &tick, ev_kind, &eaten) != 2) {
                LOG_E("shotmode: %s:%d bad 'at'", path, lineno); ok = false;
                continue;
            }
            char *args = trim(rest + eaten);

            Event ev = {0};
            ev.tick = tick;
            if (strcmp(ev_kind, "press") == 0 || strcmp(ev_kind, "release") == 0
                || strcmp(ev_kind, "tap") == 0) {
                char btn[32];
                if (sscanf(args, "%31s", btn) != 1 || !button_bit(btn)) {
                    LOG_E("shotmode: %s:%d unknown button '%s'", path, lineno, args);
                    ok = false; continue;
                }
                ev.button = button_bit(btn);
                if (strcmp(ev_kind, "tap") == 0) {
                    /* Lower into press@N + release@N+1. */
                    ev.kind = EV_PRESS; script_push(out, ev);
                    ev.kind = EV_RELEASE; ev.tick = tick + 1; script_push(out, ev);
                    continue;
                }
                ev.kind = (strcmp(ev_kind, "press") == 0) ? EV_PRESS : EV_RELEASE;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "aim") == 0) {
                if (sscanf(args, "%f %f", &ev.ax, &ev.ay) != 2) {
                    LOG_E("shotmode: %s:%d bad 'at .. aim'", path, lineno);
                    ok = false; continue;
                }
                ev.kind = EV_AIM;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "mouse") == 0) {
                if (sscanf(args, "%f %f", &ev.ax, &ev.ay) != 2) {
                    LOG_E("shotmode: %s:%d bad 'at .. mouse'", path, lineno);
                    ok = false; continue;
                }
                ev.kind = EV_MOUSE;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "shot") == 0) {
                if (sscanf(args, "%63s", ev.name) != 1) {
                    LOG_E("shotmode: %s:%d 'shot' needs a name", path, lineno);
                    ok = false; continue;
                }
                ev.kind = EV_SHOT;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "end") == 0) {
                ev.kind = EV_END;
                script_push(out, ev);
                if (out->end_tick < 0 || tick < out->end_tick) out->end_tick = tick;
            } else if (strcmp(ev_kind, "give_invis") == 0) {
                /* "at <tick> give_invis [seconds]" — sets the local
                 * mech's powerup_invis_remaining timer. Used to bench
                 * "fire while invis" without authoring a level + grab
                 * pass. */
                float secs = 8.0f;
                if (args[0]) sscanf(args, "%f", &secs);
                ev.kind = EV_GIVE_INVIS;
                ev.ax = secs;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "arm_carry") == 0) {
                /* "at <tick> arm_carry <flag_idx> <mech_id>" — host-only.
                 * Forces the server's flag state to CARRIED with the
                 * named mech as carrier so the drop-on-kill flow can
                 * be exercised end-to-end. flag_carry on a CLIENT only
                 * mutates the client's local state; the server stays
                 * unaware. This directive does the server-side mutation
                 * directly + sets dirty so the broadcast goes out. */
                int fidx = -1, mid = -1;
                if (sscanf(args, "%d %d", &fidx, &mid) != 2) {
                    LOG_E("shotmode: %s:%d 'arm_carry' needs <flag_idx> <mech_id>",
                          path, lineno); ok = false; continue;
                }
                ev.kind = EV_ARM_CARRY;
                ev.ax = (float)fidx;
                ev.ay = (float)mid;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "kill_peer") == 0) {
                /* "at <tick> kill_peer <mech_id> [<shooter_mech_id>]"
                 * — host-side debug that directly invokes
                 * mech_apply_damage on the named mech. Lets a CTF
                 * drop-on-kill shot test bench the death flow without
                 * requiring perfect weapons aim across the recoil-
                 * and-bink window. The optional second arg is the
                 * shooter mech_id; without it kills are
                 * environmental (= suicide → no score credit), so
                 * FFA / TDM score tests must supply a shooter to get
                 * the kill credited to the right slot. CTF drop-on-
                 * kill tests don't need the credit. The directive is
                 * a no-op for clients (mech_kill is server-
                 * authoritative). */
                int mid = -1, shooter = -1;
                if (args[0]) sscanf(args, "%d %d", &mid, &shooter);
                ev.kind = EV_KILL_PEER;
                ev.ax = (float)mid;
                ev.ay = (float)shooter;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "damage") == 0) {
                /* "at <tick> damage <mech_id> <part> <amount>" — M6 P04
                 * single-process color-tier test driver. Calls
                 * mech_apply_damage with environmental (no shooter)
                 * credit; the FX_DAMAGE_NUMBER spawn fires from the
                 * normal seam. <part> is a numeric PART_* enum value
                 * (0=HEAD, 2=CHEST, 3=PELVIS, etc. per src/world.h:91)
                 * or one of the keyword shorthands HEAD / CHEST /
                 * PELVIS / L_ARM / R_ARM / L_LEG / R_LEG (uppercase). */
                int  mid = -1, amount = 0;
                char part_tok[32] = {0};
                if (sscanf(args, "%d %31s %d",
                           &mid, part_tok, &amount) != 3) {
                    LOG_E("shotmode: %s:%d 'damage' needs "
                          "<mech_id> <part> <amount>", path, lineno);
                    ok = false; continue;
                }
                /* Map part keyword → PART_* enum. Numeric input also
                 * accepted (a digit-only token parses via atoi). */
                int part = -1;
                if (part_tok[0] >= '0' && part_tok[0] <= '9') {
                    part = atoi(part_tok);
                } else if (strcmp(part_tok, "HEAD")  == 0)  part = PART_HEAD;
                else if   (strcmp(part_tok, "NECK")  == 0)  part = PART_NECK;
                else if   (strcmp(part_tok, "CHEST") == 0)  part = PART_CHEST;
                else if   (strcmp(part_tok, "PELVIS")== 0)  part = PART_PELVIS;
                else if   (strcmp(part_tok, "L_ARM") == 0)  part = PART_L_SHOULDER;
                else if   (strcmp(part_tok, "R_ARM") == 0)  part = PART_R_SHOULDER;
                else if   (strcmp(part_tok, "L_LEG") == 0)  part = PART_L_HIP;
                else if   (strcmp(part_tok, "R_LEG") == 0)  part = PART_R_HIP;
                if (part < 0 || part >= PART_COUNT) {
                    LOG_E("shotmode: %s:%d 'damage' bad part '%s'",
                          path, lineno, part_tok);
                    ok = false; continue;
                }
                if (amount < 0)   amount = 0;
                if (amount > 255) amount = 255;
                ev.kind = EV_DAMAGE;
                ev.ax   = (float)mid;
                ev.ay   = (float)part;
                ev.az   = amount;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "team_change") == 0) {
                /* "at <tick> team_change <team_id>" — drives the same
                 * codepath as the lobby UI's TEAM picker. Used by
                 * tests/shots/net/2p_team_change.* to verify the wire
                 * round-trip without requiring real mouse simulation. */
                int team = MATCH_TEAM_NONE;
                if (args[0]) sscanf(args, "%d", &team);
                ev.kind = EV_TEAM_CHANGE;
                ev.ax = (float)team;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "bot_team") == 0) {
                /* "at <tick> bot_team <slot> <team>" — host clicks a
                 * bot row's team chip to flip RED↔BLUE. Drives the
                 * same path as the lobby UI: NET_MSG_LOBBY_BOT_TEAM
                 * over the wire when running as a client (host UI
                 * post wan-fixes-16), direct lobby_set_team on a
                 * native server. */
                int slot = -1, team = MATCH_TEAM_RED;
                if (args[0]) sscanf(args, "%d %d", &slot, &team);
                ev.kind = EV_BOT_TEAM;
                ev.ax = (float)slot;
                ev.ay = (float)team;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "add_bot") == 0) {
                /* "at <tick> add_bot <tier>" — host clicks the "Add
                 * Bot" button. Drives net_client_send_add_bot under
                 * the hood; server adds a bot slot and re-broadcasts
                 * LOBBY_LIST. */
                int tier = 1;  /* veteran */
                if (args[0]) sscanf(args, "%d", &tier);
                ev.kind = EV_ADD_BOT;
                ev.ax = (float)tier;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "flag_carry") == 0) {
                /* "at <tick> flag_carry <flag_idx>" — force flag
                 * `flag_idx` into CARRIED state with carrier_mech =
                 * local_mech_id. Lets a CTF shot test bench the
                 * capture-flow (carrier touches own home flag) without
                 * having to walk across the entire arena to grab the
                 * enemy flag first. flag_idx 0 = RED, 1 = BLUE. */
                int fidx = 1;
                if (args[0]) sscanf(args, "%d", &fidx);
                ev.kind = EV_FLAG_CARRY;
                ev.ax = (float)fidx;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "kick") == 0 ||
                       strcmp(ev_kind, "ban")  == 0) {
                /* "at <tick> kick <slot>" / "ban <slot>" — host-only.
                 * Drives the same codepath as the lobby UI's [Kick] /
                 * [Ban] confirmation modal: net_server_kick_or_ban_slot
                 * disconnects the peer + (for ban) records the name in
                 * the bans list + lobby_chat_systems the announcement.
                 * No-op on a client. */
                int slot = -1;
                if (args[0]) sscanf(args, "%d", &slot);
                ev.kind = (strcmp(ev_kind, "ban") == 0) ? EV_BAN : EV_KICK;
                ev.ax = (float)slot;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "vote_map") == 0) {
                /* "at <tick> vote_map <choice 0/1/2>" — drives the
                 * 3-card map vote picker. Host slot calls
                 * lobby_vote_cast directly + rebroadcasts; client
                 * slot sends NET_MSG_LOBBY_MAP_VOTE. */
                int choice = -1;
                if (args[0]) sscanf(args, "%d", &choice);
                ev.kind = EV_VOTE_MAP;
                ev.ax = (float)choice;
                script_push(out, ev);
            } else if (strcmp(ev_kind, "kick_modal") == 0 ||
                       strcmp(ev_kind, "ban_modal")  == 0) {
                /* "at <tick> kick_modal <slot>" / "ban_modal <slot>" —
                 * host-only. Sets LobbyUIState's kick/ban target so the
                 * confirmation modal renders on the next frame. Used by
                 * GUI-layout regression shots to capture the modal
                 * without simulating a mouse click on the row buttons. */
                int slot = -1;
                if (args[0]) sscanf(args, "%d", &slot);
                ev.kind = (strcmp(ev_kind, "ban_modal") == 0)
                          ? EV_BAN_MODAL : EV_KICK_MODAL;
                ev.ax = (float)slot;
                script_push(out, ev);
            } else {
                LOG_E("shotmode: %s:%d unknown event '%s'", path, lineno, ev_kind);
                ok = false;
            }
        } else if (strcmp(tok, "mouse_lerp") == 0) {
            float to_x, to_y;
            char from_kw[8], to_kw[8];
            int t0, t1;
            if (sscanf(rest, "%f %f %7s %d %7s %d",
                       &to_x, &to_y, from_kw, &t0, to_kw, &t1) != 6
                || strcmp(from_kw, "from") != 0
                || strcmp(to_kw,   "to"  ) != 0
                || t1 <= t0) {
                LOG_E("shotmode: %s:%d bad 'mouse_lerp' (expected: mouse_lerp "
                      "<sx> <sy> from <t0> to <t1>)", path, lineno);
                ok = false;
                continue;
            }
            /* Lerp segments are stored separately and consulted each tick
             * by the runner; we don't know the segment's start position
             * at parse time (depends on whichever mouse state is live at
             * t0), so the runner snapshots it on entry. */
            if (out->lerp_count == out->lerp_capacity) {
                int cap = out->lerp_capacity ? out->lerp_capacity * 2 : 4;
                out->lerps = (LerpSeg *)realloc(out->lerps, sizeof(LerpSeg) * (size_t)cap);
                out->lerp_capacity = cap;
            }
            out->lerps[out->lerp_count++] = (LerpSeg){
                .t0 = t0, .t1 = t1, .to_x = to_x, .to_y = to_y,
            };
        } else if (strcmp(tok, "contact_sheet") == 0) {
            char name[64] = {0};
            int cols = 4, cw = 320, ch = 180;
            /* Parse name then optional 'cols N' and 'cell W H' in any order. */
            int eaten = 0;
            if (sscanf(rest, "%63s%n", name, &eaten) != 1) {
                LOG_E("shotmode: %s:%d 'contact_sheet' needs a name", path, lineno);
                ok = false; continue;
            }
            char *q = trim(rest + eaten);
            while (*q) {
                char kw[16];
                int k_eaten = 0;
                if (sscanf(q, "%15s%n", kw, &k_eaten) != 1) break;
                q = trim(q + k_eaten);
                if (strcmp(kw, "cols") == 0) {
                    if (sscanf(q, "%d%n", &cols, &k_eaten) != 1 || cols <= 0) {
                        LOG_E("shotmode: %s:%d 'cols' needs a positive int", path, lineno);
                        ok = false; break;
                    }
                    q = trim(q + k_eaten);
                } else if (strcmp(kw, "cell") == 0) {
                    if (sscanf(q, "%d %d%n", &cw, &ch, &k_eaten) != 2 || cw <= 0 || ch <= 0) {
                        LOG_E("shotmode: %s:%d 'cell' needs W H", path, lineno);
                        ok = false; break;
                    }
                    q = trim(q + k_eaten);
                } else {
                    LOG_E("shotmode: %s:%d unknown contact_sheet option '%s'", path, lineno, kw);
                    ok = false; break;
                }
            }
            out->make_contact = true;
            snprintf(out->contact_name, sizeof(out->contact_name), "%s", name);
            out->contact_cols   = cols;
            out->contact_cell_w = cw;
            out->contact_cell_h = ch;
        } else if (strcmp(tok, "perf_overlay") == 0) {
            /* M6 P03 — `perf_overlay on|off`. When on, the renderer draws
             * a small FPS+resolution readout on every frame AND emits a
             * SHOT_LOG line once per second with the same values. Bench
             * scripts opt in; regression scripts leave it off (default)
             * so their PNGs stay byte-identical. */
            char val[8] = {0};
            if (sscanf(rest, "%7s", val) != 1) {
                LOG_E("shotmode: %s:%d 'perf_overlay' needs on|off", path, lineno);
                ok = false; continue;
            }
            if      (strcmp(val, "on")  == 0) out->perf_overlay = true;
            else if (strcmp(val, "off") == 0) out->perf_overlay = false;
            else {
                LOG_E("shotmode: %s:%d 'perf_overlay' value '%s' not on|off",
                      path, lineno, val);
                ok = false;
            }
        } else if (strcmp(tok, "internal_h") == 0) {
            /* M6 P03 — `internal_h N`. Override the default 0 (no cap).
             * Bench scripts use this to measure the post-P03 internal-RT
             * fillrate at e.g. 1080 lines while the window stays at the
             * `window` directive's size. Accepts 0 (identity) and
             * 360..4320 inclusive. */
            int n;
            if (sscanf(rest, "%d", &n) != 1) {
                LOG_E("shotmode: %s:%d 'internal_h' needs an int", path, lineno);
                ok = false; continue;
            }
            if (n != 0 && (n < 360 || n > 4320)) {
                LOG_E("shotmode: %s:%d 'internal_h %d' out of range (0 or 360..4320)",
                      path, lineno, n);
                ok = false; continue;
            }
            out->internal_h = n;
        } else if (strcmp(tok, "no_parallax") == 0) {
            /* M6 P07 — `no_parallax` (no value). After world build,
             * zero g_map_kit.parallax_*.id so draw_parallax_layer
             * early-outs. Used by movement-tuning shots. */
            out->no_parallax = true;
        } else if (strcmp(tok, "countdown_seconds") == 0) {
            /* M6 countdown-fix — `countdown_seconds N`. Override the
             * shot-mode 1s default so a burst can capture the full
             * 3-2-1-GO arc (interactive default = 3s). */
            float n;
            if (sscanf(rest, "%f", &n) != 1 || n < 0.0f || n > 30.0f) {
                LOG_E("shotmode: %s:%d 'countdown_seconds' needs a float in [0, 30]",
                      path, lineno);
                ok = false; continue;
            }
            out->countdown_seconds = n;
        } else if (strcmp(tok, "paint_tile") == 0) {
            /* M6 P09 — paint_tile <tx> <ty> <flags_hex>. Stamps a single
             * tile after world build, before tick 0. Useful for the
             * per-flag visual differentiation shot. */
            int tx, ty;
            unsigned int flags;
            if (sscanf(rest, "%d %d %x", &tx, &ty, &flags) != 3) {
                LOG_E("shotmode: %s:%d 'paint_tile' needs <tx> <ty> <flags_hex>", path, lineno);
                ok = false; continue;
            }
            if (out->paint_count >= SHOTMODE_MAX_PAINTS) {
                LOG_W("shotmode: %s:%d 'paint_tile' cap reached (%d)", path, lineno, SHOTMODE_MAX_PAINTS);
                continue;
            }
            int p = out->paint_count++;
            out->paints[p].is_rect = false;
            out->paints[p].tx0 = tx; out->paints[p].ty0 = ty;
            out->paints[p].tx1 = tx + 1; out->paints[p].ty1 = ty + 1;
            out->paints[p].flags = (uint16_t)flags;
        } else if (strcmp(tok, "paint_rect") == 0) {
            int tx0, ty0, tx1, ty1;
            unsigned int flags;
            if (sscanf(rest, "%d %d %d %d %x",
                       &tx0, &ty0, &tx1, &ty1, &flags) != 5) {
                LOG_E("shotmode: %s:%d 'paint_rect' needs <tx0> <ty0> <tx1> <ty1> <flags_hex>", path, lineno);
                ok = false; continue;
            }
            if (out->paint_count >= SHOTMODE_MAX_PAINTS) {
                LOG_W("shotmode: %s:%d 'paint_rect' cap reached", path, lineno);
                continue;
            }
            int p = out->paint_count++;
            out->paints[p].is_rect = true;
            out->paints[p].tx0 = tx0; out->paints[p].ty0 = ty0;
            out->paints[p].tx1 = tx1; out->paints[p].ty1 = ty1;
            out->paints[p].flags = (uint16_t)flags;
        } else if (strcmp(tok, "weather") == 0) {
            /* M6 P09 — weather <kind_int> <density_float>. Overrides
             * the loaded map's atmosphere weather AFTER world build
             * (via direct g_atmosphere mutation in seed_world). 1=SNOW,
             * 2=RAIN, 3=DUST, 4=EMBERS, 0=NONE. Used by tests that
             * need a specific weather setup without authoring a
             * dedicated .lvl. Density 0..1. */
            int kind;
            float density;
            if (sscanf(rest, "%d %f", &kind, &density) != 2) {
                LOG_E("shotmode: %s:%d 'weather' needs <kind> <density>", path, lineno);
                ok = false; continue;
            }
            if (kind < 0 || kind > 4) {
                LOG_W("shotmode: %s:%d 'weather kind %d' out of range; clamping",
                      path, lineno, kind);
                if (kind < 0) kind = 0;
                if (kind > 4) kind = 4;
            }
            if (density < 0.0f) density = 0.0f;
            if (density > 1.0f) density = 1.0f;
            out->weather_kind_override     = kind;
            out->weather_density_override  = density;
            out->have_weather_override     = true;
        } else if (strcmp(tok, "paint_ambi") == 0) {
            /* paint_ambi <kind_int> <wx0> <wy0> <wx1> <wy1> [strength dirx diry] */
            int kind;
            int x0, y0, x1, y1;
            float st = 0.5f, dx = 1.0f, dy = 0.0f;
            int n = sscanf(rest, "%d %d %d %d %d %f %f %f",
                           &kind, &x0, &y0, &x1, &y1, &st, &dx, &dy);
            if (n < 5) {
                LOG_E("shotmode: %s:%d 'paint_ambi' needs <kind> <wx0> <wy0> <wx1> <wy1> [strength dx dy]", path, lineno);
                ok = false; continue;
            }
            if (out->paint_ambi_count >= SHOTMODE_MAX_PAINT_AMBIS) {
                LOG_W("shotmode: %s:%d 'paint_ambi' cap reached", path, lineno);
                continue;
            }
            int p = out->paint_ambi_count++;
            out->paint_ambis[p].kind = (uint16_t)kind;
            out->paint_ambis[p].x0 = x0; out->paint_ambis[p].y0 = y0;
            out->paint_ambis[p].x1 = x1; out->paint_ambis[p].y1 = y1;
            out->paint_ambis[p].strength = st;
            out->paint_ambis[p].dir_x = dx;
            out->paint_ambis[p].dir_y = dy;
        } else if (strcmp(tok, "network") == 0) {
            char kind[16] = {0};
            int eaten2 = 0;
            if (sscanf(rest, "%15s%n", kind, &eaten2) != 1) {
                LOG_E("shotmode: %s:%d 'network' needs host|connect", path, lineno);
                ok = false; continue;
            }
            char *q = trim(rest + eaten2);
            if (strcmp(kind, "host") == 0) {
                int port;
                if (sscanf(q, "%d", &port) != 1 || port <= 0 || port > 65535) {
                    LOG_E("shotmode: %s:%d 'network host' needs PORT", path, lineno);
                    ok = false; continue;
                }
                out->netmode = NETMODE_HOST;
                out->netport = (uint16_t)port;
            } else if (strcmp(kind, "connect") == 0) {
                /* HOST[:PORT] — clamp to nethost size so a long input
                 * can't overflow the destination buffer. */
                char addr[80] = {0};
                if (sscanf(q, "%79s", addr) != 1) {
                    LOG_E("shotmode: %s:%d 'network connect' needs HOST[:PORT]", path, lineno);
                    ok = false; continue;
                }
                const char *colon = strrchr(addr, ':');
                if (colon) {
                    size_t hl = (size_t)(colon - addr);
                    if (hl >= sizeof out->nethost) hl = sizeof out->nethost - 1;
                    memcpy(out->nethost, addr, hl); out->nethost[hl] = '\0';
                    int port = atoi(colon + 1);
                    out->netport = (uint16_t)(port > 0 ? port : SOLDUT_DEFAULT_PORT);
                } else {
                    /* Copy with explicit length cap so the dst-size FORTIFY
                     * check is happy at -O0. */
                    size_t hl = strlen(addr);
                    if (hl >= sizeof out->nethost) hl = sizeof out->nethost - 1;
                    memcpy(out->nethost, addr, hl); out->nethost[hl] = '\0';
                    (void)snprintf;
                    out->netport = SOLDUT_DEFAULT_PORT;
                }
                out->netmode = NETMODE_CONNECT;
            } else {
                LOG_E("shotmode: %s:%d 'network' kind must be host|connect", path, lineno);
                ok = false;
            }
        } else if (strcmp(tok, "peer_spawn") == 0) {
            int slot;
            float x, y;
            if (sscanf(rest, "%d %f %f", &slot, &x, &y) != 3) {
                LOG_E("shotmode: %s:%d 'peer_spawn' needs SLOT WX WY", path, lineno);
                ok = false; continue;
            }
            if (out->peer_spawn_count < 8) {
                out->peer_spawns[out->peer_spawn_count].slot = slot;
                out->peer_spawns[out->peer_spawn_count].x    = x;
                out->peer_spawns[out->peer_spawn_count].y    = y;
                out->peer_spawn_count++;
            } else {
                LOG_W("shotmode: %s:%d 'peer_spawn' table full (8 max)",
                      path, lineno);
            }
        } else if (strcmp(tok, "name") == 0) {
            /* Read directly into out->netname, capped at its size minus
             * one — keeps FORTIFY at -O0 happy without needing an
             * intermediate buffer. */
            char nm[24] = {0};
            if (sscanf(rest, "%23s", nm) != 1) {
                LOG_E("shotmode: %s:%d 'name' needs an identifier", path, lineno);
                ok = false; continue;
            }
            memcpy(out->netname, nm, sizeof nm);
            out->netname[sizeof out->netname - 1] = '\0';
        } else if (strcmp(tok, "burst") == 0) {
            char prefix[48], from_kw[8], to_kw[8], every_kw[8];
            int t0, t1, k;
            if (sscanf(rest, "%47s %7s %d %7s %d %7s %d",
                       prefix, from_kw, &t0, to_kw, &t1, every_kw, &k) != 7
                || strcmp(from_kw, "from") != 0
                || strcmp(to_kw,   "to"  ) != 0
                || strcmp(every_kw,"every") != 0
                || k <= 0 || t1 < t0) {
                LOG_E("shotmode: %s:%d bad 'burst' (expected: burst <prefix> "
                      "from <t0> to <t1> every <k>)", path, lineno);
                ok = false;
                continue;
            }
            for (int t = t0; t <= t1; t += k) {
                Event ev = { .tick = t, .kind = EV_SHOT };
                snprintf(ev.name, sizeof(ev.name), "%s_t%03d", prefix, t);
                script_push(out, ev);
            }
        } else {
            LOG_E("shotmode: %s:%d unknown directive '%s'", path, lineno, tok);
            ok = false;
        }
    }
    fclose(f);

    if (out->event_count) {
        qsort(out->events, (size_t)out->event_count, sizeof(Event), cmp_event);
    }
    if (out->end_tick < 0 && out->event_count) {
        out->end_tick = out->events[out->event_count - 1].tick + 60;
    }
    if (out->end_tick < 0) out->end_tick = 60;

    return ok;
}

/* ---- Output dir: mkdir -p style ------------------------------------ */
static void mkdir_p(const char *path) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            SHOT_MKDIR(buf);
            *p = '/';
        }
    }
    SHOT_MKDIR(buf);
}

/* ---- Networked shot — main-loop dispatcher --------------------------
 *
 * When the script declares `network host PORT` or `network connect
 * HOST:PORT`, shotmode skips its direct seed_world+sim path and
 * instead drives the full game-mode dispatcher each tick: TITLE →
 * LOBBY → COUNTDOWN → MATCH → SUMMARY. This mirrors main.c's run
 * loop so the shotmode binary exercises the same code paths real
 * play does, with screenshots captured per scripted tick.
 *
 * Input button events in the script apply to the local mech during
 * MODE_MATCH (same as the legacy path). Click/keyboard events for
 * lobby UI driving aren't supported yet — we rely on the host's
 * auto-start countdown to fire the round, so a one-shot test can
 * cover lobby + match + summary without needing synthesized clicks.
 */
static void networked_shot_bootstrap(Game *g, const Script *s) {
    /* Fast auto-start + short summary so shot tests don't sit in
     * lobby / summary for half a minute. We set both the LIVE values
     * (lobby/match) AND the config so that start_round's "re-derive
     * limits from config" path doesn't reset them back to long
     * defaults mid-round.
     *
     * P07 — when the script's run cwd ships a soldut.cfg (CTF tests do
     * this so mode/map_rotation reach config_pick_*), we honor the
     * config-loaded values for score/time limits and only fast-track
     * auto_start + countdown for iteration speed. The flag
     * `loaded_from_file` is set by config_load and cleared otherwise. */
    /* Default to fast-iteration values, but respect cfg overrides for
     * any test that wants longer lobby/match phases (e.g. team-change
     * tests need enough lobby time to send the wire after the
     * handshake settles). */
    if (!g->config.loaded_from_file) {
        g->config.auto_start_seconds = 1.0f;
        g->config.time_limit         = 8.0f;
        g->config.score_limit        = 10;
    }
    g->lobby.auto_start_default  = g->config.auto_start_seconds;
    g->match.countdown_default   = (s->countdown_seconds > 0.0f)
                                     ? s->countdown_seconds : 1.0f;
    g->match.summary_default     = 4.0f;   /* 15 s default; 4 s for tests */
    g->match.time_limit          = g->config.time_limit;
    g->match.score_limit         = g->config.score_limit;
    g->match.rounds_per_match    = g->config.rounds_per_match;
    g->match.inter_round_countdown_default = 2.0f;  /* tighter for shot tests */

    if (s->netmode == NETMODE_HOST) {
        if (!net_init()) {
            LOG_E("shotmode: net_init failed"); return;
        }
        if (!net_server_start(&g->net, s->netport, g)) {
            LOG_E("shotmode: failed to bind UDP %u", (unsigned)s->netport);
            return;
        }
        net_discovery_open(&g->net);
        /* P07/wan-fixes-3 — `map <short>` directive applies to network
         * host mode too. Without this, the host always builds
         * MAP_FOUNDRY for the lobby + first round (Foundry's parallax_
         * near layer obscures mechs in screenshots; using `map
         * slipstream` lets tests render mech bodies cleanly). Seed
         * `config.map_rotation[0]` so start_round's `config_pick_map`
         * lands on the chosen map. */
        if (s->map_id >= 0) {
            g->config.map_rotation[0]   = s->map_id;
            g->config.map_rotation_count = 1;
        }
        /* Mirror main.c::bootstrap_host — load any persisted bans so
         * shotmode tests can exercise the auto-save+reload of bans.txt
         * (without this, ban directives in scripts wouldn't write the
         * file because L->ban_path stays empty). */
        lobby_load_bans(&g->lobby, "bans.txt");
        const char *nm = s->netname[0] ? s->netname : "host-shot";
        int slot = lobby_add_slot(&g->lobby, /*peer_id*/-1, nm, /*is_host*/true);
        g->local_slot_id = slot;
        g->world.authoritative = true;
        /* Apply script `loadout` directive to the host's slot so the
         * spawned mech uses the requested chassis / weapons. */
        if (s->have_loadout && slot >= 0) {
            lobby_set_loadout(&g->lobby, slot, s->loadout);
        }
        /* M6 P13 — honor cfg `bots=N bot_tier=...` in shot tests
         * (mirrors main.c::bootstrap_host's bot-fill block). Without
         * this, the only way to add bots to a shotmode host is via
         * the wire ADD_BOT from a client — which the server rejects
         * for non-host slots (see `server: non-host slot N tried
         * ADD_BOT` warning). With it, bot frag-charge / nav tests can
         * declare `bots=1 bot_tier=champion` in a per-test cfg and the
         * host bootstraps with the bot already in the lobby. */
        if (g->config.bots > 0) {
            bool team_balance = (g->match.mode == MATCH_MODE_TDM ||
                                 g->match.mode == MATCH_MODE_CTF);
            lobby_apply_bot_fill(&g->lobby, g->config.bots,
                                 (uint8_t)g->config.bot_tier,
                                 team_balance);
            LOG_I("shotmode: bot fill — %d slot(s) at tier %s",
                  lobby_bot_count(&g->lobby),
                  bot_tier_name((BotTier)g->config.bot_tier));
        }
        /* Pre-build the level so the lobby has something to draw the
         * world frame against. ROUND_START rebuilds for the chosen map.
         * wan-fixes-3 — honors the `map` directive (via map_rotation[0]
         * seeded above); Foundry stays the fallback. */
        MapId pre_map = (s->map_id >= 0) ? (MapId)s->map_id : MAP_FOUNDRY;
        map_build(pre_map, &g->world, &g->level_arena);
        decal_init((int)level_width_px(&g->world.level),
                   (int)level_height_px(&g->world.level));
        /* P08 — refresh the host's serve descriptor so INITIAL_STATE
         * carries the map crc. Mirrors main.c::bootstrap_host. */
        maps_refresh_serve_info(map_def(pre_map)->short_name,
                                NULL, &g->server_map_desc,
                                g->server_map_serve_path,
                                sizeof(g->server_map_serve_path));
        g->mode = MODE_LOBBY;
        LOG_I("shotmode: hosting on port %u as '%s'", (unsigned)s->netport, nm);
    } else if (s->netmode == NETMODE_CONNECT) {
        if (!net_init()) {
            LOG_E("shotmode: net_init failed"); return;
        }
        const char *nm = s->netname[0] ? s->netname : "client-shot";
        if (!net_client_connect(&g->net, s->nethost, s->netport, nm, g)) {
            LOG_E("shotmode: connect to %s:%u failed",
                  s->nethost, (unsigned)s->netport);
            return;
        }
        g->world.authoritative = false;
        /* wan-fixes-10 — the lobby UI is what publishes loadout to the
         * server (via sync_loadout_from_server's push branch). The
         * UI's L->lobby_* fields are seeded after this function
         * returns (see the `if (s.netmode != NETMODE_NONE)` block
         * below). The earlier net_client_send_loadout from here was
         * superfluous AND racy — the lobby UI's first push could land
         * on the same wire frame and overwrite this one. */
        /* The handshake moved us to MODE_LOBBY via INITIAL_STATE. */
        LOG_I("shotmode: connected to %s:%u as '%s' (slot %d)",
              s->nethost, (unsigned)s->netport, nm, g->local_slot_id);
    }
}

/* Same kill-feed → lobby score path main.c uses. We re-implement it
 * here (rather than refactor main.c) to keep shotmode self-contained. */
static int g_shot_kill_processed     = 0;
static int g_shot_hit_processed      = 0;
static int g_shot_fire_processed     = 0;
static int g_shot_pickup_processed   = 0;
static int g_shot_explosion_processed = 0;

/* Mirror main.c's broadcast_new_hits — drain the world hitfeed and
 * ship NET_MSG_HIT_EVENT to every peer. Without this, networked shot
 * tests don't replicate blood/spark FX onto the client. */
static void shot_broadcast_new_hits(Game *g) {
    if (g->net.role != NET_ROLE_SERVER) return;
    int cur = g->world.hitfeed_count;
    int begin = g_shot_hit_processed;
    if (cur - begin > HITFEED_CAPACITY) begin = cur - HITFEED_CAPACITY;
    for (int n = begin; n < cur; ++n) {
        int idx = n % HITFEED_CAPACITY;
        if (idx < 0) idx += HITFEED_CAPACITY;
        const HitFeedEntry *h = &g->world.hitfeed[idx];
        net_server_broadcast_hit(&g->net,
            (int)h->victim_mech_id, (int)h->hit_part,
            h->pos_x, h->pos_y, h->dir_x, h->dir_y, (int)h->damage);
    }
    g_shot_hit_processed = cur;
}

/* wan-fixes-10 — Mirror broadcast_new_explosions for the shotmode
 * host path so paired-shot tests exercise the new EXPLOSION wire
 * event. Without this, AOE detonations on the shot-server never
 * cross the wire and tests still see the old client-locally-spawned
 * explosion (defeating the regression coverage). */
static void shot_broadcast_new_explosions(Game *g) {
    if (g->net.role != NET_ROLE_SERVER) return;
    int cur = g->world.explosionfeed_count;
    int begin = g_shot_explosion_processed;
    if (cur - begin > EXPLOSIONFEED_CAPACITY) {
        begin = cur - EXPLOSIONFEED_CAPACITY;
    }
    for (int n = begin; n < cur; ++n) {
        int idx = n % EXPLOSIONFEED_CAPACITY;
        if (idx < 0) idx += EXPLOSIONFEED_CAPACITY;
        const ExplosionFeedEntry *e = &g->world.explosionfeed[idx];
        net_server_broadcast_explosion(&g->net,
            (int)e->owner_mech_id, (int)e->weapon_id, e->pos_x, e->pos_y);
    }
    g_shot_explosion_processed = cur;
}

/* Mirror broadcast_new_fires — without this, FIRE_EVENT never crosses
 * the wire in shot tests, so the client sees no remote tracers /
 * projectiles / grapple heads despite the server actually firing them. */
static void shot_broadcast_new_fires(Game *g) {
    if (g->net.role != NET_ROLE_SERVER) return;
    int cur = g->world.firefeed_count;
    int begin = g_shot_fire_processed;
    if (cur - begin > FIREFEED_CAPACITY) begin = cur - FIREFEED_CAPACITY;
    for (int n = begin; n < cur; ++n) {
        int idx = n % FIREFEED_CAPACITY;
        if (idx < 0) idx += FIREFEED_CAPACITY;
        const FireFeedEntry *f = &g->world.firefeed[idx];
        net_server_broadcast_fire(&g->net,
            (int)f->shooter_mech_id, (int)f->weapon_id,
            f->origin_x, f->origin_y, f->dir_x, f->dir_y);
    }
    g_shot_fire_processed = cur;
}

/* Mirror main.c::host_start_map_vote — pick 3 distinct mode-compatible
 * map candidates from g_map_registry, arm lobby_vote_start, broadcast.
 * Without this, shotmode's host doesn't drive the vote-picker code at
 * round-end and tests can't verify the next-round transition through
 * a real vote. */
static void shot_host_start_map_vote(Game *g) {
    if (g->offline_solo) {
        lobby_vote_clear(&g->lobby);
        return;
    }
    unsigned mode_bit = 1u << (unsigned)g->match.mode;
    int candidates[MAP_REGISTRY_MAX];
    int n = 0;
    for (int i = 0; i < g_map_registry.count && n < MAP_REGISTRY_MAX; ++i) {
        if (i == g->match.map_id) continue;
        if (!(g_map_registry.entries[i].mode_mask & mode_bit)) continue;
        candidates[n++] = i;
    }
    if (n == 0) {
        lobby_vote_clear(&g->lobby);
        return;
    }
    /* Fisher-Yates shuffle. */
    for (int i = n - 1; i > 0; --i) {
        int j = (int)pcg32_range(g->world.rng, 0, i + 1);
        int tmp = candidates[i];
        candidates[i] = candidates[j];
        candidates[j] = tmp;
    }
    int a = candidates[0];
    int b = (n >= 2) ? candidates[1] : -1;
    int c = (n >= 3) ? candidates[2] : -1;
    float dur = (g->match.summary_remaining > 0.0f)
                ? g->match.summary_remaining * 0.8f : 12.0f;
    lobby_vote_start(&g->lobby, a, b, c, dur);
    lobby_vote_cast_bots(&g->lobby, a, b, c, g->world.rng);
    if (g->net.role == NET_ROLE_SERVER) {
        net_server_broadcast_vote_state(&g->net, &g->lobby);
    }
    LOG_I("match_flow: map vote: %s / %s / %s (%.1fs)",
          map_def(a) ? map_def(a)->display_name : "—",
          (b >= 0 && map_def(b)) ? map_def(b)->display_name : "—",
          (c >= 0 && map_def(c)) ? map_def(c)->display_name : "—",
          (double)dur);
}

/* Mirror broadcast_new_pickups — drain pickupfeed for client mirror. */
static void shot_broadcast_new_pickups(Game *g) {
    if (g->net.role != NET_ROLE_SERVER) return;
    int cur = g->world.pickupfeed_count;
    int begin = g_shot_pickup_processed;
    if (cur - begin > PICKUPFEED_CAPACITY) begin = cur - PICKUPFEED_CAPACITY;
    for (int n = begin; n < cur; ++n) {
        int idx = n % PICKUPFEED_CAPACITY;
        if (idx < 0) idx += PICKUPFEED_CAPACITY;
        int spawner_id = g->world.pickupfeed[idx];
        if (spawner_id < 0 || spawner_id >= g->world.pickups.count) continue;
        const PickupSpawner *s = &g->world.pickups.items[spawner_id];
        net_server_broadcast_pickup_state(&g->net, spawner_id, s);
    }
    g_shot_pickup_processed = cur;
}

static void shot_apply_new_kills(Game *g) {
    int cur = g->world.killfeed_count;
    int begin = g_shot_kill_processed;
    if (cur - begin > KILLFEED_CAPACITY) begin = cur - KILLFEED_CAPACITY;
    bool any = false;
    for (int n = begin; n < cur; ++n) {
        int idx = n % KILLFEED_CAPACITY;
        if (idx < 0) idx += KILLFEED_CAPACITY;
        KillFeedEntry *k = &g->world.killfeed[idx];
        int killer_slot = (k->killer_mech_id >= 0)
            ? lobby_find_slot_by_mech(&g->lobby, k->killer_mech_id) : -1;
        int victim_slot = lobby_find_slot_by_mech(&g->lobby, k->victim_mech_id);
        /* wan-fixes-13 — see apply_new_kills in main.c for rationale. */
        const char *kn = (killer_slot >= 0) ? g->lobby.slots[killer_slot].name : "world";
        const char *vn = (victim_slot >= 0) ? g->lobby.slots[victim_slot].name : "?";
        snprintf(k->killer_name, sizeof k->killer_name, "%s", kn);
        snprintf(k->victim_name, sizeof k->victim_name, "%s", vn);
        match_apply_kill(&g->match, &g->lobby, killer_slot, victim_slot, k->flags);
        net_server_broadcast_kill(&g->net, k->killer_mech_id,
                                  k->victim_mech_id, k->weapon_id,
                                  k->flags,
                                  k->killer_name, k->victim_name);
        any = true;
    }
    g_shot_kill_processed = cur;
    if (any) g->lobby.dirty = true;
}

/* Host-side match-flow controller, condensed from main.c. */
static bool g_shot_prev_active = false;
static float g_shot_cd_accum = 0.0f;
static void shot_host_flow(Game *g, float dt) {
    /* Countdown broadcast (same logic as main.c's
     * host_broadcast_countdown_if_changed). */
    if (g->net.role == NET_ROLE_SERVER) {
        bool active = g->lobby.auto_start_active;
        if (active != g_shot_prev_active) {
            net_server_broadcast_countdown(&g->net,
                active ? g->lobby.auto_start_remaining : 0.0f,
                active ? 1u : 0u);
            g_shot_cd_accum = 0.0f;
        } else if (active) {
            g_shot_cd_accum += dt;
            if (g_shot_cd_accum >= 0.5f) {
                g_shot_cd_accum -= 0.5f;
                net_server_broadcast_countdown(&g->net,
                    g->lobby.auto_start_remaining, 1u);
            }
        }
        g_shot_prev_active = active;
    }

    switch (g->match.phase) {
        case MATCH_PHASE_LOBBY: {
            /* Auto-arm so the round actually starts in shot mode. We
             * use a 1-second countdown for shot iteration speed. */
            if (!g->lobby.auto_start_active) {
                lobby_auto_start_arm(&g->lobby, g->lobby.auto_start_default);
            }
            if (lobby_tick(&g->lobby, dt)) {
                /* M6 P07 — round prep happens BEFORE countdown begins.
                 * Inlined start_round equivalent (mirrors main.c).
                 * Mirrors main.c::start_round — first round derives
                 * map/mode from rotation; subsequent rounds inherit
                 * from begin_next_lobby (which honors the vote winner). */
                if (g->round_counter == 0) {
                    g->match.map_id = config_pick_map(&g->config, 0);
                    g->match.mode   = config_pick_mode(&g->config, 0);
                }

                /* P07 — CTF mode-mask validation. Build, check,
                 * demote to TDM if the map can't host CTF. */
                if (g->match.mode == MATCH_MODE_CTF) {
                    arena_reset(&g->level_arena);
                    map_build((MapId)g->match.map_id, &g->world, &g->level_arena);
                    bool mode_ok  = (g->world.level.meta.mode_mask & (1u << MATCH_MODE_CTF)) != 0;
                    bool flags_ok = (g->world.level.flag_count == 2 && g->world.level.flags);
                    if (!mode_ok || !flags_ok) {
                        LOG_W("shot_host_flow: map %s doesn't support CTF — demoting to TDM",
                              map_def(g->match.map_id)->short_name);
                        g->match.mode = MATCH_MODE_TDM;
                    }
                }

                g->match.score_limit  = g->config.score_limit;
                g->match.time_limit   = g->config.time_limit;
                g->match.friendly_fire= g->config.friendly_fire;
                if (g->match.mode == MATCH_MODE_CTF && g->match.score_limit >= 25) {
                    g->match.score_limit = FLAG_CAPTURE_DEFAULT;
                }
                /* See comment in main.c::start_round — FFA needs
                 * world.friendly_fire forced on or no hit lands. */
                g->world.friendly_fire= g->config.friendly_fire ||
                                        (g->match.mode == MATCH_MODE_FFA);

                /* P07 — TDM/CTF team auto-balance, mirroring main.c. */
                if (g->match.mode == MATCH_MODE_TDM ||
                    g->match.mode == MATCH_MODE_CTF) {
                    int red = 0, blue = 0;
                    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
                        LobbySlot *ss = &g->lobby.slots[i];
                        if (!ss->in_use) continue;
                        if (ss->team == MATCH_TEAM_RED)  red++;
                        else if (ss->team == MATCH_TEAM_BLUE) blue++;
                    }
                    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
                        LobbySlot *ss = &g->lobby.slots[i];
                        if (!ss->in_use) continue;
                        if (ss->team == MATCH_TEAM_NONE) continue;
                        if (ss->team == MATCH_TEAM_RED || ss->team == MATCH_TEAM_BLUE) continue;
                        int target = (red <= blue) ? MATCH_TEAM_RED : MATCH_TEAM_BLUE;
                        ss->team = target;
                        if (target == MATCH_TEAM_RED) red++; else blue++;
                    }
                    while (red - blue >= 2) {
                        for (int i = MAX_LOBBY_SLOTS - 1; i >= 0; --i) {
                            LobbySlot *ss = &g->lobby.slots[i];
                            if (!ss->in_use) continue;
                            if (ss->team != MATCH_TEAM_RED) continue;
                            ss->team = MATCH_TEAM_BLUE; red--; blue++;
                            break;
                        }
                        if (red - blue < 2) break;
                    }
                    while (blue - red >= 2) {
                        for (int i = MAX_LOBBY_SLOTS - 1; i >= 0; --i) {
                            LobbySlot *ss = &g->lobby.slots[i];
                            if (!ss->in_use) continue;
                            if (ss->team != MATCH_TEAM_BLUE) continue;
                            ss->team = MATCH_TEAM_RED; blue--; red++;
                            break;
                        }
                        if (blue - red < 2) break;
                    }
                    LOG_I("shot_host_flow: %s team auto-balance → red=%d blue=%d",
                          match_mode_name(g->match.mode), red, blue);
                    g->lobby.dirty = true;
                }

                /* Rebuild the level (CTF-validation may have already
                 * built it; re-arena-reset + rebuild for a clean slate
                 * — same shape as main.c::start_round). */
                arena_reset(&g->level_arena);
                map_build((MapId)g->match.map_id, &g->world, &g->level_arena);
                decal_init((int)level_width_px(&g->world.level),
                           (int)level_height_px(&g->world.level));
                /* P08 — refresh + broadcast the serve descriptor so any
                 * mid-round-joining client (and any client that arrived
                 * before round_start) knows what the new round's map is. */
                maps_refresh_serve_info(map_def(g->match.map_id)->short_name,
                                        NULL, &g->server_map_desc,
                                        g->server_map_serve_path,
                                        sizeof(g->server_map_serve_path));
                if (g->net.role == NET_ROLE_SERVER) {
                    net_server_broadcast_map_descriptor(&g->net,
                                                        &g->server_map_desc);
                }
                lobby_spawn_round_mechs(&g->lobby, &g->world,
                                        g->match.map_id, g->local_slot_id,
                                        g->match.mode);
                g->lobby.dirty = true;

                /* M6 P13 — bot driver wiring (mirrors main.c::start_round).
                 * Pre-P13 shotmode never built the nav graph or attached
                 * BotMinds, so any cfg-spawned or wire-added bot stood
                 * motionless throughout the round. Without this, bot-AI
                 * regression tests in shotmode have no observable bot
                 * behavior. Server-only — clients render the bot's mech
                 * through the snapshot stream like any other peer. */
                if (g->net.role == NET_ROLE_SERVER) {
                    bot_system_reset_minds(&g->bots);
                    bot_system_build_nav(&g->bots, &g->world.level,
                                          &g->level_arena);
                    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
                        LobbySlot *s = &g->lobby.slots[i];
                        if (!s->in_use || !s->is_bot) continue;
                        if (s->mech_id < 0)            continue;
                        bot_attach(&g->bots, s->mech_id,
                                    (BotTier)s->bot_tier,
                                    (uint64_t)i * 0xBF58476D1CE4E5B9uLL);
                    }
                    bot_assign_team_roles(&g->bots, &g->world);
                }

                /* P05 — pickup pool. */
                pickup_init_round(&g->world);
                g->world.pickupfeed_count = 0;

                /* P07 — flags + match_mode_cached on World. */
                g->world.match_mode_cached = (int)g->match.mode;
                ctf_init_round(&g->world, g->match.mode);
                g->world.flag_state_dirty = false;

                match_begin_countdown(&g->match, g->match.countdown_default);
                g->mode = MODE_MATCH;
                g_shot_kill_processed = g->world.killfeed_count;
                if (g->net.role == NET_ROLE_SERVER) {
                    net_server_broadcast_lobby_list (&g->net, &g->lobby);
                    g->lobby.dirty = false;
                    net_server_broadcast_round_start(&g->net, &g->match);
                    /* Initial flag-state broadcast — same as main.c. */
                    if (g->world.flag_count > 0) {
                        net_server_broadcast_flag_state(&g->net, &g->world);
                    }
                }
            }
            lobby_chat_age(&g->lobby, dt);
            break;
        }
        case MATCH_PHASE_COUNTDOWN:
            if (match_tick(&g->match, dt)) {
                /* M6 P07 — COUNTDOWN done; flip to ACTIVE and tell
                 * clients. World is already prepped from when the
                 * lobby auto-start fired. */
                match_begin_round(&g->match);
                if (g->net.role == NET_ROLE_SERVER) {
                    net_server_broadcast_match_state(&g->net, &g->match);
                }
            }
            break;
        case MATCH_PHASE_ACTIVE: {
            /* P07 — CTF tick (server only). Same shape as main.c. */
            ctf_step(g, dt);
            shot_apply_new_kills(g);
            /* Mid-round respawn — mirrors main.c. CTF-only inside
             * match_process_respawns. */
            match_process_respawns(&g->world, &g->match, &g->lobby);
            /* Mirror main.c's per-tick fan-out queues. Without these,
             * the client sees no remote HIT_EVENT / FIRE_EVENT /
             * PICKUP_STATE during shot tests — and bugs like the P09
             * "RMB grapple invisible to remote" gap can't be caught
             * in the test scaffold. */
            shot_broadcast_new_hits(g);
            shot_broadcast_new_fires(g);
            shot_broadcast_new_pickups(g);
            shot_broadcast_new_explosions(g);
            /* Drain CTF dirty bit on the host. */
            if (g->net.role == NET_ROLE_SERVER && g->world.flag_state_dirty) {
                net_server_broadcast_flag_state(&g->net, &g->world);
                g->world.flag_state_dirty = false;
            } else if (g->net.role != NET_ROLE_SERVER) {
                g->world.flag_state_dirty = false;
            }
            /* Drain match score dirty bit on the host — re-ships
             * MATCH_STATE so the client's HUD banner picks up team
             * score updates (CTF captures, TDM kill credit). Same
             * shape as main.c::broadcast_match_state_if_dirty. */
            if (g->net.role == NET_ROLE_SERVER && g->match.score_dirty) {
                net_server_broadcast_match_state(&g->net, &g->match);
                g->match.score_dirty = false;
            } else if (g->net.role != NET_ROLE_SERVER) {
                g->match.score_dirty = false;
            }
            bool end = false;
            if (g->match.mode == MATCH_MODE_FFA) {
                for (int i = 0; i < MAX_LOBBY_SLOTS; ++i)
                    if (g->lobby.slots[i].in_use &&
                        g->lobby.slots[i].score >= g->match.score_limit) end = true;
            }
            if (!end && match_round_should_end(&g->match)) end = true;
            if (match_tick(&g->match, dt)) end = true;
            /* "Only one alive" → 3s countdown → end. Mirrors main.c. */
            if (!end && match_step_solo_warning(&g->match, &g->world, dt))
                end = true;
            if (end) {
                match_end_round(&g->match, &g->lobby);
                g->mode = MODE_SUMMARY;
                if (g->net.role == NET_ROLE_SERVER) {
                    net_server_broadcast_lobby_list (&g->net, &g->lobby);
                    g->lobby.dirty = false;
                    net_server_broadcast_round_end  (&g->net, &g->match);
                }
                /* Arm the 3-card vote picker (P09) so the lobby UI's
                 * summary screen has candidates to render. */
                shot_host_start_map_vote(g);
                LOG_I("match_flow: round %d end (mvp=%d)",
                      g->round_counter, g->match.mvp_slot);
            }
            break;
        }
        case MATCH_PHASE_SUMMARY: {
            (void)lobby_tick(&g->lobby, dt);

            /* Fast-forward when every active in-use slot has voted —
             * mirrors main.c. Floors summary_remaining at 1 s so the
             * banner reads briefly. */
            if (g->match.summary_remaining > 1.0f) {
                int active = 0, voted = 0;
                uint32_t cast_mask = g->lobby.vote_mask_a |
                                     g->lobby.vote_mask_b |
                                     g->lobby.vote_mask_c;
                for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
                    if (!g->lobby.slots[i].in_use) continue;
                    if (g->lobby.slots[i].team == MATCH_TEAM_NONE) continue;
                    active++;
                    if (cast_mask & (1u << i)) voted++;
                }
                if (active >= 1 && voted == active) {
                    g->match.summary_remaining = 1.0f;
                    LOG_I("match_flow: all %d voted — fast-forwarding summary",
                          voted);
                }
            }

            if (match_tick(&g->match, dt)) {
                g->round_counter++;
                g->match.rounds_played++;
                /* Default next map/mode from rotation; vote winner overrides. */
                g->match.map_id = config_pick_map(&g->config, g->round_counter);
                g->match.mode   = config_pick_mode(&g->config, g->round_counter);
                if (g->lobby.vote_map_a >= 0 || g->lobby.vote_map_b >= 0 ||
                    g->lobby.vote_map_c >= 0)
                {
                    int winner = lobby_vote_winner(&g->lobby);
                    if (winner >= 0 && winner < g_map_registry.count) {
                        g->match.map_id = winner;
                        LOG_I("match_flow: vote winner → map %s",
                              map_def(winner) ? map_def(winner)->display_name : "?");
                    }
                    lobby_vote_clear(&g->lobby);
                    if (g->net.role == NET_ROLE_SERVER) {
                        net_server_broadcast_vote_state(&g->net, &g->lobby);
                    }
                }

                if (g->match.rounds_played >= g->match.rounds_per_match) {
                    /* Match over — full reset + back to lobby. */
                    LOG_I("match_flow: match over (%d rounds played) — back to lobby",
                          g->match.rounds_played);
                    g->match.rounds_played = 0;
                    g->match.score_limit  = g->config.score_limit;
                    g->match.time_limit   = g->config.time_limit;
                    g->match.friendly_fire= g->config.friendly_fire;
                    g->match.phase        = MATCH_PHASE_LOBBY;
                    lobby_clear_round_mechs(&g->lobby, &g->world);
                    lobby_reset_round_stats(&g->lobby);
                    g->mode = MODE_LOBBY;
                    if (g->net.role == NET_ROLE_SERVER) {
                        net_server_broadcast_match_state(&g->net, &g->match);
                        net_server_broadcast_lobby_list (&g->net, &g->lobby);
                    }
                } else {
                    /* Inter-round: brief countdown, no lobby. Mirrors
                     * main.c::advance_to_next_round which goes via
                     * start_round (full level rebuild + fresh
                     * lobby_spawn_round_mechs + ROUND_START broadcast).
                     * Without the full setup the client would never
                     * exit MODE_SUMMARY into MODE_MATCH for round 2 +,
                     * and round 2's mech_count would stay at 0 from
                     * lobby_clear_round_mechs. */
                    LOG_I("match_flow: round %d/%d — continuing match",
                          g->match.rounds_played + 1, g->match.rounds_per_match);
                    lobby_clear_round_mechs(&g->lobby, &g->world);
                    /* Per-round slot reset — score / kills / deaths /
                     * team_kills / current_streak. Ready +
                     * longest_streak persist across rounds within a
                     * match. Pre-fix, round 2 inherited round 1's
                     * slot.score (already at the cap) and the FFA
                     * round-end gate fired the same tick round 2
                     * began. */
                    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
                        if (!g->lobby.slots[i].in_use) continue;
                        LobbySlot *s = &g->lobby.slots[i];
                        s->score          = 0;
                        s->kills          = 0;
                        s->deaths         = 0;
                        s->team_kills     = 0;
                        s->current_streak = 0;
                    }
                    /* Rebuild level for the next round (vote winner
                     * already applied to g->match.map_id before
                     * after_summary returned here). */
                    arena_reset(&g->level_arena);
                    map_build((MapId)g->match.map_id, &g->world,
                              &g->level_arena);
                    decal_init((int)level_width_px(&g->world.level),
                               (int)level_height_px(&g->world.level));
                    maps_refresh_serve_info(map_def(g->match.map_id)->short_name,
                                            NULL, &g->server_map_desc,
                                            g->server_map_serve_path,
                                            sizeof(g->server_map_serve_path));
                    if (g->net.role == NET_ROLE_SERVER) {
                        net_server_broadcast_map_descriptor(&g->net,
                                                            &g->server_map_desc);
                    }
                    lobby_spawn_round_mechs(&g->lobby, &g->world,
                                            g->match.map_id, g->local_slot_id,
                                            g->match.mode);
                    pickup_init_round(&g->world);
                    g->world.pickupfeed_count = 0;
                    g->world.match_mode_cached = (int)g->match.mode;
                    ctf_init_round(&g->world, g->match.mode);
                    g->world.flag_state_dirty = false;
                    match_begin_countdown(&g->match,
                                          g->match.inter_round_countdown_default);
                    g->mode = MODE_MATCH;
                    if (g->net.role == NET_ROLE_SERVER) {
                        /* Order: lobby table first so clients have
                         * mech_id mappings before ROUND_START + the
                         * snapshot stream. Same as main.c. */
                        net_server_broadcast_lobby_list (&g->net, &g->lobby);
                        g->lobby.dirty = false;
                        net_server_broadcast_round_start(&g->net, &g->match);
                        if (g->world.flag_count > 0) {
                            net_server_broadcast_flag_state(&g->net, &g->world);
                        }
                    }
                }
            }
            break;
        }
    }
}

/* Client-side per-tick housekeeping: chat aging + countdown decay +
 * match time decay (host doesn't broadcast match_state during the
 * ACTIVE phase, so without local decay the client's overlay shows the
 * round-start value frozen for the whole match). */
static void shot_client_tick(Game *g, float dt) {
    lobby_chat_age(&g->lobby, dt);
    if (g->lobby.auto_start_active && g->lobby.auto_start_remaining > 0.0f) {
        g->lobby.auto_start_remaining -= dt;
        if (g->lobby.auto_start_remaining < 0.0f) g->lobby.auto_start_remaining = 0.0f;
    }
    if (g->match.phase == MATCH_PHASE_COUNTDOWN &&
        g->match.countdown_remaining > 0.0f)
    {
        g->match.countdown_remaining -= dt;
        if (g->match.countdown_remaining < 0.0f) g->match.countdown_remaining = 0.0f;
    }
    if (g->match.phase == MATCH_PHASE_ACTIVE &&
        g->match.time_remaining > 0.0f)
    {
        g->match.time_remaining -= dt;
        if (g->match.time_remaining < 0.0f) g->match.time_remaining = 0.0f;
    }
    if (g->match.phase == MATCH_PHASE_SUMMARY &&
        g->match.summary_remaining > 0.0f)
    {
        g->match.summary_remaining -= dt;
        if (g->match.summary_remaining < 0.0f) g->match.summary_remaining = 0.0f;
    }
}

/* Renderer-overlay-callback adapter for the match HUD banner and the
 * round-summary panel. The renderer's overlay slot takes a
 * (void *, sw, sh) signature; we pack Game * and the LobbyUIState
 * into a small struct so each callback can pull what it needs. */
typedef struct {
    Game         *game;
    LobbyUIState *ui;
} ShotOverlayCtx;

static void match_overlay_draw_thunk(void *user, int sw, int sh) {
    ShotOverlayCtx *c = (ShotOverlayCtx *)user;
    /* M6 countdown-fix — same ordering as draw_diag in main.c: arm
     * the GO!-splash latch + per-second beep BEFORE the overlay
     * render reads it. Otherwise the splash has a 1-frame lag at
     * the COUNTDOWN→ACTIVE seam (visible in shot mode where every
     * frame is a fresh PNG). */
    lobby_ui_update_match_loading(c->ui, c->game);
    match_overlay_draw(c->ui, c->game, sw, sh);
}

static void summary_overlay_draw_thunk(void *user, int sw, int sh) {
    ShotOverlayCtx *c = (ShotOverlayCtx *)user;
    summary_screen_run(c->ui, c->game, sw, sh);
}

/* Late-bind world.local_mech_id from local_slot.mech_id once the
 * matching mech actually shows up. Same logic as main.c's MODE_MATCH
 * pre-loop step. */
static void shot_client_late_bind_mech(Game *g) {
    if (g->world.local_mech_id < 0 && g->local_slot_id >= 0) {
        int mid = g->lobby.slots[g->local_slot_id].mech_id;
        if (mid >= 0 && mid < g->world.mech_count) {
            g->world.local_mech_id = mid;
            LOG_I("shotmode: client local_mech_id resolved → %d (slot %d)",
                  mid, g->local_slot_id);
        }
    }
}

/* ---- Spawn — mirror of main.c::seed_world. Kept inline so this module
 * stays self-contained and main.c isn't refactored just for shot mode. */
static void seed_world(Game *g, const Script *s) {
    World *w = &g->world;
    bool loaded = false;
    /* Map source priority: code-built map (P07 `map` directive)
     * > on-disk .lvl (P04 `load_lvl`) > hardcoded tutorial. */
    if (s && s->map_id >= 0) {
        map_build((MapId)s->map_id, w, &g->level_arena);
        loaded = true;
        LOG_I("shotmode: built map '%s' (%dx%d, mode_mask=0x%x, %d flags)",
              map_def(s->map_id)->short_name, w->level.width, w->level.height,
              (unsigned)w->level.meta.mode_mask, w->level.flag_count);
    }
    if (!loaded && s && s->load_lvl[0]) {
        loaded = map_build_from_path(w, &g->level_arena, s->load_lvl);
        if (!loaded) {
            LOG_W("shotmode: load_lvl '%s' failed — falling back to tutorial",
                  s->load_lvl);
        } else {
            LOG_I("shotmode: loaded custom level '%s' (%dx%d)",
                  s->load_lvl, w->level.width, w->level.height);
        }
    }
    if (!loaded) {
        level_build_tutorial(&w->level, &g->level_arena);
    }

    /* M6 P09 — Apply paint_tile/paint_rect directives AFTER the map
     * loads but BEFORE the renderer/decal init reads the level
     * dimensions. This stamps editor-checkbox semantics on top of
     * whatever map shipped, useful for the per-flag visual
     * differentiation regression test. */
    if (s && s->paint_count > 0 && w->level.tiles) {
        int W = w->level.width, H = w->level.height;
        for (int p = 0; p < s->paint_count; ++p) {
            int tx0 = s->paints[p].tx0;
            int ty0 = s->paints[p].ty0;
            int tx1 = s->paints[p].tx1;
            int ty1 = s->paints[p].ty1;
            uint16_t flags = s->paints[p].flags;
            if (tx0 < 0) tx0 = 0;
            if (ty0 < 0) ty0 = 0;
            if (tx1 > W) tx1 = W;
            if (ty1 > H) ty1 = H;
            for (int ty = ty0; ty < ty1; ++ty) {
                for (int tx = tx0; tx < tx1; ++tx) {
                    LvlTile *t = &w->level.tiles[ty * W + tx];
                    t->flags = flags;
                }
            }
            SHOT_LOG("paint_tile rect=(%d,%d)-(%d,%d) flags=0x%x",
                     tx0, ty0, tx1, ty1, (unsigned)flags);
        }
    }
    /* M6 P09 — paint_ambi appends ambient zones after world build.
     * Since LvlAmbi storage is owned by the level arena (a vector of
     * fixed size at load time), we can't realloc; reuse the existing
     * array if there's headroom (cook_maps reserves up to ~16 ambis
     * per map; if the live map already used all of them, the directive
     * silently drops with a WARN). Re-init atmosphere so it sees the
     * new zones. */
    if (s && s->paint_ambi_count > 0) {
        Level *L = &w->level;
        /* Allocate fresh ambi storage that can hold the original plus
         * the painted ones. The level arena is reset between map
         * loads, so allocating here is fine. */
        int extra = s->paint_ambi_count;
        int total = L->ambi_count + extra;
        LvlAmbi *new_a = (LvlAmbi *)arena_alloc(&g->level_arena,
                                                sizeof(LvlAmbi) * (size_t)total);
        if (new_a) {
            if (L->ambis && L->ambi_count > 0) {
                memcpy(new_a, L->ambis, sizeof(LvlAmbi) * (size_t)L->ambi_count);
            }
            for (int p = 0; p < extra; ++p) {
                int idx = L->ambi_count + p;
                int x0 = s->paint_ambis[p].x0, y0 = s->paint_ambis[p].y0;
                int x1 = s->paint_ambis[p].x1, y1 = s->paint_ambis[p].y1;
                if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
                if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
                new_a[idx] = (LvlAmbi){
                    .rect_x = (int16_t)x0,
                    .rect_y = (int16_t)y0,
                    .rect_w = (int16_t)(x1 - x0),
                    .rect_h = (int16_t)(y1 - y0),
                    .kind = s->paint_ambis[p].kind,
                    .strength_q = (int16_t)(s->paint_ambis[p].strength * 32767.0f),
                    .dir_x_q = (int16_t)(s->paint_ambis[p].dir_x * 32767.0f),
                    .dir_y_q = (int16_t)(s->paint_ambis[p].dir_y * 32767.0f),
                };
                SHOT_LOG("paint_ambi kind=%u rect=(%d,%d)-(%d,%d) st=%.2f dir=(%.2f,%.2f)",
                         (unsigned)s->paint_ambis[p].kind,
                         x0, y0, x1, y1,
                         (double)s->paint_ambis[p].strength,
                         (double)s->paint_ambis[p].dir_x,
                         (double)s->paint_ambis[p].dir_y);
            }
            L->ambis      = new_a;
            L->ambi_count = total;
            /* Re-run atmosphere init so g_atmosphere.zone_audio_state
             * is sized appropriately for the painted zones. */
            atmosphere_init_for_map(L);
        }
    }
    /* M6 P09 — weather override (per shotmode `weather <kind> <density>`
     * directive). Clobbers g_atmosphere AFTER init_for_map so the
     * shot author can swap in SNOW/RAIN on any map without recooking.
     * Applied to BOTH the live g_atmosphere AND L->meta so a host's
     * snapshot stream replicates the override to remote clients. */
    if (s && s->have_weather_override) {
        g_atmosphere.weather_kind    = (uint8_t)s->weather_kind_override;
        g_atmosphere.weather_density = s->weather_density_override;
        w->level.meta.weather_kind      = (uint16_t)s->weather_kind_override;
        w->level.meta.weather_density_q = (uint16_t)(s->weather_density_override * 65535.0f);
        SHOT_LOG("weather override: kind=%d density=%.2f",
                 s->weather_kind_override,
                 (double)s->weather_density_override);
    }

    decal_init((int)level_width_px(&w->level), (int)level_height_px(&w->level));

    /* P07 — apply mode override (if any) before spawning so ctf_init_round
     * sees the right state. Mirror match_mode_cached so mech.c can
     * branch in mech_kill (drop flag on death) and apply_jet_force
     * (carrier penalty). The single-player shot path bypasses the
     * lobby/match flow controllers, so set match.phase to ACTIVE
     * directly — without it, ctf_step / pickup_step / match_apply_kill
     * would silently no-op. */
    if (s && s->match_mode >= 0) {
        g->match.mode = (MatchModeId)s->match_mode;
    }
    g->match.phase             = MATCH_PHASE_ACTIVE;
    g->world.match_mode_cached = (int)g->match.mode;
    g->world.authoritative     = true;

    /* If the loaded level supplied authored spawns AND the player team
     * (RED) has a Red spawn, prefer it over the hardcoded shot-mode
     * tutorial position. Same logic for the dummy on team BLUE. */
    const float feet_below_pelvis = 36.0f;
    const float foot_clearance    = 4.0f;
    Vec2 player_spawn = { 16.0f * 32.0f + 8.0f,
                          30.0f * 32.0f - feet_below_pelvis - foot_clearance };
    Vec2 dummy_spawn  = { 75.0f * 32.0f,
                          32.0f * 32.0f - feet_below_pelvis - foot_clearance };
    if (w->level.spawn_count > 0) {
        for (int i = 0; i < w->level.spawn_count; ++i) {
            const LvlSpawn *sp = &w->level.spawns[i];
            if (sp->team == 1) { player_spawn = (Vec2){ (float)sp->pos_x, (float)sp->pos_y }; break; }
        }
        for (int i = 0; i < w->level.spawn_count; ++i) {
            const LvlSpawn *sp = &w->level.spawns[i];
            if (sp->team == 2) { dummy_spawn = (Vec2){ (float)sp->pos_x, (float)sp->pos_y }; break; }
        }
    }

    MechLoadout lo = (s && s->have_loadout) ? s->loadout : mech_default_loadout();
    w->local_mech_id = mech_create_loadout(w, lo, player_spawn,
                                           /*team*/ 1, /*is_dummy*/ false);

    MechLoadout dlo = mech_default_loadout();
    dlo.armor_id = ARMOR_NONE;
    w->dummy_mech_id = mech_create_loadout(w, dlo, dummy_spawn,
                                           /*team*/ 2, /*is_dummy*/ true);

    /* M5 P10 — extra chassis dummies for side-by-side comparison shots
     * (e.g. tests/shots/m5_chassis_distinctness.shot). Spawned as
     * `is_dummy=true` so they stand still without aiming. */
    if (s) {
        for (int i = 0; i < s->extra_chassis_count; ++i) {
            MechLoadout xlo = mech_default_loadout();
            xlo.chassis_id  = s->extra_chassis[i].chassis_id;
            xlo.armor_id    = ARMOR_NONE;
            /* P11 — optional primary override so a single shot can put
             * Mass Driver / Sidearm / Pulse Rifle / etc. in the same
             * frame for visible-size comparison. */
            if (s->extra_chassis[i].primary_id >= 0) {
                xlo.primary_id = s->extra_chassis[i].primary_id;
            }
            Vec2 ex_spawn = { s->extra_chassis[i].x, s->extra_chassis[i].y };
            mech_create_loadout(w, xlo, ex_spawn, /*team*/ 2, /*is_dummy*/ true);
        }
    }

    /* Populate the pickup pool from the level's PICK records so
     * pickup-aware shots see the same runtime state as the lobby →
     * match transition does in main.c. Without this `w->pickups.count`
     * stays at 0 and `draw_pickups` walks an empty pool. */
    pickup_init_round(w);
    w->pickupfeed_count = 0;

    /* P07 — populate flags[] when in CTF mode. ctf_init_round bails
     * silently if the level lacks a Red+Blue flag pair. */
    ctf_init_round(w, g->match.mode);

    w->camera_target = player_spawn;
    w->camera_smooth = player_spawn;
    w->camera_zoom   = 1.4f;
}

/* ---- Contact sheet composer ---------------------------------------- */

/* Compose every PNG produced by the run into a single grid image.
 * Cells are scaled-down screenshots; a thin label strip beneath each
 * shows the shot name + tick so a reviewer can locate any cell back
 * in the per-shot output. */
static void build_contact_sheet(const Script *s) {
    int label_h = 14;
    int cell_w  = s->contact_cell_w;
    int cell_h  = s->contact_cell_h;
    int img_h   = cell_h - label_h;
    if (img_h < 16) img_h = 16;
    int cols    = s->contact_cols;

    /* Count actual shot events. */
    int n = 0;
    for (int i = 0; i < s->event_count; ++i)
        if (s->events[i].kind == EV_SHOT) n++;
    if (n == 0) return;

    int rows = (n + cols - 1) / cols;
    Image sheet = GenImageColor(cols * cell_w, rows * cell_h,
                                (Color){12, 14, 18, 255});

    int idx = 0;
    for (int i = 0; i < s->event_count; ++i) {
        if (s->events[i].kind != EV_SHOT) continue;
        int col = idx % cols;
        int row = idx / cols;
        int dx  = col * cell_w;
        int dy  = row * cell_h;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s.png", s->out_dir, s->events[i].name);
        Image cell = LoadImage(path);
        if (cell.data) {
            ImageResize(&cell, cell_w, img_h);
            ImageDraw(&sheet, cell,
                      (Rectangle){0, 0, (float)cell_w, (float)img_h},
                      (Rectangle){(float)dx, (float)dy,
                                  (float)cell_w, (float)img_h},
                      WHITE);
            UnloadImage(cell);
        } else {
            ImageDrawRectangle(&sheet, dx, dy, cell_w, img_h,
                               (Color){50, 20, 20, 255});
        }

        char label[80];
        snprintf(label, sizeof(label), "%s @t%d",
                 s->events[i].name, s->events[i].tick);
        ImageDrawRectangle(&sheet, dx, dy + img_h, cell_w, label_h,
                           (Color){0, 0, 0, 220});
        ImageDrawText(&sheet, label, dx + 4, dy + img_h, 10,
                      (Color){200, 210, 220, 255});

        idx++;
    }

    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/%s.png", s->out_dir, s->contact_name);
    if (ExportImage(sheet, out_path)) {
        LOG_I("shotmode: contact sheet → %s (%dx%d, %d cells)",
              out_path, sheet.width, sheet.height, n);
    } else {
        LOG_E("shotmode: ExportImage failed for %s", out_path);
    }
    UnloadImage(sheet);
}

/* ---- The runner ---------------------------------------------------- */

int shotmode_run(const char *script_path) {
    /* P08b: the script parser resolves `map <short_name>` via
     * map_id_from_name(), which reads g_map_registry. game_init()
     * populates the registry — but it runs AFTER parse_script below.
     * Seed the builtin entries here so `map foundry` / `map crossfire`
     * etc. resolve at parse time. game_init() re-runs this idempotently
     * (rescans assets/maps/ for .lvl files to surface custom maps). */
    map_registry_init();

    Script s = {0};
    if (!parse_script(script_path, &s)) {
        free(s.events);
        free(s.lerps);
        return EXIT_FAILURE;
    }

    mkdir_p(s.out_dir);

    /* Redirect logging to a per-script file under <out_dir> so the run
     * produces a paired (.png + .log) output that an LLM can review
     * without hunting through soldut.log. main.c already opened
     * soldut.log; close it and re-open at the new path before any
     * SHOT_LOG fires. */
    {
        const char *base = strrchr(script_path, '/');
        base = base ? base + 1 : script_path;
        char base_no_ext[128];
        snprintf(base_no_ext, sizeof(base_no_ext), "%s", base);
        char *dot = strrchr(base_no_ext, '.');
        if (dot) *dot = 0;
        char log_path[512];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", s.out_dir, base_no_ext);
        log_shutdown();
        log_init(log_path);
    }
    g_shot_mode = 1;
    /* M6 P03 — propagate the opt-in perf-overlay flag. The renderer
     * reads this per frame; cleared on shotmode exit alongside
     * g_shot_mode. */
    g_shot_perf_overlay = s.perf_overlay ? 1 : 0;

    LOG_I("shotmode: script=%s window=%dx%d events=%d end_tick=%d",
          script_path, s.window_w, s.window_h, s.event_count, s.end_tick);

    Game game;
    if (!game_init(&game)) { free(s.events); free(s.lerps); return EXIT_FAILURE; }
    if (s.seed_override) pcg32_seed(&game.rng, s.seed_hi, s.seed_lo);

    /* Pick up any soldut.cfg in cwd — shot mode is otherwise stuck
     * on game_init's config_defaults, so a `mode=ctf` cfg in the
     * shot run's tmpdir would be ignored. Mirrors main.c's flow.
     * Re-init MatchState from the loaded config so config-driven
     * mode / score_limit / time_limit reach the (host's) start_round
     * via config_pick_*. */
    config_load(&game.config, "soldut.cfg");
    match_init(&game.match, game.config.mode, game.config.score_limit,
               game.config.time_limit, game.config.friendly_fire);
    game.match.map_id = config_pick_map(&game.config, 0);
    game.lobby.auto_start_default = game.config.auto_start_seconds;
    game.world.friendly_fire = game.config.friendly_fire;

    /* Window title — show host/client role so screenshots are
     * self-labeling. */
    char title_buf[160];
    if (s.netmode == NETMODE_HOST) {
        snprintf(title_buf, sizeof title_buf,
                 "Soldut %s — shot host:%u", SOLDUT_VERSION_STRING,
                 (unsigned)s.netport);
    } else if (s.netmode == NETMODE_CONNECT) {
        snprintf(title_buf, sizeof title_buf,
                 "Soldut %s — shot client to %s:%u", SOLDUT_VERSION_STRING,
                 s.nethost, (unsigned)s.netport);
    } else {
        snprintf(title_buf, sizeof title_buf,
                 "Soldut %s — shot mode", SOLDUT_VERSION_STRING);
    }
    PlatformConfig pcfg = {
        .window_w = s.window_w, .window_h = s.window_h,
        .vsync = false, .fullscreen = false,
        /* M6 P03 — default 0 = identity pipeline (internal==window) so
         * existing regression shot tests stay byte-identical to pre-P03.
         * Bench scripts override via the `internal_h N` directive (see
         * tests/shots/perf_bench.shot). */
        .internal_h = s.internal_h,
        .title = title_buf,
    };
    if (!platform_init(&pcfg)) {
        game_shutdown(&game); free(s.events); free(s.lerps);
        return EXIT_FAILURE;
    }

    /* M5 P10 — chassis sprite atlases (capsule fallback when missing).
     * M5 P11 — shared weapon atlas (line fallback when missing).
     * M5 P14 — audio module init (no-op silently for missing assets).
     * Shotmode does not read soldut-prefs.cfg (deterministic runs),
     * so audio_init's defaults stick — pin the master bus to 30% so
     * any developer auditioning a .shot script hears a comfortable
     * mix without their last-saved game-volume bleeding into the
     * test. */
    mech_sprites_load_all();
    weapons_atlas_load();
    audio_init(&game);
    audio_set_bus_volume(AUDIO_BUS_MASTER, 0.30f);

    /* Networked path: bootstrap host or client, then run the full
     * mode dispatcher each tick. The legacy match-only path below is
     * kept for the existing M1/M3 shot tests. */
    if (s.netmode != NETMODE_NONE) {
        networked_shot_bootstrap(&game, &s);
        if ((s.netmode == NETMODE_HOST && game.net.role != NET_ROLE_SERVER) ||
            (s.netmode == NETMODE_CONNECT && game.net.role != NET_ROLE_CLIENT))
        {
            LOG_E("shotmode: networked bootstrap failed");
            audio_shutdown();
            weapons_atlas_unload();
            mech_sprites_unload_all();
            map_kit_unload();
            renderer_decorations_unload();
            hud_atlas_unload();
            renderer_post_shutdown();
            platform_shutdown();
            game_shutdown(&game); free(s.events); free(s.lerps);
            net_shutdown();
            return EXIT_FAILURE;
        }

        Renderer rd_n;
        renderer_init(&rd_n, GetScreenWidth(), GetScreenHeight(),
                      (Vec2){640, 360});
        rd_n.cam_dt_override = 1.0f / 60.0f;
        LobbyUIState ui_n = {0};
        lobby_ui_init(&ui_n);
        snprintf(ui_n.player_name, sizeof ui_n.player_name, "%s",
                 s.netname[0] ? s.netname : "shot");
        /* wan-fixes-10 — seed the UI loadout draft from the script's
         * `loadout` directive (mirrors how main.c seeds from
         * soldut-prefs.cfg in production). Without this, the lobby
         * UI's first push (sync_loadout_from_server) sends
         * lobby_ui_init defaults regardless of what the script
         * requested, and the spawned mech matches the wrong loadout. */
        if (s.have_loadout) {
            ui_n.lobby_chassis   = s.loadout.chassis_id;
            ui_n.lobby_primary   = s.loadout.primary_id;
            ui_n.lobby_secondary = s.loadout.secondary_id;
            ui_n.lobby_armor     = s.loadout.armor_id;
            ui_n.lobby_jet       = s.loadout.jetpack_id;
        }

        const float TICK_DT = 1.0f / 60.0f;
        int ev_idx_n = 0;
        bool ended_n = false;
        int shots_taken_n = 0;
        uint16_t held_n = 0;

        /* `spawn_at` for networked mode: each round start respawns
         * the local mech at the FFA/team lane. We re-teleport to the
         * scripted point on every -1 → valid transition of
         * world.local_mech_id, so multi-round tests honor it across
         * rounds. */
        int prev_local_mech_n = -1;

        /* Aim state for the networked path. If the script sets an
         * aim point (via `aim WX WY` or `at T aim WX WY`) we use that
         * world-space point. Otherwise default to "200 px right of
         * the local mech's chest" so the mech faces a predictable
         * direction. */
        bool  aim_set_n      = (s.initial_aim_mode == AIM_WORLD);
        float aim_world_x_n  = aim_set_n ? s.initial_ax : 0.0f;
        float aim_world_y_n  = aim_set_n ? s.initial_ay : 0.0f;

        for (int tick = 0; tick <= s.end_tick && !ended_n; ++tick) {
            if (WindowShouldClose()) { LOG_W("shotmode: window closed early"); break; }

            /* Apply scheduled press/release/end/aim events. Click /
             * key / mouse-screen events for lobby UI driving aren't
             * yet supported. */
            while (ev_idx_n < s.event_count && s.events[ev_idx_n].tick == tick) {
                const Event *ev = &s.events[ev_idx_n];
                switch (ev->kind) {
                case EV_PRESS:   held_n |= ev->button; break;
                case EV_RELEASE: held_n &= (uint16_t)~ev->button; break;
                case EV_AIM:
                    aim_set_n     = true;
                    aim_world_x_n = ev->ax;
                    aim_world_y_n = ev->ay;
                    break;
                case EV_END:     ended_n = true; break;
                case EV_SHOT:    /* handled after render */ break;
                case EV_GIVE_INVIS: {
                    int lid = game.world.local_mech_id;
                    if (lid >= 0 && lid < game.world.mech_count) {
                        game.world.mechs[lid].powerup_invis_remaining = ev->ax;
                        SHOT_LOG("t=%d give_invis local_mech=%d secs=%.2f",
                                 tick, lid, (double)ev->ax);
                    }
                    break;
                }
                case EV_FLAG_CARRY: {
                    int lid = game.world.local_mech_id;
                    int fidx = (int)ev->ax;
                    if (lid >= 0 && lid < game.world.mech_count &&
                        fidx >= 0 && fidx < game.world.flag_count) {
                        game.world.flags[fidx].status       = FLAG_CARRIED;
                        game.world.flags[fidx].carrier_mech = (int8_t)lid;
                        game.world.flag_state_dirty         = true;
                        SHOT_LOG("t=%d flag_carry flag=%d local_mech=%d",
                                 tick, fidx, lid);
                    }
                    break;
                }
                case EV_TEAM_CHANGE: {
                    int team = (int)ev->ax;
                    if (game.net.role == NET_ROLE_CLIENT) {
                        net_client_send_team_change(&game.net, team);
                        LOG_I("shot: team_change → %d (sent to host)", team);
                    } else if (game.local_slot_id >= 0) {
                        lobby_set_team(&game.lobby, game.local_slot_id, team);
                        LOG_I("shot: team_change → %d (host slot=%d)",
                              team, game.local_slot_id);
                    }
                    break;
                }
                case EV_BOT_TEAM: {
                    int slot = (int)ev->ax;
                    int team = (int)ev->ay;
                    if (game.net.role == NET_ROLE_CLIENT) {
                        net_client_send_bot_team(&game.net, slot, team);
                        LOG_I("shot: bot_team slot=%d team=%d (sent to host)",
                              slot, team);
                    } else if (slot >= 0 && slot < MAX_LOBBY_SLOTS &&
                               game.lobby.slots[slot].in_use)
                    {
                        lobby_set_team(&game.lobby, slot, team);
                        LOG_I("shot: bot_team slot=%d team=%d (host-direct)",
                              slot, team);
                    }
                    break;
                }
                case EV_ADD_BOT: {
                    int tier = (int)ev->ax;
                    if (game.net.role == NET_ROLE_CLIENT) {
                        net_client_send_add_bot(&game.net, (uint8_t)tier);
                        LOG_I("shot: add_bot tier=%d (sent to host)", tier);
                    }
                    break;
                }
                case EV_ARM_CARRY: {
                    int fidx = (int)ev->ax;
                    int mid  = (int)ev->ay;
                    if (game.net.role == NET_ROLE_SERVER &&
                        fidx >= 0 && fidx < game.world.flag_count &&
                        mid  >= 0 && mid  < game.world.mech_count) {
                        game.world.flags[fidx].status         = FLAG_CARRIED;
                        game.world.flags[fidx].carrier_mech   = (int8_t)mid;
                        game.world.flag_state_dirty           = true;
                        LOG_I("shot: arm_carry flag=%d mech=%d (host-side)",
                              fidx, mid);
                    }
                    break;
                }
                case EV_KILL_PEER: {
                    int mid = (int)ev->ax;
                    int shooter = (int)ev->ay;
                    if (game.net.role == NET_ROLE_SERVER &&
                        mid >= 0 && mid < game.world.mech_count &&
                        game.world.mechs[mid].alive) {
                        Mech *m = &game.world.mechs[mid];
                        Vec2 pelv = (Vec2){
                            game.world.particles.pos_x[m->particle_base + PART_PELVIS],
                            game.world.particles.pos_y[m->particle_base + PART_PELVIS],
                        };
                        /* Use mech_apply_damage so the regular damage
                         * path runs (kill-feed, hit-event broadcast,
                         * ctf_drop_on_death from mech_kill, etc.).
                         * 9999 dmg ensures lethal regardless of armor. */
                        mech_apply_damage(&game.world, mid, PART_CHEST,
                                          9999.0f, (Vec2){0.0f, -1.0f},
                                          shooter);
                        (void)pelv;
                        LOG_I("shot: kill_peer mech=%d shooter=%d (host-side)",
                              mid, shooter);
                    }
                    break;
                }
                case EV_DAMAGE: {
                    /* M6 P04 — exercise the damage-number color-tier
                     * spawn from a single-process shot test. Works on
                     * the standalone (NET_ROLE_NONE) world used by the
                     * single-process tier test; also works on a host
                     * (NET_ROLE_SERVER) for completeness. dir = -Y so
                     * the spew direction is sideways (perpendicular)
                     * with a slight upward bias — reads as a spew, not
                     * as a downward squirt. */
                    int mid    = (int)ev->ax;
                    int part   = (int)ev->ay;
                    int amount = ev->az;
                    if (mid >= 0 && mid < game.world.mech_count &&
                        part >= 0 && part < PART_COUNT &&
                        game.world.mechs[mid].alive) {
                        mech_apply_damage(&game.world, mid, part,
                                          (float)amount,
                                          (Vec2){0.0f, -1.0f},
                                          /*shooter=*/-1);
                        LOG_I("shot: damage mech=%d part=%d amount=%d",
                              mid, part, amount);
                    }
                    break;
                }
                case EV_KICK:
                case EV_BAN: {
                    int slot = (int)ev->ax;
                    bool ban = (ev->kind == EV_BAN);
                    if (game.net.role == NET_ROLE_SERVER && slot >= 0) {
                        net_server_kick_or_ban_slot(&game.net, &game,
                                                    slot, ban);
                        LOG_I("shot: %s slot=%d (host-side)",
                              ban ? "ban" : "kick", slot);
                    }
                    break;
                }
                case EV_VOTE_MAP: {
                    int choice = (int)ev->ax;
                    if (choice < 0 || choice > 2) break;
                    if (game.net.role == NET_ROLE_CLIENT) {
                        net_client_send_map_vote(&game.net, choice);
                        LOG_I("shot: vote_map=%d (sent to host)", choice);
                    } else if (game.net.role == NET_ROLE_SERVER &&
                               game.local_slot_id >= 0) {
                        lobby_vote_cast(&game.lobby,
                                        game.local_slot_id, choice);
                        net_server_broadcast_vote_state(&game.net,
                                                        &game.lobby);
                        LOG_I("shot: vote_map=%d (host slot=%d)",
                              choice, game.local_slot_id);
                    }
                    break;
                }
                case EV_KICK_MODAL:
                case EV_BAN_MODAL: {
                    int slot = (int)ev->ax;
                    bool ban = (ev->kind == EV_BAN_MODAL);
                    if (game.net.role == NET_ROLE_SERVER && slot >= 0) {
                        if (ban) {
                            ui_n.ban_target_slot  = slot;
                            ui_n.kick_target_slot = -1;
                        } else {
                            ui_n.kick_target_slot = slot;
                            ui_n.ban_target_slot  = -1;
                        }
                        LOG_I("shot: %s_modal slot=%d (host UI)",
                              ban ? "ban" : "kick", slot);
                    }
                    break;
                }
                default:         break;  /* mouse-screen / tap unused here */
                }
                ev_idx_n++;
            }

            /* Pump network FIRST. */
            net_poll(&game.net, &game, TICK_DT);

            /* Forced-disconnect detection — same hook as main.c so
             * shotmode tests can verify the kick/ban path. When the
             * server drops a client, ENet sets ns->connected=false on
             * the client. We log it (the runner asserts on the log
             * line) and stop the script — there's nothing to render
             * once the connection is gone. */
            if (game.net.role == NET_ROLE_CLIENT && !game.net.connected) {
                LOG_I("client: server connection lost — ending shot script");
                ended_n = true;
                break;
            }

            /* Per-mode dispatch. */
            switch (game.mode) {
            case MODE_LOBBY:
                if (game.net.role == NET_ROLE_SERVER) shot_host_flow(&game, TICK_DT);
                else                                   shot_client_tick(&game, TICK_DT);
                break;
            case MODE_MATCH: {
                shot_client_late_bind_mech(&game);
                if (game.net.role == NET_ROLE_SERVER) shot_host_flow(&game, TICK_DT);
                else if (game.net.role == NET_ROLE_CLIENT) shot_client_tick(&game, TICK_DT);

                /* Detect "local mech just spawned" (-1 → valid) and
                 * teleport to the scripted spawn_at point if any.
                 * Fires once per round; we update prev IMMEDIATELY
                 * after the check so a single iter's transition is
                 * processed exactly once. */
                if (s.have_spawn_at &&
                    prev_local_mech_n < 0 &&
                    game.world.local_mech_id >= 0)
                {
                    Mech *pm = &game.world.mechs[game.world.local_mech_id];
                    ParticlePool *pp = &game.world.particles;
                    int b = pm->particle_base;
                    float cur_x = pp->pos_x[b + PART_PELVIS];
                    float cur_y = pp->pos_y[b + PART_PELVIS];
                    float dx = s.spawn_x - cur_x;
                    float dy = s.spawn_y - cur_y;
                    for (int i = 0; i < PART_COUNT; ++i) {
                        physics_translate_kinematic(pp, b + i, dx, dy);
                    }
                    LOG_I("shotmode: spawn_at teleport mech=%d to (%.1f,%.1f)",
                          game.world.local_mech_id, s.spawn_x, s.spawn_y);
                }
                /* Host-only: also teleport peer mechs per the
                 * `peer_spawn SLOT WX WY` directives. Only the host
                 * has authoritative positions; the client's own
                 * spawn_at is cosmetic until the server agrees. */
                if (game.net.role == NET_ROLE_SERVER &&
                    prev_local_mech_n < 0 &&
                    game.world.local_mech_id >= 0)
                {
                    for (int pi = 0; pi < s.peer_spawn_count; ++pi) {
                        int slot = s.peer_spawns[pi].slot;
                        if (slot < 0 || slot >= MAX_LOBBY_SLOTS) continue;
                        int mid = game.lobby.slots[slot].mech_id;
                        if (mid < 0 || mid >= game.world.mech_count) continue;
                        Mech *pm = &game.world.mechs[mid];
                        ParticlePool *pp = &game.world.particles;
                        int b = pm->particle_base;
                        float dx = s.peer_spawns[pi].x - pp->pos_x[b + PART_PELVIS];
                        float dy = s.peer_spawns[pi].y - pp->pos_y[b + PART_PELVIS];
                        for (int i = 0; i < PART_COUNT; ++i) {
                            physics_translate_kinematic(pp, b + i, dx, dy);
                        }
                        LOG_I("shotmode: peer_spawn teleport slot=%d mech=%d to (%.1f,%.1f)",
                              slot, mid, s.peer_spawns[pi].x, s.peer_spawns[pi].y);
                    }
                }
                /* Track local_mech_id transitions HERE (inside the
                 * MATCH case) rather than at end-of-iter. The
                 * MODE_LOBBY → MODE_MATCH switch can set
                 * local_mech_id mid-iter (in shot_host_flow's
                 * start_round); updating prev at end-of-iter would
                 * miss the -1→valid transition that actually
                 * happens AFTER start_round but BEFORE the next
                 * MATCH iter. */
                prev_local_mech_n = game.world.local_mech_id;
                /* Build input + simulate. Use the script-provided aim
                 * point if any; otherwise default to "200 px right of
                 * local mech chest" so the mech faces predictably. */
                Vec2 aim;
                if (aim_set_n) {
                    aim = (Vec2){ aim_world_x_n, aim_world_y_n };
                } else if (game.world.local_mech_id >= 0) {
                    Vec2 cp = mech_chest_pos(&game.world, game.world.local_mech_id);
                    aim = (Vec2){ cp.x + 200.0f, cp.y };
                } else {
                    aim = (Vec2){0, 0};
                }
                ClientInput nin = {
                    .buttons = held_n,
                    .seq     = (uint16_t)(game.world.tick + 1),
                    .aim_x   = aim.x, .aim_y = aim.y,
                    .dt      = TICK_DT,
                };
                if (game.world.local_mech_id >= 0) {
                    game.world.mechs[game.world.local_mech_id].latched_input = nin;
                    game.world.mechs[game.world.local_mech_id].aim_world = aim;
                }
                if (game.net.role == NET_ROLE_CLIENT) {
                    /* M6 P07 — input gate during pre-round countdown. */
                    if (game.match.phase == MATCH_PHASE_COUNTDOWN) {
                        match_lock_inputs(&game.world);
                    }
                    simulate_step(&game.world, TICK_DT);
                    /* P03: same as main.c — pull remote mechs to the
                     * interpolated server position after physics. The
                     * helper handles drift correction (catches the
                     * LOBBY-froze-the-clock case). */
                    if (game.net.client_render_clock_armed) {
                        net_client_advance_render_clock(&game.net,
                                                        TICK_DT * 1000.0);
                        double rt = game.net.client_render_time_ms;
                        uint32_t rt_u32 = (rt > 0.0) ? (uint32_t)rt : 0u;
                        snapshot_interp_remotes(&game.world, rt_u32);
                    }
                    reconcile_push_input(&game.reconcile, nin);
                    net_client_send_input(&game.net, nin);
                    reconcile_tick_smoothing(&game.reconcile);
                } else {
                    /* M6 P07 — input gate during pre-round countdown. */
                    if (game.match.phase == MATCH_PHASE_COUNTDOWN) {
                        match_lock_inputs(&game.world);
                    }
                    /* M6 P13 — drive bot AI before simulate so the
                     * latched_input for each bot mech is populated for
                     * this tick. Mirrors main.c::poll_and_simulate. */
                    bot_step(&game.bots, &game.world, &game, TICK_DT);
                    simulate_step(&game.world, TICK_DT);
                }
                break;
            }
            case MODE_SUMMARY:
                if (game.net.role == NET_ROLE_SERVER) shot_host_flow(&game, TICK_DT);
                else                                   shot_client_tick(&game, TICK_DT);
                /* Round ended → reset prev so next round_start
                 * triggers a fresh teleport. local_mech_id may
                 * still be the dead-mech id at this point; what
                 * matters is the next MATCH transition starts from
                 * "we have not yet teleported this round". */
                prev_local_mech_n = -1;
                break;
            default: break;
            }

            /* Render. The renderer always draws the world frame
             * inside one Begin/EndDrawing pair. We layer per-mode
             * overlays via the overlay callback so everything lands
             * in the same frame:
             *   MATCH   → top score banner
             *   SUMMARY → translucent panel + scoreboard
             *   LOBBY   → no world overlay; lobby UI runs in a
             *             separate pair afterward (the lobby is
             *             chrome-only, the world frame underneath
             *             is mostly empty).
             * Doing two Begin/End pairs per frame double-presents
             * and reads as flicker (M4 bug — see CURRENT_STATE), so
             * we only do it for LOBBY where the underlying frame
             * doesn't matter.
             *
             * Cursor: track the scripted aim point projected to
             * screen so the rendered reticle visibly follows what
             * the player is aiming at. Defaults to screen center
             * when no aim is set. */
            Vec2 cursor;
            if (aim_set_n) {
                Vector2 sc = GetWorldToScreen2D(
                    (Vector2){ aim_world_x_n, aim_world_y_n },
                    rd_n.camera);
                cursor = (Vec2){ sc.x, sc.y };
            } else {
                cursor = (Vec2){ (float)s.window_w * 0.5f,
                                 (float)s.window_h * 0.5f };
            }
            ShotOverlayCtx octx = { .game = &game, .ui = &ui_n };
            RendererOverlayFn overlay = NULL;
            if (game.mode == MODE_MATCH)   overlay = match_overlay_draw_thunk;
            if (game.mode == MODE_SUMMARY) overlay = summary_overlay_draw_thunk;
            int wnd_w = GetScreenWidth(), wnd_h = GetScreenHeight();
            int int_w = wnd_w, int_h = wnd_h;
            if (s.internal_h > 0 && s.internal_h < wnd_h) {
                int_h = s.internal_h;
                int_w = (wnd_w * int_h + wnd_h / 2) / wnd_h;
            }
            renderer_draw_frame(&rd_n, &game.world,
                                int_w, int_h,
                                wnd_w, wnd_h,
                                0.0f, (Vec2){0.0f, 0.0f},
                                cursor, overlay, &octx);
            if (game.mode == MODE_LOBBY) {
                BeginDrawing();
                lobby_screen_run(&ui_n, &game,
                                 GetScreenWidth(), GetScreenHeight());
                EndDrawing();
            }

            /* Capture screenshots scheduled at this tick. */
            bool grabbed = false;
            Image shot = {0};
            for (int j = 0; j < s.event_count; ++j) {
                if (s.events[j].tick != tick) continue;
                if (s.events[j].kind != EV_SHOT) continue;
                if (!grabbed) { shot = LoadImageFromScreen(); grabbed = true; }
                char path[512];
                snprintf(path, sizeof(path), "%s/%s.png", s.out_dir, s.events[j].name);
                if (ExportImage(shot, path)) {
                    LOG_I("shotmode: tick %d → %s (mode=%s, mech=%d)",
                          tick, path,
                          game.mode == MODE_LOBBY   ? "LOBBY" :
                          game.mode == MODE_MATCH   ? "MATCH" :
                          game.mode == MODE_SUMMARY ? "SUMMARY" : "?",
                          game.world.local_mech_id);
                    shots_taken_n++;
                } else {
                    LOG_E("shotmode: ExportImage failed for %s", path);
                }
            }
            if (grabbed) UnloadImage(shot);
        }

        LOG_I("shotmode: networked done. ticks=%llu shots=%d (mode=%s, role=%s)",
              (unsigned long long)game.world.tick, shots_taken_n,
              game.mode == MODE_LOBBY ? "LOBBY" :
              game.mode == MODE_MATCH ? "MATCH" :
              game.mode == MODE_SUMMARY ? "SUMMARY" : "?",
              game.net.role == NET_ROLE_SERVER ? "host" :
              game.net.role == NET_ROLE_CLIENT ? "client" : "offline");
        if (s.make_contact && shots_taken_n > 0) {
            build_contact_sheet(&s);
        }
        net_close(&game.net);
        net_shutdown();
        decal_shutdown();
        audio_shutdown();
        weapons_atlas_unload();
        mech_sprites_unload_all();
        map_kit_unload();
        renderer_decorations_unload();
        hud_atlas_unload();
        renderer_post_shutdown();
        platform_shutdown();
        game_shutdown(&game);
        free(s.events);
        free(s.lerps);
        g_shot_mode = 0;
        g_shot_perf_overlay = 0;
        return EXIT_SUCCESS;
    }

    seed_world(&game, &s);

    /* Optional one-shot teleport — moves every body particle by the
     * delta from current pelvis to the requested spawn point so the
     * skeleton arrives at the new location intact and at rest. */
    if (s.have_spawn_at && game.world.local_mech_id >= 0) {
        Mech *pm = &game.world.mechs[game.world.local_mech_id];
        ParticlePool *pp = &game.world.particles;
        int b = pm->particle_base;
        float cur_x = pp->pos_x[b + PART_PELVIS];
        float cur_y = pp->pos_y[b + PART_PELVIS];
        float dx = s.spawn_x - cur_x;
        float dy = s.spawn_y - cur_y;
        for (int i = 0; i < PART_COUNT; ++i) {
            physics_translate_kinematic(pp, b + i, dx, dy);
        }
        game.world.camera_target = (Vec2){ s.spawn_x, s.spawn_y };
        game.world.camera_smooth = (Vec2){ s.spawn_x, s.spawn_y };
        LOG_I("shotmode: spawn_at %.1f %.1f", s.spawn_x, s.spawn_y);
    }

    if (s.no_parallax) {
        g_map_kit.parallax_far.id  = 0;
        g_map_kit.parallax_mid.id  = 0;
        g_map_kit.parallax_near.id = 0;
        LOG_I("shotmode: no_parallax — parallax layers disabled");
    }

    Renderer rd;
    renderer_init(&rd,
        GetScreenWidth(), GetScreenHeight(),
        mech_chest_pos(&game.world, game.world.local_mech_id));
    /* Pin camera smoothing to the simulation rate. In shot mode each
     * draw is wall-clock-fast (~1 ms), so the default GetFrameTime()
     * path would barely advance the camera per tick and the player
     * would visibly run off-screen. */
    rd.cam_dt_override = 1.0f / 60.0f;

    /* Persistent input state across ticks — held buttons, current aim. */
    uint16_t held_buttons = 0;
    AimMode  aim_mode = s.initial_aim_mode;

    /* World-space aim target (used when aim_mode == AIM_WORLD). */
    float aim_world_x = 0.0f, aim_world_y = 0.0f;

    /* Screen-space cursor (used when aim_mode == AIM_SCREEN); converted
     * to world each tick via the live camera, exactly like main.c. */
    float mouse_x = (float)s.window_w * 0.5f, mouse_y = (float)s.window_h * 0.5f;

    if (s.initial_aim_mode == AIM_WORLD) {
        aim_world_x = s.initial_ax; aim_world_y = s.initial_ay;
    } else if (s.initial_aim_mode == AIM_SCREEN) {
        mouse_x = s.initial_ax; mouse_y = s.initial_ay;
    } else {
        /* No aim/mouse specified: aim slightly to the right of the
         * player so the mech faces and renders predictably. */
        Vec2 cp = mech_chest_pos(&game.world, game.world.local_mech_id);
        aim_world_x = cp.x + 200.0f;
        aim_world_y = cp.y;
        aim_mode = AIM_WORLD;
    }

    const float TICK_DT = 1.0f / 60.0f;
    int ev_idx = 0;
    bool ended = false;
    int shots_taken = 0;

    for (int tick = 0; tick <= s.end_tick && !ended; ++tick) {
        if (WindowShouldClose()) { LOG_W("shotmode: window closed early"); break; }

        /* (1) Apply press/release/aim/mouse events scheduled for this tick. */
        while (ev_idx < s.event_count && s.events[ev_idx].tick == tick) {
            const Event *ev = &s.events[ev_idx];
            switch (ev->kind) {
            case EV_PRESS:   held_buttons |= ev->button; break;
            case EV_RELEASE: held_buttons &= (uint16_t)~ev->button; break;
            case EV_AIM:
                aim_world_x = ev->ax; aim_world_y = ev->ay;
                aim_mode = AIM_WORLD;
                break;
            case EV_MOUSE:
                mouse_x = ev->ax; mouse_y = ev->ay;
                aim_mode = AIM_SCREEN;
                break;
            case EV_SHOT:    /* handled after render, see below */         break;
            case EV_END:     ended = true; break;
            case EV_TAP:     break; /* lowered at parse time */
            case EV_GIVE_INVIS: {
                int lid = game.world.local_mech_id;
                if (lid >= 0 && lid < game.world.mech_count) {
                    game.world.mechs[lid].powerup_invis_remaining = ev->ax;
                    SHOT_LOG("t=%d give_invis local_mech=%d secs=%.2f",
                             tick, lid, (double)ev->ax);
                }
                break;
            }
            case EV_FLAG_CARRY: {
                int lid = game.world.local_mech_id;
                int fidx = (int)ev->ax;
                if (lid >= 0 && lid < game.world.mech_count &&
                    fidx >= 0 && fidx < game.world.flag_count) {
                    game.world.flags[fidx].status       = FLAG_CARRIED;
                    game.world.flags[fidx].carrier_mech = (int8_t)lid;
                    game.world.flag_state_dirty         = true;
                    SHOT_LOG("t=%d flag_carry flag=%d local_mech=%d",
                             tick, fidx, lid);
                }
                break;
            }
            case EV_TEAM_CHANGE: {
                /* Single-player path doesn't have a lobby slot table —
                 * just adjust the local mech's team directly. */
                int team = (int)ev->ax;
                int lid = game.world.local_mech_id;
                if (lid >= 0 && lid < game.world.mech_count) {
                    game.world.mechs[lid].team = team;
                    LOG_I("shot: team_change → %d (local mech=%d)", team, lid);
                }
                break;
            }
            case EV_KILL_PEER: {
                int mid = (int)ev->ax;
                int shooter = (int)ev->ay;
                if (mid >= 0 && mid < game.world.mech_count &&
                    game.world.mechs[mid].alive) {
                    mech_apply_damage(&game.world, mid, PART_CHEST,
                                      9999.0f, (Vec2){0.0f, -1.0f},
                                      shooter);
                    LOG_I("shot: kill_peer mech=%d shooter=%d (single-player)",
                          mid, shooter);
                }
                break;
            }
            case EV_DAMAGE: {
                /* M6 P04 — single-process color-tier driver. Same
                 * mech_apply_damage call the multiplayer host-side
                 * branch makes; the FX_DAMAGE_NUMBER spawn fires from
                 * the canonical seam in mech.c so this exercises the
                 * actual feature path, not a synthetic shortcut. */
                int mid    = (int)ev->ax;
                int part   = (int)ev->ay;
                int amount = ev->az;
                if (mid >= 0 && mid < game.world.mech_count &&
                    part >= 0 && part < PART_COUNT &&
                    game.world.mechs[mid].alive) {
                    mech_apply_damage(&game.world, mid, part,
                                      (float)amount,
                                      (Vec2){0.0f, -1.0f},
                                      /*shooter=*/-1);
                    LOG_I("shot: damage mech=%d part=%d amount=%d (single-player)",
                          mid, part, amount);
                }
                break;
            }
            case EV_ARM_CARRY: {
                int fidx = (int)ev->ax;
                int mid  = (int)ev->ay;
                if (fidx >= 0 && fidx < game.world.flag_count &&
                    mid  >= 0 && mid  < game.world.mech_count) {
                    game.world.flags[fidx].status         = FLAG_CARRIED;
                    game.world.flags[fidx].carrier_mech   = (int8_t)mid;
                    game.world.flag_state_dirty           = true;
                    LOG_I("shot: arm_carry flag=%d mech=%d (single-player)",
                          fidx, mid);
                }
                break;
            }
            case EV_KICK:
            case EV_BAN:
            case EV_KICK_MODAL:
            case EV_BAN_MODAL:
            case EV_VOTE_MAP:
            case EV_BOT_TEAM:
            case EV_ADD_BOT:
                /* Single-player has no peers / lobby vote state. */
                break;
            }
            ev_idx++;
        }

        /* (1b) Apply any active mouse_lerp segment. Sweeps the cursor in
         *      screen space — switches us to AIM_SCREEN mode and forces a
         *      camera-relative conversion each tick, matching real play. */
        for (int li = 0; li < s.lerp_count; ++li) {
            LerpSeg *L = &s.lerps[li];
            if (tick < L->t0 || tick > L->t1) continue;
            if (!L->snapshotted) {
                L->from_x = mouse_x;
                L->from_y = mouse_y;
                L->snapshotted = true;
            }
            float u = (float)(tick - L->t0) / (float)(L->t1 - L->t0);
            mouse_x = L->from_x + (L->to_x - L->from_x) * u;
            mouse_y = L->from_y + (L->to_y - L->from_y) * u;
            aim_mode = AIM_SCREEN;
        }

        /* (2) Build ClientInput for this tick. We mirror main.c's
         *     contract: ClientInput.aim_x/y are WORLD-SPACE coords —
         *     the client (us) converts cursor screen→world via the live
         *     camera before handing the input to simulate. Putting
         *     screen coords here would land in mech.aim_world (which
         *     mech_step_drive latches from the input) and the firing
         *     ray would aim at e.g. (1100, 360) treated as world,
         *     not the world point under the cursor. */
        Vec2 world_aim;
        if (aim_mode == AIM_SCREEN) {
            world_aim = renderer_screen_to_world(&rd, (Vec2){ mouse_x, mouse_y });
        } else {
            world_aim = (Vec2){ aim_world_x, aim_world_y };
        }

        ClientInput in = {
            .buttons = held_buttons,
            .seq = (uint16_t)(game.world.tick + 1),
            .aim_x = world_aim.x,
            .aim_y = world_aim.y,
            .dt = TICK_DT,
        };
        if (game.world.local_mech_id >= 0) {
            game.world.mechs[game.world.local_mech_id].aim_world = world_aim;
        }

        simulate(&game.world, in, TICK_DT);
        /* P07 — CTF tick on the single-player path. ctf_step is a
         * no-op when match.mode != CTF or flag_count == 0. The
         * dirty bit is cleared here without broadcasting (no client
         * to inform in single-player). */
        ctf_step(&game, TICK_DT);
        /* Mid-round respawn — same gating as main.c. Pass NULL lobby
         * so the helper uses mech.team directly (no slot mapping in
         * single-player shot mode). */
        match_process_respawns(&game.world, &game.match, NULL);
        game.world.flag_state_dirty = false;
        game.input = in;

        /* (3) Render the tick we just simulated. We render every tick so
         *     camera smoothing, FX particles, and decals all evolve as
         *     they would in interactive play. */
        /* Draw the cursor overlay at the screen-space position when in
         * AIM_SCREEN mode (so the crosshair tracks the mouse_lerp /
         * mouse directives), otherwise center it. The aim/firing ray
         * has already been computed from world_aim above. */
        Vec2 cursor_screen = (aim_mode == AIM_SCREEN)
            ? (Vec2){ mouse_x, mouse_y }
            : (Vec2){ (float)s.window_w * 0.5f, (float)s.window_h * 0.5f };
        int wnd_w = GetScreenWidth(), wnd_h = GetScreenHeight();
        int int_w = wnd_w, int_h = wnd_h;
        if (s.internal_h > 0 && s.internal_h < wnd_h) {
            int_h = s.internal_h;
            int_w = (wnd_w * int_h + wnd_h / 2) / wnd_h;
        }
        renderer_draw_frame(&rd, &game.world,
                            int_w, int_h,
                            wnd_w, wnd_h,
                            /*alpha*/ 0.0f,
                            /*local_visual_offset*/ (Vec2){0.0f, 0.0f},
                            cursor_screen,
                            /*overlay*/ NULL, NULL);

        /* (4) Any 'shot' events scheduled for this tick fire after the
         *     draw — TakeScreenshot grabs the framebuffer that was just
         *     presented. We rewind ev_idx briefly to find them. */
        /* TakeScreenshot strips directories and writes to the working
         * dir; LoadImageFromScreen + ExportImage honor full paths. */
        bool grabbed = false;
        Image shot = {0};
        for (int j = 0; j < s.event_count; ++j) {
            if (s.events[j].tick != tick) continue;
            if (s.events[j].kind != EV_SHOT) continue;
            if (!grabbed) { shot = LoadImageFromScreen(); grabbed = true; }
            char path[512];
            snprintf(path, sizeof(path), "%s/%s.png", s.out_dir, s.events[j].name);
            if (ExportImage(shot, path)) {
                LOG_I("shotmode: tick %d → %s", tick, path);
                shots_taken++;
            } else {
                LOG_E("shotmode: ExportImage failed for %s", path);
            }
        }
        if (grabbed) UnloadImage(shot);
    }

    LOG_I("shotmode: done. ticks=%llu shots=%d",
          (unsigned long long)game.world.tick, shots_taken);

    if (s.make_contact && shots_taken > 0) {
        build_contact_sheet(&s);
    }

    decal_shutdown();
    audio_shutdown();
    weapons_atlas_unload();
    mech_sprites_unload_all();
    map_kit_unload();
    renderer_decorations_unload();
    hud_atlas_unload();
    renderer_post_shutdown();
    platform_shutdown();
    game_shutdown(&game);
    free(s.events);
    free(s.lerps);
    g_shot_mode = 0;
    g_shot_perf_overlay = 0;
    return EXIT_SUCCESS;
}
