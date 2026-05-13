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

const char *bot_goal_name(BotGoal g);

/* ---- Per-bot mind --------------------------------------------------- */

#define BOT_MAX_PATH_LEN  16

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
    pcg32_t         rng;
} BotMind;

/* ---- Bot system ----------------------------------------------------- */

struct BotNav;  /* opaque — defined in bot.c */

typedef struct BotSystem {
    BotMind        minds[MAX_MECHS];
    int            count;
    struct BotNav *nav;           /* allocated from level_arena in bot_system_build_nav */
    uint64_t       seed;
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

/* Diagnostic helpers (used by bake/test scaffolding + hud overlays). */
int  bot_nav_node_count(const BotSystem *bs);
bool bot_nav_node_pos  (const BotSystem *bs, int node_id, Vec2 *out);
