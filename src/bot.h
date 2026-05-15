#pragma once

#include "hash.h"
#include "input.h"
#include "math.h"
#include "mech.h"
#include "world.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * bot — AI-driven mechs. Two consumers, one module:
 *
 *   1. tools/bake/run_bake.c — headless map-validation harness. Needs
 *      bots competent enough that lack of coverage means lack of
 *      geometry, not lack of AI.
 *   2. host-side in-game bots — players spawn N bots in the lobby and
 *      fight them in single-player or LAN play.
 *
 * Both paths call the same bot_step(World*, Game*, dt) and let it write
 * `mech.latched_input` for every mech under control. See
 * `documents/13-bot-ai.md` for the full design.
 *
 * Architecture (layered classical AI, Quake III-style):
 *
 *     Strategy (utility scorer, 10 Hz)
 *         │  picks one of {ENGAGE, PURSUE_PICKUP, GRAB_FLAG, ...}
 *         ▼
 *     Tactic (small per-goal switch, every tick)
 *         │  fills BotWants { move_target, aim_target, want_fire, ... }
 *         ▼
 *     Motor (every tick)
 *         │  translates wants → ClientInput + applies aim slew + jitter
 *         ▼
 *     mech.latched_input (consumed by simulate_step)
 *
 * Plus the cross-cutting **navigation layer** built once per
 * map_build that the strategy + tactic layers both query.
 */

struct Game;

/* ---- Difficulty tiers ---------------------------------------------- */

typedef enum {
    BOT_TIER_RECRUIT  = 0,  /* intro tier; large aim jitter, slow reactions */
    BOT_TIER_VETERAN  = 1,  /* steady; reasonable aim and routing */
    BOT_TIER_ELITE    = 2,  /* strong; clean aim, good pickup priorities */
    BOT_TIER_CHAMPION = 3,  /* top classical tier; near-frame-perfect */
    BOT_TIER_COUNT,
} BotTier;

typedef struct BotPersonality {
    uint8_t tier;                 /* BotTier */
    float   aim_jitter_rad;       /* per-shot Gaussian noise on aim */
    float   aim_lead_frac;        /* 0..1; how much we lead moving targets */
    float   aim_slew_rad_per_tick;/* cap on aim slew per tick */
    float   reaction_ticks;       /* ticks of delay before reacting to a new enemy */
    float   awareness_radius_px;  /* how far we "notice" enemies (LOS still required) */
    float   pickup_priority;      /* 0..1 weight on grab-pickup utility */
    float   flag_priority;        /* 0..1 weight on flag goals (CTF) */
    float   grapple_priority;     /* 0..1 weight on using grapple for vertical reach */
    float   aggression;           /* 0..1; biases pursue vs retreat */
    /* M6 P05 Phase 4 — aggression-based retreat. When `bot_aggression`
     * falls below this threshold the bot breaks engagement and seeks
     * cover near a healing pickup. Recruit's threshold is 0.0 so it
     * never retreats (it's the bad-on-purpose tier; retreating would
     * just make Recruits never fight). Champion's stays above 0.30 so
     * even high-skill bots disengage when very hurt. */
    float   retreat_threshold;
    uint8_t knows_full_map;       /* false → only nearby nodes become routes */
    uint8_t uses_powerups;        /* false → ignores POWERUP_* spawners */
} BotPersonality;

/* ---- Strategy goals ------------------------------------------------- */

typedef enum {
    BOT_GOAL_IDLE          = 0,
    BOT_GOAL_ENGAGE        = 1,
    BOT_GOAL_REPOSITION    = 2,
    BOT_GOAL_PURSUE_PICKUP = 3,
    BOT_GOAL_GRAB_FLAG     = 4,
    BOT_GOAL_RETURN_FLAG   = 5,
    BOT_GOAL_DEFEND_FLAG   = 6,
    BOT_GOAL_CHASE_CARRIER = 7,
    BOT_GOAL_CAPTURE       = 8,
    BOT_GOAL_RETREAT       = 9,
    BOT_GOAL_COUNT,
} BotGoal;

/* M6 P05 Phase 5 — CTF team-role coordination. Assigned at round
 * start and on flag-state transitions; the strategy scorer multiplies
 * each goal's score by a role-specific weight (table in `bot.c`). */
