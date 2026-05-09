#include "config.h"
#include "ctf.h"
#include "decal.h"
#include "game.h"
#include "level.h"
#include "level_io.h"
#include "lobby.h"
#include "lobby_ui.h"
#include "log.h"
#include "maps.h"
#include "match.h"
#include "mech.h"
#include "mech_sprites.h"
#include "net.h"
#include "pickup.h"
#include "platform.h"
#include "reconcile.h"
#include "render.h"
#include "shotmode.h"
#include "simulate.h"
#include "snapshot.h"
#include "version.h"
#include "weapons.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/*
 * Entry point.
 *
 * M4 game flow:
 *   ./soldut                      title screen → user picks
 *   ./soldut --host [PORT]        skip title; host server, sit in lobby
 *   ./soldut --connect HOST[:P]   skip title; connect to server, sit in lobby
 *   ./soldut --shot scripts/x     scripted scene → PNGs (legacy)
 *
 * The simulation step is the same in every mode; the wrapping state
 * machine in this file decides whether we're showing the title screen,
 * a server browser, the lobby, an active round, or a summary.
 */

#define SIM_HZ        60
static const double TICK_DT = 1.0 / (double)SIM_HZ;
#define MAX_FRAME_DT  0.25

typedef enum {
    LAUNCH_OFFLINE = 0,
    LAUNCH_HOST,
    LAUNCH_CLIENT,
} LaunchMode;

typedef struct {
    LaunchMode  mode;
    uint16_t    port;
    char        host[64];
    char        name[24];
    char        chassis[16];
    char        primary[24];
    char        secondary[24];
    char        armor[16];
    char        jetpack[16];
    char        test_play_lvl[256];   /* M5 P04 — editor F5 hands us a .lvl path */
    char        mode_override[8];     /* M5 P07 — --mode <ffa|tdm|ctf>, overrides
                                       * the test-play META auto-detect. Empty
                                       * means "use the auto-detect path". */
    bool        friendly_fire;
    bool        skip_title;
    bool        ff_set;
} LaunchArgs;

