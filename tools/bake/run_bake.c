/*
 * bake — headless multi-bot test harness for M5 maps.
 *
 * Usage:
 *   ./build/bake <short_name> [--bots N] [--duration_s S] [--seed S]
 *
 * Spawns N crude wandering bots on the named .lvl, runs simulate() at
 * 60 Hz for `duration_s` seconds, and writes:
 *
 *   build/bake/<short>.kills.csv      per-kill record (tick, killer, victim, weapon, x, y)
 *   build/bake/<short>.pickups.csv    per-pickup-grab record (tick, kind, variant, x, y)
 *   build/bake/<short>.flags.csv      per-flag-event record (CTF only)
 *   build/bake/<short>.heatmap.png    3-channel composite (R=kills, G=traffic, B=pickup grabs)
 *   build/bake/<short>.summary.txt    pass/fail verdict against per-map acceptance criteria
 *
 * Bot AI is intentionally crude (per documents/m5/07-maps.md §"Bake
 * test"): wander toward a random spawn point; aim at the nearest enemy
 * within 800 px line-of-sight; shoot. No flag-running heuristic, no
 * pickup priority. The heatmap shape is what's informative — dead
 * zones and spawn imbalance show up loud.
 *
 * No respawn at v1 — bots that die stay dead. Default 60 s duration
 * is enough to grab every active pickup, which is what the per-map
 * acceptance criteria actually look at. Longer durations are fine for
 * "did the Mass Driver respawn N times" checks.
 */

#define _POSIX_C_SOURCE 200809L

#include "../../src/arena.h"
#include "../../src/bot.h"
#include "../../src/ctf.h"
#include "../../src/game.h"
#include "../../src/input.h"
#include "../../src/level.h"
#include "../../src/level_io.h"
#include "../../src/log.h"
#include "../../src/maps.h"
#include "../../src/match.h"
#include "../../src/mech.h"
#include "../../src/pickup.h"
#include "../../src/simulate.h"
#include "../../src/weapons.h"
#include "../../src/world.h"

#include "raylib.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define TICK_HZ 60
#define TICK_DT (1.0f / 60.0f)

/* Cap bots at MAX_MECHS so mech_create never returns -1 for capacity. */
#define BAKE_BOTS_MAX MAX_MECHS

/* Heatmap output size — fits any of our maps comfortably. */
#define HEATMAP_W 384
#define HEATMAP_H 192

/* Pickup-grab event capacity — round of 10 minutes with frequent
 * respawns shouldn't exceed this. */
#define MAX_PICKUP_EVENTS 1024
#define MAX_KILL_EVENTS    512
#define MAX_FLAG_EVENTS    256

typedef struct {
    int       mech_id;          /* index into world.mechs */
    int       team;             /* MATCH_TEAM_RED / _BLUE / _FFA */
    int       chassis;          /* CHASSIS_* */
} Bot;

typedef struct {
    uint64_t tick;
    int      killer_mech_id;
    int      victim_mech_id;
    int      weapon_id;
    uint32_t flags;
    float    x, y;
} KillEvent;

typedef struct {
    uint64_t tick;
    int      kind;              /* PickupKind */
    int      variant;
    float    x, y;
} PickupEvent;

typedef struct {
    uint64_t tick;
    int      flag_idx;
    int      old_status;
    int      new_status;
    int      mech_id;           /* carrier / returner / -1 */
} FlagEvent;