typedef enum {
    BOT_ROLE_NONE      = 0,
    BOT_ROLE_ATTACKER  = 1,
    BOT_ROLE_DEFENDER  = 2,
    BOT_ROLE_FLOATER   = 3,
    BOT_ROLE_CARRIER   = 4,
    BOT_ROLE_COUNT,
} BotTeamRole;

const char *bot_team_role_name(BotTeamRole r);

const char *bot_goal_name(BotGoal g);

/* ---- Per-bot mind --------------------------------------------------- */

/* Path-prefix cap. 32 nodes × 96 px sample step = ~3 km of map can be
 * pre-planned in one A* run. Big enough that Aurora / Citadel /
 * Crossfire (5–6 km wide) get one continuous route from spawn into
 * enemy territory instead of stalling at the prefix end. */
#define BOT_MAX_PATH_LEN  32

typedef struct BotMind {
    int8_t          mech_id;
    bool            in_use;
    BotPersonality  pers;
    uint8_t         goal;                  /* BotGoal */
    int16_t         goal_target_mech;      /* -1 if none */
    int16_t         goal_target_node;      /* nav node id, -1 if none */
    int16_t         path[BOT_MAX_PATH_LEN];/* node ids; head at [path_step] */
    uint8_t         path_len;
    uint8_t         path_step;
    uint8_t         path_reach_kind;       /* BotReachKind of the in-progress hop */
    uint8_t         pad0;
    int16_t         reaction_ticks_remaining;
    uint64_t        last_strategy_tick;
    uint64_t        last_replan_tick;
    uint64_t        first_seen_enemy_tick;
    uint64_t        last_jump_tick;
    uint64_t        stuck_since_tick;
    float           stuck_last_x;
    float           goal_score_cached;
    /* Aim model — `cur_aim` slews toward the strategy target with a
     * per-tier cap. Jitter is sampled on each fire tick. */
    Vec2            cur_aim;
    Vec2            wander_pos;
    int16_t         seen_enemy_id;
    int16_t         pad1;
    /* Jet fuel hysteresis — true when we've recently drained the
     * jet and are waiting for it to refill before re-engaging.
     * Without this, bots mash JET in tight corners while fuel is
     * empty: the input flows but the chassis applies no thrust,
     * so they look "stuck on JET" forever. */
    bool            jet_locked_out;
    uint8_t         pad2[3];
    /* M6 P05 Phase 2 — cover-aware engagement positioning. When the
     * enemy is out of LOS, tactic_engage runs a BFS for a position
     * that CAN see them, plans a path there, and caches the result so
     * subsequent ticks reuse the path. Refresh every ~2 s (or on
     * enemy change). */
    int16_t         engagement_node;
    int16_t         engagement_for_enemy;
    uint64_t        engagement_node_tick;
    /* M6 P05 Phase 4 — aggression tracking. `last_hp_observed` is set
     * on every strategy tick; when it drops we mark `last_hurt_tick`
     * which the aggression formula consumes for the "recently hurt →
     * low aggression" term. */
    float           last_hp_observed;
    uint64_t        last_hurt_tick;
    /* M6 P05 Phase 5 — CTF team role. Stamped at round start and on
     * flag-state transitions by `bot_assign_team_roles`. */
    uint8_t         team_role;            /* BotTeamRole */
    uint8_t         pad3[3];
    /* M6 bot-stuck-fix — abandoned-target memory. When the bot has
     * been stuck pursuing a goal_target_node for too long (the
     * unreachable-pickup loop where pelvis can't physically clear the
     * climb), record that node and refuse to re-pick it for ~10 s.
     * Small ring of 4 entries — covers chains of "tried this, then
     * the next one, then the next" without forgetting the first. */
    int16_t         abandoned_nodes[4];
    uint64_t        abandoned_until[4];   /* world tick at which the entry expires */
    /* M6 many-bots stuck-fix — meaningful-progress tracker (sampled
     * once per strategy tick, ~10 Hz). The pre-existing per-tick
     * `stuck_since_tick` resets the moment pelvis x moves any amount,
     * which masks the "JET up to unreachable pickup → fall back →
     * try again" loop: the pelvis bounces through 100s of px each
     * iteration so it never stays still for the abandonment window.
     * `progress_anchor_*` snapshots position + tick when we last
     * saw meaningful progress toward goal_target_node; if we haven't
     * gotten 96 px closer to the goal in BOT_PROGRESS_STALL_TICKS
     * (5 s), abandon. */
    float           progress_anchor_x;
    float           progress_anchor_y;
    float           progress_anchor_dist_to_goal;
    int16_t         progress_anchor_goal_node;
    uint64_t        progress_anchor_tick;
    pcg32_t         rng;
} BotMind;