static void parse_args(int argc, char **argv, LaunchArgs *out) {
    memset(out, 0, sizeof *out);
    out->port = SOLDUT_DEFAULT_PORT;
    snprintf(out->name, sizeof out->name, "player");
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0) {
            out->mode = LAUNCH_HOST;
            out->skip_title = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                out->port = (uint16_t)atoi(argv[++i]);
                if (out->port == 0) out->port = SOLDUT_DEFAULT_PORT;
            }
        }
        else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            out->mode = LAUNCH_CLIENT;
            out->skip_title = true;
            const char *s = argv[++i];
            const char *colon = strrchr(s, ':');
            if (colon) {
                size_t hl = (size_t)(colon - s);
                if (hl >= sizeof out->host) hl = sizeof out->host - 1;
                memcpy(out->host, s, hl); out->host[hl] = '\0';
                int port = atoi(colon + 1);
                out->port = (uint16_t)(port > 0 ? port : SOLDUT_DEFAULT_PORT);
            } else {
                snprintf(out->host, sizeof out->host, "%s", s);
                out->port = SOLDUT_DEFAULT_PORT;
            }
        }
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            snprintf(out->name, sizeof out->name, "%s", argv[++i]);
        }
        /* M3 loadout flags — pre-fill the lobby loadout for the local
         * mech; the lobby UI still lets the user retoss. */
        else if (strcmp(argv[i], "--chassis") == 0 && i + 1 < argc) {
            snprintf(out->chassis, sizeof out->chassis, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--primary") == 0 && i + 1 < argc) {
            snprintf(out->primary, sizeof out->primary, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--secondary") == 0 && i + 1 < argc) {
            snprintf(out->secondary, sizeof out->secondary, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--armor") == 0 && i + 1 < argc) {
            snprintf(out->armor, sizeof out->armor, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--jetpack") == 0 && i + 1 < argc) {
            snprintf(out->jetpack, sizeof out->jetpack, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--ff") == 0) {
            out->friendly_fire = true; out->ff_set = true;
        }
        /* M5 P04 — editor F5 test-play. Boots into an offline-solo round
         * on the supplied .lvl file. We force LAUNCH_HOST + skip_title;
         * the main flow detects test_play_lvl[0] and switches to offline
         * + FFA + 1 s auto-start. */
        else if (strcmp(argv[i], "--test-play") == 0 && i + 1 < argc) {
            snprintf(out->test_play_lvl, sizeof out->test_play_lvl, "%s", argv[++i]);
            out->mode = LAUNCH_HOST;
            out->skip_title = true;
        }
        /* M5 P07 — explicit match-mode override for test-play. The
         * editor's loadout modal forwards this so a designer can stress
         * the same .lvl in FFA / TDM / CTF without round-tripping
         * through META edits. Applies AFTER the META auto-detect runs,
         * so an explicit pick wins over the .lvl's mode_mask hint. */
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            snprintf(out->mode_override, sizeof out->mode_override, "%s", argv[++i]);
        }
    }
}

static int resolve_weapon_id(const char *name, int default_id) {
    if (!name || !*name) return default_id;
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
    LOG_W("resolve_weapon_id: unknown '%s' — defaulting", name);
    return default_id;
}

static int resolve_armor_id(const char *name, int default_id) {
    if (!name || !*name) return default_id;
    for (int i = 0; i < 8; ++i) {
        const Armor *a = armor_def(i);
        if (!a || !a->name) continue;
        if (strcasecmp(name, a->name) == 0) return i;
    }
    return default_id;
}

static int resolve_jetpack_id(const char *name, int default_id) {
    if (!name || !*name) return default_id;
    for (int i = 0; i < 8; ++i) {
        const Jetpack *j = jetpack_def(i);
        if (!j || !j->name) continue;
        if (strcasecmp(name, j->name) == 0) return i;
    }
    return default_id;
}

/* ---- Match flow controller (host-side) --------------------------- */

/* Walk newly-recorded killfeed entries and credit the lobby slots. We
 * track how many we've consumed so subsequent ticks pick up only the
 * new ones. */
static int g_killfeed_processed  = 0;
static int g_hitfeed_processed   = 0;
static int g_firefeed_processed  = 0;
static int g_pickupfeed_processed = 0;

/* Server: walk new hit events from the world's hitfeed queue and
 * broadcast each one to clients so they can spawn matching blood/spark
 * FX (without this, the client falls back to chest-pos blood from
 * snapshot health-decrease which renders visibly different). */
static void broadcast_new_hits(Game *g) {
    if (g->net.role != NET_ROLE_SERVER) return;
    int cur = g->world.hitfeed_count;
    int begin = g_hitfeed_processed;
    if (cur - begin > HITFEED_CAPACITY) {
        /* Fell behind by more than the ring; skip the lost ones. */
        begin = cur - HITFEED_CAPACITY;
    }
    for (int n = begin; n < cur; ++n) {
        int idx = n % HITFEED_CAPACITY;
        if (idx < 0) idx += HITFEED_CAPACITY;
        const HitFeedEntry *h = &g->world.hitfeed[idx];
        net_server_broadcast_hit(&g->net,
            (int)h->victim_mech_id, (int)h->hit_part,
            h->pos_x, h->pos_y, h->dir_x, h->dir_y, (int)h->damage);
    }
    g_hitfeed_processed = cur;
}

/* Server: walk new fire events and broadcast so clients can spawn
 * matching tracer (hitscan) or visual-only projectile (everything
 * else). Without this, remote players' shots are invisible on the
 * client — only the local shooter's predict path puts FX on screen. */
static void broadcast_new_fires(Game *g) {
    if (g->net.role != NET_ROLE_SERVER) return;
    int cur = g->world.firefeed_count;
    int begin = g_firefeed_processed;
    if (cur - begin > FIREFEED_CAPACITY) {
        begin = cur - FIREFEED_CAPACITY;
    }
    for (int n = begin; n < cur; ++n) {
        int idx = n % FIREFEED_CAPACITY;
        if (idx < 0) idx += FIREFEED_CAPACITY;
        const FireFeedEntry *f = &g->world.firefeed[idx];
        net_server_broadcast_fire(&g->net,
            (int)f->shooter_mech_id, (int)f->weapon_id,
            f->origin_x, f->origin_y, f->dir_x, f->dir_y);
    }
    g_firefeed_processed = cur;
}

/* Server: ship CTF flag state when ctf transitions have flipped the
 * dirty bit. Single broadcast per tick at most — coalesces same-tick
 * pickup+capture pairs. ctf operations themselves don't broadcast;
 * keeping that here means ctf.c stays independent of net.c. */
static void broadcast_flag_state_if_dirty(Game *g) {
    if (g->net.role != NET_ROLE_SERVER) return;
    if (!g->world.flag_state_dirty) return;
    net_server_broadcast_flag_state(&g->net, &g->world);
    g->world.flag_state_dirty = false;
}

/* Server: drain pickup-state events queued by pickup_step /
 * pickup_spawn_transient. Each event ships the full spawner record
 * (20 bytes) so clients can both mirror state transitions on level-
 * defined entries and learn about transient spawners (engineer
 * repair packs). Same monotonic-counter ring pattern as
 * broadcast_new_hits / broadcast_new_fires. */
static void broadcast_new_pickups(Game *g) {
    if (g->net.role != NET_ROLE_SERVER) return;
    int cur = g->world.pickupfeed_count;
    int begin = g_pickupfeed_processed;
    if (cur - begin > PICKUPFEED_CAPACITY) {
        begin = cur - PICKUPFEED_CAPACITY;
    }
    for (int n = begin; n < cur; ++n) {
        int idx = n % PICKUPFEED_CAPACITY;
        if (idx < 0) idx += PICKUPFEED_CAPACITY;
        int spawner_id = g->world.pickupfeed[idx];
        if (spawner_id < 0 || spawner_id >= g->world.pickups.count) continue;
        const PickupSpawner *s = &g->world.pickups.items[spawner_id];
        net_server_broadcast_pickup_state(&g->net, spawner_id, s);
    }
    g_pickupfeed_processed = cur;
}

static void apply_new_kills(Game *g) {
    /* killfeed_count is a monotonic counter; killfeed[] is a CAP-sized
     * ring. If we've fallen behind by more than the ring capacity, we
     * silently skip the missed kills (they're scoreboard only). */
    int cur = g->world.killfeed_count;
    int begin = g_killfeed_processed;
    if (cur - begin > KILLFEED_CAPACITY) {
        begin = cur - KILLFEED_CAPACITY;
    }
    bool any = false;
    for (int n = begin; n < cur; ++n) {
        int idx = n % KILLFEED_CAPACITY;
        if (idx < 0) idx += KILLFEED_CAPACITY;
        const KillFeedEntry *k = &g->world.killfeed[idx];
        int killer_slot = (k->killer_mech_id >= 0)
            ? lobby_find_slot_by_mech(&g->lobby, k->killer_mech_id) : -1;
        int victim_slot = lobby_find_slot_by_mech(&g->lobby, k->victim_mech_id);
        match_apply_kill(&g->match, &g->lobby, killer_slot, victim_slot, k->flags);
        net_server_broadcast_kill(&g->net, k->killer_mech_id,
                                   k->victim_mech_id, k->weapon_id);
        any = true;
    }
    g_killfeed_processed = cur;
    /* Slot scores changed → reship the lobby table on the next net_poll
     * so client scoreboards stay current and the summary MVP is right. */
    if (any) g->lobby.dirty = true;
}

static void start_round(Game *g) {
    /* For the FIRST round, derive map+mode from the rotation table
     * (match_init left them at defaults). Subsequent rounds get
     * map/mode from begin_next_lobby — which honors the P09 map-vote
     * winner. Re-deriving here would clobber the vote winner with
     * the rotation default, which was the user-reported "voted a
     * map but the next round didn't start cleanly" symptom. */
    if (g->round_counter == 0) {
        g->match.map_id = config_pick_map (&g->config, 0);
        g->match.mode   = config_pick_mode(&g->config, 0);
    }

    /* P07 — CTF mode-mask validation. If the rotation lands on CTF and
     * the picked map's META.mode_mask doesn't allow CTF (or the map
     * doesn't carry a Red+Blue flag pair), demote the round to TDM
     * rather than skip silently — that way the players still get a
     * round on the same map; CTF resumes when the rotation hits a
     * compatible map. We can't read mode_mask before building the
     * level (it's in the .lvl META), so we build first, then validate,
     * then rebuild. The double build is cheap (small maps) and only
     * happens when CTF is selected. */
    if (g->match.mode == MATCH_MODE_CTF && !g->test_play_lvl[0]) {
        arena_reset(&g->level_arena);
        map_build((MapId)g->match.map_id, &g->world, &g->level_arena);
        bool mode_ok = (g->world.level.meta.mode_mask & (1u << MATCH_MODE_CTF)) != 0;
        bool flags_ok = (g->world.level.flag_count == 2 && g->world.level.flags);
        if (!mode_ok || !flags_ok) {
            LOG_W("start_round: map %s doesn't support CTF (mode_mask=0x%x flag_count=%d) "
                  "— demoting to TDM",
                  map_def(g->match.map_id)->short_name,
                  (unsigned)g->world.level.meta.mode_mask,
                  g->world.level.flag_count);
            g->match.mode = MATCH_MODE_TDM;
        }
    }

    /* Re-derive limits from config to account for mode-rotation. */
    g->match.score_limit  = g->config.score_limit;
    g->match.time_limit   = g->config.time_limit;
    g->match.friendly_fire= g->config.friendly_fire;
    /* P07 — CTF default score limit is 5 captures (per the design
     * canon), much smaller than the FFA default of 25 kills. If the
     * config's score_limit looks like the FFA default (>= 25), assume
     * the host hasn't customized for CTF and clamp to FLAG_CAPTURE_DEFAULT.
     * A host who explicitly sets score_limit=10 in soldut.cfg keeps 10. */
    if (g->match.mode == MATCH_MODE_CTF && g->match.score_limit >= 25) {
        g->match.score_limit = FLAG_CAPTURE_DEFAULT;
    }
    /* FFA mode: every player is on team 1 (MATCH_TEAM_FFA aliases
     * MATCH_TEAM_RED). The friendly-fire check in mech_apply_damage
     * compares teams and drops same-team hits when ff is off — so
     * with FFA + ff=off, NO damage ever lands (every kill request
     * gets dropped because shooter and victim are same-team). Force
     * ff on for FFA so hits register; ff toggle remains meaningful
     * for TDM/CTF where teams are distinct. */
    g->world.friendly_fire= g->config.friendly_fire ||
                            (g->match.mode == MATCH_MODE_FFA);

    /* Build map. M5 P04: in test-play mode, ignore the rotation and
     * reload the scratch .lvl so successive rounds keep using the
     * editor's map. The CTF validation above may have already built
     * the level; rebuild unconditionally here so the slate is clean
     * (arena reset wipes the previous build's allocations). */
    arena_reset(&g->level_arena);
    if (g->test_play_lvl[0]) {
        map_build_from_path(&g->world, &g->level_arena, g->test_play_lvl);
    } else {
        map_build((MapId)g->match.map_id, &g->world, &g->level_arena);
    }
    decal_init((int)level_width_px(&g->world.level),
               (int)level_height_px(&g->world.level));

    /* P08 — refresh the host's serve descriptor so INITIAL_STATE +
     * map-share streaming reflect the round's chosen map, and push
     * the new descriptor to all peers so any client that doesn't have
     * the file kicks off a download immediately. */
    {
        const char *short_name = g->test_play_lvl[0]
                               ? NULL
                               : map_def(g->match.map_id)->short_name;
        const char *serve_in   = g->test_play_lvl[0] ? g->test_play_lvl : NULL;
        maps_refresh_serve_info(short_name, serve_in,
                                &g->server_map_desc,
                                g->server_map_serve_path,
                                sizeof(g->server_map_serve_path));
        if (g->net.role == NET_ROLE_SERVER) {
            net_server_broadcast_map_descriptor(&g->net, &g->server_map_desc);
        }
    }

    /* P07 — TDM/CTF team auto-balance. In FFA the lobby_add_slot
     * default `team = MATCH_TEAM_FFA` (= 1 = RED) is correct. In
     * TDM/CTF that puts everyone on RED, which makes CTF never trigger
     * a touch transition (same-team mech walking through their own
     * flag = no-op) AND breaks combat (same-team friendly_fire = off
     * by default in CTF). Force a deterministic split: in-use slots go
     * RED / BLUE / RED / BLUE / ... by slot index, skipping spectators
     * (team == NONE). Players who change team via UI between rounds
     * keep their choice — but if the mode flipped between rounds, the
     * old assignment may not be valid (e.g., FFA default wins
     * everyone), so we respect EXPLICIT RED/BLUE choices and
     * reassign anything else.
     *
     * **MUST run before lobby_spawn_round_mechs** — that helper reads
     * `slot.team` and bakes it into `mech.team`, which is what
     * mech_apply_damage's friendly-fire check uses. Running balance
     * AFTER the spawn means the mechs were already created on RED
     * and shooting any other player gets dropped as same-team. */
    if (g->match.mode == MATCH_MODE_TDM || g->match.mode == MATCH_MODE_CTF) {
        int red = 0, blue = 0;
        /* First pass: count slots that already have an explicit
         * RED/BLUE choice, leaving them alone. */
        for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
            LobbySlot *s = &g->lobby.slots[i];
            if (!s->in_use) continue;
            if (s->team == MATCH_TEAM_RED)  red++;
            else if (s->team == MATCH_TEAM_BLUE) blue++;
        }
        /* Second pass: assign anyone who isn't on RED/BLUE/NONE to the
         * smaller team (slot index breaks ties).
         *
         * Note: MATCH_TEAM_FFA aliases MATCH_TEAM_RED, so slots whose
         * team is the FFA-default would already be counted as RED in
         * the first pass. To break the everyone-on-RED problem, we do
         * a third equal-out pass below. */
        for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
            LobbySlot *s = &g->lobby.slots[i];
            if (!s->in_use) continue;
            if (s->team == MATCH_TEAM_NONE) continue;
            if (s->team == MATCH_TEAM_RED || s->team == MATCH_TEAM_BLUE) continue;
            int target = (red <= blue) ? MATCH_TEAM_RED : MATCH_TEAM_BLUE;
            s->team = target;
            if (target == MATCH_TEAM_RED) red++; else blue++;
        }
        /* Third pass: equal-out. If RED has 2+ more than BLUE, walk
         * the slots in reverse and reassign until balanced. Reverse so
         * slot 0 (the host) stays put. The auto-balance is reset only
         * when |red - blue| > 1; explicit player picks (which set the
         * team explicitly) survive single-step imbalance. */
        while (red - blue >= 2) {
            for (int i = MAX_LOBBY_SLOTS - 1; i >= 0; --i) {
                LobbySlot *s = &g->lobby.slots[i];
                if (!s->in_use) continue;
                if (s->team != MATCH_TEAM_RED) continue;
                s->team = MATCH_TEAM_BLUE;
                red--; blue++;
                break;
            }
            if (red - blue < 2) break;
        }
        while (blue - red >= 2) {
            for (int i = MAX_LOBBY_SLOTS - 1; i >= 0; --i) {
                LobbySlot *s = &g->lobby.slots[i];
                if (!s->in_use) continue;
                if (s->team != MATCH_TEAM_BLUE) continue;
                s->team = MATCH_TEAM_RED;
                blue--; red++;
                break;
            }
            if (blue - red < 2) break;
        }
        LOG_I("match: %s team auto-balance → red=%d blue=%d",
              match_mode_name(g->match.mode), red, blue);
        g->lobby.dirty = true;
    }

    /* Spawn mechs for every active slot. lobby_spawn_round_mechs sets
     * each slot's mech_id and reads slot.team to set mech.team; mark
     * the table dirty so the next net_poll iteration broadcasts the
     * updated mapping to clients. The client uses slot.mech_id to
     * resolve its own world.local_mech_id — without this it stays at
     * -1, the camera doesn't follow, and the client renders a black
     * screen. (Auto-balance above MUST have happened first or every
     * mech ends up on the FFA-default RED team.) */
    lobby_spawn_round_mechs(&g->lobby, &g->world,
                            g->match.map_id, g->local_slot_id,
                            g->match.mode);
    g->lobby.dirty = true;

    /* P05 — populate spawner pool from the level's PICK records and
     * spawn any practice-dummy mechs. Server-side; clients call the
     * same function from client_handle_round_start so the pool indexes
     * line up. Subsequent transient spawns (engineer repair packs)
     * propagate via NET_MSG_PICKUP_STATE. Reset feed cursors so the
     * new round's events index from zero. */
    pickup_init_round(&g->world);
    g->world.pickupfeed_count = 0;
    g_pickupfeed_processed = 0;

    /* P07 — populate flags[] from level.flags (only when mode == CTF;
     * ctf_init_round zeroes flag_count for other modes). Mirror the
     * mode onto World so mech.c can branch (mech_kill → drop). The
     * client runs the same path from client_handle_round_start. */
    g->world.match_mode_cached = (int)g->match.mode;
    ctf_init_round(&g->world, g->match.mode);
    g->world.flag_state_dirty = false;

    match_begin_round(&g->match);
    g->mode = MODE_MATCH;
    g_killfeed_processed = g->world.killfeed_count;

    if (g->net.role == NET_ROLE_SERVER) {
        /* Order: ship the lobby table first so clients have mech_id
         * mappings *before* ROUND_START and the snapshot stream. Both
         * are reliable+ordered on NET_CH_LOBBY. */
        net_server_broadcast_lobby_list  (&g->net, &g->lobby);
        g->lobby.dirty = false;
        net_server_broadcast_round_start (&g->net, &g->match);
        /* P07 — initial flag state. Both sides ran ctf_init_round so
         * home_pos is locally available; this broadcast is mostly a
         * sanity rebroadcast (status=HOME / carrier=-1) but covers
         * any odd race + matches the pickup pattern. No-op when not
         * in CTF mode (flag_count == 0). */
        if (g->world.flag_count > 0) {
            net_server_broadcast_flag_state(&g->net, &g->world);
        }
    }
    LOG_I("match_flow: round %d begin (mode=%s map=%s)",
          g->round_counter,
          match_mode_name(g->match.mode),
          map_def(g->match.map_id)->display_name);
}