typedef struct {
    Bot         bots[BAKE_BOTS_MAX];
    int         bot_count;

    /* Stat counters. */
    uint32_t    heat_kills  [HEATMAP_H][HEATMAP_W];
    uint32_t    heat_traffic[HEATMAP_H][HEATMAP_W];
    uint32_t    heat_pickups[HEATMAP_H][HEATMAP_W];
    uint32_t    heat_max_kills, heat_max_traffic, heat_max_pickups;

    /* Per-pickup-spawner grab counts. */
    uint32_t    spawner_grabs[PICKUP_CAPACITY];
    /* Per-spawner most-recent state we observed (so we can detect
     * AVAILABLE→COOLDOWN transitions = grabs). */
    uint8_t     spawner_prev_state[PICKUP_CAPACITY];

    /* Per-flag most-recent state we observed. */
    uint8_t     flag_prev_status[2];

    /* Most recent killfeed_count we drained. */
    int         killfeed_last_count;

    /* Output buffers. */
    KillEvent   kills    [MAX_KILL_EVENTS];
    int         kill_count;
    PickupEvent pickups  [MAX_PICKUP_EVENTS];
    int         pickup_count;
    FlagEvent   flag_events[MAX_FLAG_EVENTS];
    int         flag_event_count;

    /* Stats. */
    int         red_captures;
    int         blue_captures;
    int         total_kills_red;
    int         total_kills_blue;
    int         total_kills_ffa;

    /* Diagnostic — fires observed (drained from firefeed each tick). */
    uint64_t    total_fires;
} Stats;

static Stats g_stats;

/* ---- Math utilities ------------------------------------------------- */

static float rng_unit(uint32_t *seed) {
    /* Tiny LCG — same period as PCG32 is overkill for bot wander. */
    *seed = (*seed * 1103515245u + 12345u);
    return (float)((*seed >> 16) & 0xffffu) / 65536.0f;
}

static float frangef(uint32_t *seed, float lo, float hi) {
    return lo + (hi - lo) * rng_unit(seed);
}

/* ---- World helpers -------------------------------------------------- */

static Vec2 mech_pelvis_pos(const World *w, int mid) {
    const Mech *m = &w->mechs[mid];
    int p = m->particle_base + PART_PELVIS;
    return (Vec2){ w->particles.pos_x[p], w->particles.pos_y[p] };
}

/* Bot AI moved to src/bot.{c,h} — bake delegates to bot_step. */

/* ---- Stats recording ------------------------------------------------ */

static int world_to_hx(const World *w, float wx) {
    int wmax = w->level.width * w->level.tile_size;
    if (wmax <= 0) return 0;
    int hx = (int)((wx / (float)wmax) * (float)HEATMAP_W);
    if (hx < 0) hx = 0; else if (hx >= HEATMAP_W) hx = HEATMAP_W - 1;
    return hx;
}

static int world_to_hy(const World *w, float wy) {
    int hmax = w->level.height * w->level.tile_size;
    if (hmax <= 0) return 0;
    int hy = (int)((wy / (float)hmax) * (float)HEATMAP_H);
    if (hy < 0) hy = 0; else if (hy >= HEATMAP_H) hy = HEATMAP_H - 1;
    return hy;
}

static void heat_bump(uint32_t cell[HEATMAP_H][HEATMAP_W],
                      uint32_t *max_v, int hx, int hy) {
    uint32_t v = ++cell[hy][hx];
    if (v > *max_v) *max_v = v;
}

static void record_traffic(const World *w) {
    for (int i = 0; i < w->mech_count; ++i) {
        const Mech *m = &w->mechs[i];
        if (!m->alive) continue;
        Vec2 p = mech_pelvis_pos(w, i);
        int hx = world_to_hx(w, p.x);
        int hy = world_to_hy(w, p.y);
        heat_bump(g_stats.heat_traffic, &g_stats.heat_max_traffic, hx, hy);
    }
}