/* ---- Bot system ----------------------------------------------------- */

struct BotNav;  /* opaque — defined in bot.c */

typedef struct BotSystem {
    BotMind        minds[MAX_MECHS];
    int            count;
    struct BotNav *nav;           /* allocated from level_arena in bot_system_build_nav */
    uint64_t       seed;
    /* M6 many-bots fix — combat-famine tracker. `last_kill_count` is
     * the world.killfeed_count seen on the last bot_step tick;
     * `famine_anchor_tick` advances each tick the count is unchanged.
     * As (now - anchor_tick) grows, the strategy's engage / pursue
     * scores get a multiplicative boost (and awareness radius
     * widens) so wandering bots are pulled together when no kills
     * have happened in a while. Reset the moment a kill happens. */
    int            last_kill_count;
    uint64_t       famine_anchor_tick;
} BotSystem;

/* Zero-init the system. Safe to call multiple times. The navigation
 * graph is NOT built here — call bot_system_build_nav after every
 * map_build (the previous nav lives in level_arena, which the caller
 * resets between rounds). */
void bot_system_init(BotSystem *bs);

/* Drop all per-bot state. Does NOT free the nav graph — that's a
 * level-arena allocation. Call between rounds; bot_system_build_nav
 * will allocate fresh nav memory from the new arena. */
void bot_system_reset_minds(BotSystem *bs);

void bot_system_destroy(BotSystem *bs);

/* (Re)build the navigation graph for the supplied level. Allocates
 * from `arena` (the caller's level_arena). Idempotent — pass a freshly
 * reset arena and the graph rebuilds for the new map.
 *
 * Returns the node count, or 0 if construction failed (out of arena). */
struct Arena;
int  bot_system_build_nav(BotSystem *bs, const Level *level,
                          struct Arena *arena);

/* Attach a bot mind to an existing mech slot. Must be called AFTER
 * mech_create_loadout for the mech_id. `seed_salt` mixes into the
 * per-bot PCG seed (the world's RNG is owned by simulate, can't share). */
void bot_attach(BotSystem *bs, int mech_id, BotTier tier, uint64_t seed_salt);

/* Release a mind on mech destruction. */
void bot_detach(BotSystem *bs, int mech_id);

bool bot_is_attached(const BotSystem *bs, int mech_id);

/* Per-tick step. Iterates every attached mind, runs the strategy
 * scorer (~10 Hz) + tactic tree (every tick) + motor, and writes
 * `mech.latched_input` for each bot under control. The Game pointer
 * is needed for CTF state (mode + flags); pass NULL when used outside
 * the lobby/match flow (the bake test does this). */
void bot_step(BotSystem *bs, World *w, struct Game *g, float dt);

/* Per-tier default personality. The numeric table is in bot.c. */
BotPersonality bot_personality_for_tier(BotTier tier);

const char *bot_tier_name(BotTier tier);
BotTier     bot_tier_from_name(const char *s);

/* Fill `out` with a recommended loadout for a bot. Mixes chassis +
 * weapons by index so a populated lobby has visible variety. */
void bot_default_loadout_for_tier(int bot_index, BotTier tier,
                                  MechLoadout *out);

/* Synthesize a display name "Bot-1 (Veteran)" etc. into `buf`. Returns
 * `buf` for convenience. */
const char *bot_name_for_index(int bot_index, BotTier tier,
                               char *buf, int cap);

/* Bot-system-wide role (re)assignment. Called at round start (after
 * lobby_spawn_round_mechs) and on flag-state transitions or bot
 * death/team-comp change. Operates over the bots already attached to
 * `bs`; does nothing in non-CTF modes. */
void bot_assign_team_roles(BotSystem *bs, const World *w);

/* Diagnostic helpers (used by bake/test scaffolding + hud overlays). */
int  bot_nav_node_count(const BotSystem *bs);
bool bot_nav_node_pos  (const BotSystem *bs, int node_id, Vec2 *out);

/* M6 P05 Phase 1 — visibility precompute diagnostics. */
int  bot_nav_visibility_edge_count(const BotSystem *bs);
bool bot_nav_node_sees             (const BotSystem *bs, int src, int target);
int  bot_nav_reach_count           (const BotSystem *bs);