/* P09 — pick three distinct candidates from g_map_registry that support
 * the current match mode (excluding the just-played map) and arm a vote.
 * No-op for offline solo or when the registry doesn't have enough
 * mode-compatible alternatives. Clients see vote state via the
 * NET_MSG_LOBBY_VOTE_STATE broadcast. */
static void host_start_map_vote(Game *g) {
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
    /* Fisher-Yates shuffle so the 3 cards vary across rounds. */
    for (int i = n - 1; i > 0; --i) {
        int j = (int)pcg32_range(g->world.rng, 0, i + 1);
        int tmp = candidates[i];
        candidates[i] = candidates[j];
        candidates[j] = tmp;
    }
    int a = candidates[0];
    int b = (n >= 2) ? candidates[1] : -1;
    int c = (n >= 3) ? candidates[2] : -1;
    /* Vote window must end before begin_next_lobby fires. The summary
     * timer (`g->match.summary_remaining`, set by match_end_round) is
     * the bound; clamp to 80% so the winner-pick at expiry doesn't race
     * with the phase transition. */
    float dur = (g->match.summary_remaining > 0.0f)
                ? g->match.summary_remaining * 0.8f : 12.0f;
    lobby_vote_start(&g->lobby, a, b, c, dur);
    if (g->net.role == NET_ROLE_SERVER) {
        net_server_broadcast_vote_state(&g->net, &g->lobby);
    }
    LOG_I("match_flow: map vote: %s / %s / %s (%.1fs)",
          map_def(a) ? map_def(a)->display_name : "—",
          (b >= 0 && map_def(b)) ? map_def(b)->display_name : "—",
          (c >= 0 && map_def(c)) ? map_def(c)->display_name : "—",
          (double)dur);
}

static void end_round(Game *g) {
    match_end_round(&g->match, &g->lobby);
    g->mode = MODE_SUMMARY;
    /* Snapshots stop flowing now (gated in net_poll on
     * MATCH_PHASE_ACTIVE), so corpses freeze nicely.
     *
     * Order matters here: ship the lobby table first (carries final
     * per-slot scores) so the round-end broadcast that follows lands
     * with the client already holding accurate score data — it can
     * compute MVP locally if mvp_slot didn't ride the wire. Both are
     * reliable+ordered on NET_CH_LOBBY. */
    if (g->net.role == NET_ROLE_SERVER) {
        net_server_broadcast_lobby_list(&g->net, &g->lobby);
        g->lobby.dirty = false;
        net_server_broadcast_round_end (&g->net, &g->match);
    }
    /* P09 — arm the three-card vote for the next map. Server-side only:
     * the broadcast inside the helper informs clients. */
    host_start_map_vote(g);
    LOG_I("match_flow: round %d end (mvp=%d)", g->round_counter, g->match.mvp_slot);
}