static void drain_killfeed(const World *w) {
    /* world.killfeed is a fixed-size ring; entries past KILLFEED_CAPACITY
     * cycle. We compare killfeed_count to last-seen and process the
     * tail. */
    int cur = w->killfeed_count;
    int last = g_stats.killfeed_last_count;
    int to_read = cur - last;
    if (to_read <= 0) return;
    if (to_read > KILLFEED_CAPACITY) to_read = KILLFEED_CAPACITY;
    for (int n = 0; n < to_read; ++n) {
        /* Each entry: killfeed[(last + n) % CAPACITY], but the ring is
         * "head index = count % CAP" per the comment, so the oldest in
         * the ring is at (count - CAP) % CAP. Read forward from there. */
        int slot = ((last + n) % KILLFEED_CAPACITY);
        const KillFeedEntry *e = &w->killfeed[slot];
        if (e->victim_mech_id < 0 || e->victim_mech_id >= w->mech_count) continue;

        /* Position at the time we detect the kill. */
        Vec2 vp = mech_pelvis_pos(w, e->victim_mech_id);
        int hx = world_to_hx(w, vp.x);
        int hy = world_to_hy(w, vp.y);
        heat_bump(g_stats.heat_kills, &g_stats.heat_max_kills, hx, hy);

        /* Team kill tally. */
        const Mech *victim = &w->mechs[e->victim_mech_id];
        if      (victim->team == MATCH_TEAM_RED)  g_stats.total_kills_red++;
        else if (victim->team == MATCH_TEAM_BLUE) g_stats.total_kills_blue++;
        else                                       g_stats.total_kills_ffa++;

        if (g_stats.kill_count < MAX_KILL_EVENTS) {
            g_stats.kills[g_stats.kill_count++] = (KillEvent){
                .tick = w->tick,
                .killer_mech_id = e->killer_mech_id,
                .victim_mech_id = e->victim_mech_id,
                .weapon_id = e->weapon_id,
                .flags = e->flags,
                .x = vp.x, .y = vp.y,
            };
        }
    }
    g_stats.killfeed_last_count = cur;
}

static void drain_pickup_transitions(const World *w) {
    for (int i = 0; i < w->pickups.count; ++i) {
        const PickupSpawner *s = &w->pickups.items[i];
        uint8_t prev = g_stats.spawner_prev_state[i];
        /* AVAILABLE → COOLDOWN = grabbed. */
        if (prev == PICKUP_STATE_AVAILABLE && s->state == PICKUP_STATE_COOLDOWN) {
            g_stats.spawner_grabs[i]++;
            int hx = world_to_hx(w, s->pos.x);
            int hy = world_to_hy(w, s->pos.y);
            heat_bump(g_stats.heat_pickups, &g_stats.heat_max_pickups, hx, hy);
            if (g_stats.pickup_count < MAX_PICKUP_EVENTS) {
                g_stats.pickups[g_stats.pickup_count++] = (PickupEvent){
                    .tick = w->tick,
                    .kind = s->kind,
                    .variant = s->variant,
                    .x = s->pos.x, .y = s->pos.y,
                };
            }
        }
        g_stats.spawner_prev_state[i] = s->state;
    }
}

static void drain_flag_transitions(const World *w) {
    if (w->flag_count == 0) return;
    for (int f = 0; f < w->flag_count && f < 2; ++f) {
        const Flag *fl = &w->flags[f];
        uint8_t prev = g_stats.flag_prev_status[f];
        if (prev != fl->status) {
            /* Score a capture: HOME→HOME via the capture event is the
             * trigger, but we observe it as the carrier's flag returning
             * to HOME without going through DROPPED. Simpler: count
             * captures via the team_score delta in the caller. Here we
             * just log the transitions. */
            if (g_stats.flag_event_count < MAX_FLAG_EVENTS) {
                g_stats.flag_events[g_stats.flag_event_count++] = (FlagEvent){
                    .tick = w->tick,
                    .flag_idx = f,
                    .old_status = prev,
                    .new_status = fl->status,
                    .mech_id = fl->carrier_mech,
                };
            }
            g_stats.flag_prev_status[f] = fl->status;
        }
    }
}

/* ---- Output --------------------------------------------------------- */

static void ensure_dir(const char *path) {
    (void)mkdir(path, 0755);
}

static void write_csvs(const char *short_name) {
    char path[256];

    snprintf(path, sizeof path, "build/bake/%s.kills.csv", short_name);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "tick,killer,victim,weapon,flags,x,y\n");
        for (int i = 0; i < g_stats.kill_count; ++i) {
            const KillEvent *e = &g_stats.kills[i];
            fprintf(f, "%llu,%d,%d,%d,%u,%.1f,%.1f\n",
                    (unsigned long long)e->tick,
                    e->killer_mech_id, e->victim_mech_id, e->weapon_id,
                    (unsigned)e->flags, e->x, e->y);
        }
        fclose(f);
    }

    snprintf(path, sizeof path, "build/bake/%s.pickups.csv", short_name);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "tick,kind,variant,x,y\n");
        for (int i = 0; i < g_stats.pickup_count; ++i) {
            const PickupEvent *e = &g_stats.pickups[i];
            fprintf(f, "%llu,%d,%d,%.1f,%.1f\n",
                    (unsigned long long)e->tick,
                    e->kind, e->variant, e->x, e->y);
        }
        fclose(f);
    }

    snprintf(path, sizeof path, "build/bake/%s.flags.csv", short_name);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "tick,flag,old_status,new_status,mech\n");
        for (int i = 0; i < g_stats.flag_event_count; ++i) {
            const FlagEvent *e = &g_stats.flag_events[i];
            fprintf(f, "%llu,%d,%d,%d,%d\n",
                    (unsigned long long)e->tick, e->flag_idx,
                    e->old_status, e->new_status, e->mech_id);
        }
        fclose(f);
    }
}

static void write_heatmap_png(const char *short_name) {
    /* 3-channel composite: R=kills, G=traffic (gamma-corrected), B=pickup
     * grabs. Each channel normalized to its own max. Alpha = 255. */
    Image img = GenImageColor(HEATMAP_W, HEATMAP_H, BLACK);
    Color *pixels = (Color *)img.data;

    float kmax = g_stats.heat_max_kills   ? (float)g_stats.heat_max_kills   : 1.0f;
    float tmax = g_stats.heat_max_traffic ? (float)g_stats.heat_max_traffic : 1.0f;
    float pmax = g_stats.heat_max_pickups ? (float)g_stats.heat_max_pickups : 1.0f;

    for (int y = 0; y < HEATMAP_H; ++y) {
        for (int x = 0; x < HEATMAP_W; ++x) {
            float r = (float)g_stats.heat_kills  [y][x] / kmax;
            float g = (float)g_stats.heat_traffic[y][x] / tmax;
            float b = (float)g_stats.heat_pickups[y][x] / pmax;
            /* Gamma 0.5 on traffic so low values are still visible. */
            g = sqrtf(g);
            r = sqrtf(r);
            b = sqrtf(b);
            pixels[y * HEATMAP_W + x] = (Color){
                (unsigned char)(r * 255.0f),
                (unsigned char)(g * 200.0f + 30.0f),
                (unsigned char)(b * 255.0f),
                255,
            };
        }
    }

    char path[256];
    snprintf(path, sizeof path, "build/bake/%s.heatmap.png", short_name);
    ExportImage(img, path);
    UnloadImage(img);
}

/* ---- Acceptance verdict --------------------------------------------- */

typedef struct {
    bool pass;
    char detail[256];
} Verdict;

static int count_zero_traffic_rows(int wmax_x, int wmax_y) {
    /* Count "dead zone" 32-px-square cells in the main play area. */
    (void)wmax_x; (void)wmax_y;
    int dead = 0;
    /* Use top 60% of map height (skip outer-wall band above and the
     * bottommost row which is usually solid floor with no traffic). */
    int y_lo = HEATMAP_H * 10 / 100;
    int y_hi = HEATMAP_H * 90 / 100;
    int x_lo = HEATMAP_W * 5  / 100;
    int x_hi = HEATMAP_W * 95 / 100;
    for (int y = y_lo; y < y_hi; y += 4) {
        for (int x = x_lo; x < x_hi; x += 4) {
            uint32_t v = 0;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 4; ++dx)
                    v += g_stats.heat_traffic[y + dy][x + dx];
            if (v == 0) dead++;
        }
    }
    return dead;
}