/* Apply the just-finished SUMMARY's map-vote winner to match.map_id
 * and clear the vote state. Both the inter-round and end-match paths
 * call this so the next round (or the lobby's pre-staged map) reflects
 * whatever the players picked. */
static void apply_vote_winner_if_any(Game *g) {
    bool any_candidate = (g->lobby.vote_map_a >= 0 ||
                          g->lobby.vote_map_b >= 0 ||
                          g->lobby.vote_map_c >= 0);
    if (!any_candidate) return;
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

/* End the match: full reset back to LOBBY for a fresh ready-up. Called
 * when rounds_played reaches rounds_per_match. Score, kills, deaths,
 * ready flags are wiped — the next match starts from zero. */
static void end_match(Game *g) {
    LOG_I("match_flow: match over (%d rounds played) — back to lobby",
          g->match.rounds_played);
    g->match.rounds_played = 0;
    g->match.phase         = MATCH_PHASE_LOBBY;
    g->match.score_limit   = g->config.score_limit;
    g->match.time_limit    = g->config.time_limit;
    g->match.friendly_fire = g->config.friendly_fire;
    /* Tear down mechs; lobby UI draws over a clean world. */
    lobby_clear_round_mechs(&g->lobby, &g->world);
    /* Reset round stats + ready flags so players READY UP for the
     * next match (no auto-arm here — wait for ready, that's the
     * design rule per the user's "ready up again for a new game"). */
    lobby_reset_round_stats(&g->lobby);
    g->mode = MODE_LOBBY;
    if (g->net.role == NET_ROLE_SERVER) {
        net_server_broadcast_match_state(&g->net, &g->match);
        net_server_broadcast_lobby_list (&g->net, &g->lobby);
    }
}

/* Inter-round transition: the just-finished round wasn't the last in
 * the match, so we go SUMMARY → COUNTDOWN(brief) → ACTIVE without
 * showing the lobby. Score persists. Mech-spawn happens at the
 * COUNTDOWN→ACTIVE edge via start_round. */
static void advance_to_next_round(Game *g) {
    /* Tear down dead mechs from the previous round. start_round will
     * re-spawn fresh ones. */
    lobby_clear_round_mechs(&g->lobby, &g->world);
    /* Brief inter-round countdown (default 3 s) so players see "Round
     * X starts in 3..." instead of an instant snap into the new round. */
    match_begin_countdown(&g->match, g->match.inter_round_countdown_default);
    if (g->net.role == NET_ROLE_SERVER) {
        net_server_broadcast_match_state(&g->net, &g->match);
        /* lobby_list ships current scores; keep the summary scoreboard
         * accurate while the countdown ticks. */
        net_server_broadcast_lobby_list (&g->net, &g->lobby);
    }
}

/* SUMMARY → next-phase dispatcher. Decides whether to run another
 * round seamlessly or end the match and return to lobby. Called when
 * the SUMMARY timer expires (or when all players have voted, see the
 * fast-forward path in host_match_flow_step). */
static void after_summary(Game *g) {
    g->round_counter++;
    g->match.rounds_played++;
    /* Default next map/mode from rotation; vote winner overrides. */
    g->match.map_id = config_pick_map (&g->config, g->round_counter);
    g->match.mode   = config_pick_mode(&g->config, g->round_counter);
    apply_vote_winner_if_any(g);

    if (g->match.rounds_played >= g->match.rounds_per_match) {
        end_match(g);
    } else {
        LOG_I("match_flow: round %d/%d — continuing match",
              g->match.rounds_played + 1, g->match.rounds_per_match);
        advance_to_next_round(g);
    }
}

/* Track whether the host's countdown was active last tick so we can
 * detect arm/cancel transitions and ship them to clients immediately,
 * AND throttle a periodic refresh while it's live. The client decays
 * its own local copy each frame for smooth display between broadcasts. */
static bool  g_prev_auto_start_active = false;
static float g_countdown_broadcast_accum = 0.0f;

static void host_broadcast_countdown_if_changed(Game *g, float dt) {
    if (g->net.role != NET_ROLE_SERVER) return;
    bool now_active = g->lobby.auto_start_active;
    bool transition = (now_active != g_prev_auto_start_active);
    g_prev_auto_start_active = now_active;

    if (transition) {
        /* Ship the new state right away so the client UI updates
         * without waiting for the periodic refresh. */
        net_server_broadcast_countdown(&g->net,
            now_active ? g->lobby.auto_start_remaining : 0.0f,
            /*reason*/ now_active ? 1u : 0u);
        g_countdown_broadcast_accum = 0.0f;
        return;
    }
    if (now_active) {
        /* Refresh every 0.5 s. With the client decaying locally, this
         * keeps drift to <0.5 s without flooding the wire. */
        g_countdown_broadcast_accum += dt;
        if (g_countdown_broadcast_accum >= 0.5f) {
            g_countdown_broadcast_accum -= 0.5f;
            net_server_broadcast_countdown(&g->net,
                g->lobby.auto_start_remaining, /*reason*/ 1u);
        }
    }
}

static void host_match_flow_step(Game *g, float dt) {
    switch (g->match.phase) {
        case MATCH_PHASE_LOBBY: {
            /* All-ready accelerator. The auto-start countdown may
             * already be running with the long default (60 s); when
             * everyone hits Ready, override it to a short 3 s so the
             * round actually starts soon — otherwise players see "all
             * ready ✓" but nothing happens. Only override down (don't
             * extend a shorter timer). */
            bool all_ready = lobby_all_ready(&g->lobby);
            if (all_ready) {
                if (!g->lobby.auto_start_active) {
                    lobby_auto_start_arm(&g->lobby, 3.0f);
                } else if (g->lobby.auto_start_remaining > 3.0f) {
                    g->lobby.auto_start_remaining = 3.0f;
                    g->lobby.dirty = true;
                }
            }
            /* P08 — hold the countdown at >=1.5s when peers are still
             * downloading the current map. Code-built maps (descriptor
             * crc=0) bypass the gate entirely — there's nothing to
             * download. Slow clients still finalize MAP_READY through
             * the chunk stream while the timer holds. */
            uint32_t cur_map_crc = g->server_map_desc.crc32;
            bool map_gate_ok = (cur_map_crc == 0) ||
                               (g->net.role != NET_ROLE_SERVER) ||
                               net_server_all_peers_map_ready(&g->net, cur_map_crc);
            if (!map_gate_ok && g->lobby.auto_start_active &&
                g->lobby.auto_start_remaining < 1.5f) {
                g->lobby.auto_start_remaining = 1.5f;
                g->lobby.dirty = true;
            }
            if (lobby_tick(&g->lobby, dt)) {
                if (!map_gate_ok) {
                    /* Defensive — re-arm in case the hold above raced. */
                    lobby_auto_start_arm(&g->lobby, 1.5f);
                } else {
                    /* Auto-start fired → enter countdown. test-play uses
                     * a 1 s countdown (set in match.countdown_default at
                     * startup) so designers' F5 round-trip stays short. */
                    float secs = g->test_play_lvl[0]
                                     ? g->match.countdown_default : 5.0f;
                    match_begin_countdown(&g->match, secs);
                    if (g->net.role == NET_ROLE_SERVER) {
                        net_server_broadcast_match_state(&g->net, &g->match);
                    }
                }
            }
            host_broadcast_countdown_if_changed(g, dt);
            lobby_chat_age(&g->lobby, dt);
            break;
        }
        case MATCH_PHASE_COUNTDOWN:
            if (match_tick(&g->match, dt)) {
                start_round(g);
            }
            break;
        case MATCH_PHASE_ACTIVE: {
            /* P07 — CTF tick (auto-return + touch detection + capture).
             * Runs before kills are applied so a kill that happens to
             * coincide with a flag touch sees the post-touch state.
             * No-op outside CTF rounds. */
            ctf_step(g, dt);
            apply_new_kills(g);
            broadcast_new_hits(g);
            broadcast_new_fires(g);
            broadcast_new_pickups(g);
            broadcast_flag_state_if_dirty(g);
            /* End on score limit (FFA = any per-player slot >= cap). */
            bool end = false;
            if (g->match.mode == MATCH_MODE_FFA) {
                for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
                    if (g->lobby.slots[i].in_use &&
                        g->lobby.slots[i].score >= g->match.score_limit)
                    {
                        end = true; break;
                    }
                }
            }
            if (!end && match_round_should_end(&g->match)) end = true;
            if (match_tick(&g->match, dt)) end = true;
            /* "Only one alive" → 3s countdown → end. Catches kill,
             * kick, disconnect; covers the "I killed the opponent
             * but the round didn't end" complaint. */
            if (!end && match_step_solo_warning(&g->match, &g->world, dt))
                end = true;
            if (end) end_round(g);
            break;
        }
        case MATCH_PHASE_SUMMARY: {
            /* Tick the lobby too so the map-vote countdown decays
             * during summary. */
            (void)lobby_tick(&g->lobby, dt);

            /* Fast-forward when every active in-use slot has voted.
             * Drops `summary_remaining` to a small floor (1 s) so the
             * banner reads briefly, then lets match_tick fire the
             * transition. Without this, a quick 2-player vote would
             * still wait the full ~4–8 s summary window. */
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
                after_summary(g);
            }
            break;
        }
    }
}