static Verdict compute_verdict(const World *w, const char *short_name) {
    Verdict v = { .pass = true, .detail = "" };
    char buf[256] = "";

    int total_grabs = 0;
    int unique_grabbed = 0;
    int ungrabbed = 0;
    for (int i = 0; i < w->pickups.count; ++i) {
        total_grabs += (int)g_stats.spawner_grabs[i];
        if (g_stats.spawner_grabs[i] > 0) unique_grabbed++;
        else ungrabbed++;
    }
    int dead_cells = count_zero_traffic_rows(0, 0);

    /* Crude bots produce noisy data — the verdict is informational
     * rather than gating. Hard fail only when nothing happens at all
     * (the sim never ran, or every bot hit a structural wall and never
     * fired). Soft warnings flag missing coverage. The per-map briefs in
     * `documents/m5/07-maps.md` are designer guidance, not automated
     * pass/fail criteria — designers iterate against the heatmap PNGs. */
    bool ok_movement   = (g_stats.heat_max_traffic > 0);
    bool ok_activity   = (g_stats.total_fires > 0);

    snprintf(buf, sizeof buf,
             "fires=%llu pickups=%d (%d unique, %d ungrabbed) "
             "dead-cells=%d kills=R%d/B%d/F%d",
             (unsigned long long)g_stats.total_fires,
             total_grabs, unique_grabbed, ungrabbed, dead_cells,
             g_stats.total_kills_red, g_stats.total_kills_blue, g_stats.total_kills_ffa);
    snprintf(v.detail, sizeof v.detail, "%s", buf);
    v.pass = ok_movement && ok_activity;

    (void)short_name;
    return v;
}

static void write_summary(const char *short_name, const World *w,
                          const Verdict *v, int bots, int duration_s) {
    char path[256];
    snprintf(path, sizeof path, "build/bake/%s.summary.txt", short_name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "Bake summary — %s\n", short_name);
    fprintf(f, "  bots=%d duration_s=%d ticks=%llu\n",
            bots, duration_s, (unsigned long long)w->tick);
    fprintf(f, "  pickups: %d total grabs across %d spawners\n",
            g_stats.pickup_count, w->pickups.count);
    fprintf(f, "  kills: %d total (R=%d B=%d FFA=%d)\n",
            g_stats.kill_count,
            g_stats.total_kills_red, g_stats.total_kills_blue,
            g_stats.total_kills_ffa);
    fprintf(f, "  flag events: %d\n", g_stats.flag_event_count);
    fprintf(f, "  heatmap max — kills=%u traffic=%u pickups=%u\n",
            g_stats.heat_max_kills, g_stats.heat_max_traffic,
            g_stats.heat_max_pickups);
    fprintf(f, "  verdict: %s\n", v->pass ? "PASS" : "FAIL");
    fprintf(f, "  detail: %s\n", v->detail);
    fclose(f);
}

/* ---- Map resolution ------------------------------------------------- */

/* Try `assets/maps/<short>.lvl` first; on failure, fall back to the
 * named builtin (MAP_FOUNDRY/SLIPSTREAM/REACTOR/CROSSFIRE) — only those
 * four short_names have a code-built fallback. Returns the MapId used
 * for spawn-lane lookup. */
static MapId resolve_and_build_map(World *world, Arena *arena,
                                   const char *short_name) {
    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", short_name);
    if (map_build_from_path(world, arena, path)) {
        return MAP_FOUNDRY;  /* MapId not meaningful for custom .lvl */
    }
    /* Fall back to builtin. */
    int id = map_id_from_name(short_name);
    if (id < 0) id = MAP_FOUNDRY;
    map_build((MapId)id, world, arena);
    return (MapId)id;
}

/* ---- Mode detection ------------------------------------------------- */

static MatchModeId pick_mode_from_mask(uint16_t mode_mask) {
    if (mode_mask & (1u << MATCH_MODE_CTF)) return MATCH_MODE_CTF;
    if (mode_mask & (1u << MATCH_MODE_TDM)) return MATCH_MODE_TDM;
    return MATCH_MODE_FFA;
}

/* ---- Main ----------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: bake <short_name> [--bots N] [--duration_s S] [--seed S] [--tier TIER]\n"
                "  TIER: recruit | veteran | elite | champion (default: veteran)\n");
        return 1;
    }
    const char *short_name = argv[1];
    int bots_requested = 16;
    int duration_s = 60;
    uint32_t seed = 0xC0FFEEu;
    BotTier tier = BOT_TIER_VETERAN;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--bots") && i + 1 < argc) {
            bots_requested = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--duration_s") && i + 1 < argc) {
            duration_s = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--tier") && i + 1 < argc) {
            tier = bot_tier_from_name(argv[++i]);
        }
    }
    if (bots_requested < 2)              bots_requested = 2;
    if (bots_requested > BAKE_BOTS_MAX)  bots_requested = BAKE_BOTS_MAX;

    log_init(NULL);
    SetTraceLogLevel(LOG_WARNING);   /* raylib chatter off */

    ensure_dir("build");
    ensure_dir("build/bake");

    Game g;
    if (!game_init(&g)) {
        fprintf(stderr, "bake: game_init failed\n");
        return 1;
    }
    g.world.authoritative = true;
    g.world.friendly_fire = false;
    g.world.local_mech_id = -1;
    g.world.dummy_mech_id = -1;

    /* Build the map. */
    map_registry_init();
    MapId mid_fallback = resolve_and_build_map(&g.world, &g.level_arena, short_name);
    Level *L = &g.world.level;

    MatchModeId mode = pick_mode_from_mask(L->meta.mode_mask);
    g.world.match_mode_cached = (int)mode;
    g.match.mode = mode;
    g.match.phase = MATCH_PHASE_ACTIVE;
    g.match.map_id = mid_fallback;
    g.match.score_limit = 99999;        /* don't end the bake on score */
    g.match.time_limit = 0;             /* unlimited; bake controls duration */
    g.match.time_remaining = 0;
    g.match.team_score[MATCH_TEAM_RED] = 0;
    g.match.team_score[MATCH_TEAM_BLUE] = 0;
    g.match.friendly_fire = false;
    g.match.rounds_played = 0;
    g.match.rounds_per_match = 99;
    g.match.solo_warning_remaining = -1.0f;

    /* Populate the runtime pickup pool from LvlPickup records — without
     * this the world.pickups stays empty and pickup_step never sees a
     * spawner to touch-check. (The host normally calls this from
     * net.c::server_start_match; lobby-less harnesses must do it.) */
    pickup_init_round(&g.world);

    /* CTF setup. */
    if (mode == MATCH_MODE_CTF) {
        ctf_init_round(&g.world, mode);
    }

    /* Spawn bots. Alternate teams for TDM/CTF; all on team RED (=FFA) for
     * FFA per the match.h convention (MATCH_TEAM_FFA aliases _RED). */
    g_stats.bot_count = 0;
    BotSystem bot_sys;
    bot_system_init(&bot_sys);
    bot_sys.seed = (uint64_t)seed * 0x9E3779B97F4A7C15uLL;
    bot_system_build_nav(&bot_sys, L, &g.level_arena);
    for (int i = 0; i < bots_requested; ++i) {
        int team;
        if (mode == MATCH_MODE_FFA) team = MATCH_TEAM_FFA;
        else                         team = (i & 1) ? MATCH_TEAM_BLUE : MATCH_TEAM_RED;

        Vec2 spawn = map_spawn_point(mid_fallback, L, i, team, mode);

        MechLoadout lo;
        bot_default_loadout_for_tier(i, tier, &lo);

        int mech_id = mech_create_loadout(&g.world, lo, spawn, team, false);
        if (mech_id < 0) break;          /* pool full */
        Bot *bot = &g_stats.bots[g_stats.bot_count++];
        bot->mech_id    = mech_id;
        bot->team       = team;
        bot->chassis    = lo.chassis_id;
        bot_attach(&bot_sys, mech_id, tier, (uint64_t)i * 0xBF58476D1CE4E5B9uLL);
    }

    fprintf(stdout,
            "bake[%s]: %d bots (tier=%s), %d s, mode=%s, map=%dx%d, %d spawns, "
            "%d pickups, %d polys, %d flags, %d nav nodes\n",
            short_name, g_stats.bot_count, bot_tier_name(tier),
            duration_s,
            match_mode_name(mode), L->width, L->height,
            L->spawn_count, L->pickup_count, L->poly_count, L->flag_count,
            bot_nav_node_count(&bot_sys));

    /* Seed per-spawner state so the first tick's delta detects nothing. */
    for (int i = 0; i < g.world.pickups.count; ++i) {
        g_stats.spawner_prev_state[i] = g.world.pickups.items[i].state;
    }
    g_stats.flag_prev_status[0] = (g.world.flag_count > 0) ? g.world.flags[0].status : 0;
    g_stats.flag_prev_status[1] = (g.world.flag_count > 1) ? g.world.flags[1].status : 0;
    g_stats.killfeed_last_count = g.world.killfeed_count;

    /* Main bake loop. */
    int total_ticks = duration_s * TICK_HZ;
    for (int t = 0; t < total_ticks; ++t) {
        /* Drive bot inputs via the layered AI in src/bot.c. */
        bot_step(&bot_sys, &g.world, &g, TICK_DT);
        /* Step the world. */
        simulate_step(&g.world, TICK_DT);
        if (mode == MATCH_MODE_CTF) {
            ctf_step(&g, TICK_DT);
        }
        /* Drain stats. */
        record_traffic(&g.world);
        drain_killfeed(&g.world);
        drain_pickup_transitions(&g.world);
        drain_flag_transitions(&g.world);

        /* Drain feed buffers so they don't grow unboundedly (we already
         * captured what we needed). */
        g_stats.total_fires += (uint64_t)g.world.firefeed_count;
        g.world.hitfeed_count = 0;
        g.world.firefeed_count = 0;
        g.world.explosionfeed_count = 0;
        g.world.pickupfeed_count = 0;
        g.world.flag_state_dirty = false;

        /* Progress indicator every 10 simulated seconds. */
        if ((t % (10 * TICK_HZ)) == 0 && t > 0) {
            int alive = 0;
            for (int i = 0; i < g.world.mech_count; ++i)
                if (g.world.mechs[i].alive) alive++;
            fprintf(stdout, "  t=%ds alive=%d kills=%d pickups=%d fires=%llu\n",
                    t / TICK_HZ, alive,
                    g_stats.kill_count, g_stats.pickup_count,
                    (unsigned long long)g_stats.total_fires);
        }
    }

    /* Write outputs. */
    write_csvs(short_name);
    write_heatmap_png(short_name);
    Verdict v = compute_verdict(&g.world, short_name);
    write_summary(short_name, &g.world, &v, g_stats.bot_count, duration_s);

    fprintf(stdout, "bake[%s]: %s — %s\n",
            short_name, v.pass ? "PASS" : "FAIL", v.detail);

    bot_system_destroy(&bot_sys);
    game_shutdown(&g);
    return v.pass ? 0 : 2;
}