/* ---- Bootstrap helpers ------------------------------------------- */

static void apply_loadout_flags(Game *g, const LaunchArgs *args) {
    if (g->local_slot_id < 0) return;
    LobbySlot *me = lobby_slot(&g->lobby, g->local_slot_id);
    if (!me) return;
    MechLoadout lo = me->loadout;
    if (args->chassis[0]) lo.chassis_id = chassis_id_from_name(args->chassis);
    lo.primary_id   = resolve_weapon_id  (args->primary,   lo.primary_id);
    lo.secondary_id = resolve_weapon_id  (args->secondary, lo.secondary_id);
    lo.armor_id     = resolve_armor_id   (args->armor,     lo.armor_id);
    lo.jetpack_id   = resolve_jetpack_id (args->jetpack,   lo.jetpack_id);
    lobby_set_loadout(&g->lobby, g->local_slot_id, lo);
}

/* For host & offline solo: stand up a server (no listening for offline)
 * and seat the local player in slot 0. */
static bool bootstrap_host(Game *g, const LaunchArgs *args, bool offline) {
    if (!offline) {
        if (!net_init()) return false;
        if (!net_server_start(&g->net, g->config.port, g)) {
            LOG_E("host: failed to bind UDP %u", (unsigned)g->config.port);
            return false;
        }
        net_discovery_open(&g->net);
        LOG_I("host: ready on port %u", (unsigned)g->config.port);
    } else {
        g->net.role = NET_ROLE_OFFLINE;
        g->offline_solo = true;
    }

    /* P09 — load any persisted bans before we start accepting peers.
     * Records the path on lobby state so subsequent lobby_ban_addr
     * calls (from the host-controls UI / kick wire path) auto-save. */
    lobby_load_bans(&g->lobby, "bans.txt");

    /* Host's own slot — slot 0, peer_id = -1, is_host = true. */
    int slot = lobby_add_slot(&g->lobby, /*peer_id*/-1,
                              args ? args->name : "player", true);
    g->local_slot_id = slot;
    g->world.authoritative = true;

    apply_loadout_flags(g, args);

    /* Pre-build the level so the first-time lobby has *something* to
     * draw under the UI. ROUND_START rebuilds it for the chosen map.
     *
     * M5 P04 — when the editor's F5 forks us with --test-play, load the
     * scratch .lvl directly; subsequent start_round calls also pick this
     * path up via g->test_play_lvl. */
    if (g->test_play_lvl[0]) {
        map_build_from_path(&g->world, &g->level_arena, g->test_play_lvl);
    } else {
        map_build(MAP_FOUNDRY, &g->world, &g->level_arena);
    }
    decal_init((int)level_width_px(&g->world.level),
               (int)level_height_px(&g->world.level));

    /* P08 — fill in the serve descriptor for the pre-round lobby map
     * so connecting clients learn what we have on disk. start_round
     * refreshes it for the actual round map. */
    {
        const char *short_name = g->test_play_lvl[0]
                               ? NULL : map_def(MAP_FOUNDRY)->short_name;
        const char *serve_in   = g->test_play_lvl[0] ? g->test_play_lvl : NULL;
        maps_refresh_serve_info(short_name, serve_in,
                                &g->server_map_desc,
                                g->server_map_serve_path,
                                sizeof(g->server_map_serve_path));
    }
    return true;
}

/* Pure client: connect, sit in lobby until ROUND_START. */
static bool bootstrap_client(Game *g, const LaunchArgs *args) {
    if (!net_init()) return false;
    if (!net_client_connect(&g->net, args->host, args->port,
                            args->name, g))
    {
        LOG_E("client: connect to %s:%u failed", args->host, (unsigned)args->port);
        return false;
    }
    /* Initial state has been applied in net.c — local_slot_id is set,
     * mode is MODE_LOBBY. */
    g->world.authoritative = false;
    return true;
}

/* ---- Diag overlay ------------------------------------------------- */

/* Frame context the overlay callbacks need. raylib's overlay-callback
 * signature gives us one void* — we pack Game + LobbyUIState into this
 * struct so both the match HUD overlay and the summary panel can run
 * inside renderer_draw_frame's single Begin/EndDrawing pair. */
typedef struct {
    Game         *game;
    LobbyUIState *ui;
} OverlayCtx;

static void draw_diag(void *user, int sw, int sh) {
    OverlayCtx *ctx = (OverlayCtx *)user;
    Game *g = ctx->game;
    int fps = GetFPS();
    Color st = (fps >= 55) ? GREEN : (fps >= 30) ? YELLOW : RED;
    DrawText("soldut " SOLDUT_VERSION_STRING, 12, 10, 18, RAYWHITE);
    DrawText(TextFormat("FPS %d", fps), 12, 32, 18, st);
    DrawText(TextFormat("tick %llu  mechs %d  parts %d  phase=%s",
                        (unsigned long long)g->world.tick,
                        g->world.mech_count,
                        g->world.particles.count,
                        match_phase_name(g->match.phase)),
             12, 52, 14, LIGHTGRAY);

    const char *role = "offline";
    Color rc = GRAY;
    if (g->net.role == NET_ROLE_SERVER) { role = "host";   rc = GREEN; }
    if (g->net.role == NET_ROLE_CLIENT) { role = "client"; rc = SKYBLUE; }

    NetStats st_n; net_get_stats(&g->net, &st_n);
    DrawText(TextFormat("%s peers=%d rtt=%ums tx=%uKB rx=%uKB",
                        role, st_n.peer_count, st_n.rtt_ms_max,
                        st_n.bytes_sent / 1024u, st_n.bytes_recv / 1024u),
             12, 70, 14, rc);

    DrawText("WASD: move/jet  SPACE: jump  LMB: fire  ESC: leave",
             12, sh - 22, 14, GRAY);

    /* Match score/timer banner. Drawn here (inside the renderer's
     * single Begin/EndDrawing pair) instead of in a second pair —
     * doing two swaps per frame produces a per-other-frame "blank +
     * banner only" present, which reads as flicker. */
    match_overlay_draw(g, sw, sh);
}

/* Summary overlay: draws the round-summary panel on top of the frozen
 * world frame inside the renderer's single Begin/EndDrawing pair (same
 * flicker logic as draw_diag). */
static void draw_summary_overlay(void *user, int sw, int sh) {
    OverlayCtx *ctx = (OverlayCtx *)user;
    summary_screen_run(ctx->ui, ctx->game, sw, sh);
}

/* ---- Main --------------------------------------------------------- */

int main(int argc, char **argv) {
    log_init("soldut.log");
    LOG_I("soldut " SOLDUT_VERSION_STRING " starting");

    /* Shot mode early-exit. */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            int rc = shotmode_run(argv[i + 1]);
            log_shutdown();
            return rc;
        }
    }

    LaunchArgs args;
    parse_args(argc, argv, &args);

    Game game;
    if (!game_init(&game)) { log_shutdown(); return EXIT_FAILURE; }
    reconcile_init(&game.reconcile);

    /* Server config — load file, then let CLI flags override. */
    config_load(&game.config, "soldut.cfg");
    if (args.mode == LAUNCH_HOST && args.port != 0) game.config.port = args.port;
    if (args.ff_set) game.config.friendly_fire = args.friendly_fire;
    /* M5 P04 — editor test-play overrides match config: FFA, 60 s round,
     * 1 s auto-start, no networking. Stash the .lvl path on Game so
     * bootstrap_host + start_round can find it. */
    if (args.test_play_lvl[0]) {
        snprintf(game.test_play_lvl, sizeof game.test_play_lvl, "%s",
                 args.test_play_lvl);
        /* P07 — peek the .lvl's META.mode_mask + flag_count so F5 from
         * the editor on a CTF-capable map runs as CTF (with 2 flags
         * placed and the META CTF bit set). Without this, every
         * test-play hardcoded FFA and CTF maps couldn't be tested via
         * F5 at all. The peek mutates g->world.level temporarily; the
         * real load happens later in bootstrap_host / start_round. */
        MatchModeId tp_mode = MATCH_MODE_FFA;
        bool        tp_ff   = true;
        int         tp_score= 50;
        {
            LvlResult r = level_load(&game.world, &game.level_arena,
                                      args.test_play_lvl);
            if (r == LVL_OK) {
                uint16_t mm = game.world.level.meta.mode_mask;
                bool have_ctf_pair = (game.world.level.flag_count == 2 &&
                                      game.world.level.flags);
                if ((mm & (1u << MATCH_MODE_CTF)) && have_ctf_pair) {
                    tp_mode = MATCH_MODE_CTF;
                    /* CTF: friendly_fire OFF (same-team can't shoot),
                     * score_limit clamped to FLAG_CAPTURE_DEFAULT (5). */
                    tp_ff    = false;
                    tp_score = FLAG_CAPTURE_DEFAULT;
                    LOG_I("test-play: detected CTF map (mode_mask=0x%x, "
                          "flag_count=%d) — running CTF mode",
                          (unsigned)mm, game.world.level.flag_count);
                } else if ((mm & (1u << MATCH_MODE_TDM)) &&
                           !(mm & (1u << MATCH_MODE_FFA))) {
                    tp_mode = MATCH_MODE_TDM;
                    tp_ff   = false;
                }
            }
            /* Reset the level arena so the post-game-init flow
             * rebuilds cleanly. The peek mutated tiles + arrays in
             * the level arena; bootstrap_host / start_round arena-
             * reset before calling map_build_from_path again. */
            arena_reset(&game.level_arena);
            /* Zero the level state so renderer / map_build don't
             * dereference freed arena pointers. */
            game.world.level = (Level){0};
        }
        /* P07 — explicit --mode override beats the META auto-detect.
         * Applies on top of the tp_* defaults computed above so that
         * forcing CTF on a non-flag map gets the CTF score-limit clamp,
         * and forcing FFA on a CTF map gets friendly-fire back on. */
        if (args.mode_override[0]) {
            const char *mo = args.mode_override;
            if      (strcasecmp(mo, "ffa") == 0) {
                tp_mode = MATCH_MODE_FFA; tp_ff = true;  tp_score = 50;
            }
            else if (strcasecmp(mo, "tdm") == 0) {
                tp_mode = MATCH_MODE_TDM; tp_ff = false; tp_score = 50;
            }
            else if (strcasecmp(mo, "ctf") == 0) {
                tp_mode = MATCH_MODE_CTF; tp_ff = false; tp_score = FLAG_CAPTURE_DEFAULT;
            }
            else {
                LOG_W("test-play: --mode '%s' unknown — keeping auto-detect (%d)",
                      mo, (int)tp_mode);
            }
            LOG_I("test-play: --mode override → mode=%d ff=%d score_limit=%d",
                  (int)tp_mode, (int)tp_ff, tp_score);
        }

        game.config.mode               = tp_mode;
        game.config.mode_rotation[0]   = tp_mode;
        game.config.mode_rotation_count= 1;
        game.config.time_limit         = 60;
        game.config.score_limit        = tp_score;
        game.config.auto_start_seconds = 1;
        game.config.friendly_fire      = tp_ff;
        /* Turn on the SHOT_LOG sink so test-play emits the diagnostic
         * trail shipped via the existing `SHOT_LOG()` macro across the
         * codebase (anim transitions, grounded toggles, hitscan, etc.)
         * plus the per-second pelvis-pos line in this file's MATCH
         * branch. SHOT_LOG is a one-branch no-op when g_shot_mode is
         * 0 — the production code path never sees these messages. */
        g_shot_mode = 1;
    }
    /* Re-apply MatchState defaults from the loaded config. */
    match_init(&game.match, game.config.mode, game.config.score_limit,
               game.config.time_limit, game.config.friendly_fire);
    game.match.rounds_per_match = game.config.rounds_per_match;
    if (args.test_play_lvl[0]) {
        /* Drop the in-match countdown to 1 s as well so a designer's
         * F5 round-trip time stays short (default is 5 s). */
        game.match.countdown_default = 1.0f;
    }
    game.match.map_id = config_pick_map(&game.config, 0);
    game.lobby.auto_start_default = game.config.auto_start_seconds;
    game.world.friendly_fire = game.config.friendly_fire;

    PlatformConfig pcfg = {
        .window_w = 1280, .window_h = 720,
        .vsync = true, .fullscreen = false,
        .title = (args.mode == LAUNCH_HOST)   ? "Soldut " SOLDUT_VERSION_STRING " — host"
               : (args.mode == LAUNCH_CLIENT) ? "Soldut " SOLDUT_VERSION_STRING " — client"
                                              : "Soldut " SOLDUT_VERSION_STRING,
    };
    if (!platform_init(&pcfg)) {
        game_shutdown(&game); log_shutdown(); return EXIT_FAILURE;
    }

    /* M5 P10 — load per-chassis sprite atlases. Missing files log INFO
     * and leave that chassis at atlas.id == 0, which `draw_mech` reads
     * as "fall back to capsule rendering for that chassis". So a fresh
     * checkout without sprite PNGs in `assets/sprites/` still renders
     * cleanly. */
    mech_sprites_load_all();

    LobbyUIState ui = (LobbyUIState){0};
    lobby_ui_init(&ui);
    if (args.name[0]) snprintf(ui.player_name, sizeof ui.player_name, "%s", args.name);

    Renderer rd;
    renderer_init(&rd, GetScreenWidth(), GetScreenHeight(), (Vec2){640, 360});

    /* Initial mode: title (unless CLI shortcut). */
    if (args.skip_title) {
        if (args.mode == LAUNCH_HOST) {
            /* test-play forces offline-solo so we don't bind a UDP port
             * the user's running game might already be using. */
            bool offline = (args.test_play_lvl[0] != 0);
            if (!bootstrap_host(&game, &args, offline)) {
                game_shutdown(&game); log_shutdown(); return EXIT_FAILURE;
            }
            game.mode = MODE_LOBBY;
            if (args.test_play_lvl[0]) {
                /* Arm the round immediately — test-play wants to skip
                 * the lobby UX and drop the player into the map. */
                lobby_auto_start_arm(&game.lobby, 1.0f);
            }
        } else if (args.mode == LAUNCH_CLIENT) {
            if (!bootstrap_client(&game, &args)) {
                game_shutdown(&game); log_shutdown(); return EXIT_FAILURE;
            }
        }
    } else {
        game.mode = MODE_TITLE;
    }

    PlatformFrame pf = {0};
    double accum = 0.0;
    double last  = GetTime();

    while (!WindowShouldClose() && game.mode != MODE_QUIT) {
        double now = GetTime();
        double dt  = now - last;
        last = now;
        if (dt > MAX_FRAME_DT) dt = MAX_FRAME_DT;
        accum += dt;

        platform_begin_frame(&pf);

        /* Pump network FIRST so inbound inputs / snapshots / lobby
         * messages are visible before this frame's sim ticks. */
        if (game.net.role != NET_ROLE_OFFLINE) {
            net_poll(&game.net, &game, dt);
        }

        /* Forced-disconnect detection (kick/ban/timeout). When the
         * server drops a client, ENet emits DISCONNECT on the client
         * which clears `ns->connected`. Without this hook the client
         * just sits in whatever mode it was in (lobby, match, summary)
         * with no server to talk to — so the kick/ban functionality
         * looked broken from the user's perspective. We tear the client
         * state down here and bounce back to title; the kick reason is
         * already in the chat ring (server posted "X was kicked by host"
         * before the disconnect), so the user gets context.
         *
         * Skipped for offline / server roles. */
        if (game.net.role == NET_ROLE_CLIENT && !game.net.connected &&
            game.mode != MODE_TITLE && game.mode != MODE_QUIT)
        {
            LOG_I("client: server connection lost — returning to title");
            net_close(&game.net);
            net_shutdown();
            lobby_clear_round_mechs(&game.lobby, &game.world);
            lobby_init(&game.lobby, game.config.auto_start_seconds);
            game.local_slot_id = -1;
            game.world.local_mech_id = -1;
            game.world.authoritative = false;
            game.offline_solo = false;
            game.mode = MODE_TITLE;
            continue;       /* skip this frame's mode dispatch */
        }

        /* Per-mode update + render. */
        switch (game.mode) {

        case MODE_TITLE: {
            BeginDrawing();
            title_screen_run(&ui, &game, pf.render_w, pf.render_h);
            EndDrawing();
            if (ui.request_quit)             { game.mode = MODE_QUIT; }
            else if (ui.request_single_player) {
                ui.request_single_player = false;
                snprintf(args.name, sizeof args.name, "%s", ui.player_name);
                if (bootstrap_host(&game, &args, /*offline*/true)) {
                    /* Auto-start with a short countdown. */
                    lobby_auto_start_arm(&game.lobby, 1.0f);
                    game.mode = MODE_LOBBY;
                }
            }
            else if (ui.request_host) {
                /* Legacy fast-path (still wired so existing CLI / older
                 * UI paths keep working). The new UI sets MODE_HOST_SETUP
                 * directly from the title button, so this branch is
                 * mainly belt-and-braces. */
                ui.request_host = false;
                snprintf(args.name, sizeof args.name, "%s", ui.player_name);
                if (bootstrap_host(&game, &args, /*offline*/false)) {
                    game.mode = MODE_LOBBY;
                }
            }
            else if (ui.request_browse) {
                ui.request_browse = false;
                /* Open a transient discovery socket so we can scan. */
                if (game.net.role == NET_ROLE_OFFLINE) {
                    net_init();
                    net_discovery_open(&game.net);
                }
                game.mode = MODE_BROWSER;
            }
            break;
        }

        case MODE_HOST_SETUP: {
            BeginDrawing();
            host_setup_screen_run(&ui, &game, pf.render_w, pf.render_h);
            EndDrawing();
            if (ui.request_start_host) {
                ui.request_start_host = false;
                /* Apply the user's choices to the live config so
                 * config_pick_mode/_map and start_round all see them.
                 * Single-entry rotations so subsequent rounds reuse
                 * the same picks (rotation across modes/maps is M5
                 * P17/18 territory; the host-setup screen here is
                 * single-mode for now). */
                game.config.mode               = (MatchModeId)ui.setup_mode;
                game.config.score_limit        = ui.setup_score_limit;
                game.config.time_limit         = (float)ui.setup_time_limit_s;
                game.config.friendly_fire      = ui.setup_friendly_fire;
                game.config.map_rotation[0]    = ui.setup_map_id;
                game.config.map_rotation_count = 1;
                game.config.mode_rotation[0]   = (MatchModeId)ui.setup_mode;
                game.config.mode_rotation_count= 1;

                /* Re-init MatchState from the new config. */
                match_init(&game.match, game.config.mode,
                           game.config.score_limit,
                           game.config.time_limit,
                           game.config.friendly_fire);
                game.match.rounds_per_match = game.config.rounds_per_match;
                game.match.map_id = ui.setup_map_id;
                game.world.friendly_fire = game.config.friendly_fire ||
                                            (game.match.mode == MATCH_MODE_FFA);

                snprintf(args.name, sizeof args.name, "%s", ui.player_name);
                if (bootstrap_host(&game, &args, /*offline*/false)) {
                    game.mode = MODE_LOBBY;
                } else {
                    game.mode = MODE_TITLE;
                }
                ui.setup_initialized = false;
            }
            break;
        }

        case MODE_BROWSER: {
            BeginDrawing();
            browser_screen_run(&ui, &game, pf.render_w, pf.render_h);
            EndDrawing();
            net_poll(&game.net, &game, dt);   /* drain discovery replies */
            if (ui.request_connect) {
                ui.request_connect = false;
                snprintf(args.name, sizeof args.name, "%s", ui.player_name);
                snprintf(args.host, sizeof args.host, "%s", game.pending_host);
                args.port = game.pending_port;
                if (bootstrap_client(&game, &args)) {
                    /* INITIAL_STATE already moved us to MODE_LOBBY. */
                } else {
                    game.mode = MODE_TITLE;
                }
            }
            break;
        }

        case MODE_CONNECT: {
            BeginDrawing();
            connect_screen_run(&ui, &game, pf.render_w, pf.render_h);
            EndDrawing();
            if (ui.request_connect) {
                ui.request_connect = false;
                snprintf(args.name, sizeof args.name, "%s", ui.player_name);
                snprintf(args.host, sizeof args.host, "%s", game.pending_host);
                args.port = game.pending_port;
                if (bootstrap_client(&game, &args)) {
                    /* INITIAL_STATE already moved us to MODE_LOBBY. */
                } else {
                    game.mode = MODE_TITLE;
                }
            }
            break;
        }

        case MODE_LOBBY: {
            /* Host: tick the match flow controller. */
            if (game.net.role == NET_ROLE_SERVER || game.offline_solo) {
                host_match_flow_step(&game, (float)dt);
            }
            /* Client: age chat + locally decay the auto-start countdown
             * so the visible "Match starts in N.Ns" ticks smoothly
             * between the host's broadcasts (which arrive every ~0.5
             * s). The host's COUNTDOWN-phase timer (match.countdown_
             * remaining) is decayed similarly so the client's UI shows
             * a consistent number even before the next match_state
             * broadcast lands. */
            if (game.net.role == NET_ROLE_CLIENT) {
                lobby_chat_age(&game.lobby, (float)dt);
                if (game.lobby.auto_start_active &&
                    game.lobby.auto_start_remaining > 0.0f)
                {
                    game.lobby.auto_start_remaining -= (float)dt;
                    if (game.lobby.auto_start_remaining < 0.0f)
                        game.lobby.auto_start_remaining = 0.0f;
                }
                if (game.match.phase == MATCH_PHASE_COUNTDOWN &&
                    game.match.countdown_remaining > 0.0f)
                {
                    game.match.countdown_remaining -= (float)dt;
                    if (game.match.countdown_remaining < 0.0f)
                        game.match.countdown_remaining = 0.0f;
                }
            }

            BeginDrawing();
            lobby_screen_run(&ui, &game, pf.render_w, pf.render_h);
            EndDrawing();

            /* Honor the lobby UI's "Leave" button. */
            if (game.mode == MODE_TITLE) {
                /* Disconnect / shut down server. */
                if (game.net.role != NET_ROLE_OFFLINE) {
                    net_close(&game.net);
                    net_shutdown();
                }
                lobby_clear_round_mechs(&game.lobby, &game.world);
                memset(&game.lobby, 0, sizeof game.lobby);
                lobby_init(&game.lobby, game.config.auto_start_seconds);
                match_init(&game.match, game.config.mode, game.config.score_limit,
                           game.config.time_limit, game.config.friendly_fire);
                game.match.rounds_per_match = game.config.rounds_per_match;
                game.local_slot_id = -1;
                game.offline_solo  = false;
            }
            break;
        }

        case MODE_MATCH: {
            /* Late-bind world.local_mech_id from our lobby slot. The
             * server ships the lobby table (with each slot's mech_id)
             * just before ROUND_START; clients need to wait until the
             * matching mech actually shows up in the snapshot stream
             * before pointing local_mech_id at it. Without this the
             * camera never follows anyone and the client renders a
             * black screen. */
            if (game.world.local_mech_id < 0 && game.local_slot_id >= 0) {
                int mid = game.lobby.slots[game.local_slot_id].mech_id;
                if (mid >= 0 && mid < game.world.mech_count) {
                    game.world.local_mech_id = mid;
                    LOG_I("client: local_mech_id resolved → %d (slot %d)",
                          mid, game.local_slot_id);
                }
            }

            /* Client: locally decay match.time_remaining so the match
             * overlay's countdown ticks smoothly. The host doesn't
             * broadcast match_state during ACTIVE phase (only on
             * round transitions), so without local decay the client
             * just shows the round-start value frozen for the entire
             * match — diverging from the host's actual remaining
             * time by up to time_limit seconds. */
            if (game.net.role == NET_ROLE_CLIENT &&
                game.match.phase == MATCH_PHASE_ACTIVE &&
                game.match.time_remaining > 0.0f)
            {
                game.match.time_remaining -= (float)dt;
                if (game.match.time_remaining < 0.0f)
                    game.match.time_remaining = 0.0f;
            }

            /* Fixed-step simulation. */
            while (accum >= TICK_DT) {
                ClientInput in;
                platform_sample_input(&in);
                in.dt  = (float)TICK_DT;
                in.seq = (uint16_t)(game.world.tick + 1);

                Vec2 cursor_world = renderer_screen_to_world(&rd,
                    (Vec2){ in.aim_x, in.aim_y });
                in.aim_x = cursor_world.x;
                in.aim_y = cursor_world.y;

                if (game.net.role == NET_ROLE_CLIENT) {
                    if (game.world.local_mech_id >= 0) {
                        game.world.mechs[game.world.local_mech_id].latched_input = in;
                    }
                    simulate_step(&game.world, (float)TICK_DT);
                    /* P03: pull remote mechs back to the interpolated
                     * server position. Runs after physics so it
                     * overrides any drift from stale latched_input
                     * extrapolation. The renderer's prev-frame
                     * snapshot (taken at the top of the next
                     * simulate_step) captures this as the new
                     * "where it was last tick" anchor — yielding
                     * smooth motion across snapshot intervals.
                     *
                     * Advance the clock in double precision —
                     * (uint32_t)(1/60*1000) truncates to 16, drifts
                     * 0.67 ms/tick = 40 ms/s = client renders ~270 ms
                     * behind server after a few seconds. */
                    if (game.net.client_render_clock_armed) {
                        game.net.client_render_time_ms += TICK_DT * 1000.0;
                        /* render_time may be negative early in a
                         * connection (we init it as
                         * `first_snap.server_time - INTERP_DELAY_MS`).
                         * Clamp before casting; snapshot_interp_remotes
                         * treats render_time <= oldest_t as "clamp to
                         * oldest snapshot", which is the right thing
                         * to do when we're conceptually before any
                         * received data. */
                        double rt = game.net.client_render_time_ms;
                        uint32_t rt_u32 = (rt > 0.0) ? (uint32_t)rt : 0u;
                        snapshot_interp_remotes(&game.world, rt_u32);
                    }
                    reconcile_push_input(&game.reconcile, in);
                    net_client_send_input(&game.net, in);
                    reconcile_tick_smoothing(&game.reconcile);
                } else {
                    if (game.world.local_mech_id >= 0) {
                        game.world.mechs[game.world.local_mech_id].latched_input = in;
                    }
                    simulate_step(&game.world, (float)TICK_DT);
                }

                game.input = in;
                accum -= TICK_DT;
            }

            /* Host: drive match flow (kills → scores → end-of-round). */
            if (game.net.role == NET_ROLE_SERVER || game.offline_solo) {
                host_match_flow_step(&game, (float)dt);
            }

            /* Editor F5 test-play: per-second pelvis log so
             * tests/spawn_e2e.sh can verify the mech settled where
             * the .lvl said it should. Gated on SHOT_LOG (== `if
             * (g_shot_mode) ...`), which we toggle on at startup
             * only when --test-play is set — production play paths
             * never hit this branch. */
            if (game.match.phase == MATCH_PHASE_ACTIVE &&
                game.world.tick > 0 && (game.world.tick % 60) == 0)
            {
                int mid = game.world.local_mech_id;
                if (mid >= 0 && mid < game.world.mech_count) {
                    const Mech *m = &game.world.mechs[mid];
                    int pb = m->particle_base + PART_PELVIS;
                    SHOT_LOG("test-play: tick %llu  pelvis (%.1f, %.1f)  grounded=%d",
                             (unsigned long long)game.world.tick,
                             (double)game.world.particles.pos_x[pb],
                             (double)game.world.particles.pos_y[pb],
                             (int)m->grounded);
                }
            }

            /* Render world + HUD + (diag + match banner via draw_diag).
             * One Begin/EndDrawing pair total per frame. The local
             * mech's visual_offset comes from reconcile (smooths out
             * server snaps over ~6 frames); the host has no reconcile
             * state and passes (0,0). */
            OverlayCtx ovc = { .game = &game, .ui = &ui };
            Vec2 visual_off = (game.net.role == NET_ROLE_CLIENT)
                            ? game.reconcile.visual_offset
                            : (Vec2){ 0.0f, 0.0f };
            renderer_draw_frame(&rd, &game.world,
                                pf.render_w, pf.render_h,
                                (float)(accum / TICK_DT),
                                visual_off,
                                (Vec2){ (float)GetMouseX(), (float)GetMouseY() },
                                draw_diag, &ovc);

            if (IsKeyPressed(KEY_ESCAPE)) {
                if (game.net.role == NET_ROLE_SERVER || game.offline_solo) {
                    /* Host: end the round early. */
                    end_round(&game);
                } else {
                    /* Client: drop the connection, return to title. */
                    net_close(&game.net);
                    net_shutdown();
                    game.mode = MODE_TITLE;
                    game.offline_solo = false;
                }
            }
            break;
        }

        case MODE_SUMMARY: {
            /* Host: tick the summary timer; clients receive ROUND_END
             * via net so their match.phase == SUMMARY automatically. */
            if (game.net.role == NET_ROLE_SERVER || game.offline_solo) {
                host_match_flow_step(&game, (float)dt);
            } else if (game.net.role == NET_ROLE_CLIENT) {
                /* Decay the visible summary timer locally. */
                if (game.match.phase == MATCH_PHASE_SUMMARY &&
                    game.match.summary_remaining > 0.0f) {
                    game.match.summary_remaining -= (float)dt;
                    if (game.match.summary_remaining < 0.0f)
                        game.match.summary_remaining = 0.0f;
                }
                /* During the inter-round COUNTDOWN, mode stays SUMMARY
                 * (no lobby in between rounds — see after_summary).
                 * Decay countdown_remaining so the "Round X starts in
                 * N s" banner ticks down between server broadcasts. */
                if (game.match.phase == MATCH_PHASE_COUNTDOWN &&
                    game.match.countdown_remaining > 0.0f) {
                    game.match.countdown_remaining -= (float)dt;
                    if (game.match.countdown_remaining < 0.0f)
                        game.match.countdown_remaining = 0.0f;
                }
            }

            /* Draw world (frozen) + summary overlay in a single
             * Begin/EndDrawing pair via the renderer's overlay
             * callback. Two pairs per frame double-swap and read as
             * flicker. */
            OverlayCtx ovc = { .game = &game, .ui = &ui };
            renderer_draw_frame(&rd, &game.world,
                                pf.render_w, pf.render_h,
                                0.0f, (Vec2){ 0.0f, 0.0f },
                                (Vec2){ (float)GetMouseX(), (float)GetMouseY() },
                                draw_summary_overlay, &ovc);

            if (game.mode == MODE_TITLE) {
                /* User clicked Leave. Tear down. */
                if (game.net.role != NET_ROLE_OFFLINE) {
                    net_close(&game.net);
                    net_shutdown();
                }
                lobby_clear_round_mechs(&game.lobby, &game.world);
                memset(&game.lobby, 0, sizeof game.lobby);
                lobby_init(&game.lobby, game.config.auto_start_seconds);
                match_init(&game.match, game.config.mode, game.config.score_limit,
                           game.config.time_limit, game.config.friendly_fire);
                game.match.rounds_per_match = game.config.rounds_per_match;
                game.local_slot_id = -1;
                game.offline_solo  = false;
            }
            break;
        }

        default:
            BeginDrawing();
            ClearBackground(BLACK);
            EndDrawing();
            break;
        }
    }

    LOG_I("soldut shutting down (ran %llu sim ticks)",
          (unsigned long long)game.world.tick);

    if (game.net.role != NET_ROLE_OFFLINE) {
        net_close(&game.net);
        net_shutdown();
    }

    log_shutdown();
    _exit(EXIT_SUCCESS);
}
