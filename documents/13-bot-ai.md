# 13 — Bot AI

This document specifies the bot AI system. It exists because (a) the
M5 P18 bake-test harness needs smarter bots than its crude wander +
shoot loop to surface real layout problems, and (b) the roadmap's
post-v1 stretch goal "**Bots** — AI-driven mechs to fill servers"
needs a concrete design rather than the one-line "behavior tree +
pathfinding on the tile grid" sketch in `11-roadmap.md`.

The bake-test bot and the in-game bot are **the same module**. Two
use cases, one implementation, one source of truth — same discipline
as the rest of the codebase.

## What we observed (M5 P18 bake)

The crude wander-and-shoot bot in `tools/bake/run_bake.c` passes the
smoke-level verdict (bots fire, motion is recorded, no crashes) but
the heatmaps reveal structural failure modes:

| Map | Coverage problem |
|---|---|
| Foundry | Bottom-floor band only. Cover-wall top + spawn platforms never reached. |
| Slipstream | Floor strip only. **Zero** upper-catwalk or basement-cave coverage. |
| Reactor | Best of the lot. Floor + bowl partial. High overlooks dark. |
| Concourse | Sparse. Bots barely traverse the 100-tile width. |
| Catwalk | Vertical map. Floor + faint upper band. Catwalks unreached. |
| Aurora | 160-wide open map. Lower-right corner only. Hills + sky struts dark. |
| Crossfire | Full bottom-floor strip (the one win). Catwalks, tunnels, alcoves dark. |
| Citadel | Faint floor. Castles, sky bridges, plaza bowl all dark. |

**Root causes** (from the same AI code):
1. The wander target is always a `LvlSpawn` position. Spawn points
   cluster — 80% of every map's area never becomes a target.
2. No path planning. Bots walk straight at `target_x`, hit walls, get
   stuck. Stuck-detection fires a jet pulse but doesn't pick a path
   around the obstacle.
3. Jet is reactive (when stuck or chasing a high-altitude enemy). It's
   never used as the *primary* traversal verb for vertical maps.
4. Pickups have zero priority. Bots walk past health/ammo/weapons
   without grabbing them. The 24 px touch radius only fires when a
   foot accidentally crosses one.
5. Flags have zero priority. CTF bots never run the flag — captures
   are zero across every CTF bake.
6. Grapple is never fired. The Grappling Hook secondary slot is
   ignored entirely.
7. LOS gate is too strict — both teams sit on opposite sides of cover
   walls and never engage.

These same failures will be obvious in real play with users.
"Bot mode" against this AI is unplayable.

## What we want

Two consumers, one bot module:

### Consumer 1 — the bake-test harness

`tools/bake/run_bake.c` calls into the bot module each tick to produce
a `ClientInput` for every mech it controls. The bake's purpose is
layout validation: dead zones, spawn imbalance, contestability of
pickups, viability of flag routes in CTF. The bot needs to be
**competent enough that absence of activity means absent geometry**,
not "bot AI didn't know how to get there." 60-second bakes at the
**Veteran** tier (defined below) should cover ≥40% of every map's
playable cells and grab ≥80% of pickups.

### Consumer 2 — in-game bots

Players in single-player practice mode + thin LAN games should be
able to spawn N bots to fill a server. The bots should:

- Pick up health/ammo/weapons/powerups on their own.
- Use the grapple where the map design assumes it.
- Defend their flag and chase enemy carriers in CTF.
- Spread across difficulty tiers so a new player can fight Recruits
  while a veteran can fight Champions.
- Run inside the existing 60 Hz sim budget — collectively under
  0.5 ms/tick at 32 bots.

### A non-goal: indistinguishable-from-human

We are not chasing Turing-test bots. Players know they're playing
bots. We want them *fun to fight*, not *spooky-realistic*.

## What we will NOT do (out of scope)

- **Procedural training of every bot from scratch on every launch.**
  If we do train (M6 stretch — §"Optional: self-play training"),
  policies ship as static `.onnx` files and load like any other
  asset.
- **Online learning during a match.** Bots don't adapt mid-game from
  the player's behavior. Difficulty is selected, not earned.
- **Per-personality character files à la Q3** at v1. We start with
  4 hand-tuned tiers; named "characters" are M6+ polish.
- **Voice / chatter / personality scripting.** Bots are silent. Real
  player chat happens in the lobby.
- **Coordinated team play** at v1 beyond "carry our flag, kill enemy
  carrier." No human-style callouts, focus-fire signals, or
  squad-leader hierarchies. Doable later; not load-bearing for the
  bake test or single-player mode.

## The decision: what kind of AI?

A two-pass survey of the standard options against our needs:

### Behavior Tree (BT)

Industry standard since *Halo 2*. Tree of selector / sequence /
condition / action nodes. Predictable, debuggable, hand-authored.
Easy to add new behaviors. The cost: large trees grow into
"spaghetti trees" — hard to extend without breaking, and the
authored behavior is exactly the authored behavior (no emergent
combinations).

For our scale (8-32 bots, ~30 behaviors), a hand-authored BT is
comfortably tractable.

### Goal-Oriented Action Planning (GOAP)

F.E.A.R. (2005) made this canonical. Plans a sequence of actions
that satisfies a goal's preconditions. Dynamic — bots find their own
sequences. Cost: harder to debug ("why did it pick this plan?"),
needs a planner that can be expensive when the action set grows.

Overkill for our scale. We don't need emergent multi-step plans like
"flank → reload behind cover → ambush"; our maps are small enough
that "go to enemy, shoot, reposition" + good navigation covers it.

### Utility AI

The Sims (2000), heavily used in modern open-world games. Each
candidate action gets a score from a per-action curve over world
state. Pick highest. Cost: tuning curves is non-obvious; bots can
look "wishy-washy" if scores oscillate.

The right shape for the **top-level goal selection** (engage / pursue
pickup / chase flag carrier / defend). NOT the right shape for the
moment-to-moment input generation — for that we want explicit state.

### Reinforcement Learning (PPO + self-play)

Recent research (2024-2026) shows PPO with curriculum learning + self-play
is the current sweet spot for FPS bots, with **curriculum learning
beating behavior cloning by ~23%** in measured arena wins (MDPI
*Reinforcement Learning as an Approach to Train Multiplayer FPS Game
Agents*, 2024). OpenAI Five (Dota 2) and DeepMind's AlphaStar
(StarCraft 2) are the high-profile cases. *DeepMind for Quake III
Capture the Flag* (2019) trained CTF bots that beat human teams.

The cost is huge: weeks of GPU time to train, a stable training
pipeline, ONNX or similar at inference time, and the bots become
fixed-once-trained black boxes. Plus: needs the simulation to step
~1000× faster than realtime in parallel — feasible with our pure
`simulate_step` (no globals, seeded RNG, headless-friendly) but a
real engineering investment.

### What we ship

**Hybrid**, with the RL path as a deliberate stretch:

1. **Layered classical AI for the v1 ship** — navigation graph +
   utility scorer for goal selection + small behavior trees per
   goal + motor (writes `ClientInput`). This is essentially the
   Quake III bot architecture (Mr. Elusive, 2001) adapted to 2D
   Verlet-skeleton mechs. Proven design, ~2-3 weeks to implement,
   tunable difficulty via continuous parameters.

2. **Optional PPO self-play training pipeline (M6+ stretch)** that
   produces a learned policy as an ONNX file. The runtime loads the
   policy and uses it as one more "tier" in the difficulty system.
   The classical AI stays as the fallback / lower tiers. Players
   who want the "trainable opponent" buy in to the self-play tier;
   players who want predictable bots stick with classical.

The classical layer alone is sufficient for the bake-test problem
and for the v1 in-game bot mode. The RL layer is a separate ship
that unblocks "an opponent that improves" — but classical-only is
the M5/M6 deliverable.

## Architecture

```
                   ┌─────────────────────────┐
                   │  BotPersonality (tier)  │  static per-bot tuning
                   └──────────┬──────────────┘
                              ↓
   ┌──────────────────────────────────────────────────────────┐
   │  Strategy layer (utility scorer, every 6 ticks = ~10 Hz) │
   │    Scores: engage, pursue_pickup, grab_flag, chase_carrier,
   │            defend_flag, retreat, reposition               │
   └──────────┬───────────────────────────────────────────────┘
              │ goal_id + target_pos
              ↓
   ┌──────────────────────────────────────────────────────────┐
   │  Tactic layer (small per-goal behavior tree, every tick) │
   │    Picks moves: approach, strafe, jump, jet, grapple-fire,
   │                 fire, reload, swap, use                   │
   └──────────┬───────────────────────────────────────────────┘
              │ wants: { move_dir, jet, jump, fire, ... }
              ↓
   ┌──────────────────────────────────────────────────────────┐
   │  Motor layer (every tick)                                │
   │    Translates wants → ClientInput (buttons + aim)         │
   │    Applies personality knobs (aim jitter, reaction delay) │
   └──────────┬───────────────────────────────────────────────┘
              │ ClientInput
              ↓
       mech.latched_input         (consumed by simulate_step)
```

Plus the cross-cutting **navigation layer** (built once per level
load) that the strategy and tactic layers both query.

### Module layout

```
src/bot.{c,h}             // public surface: bot_init, bot_step, bot_destroy
src/bot_nav.{c,h}         // navigation graph build + A*  (only if bot.c exceeds ~1500 LOC)
```

One header per module per the codebase convention. We do **not**
split into `bot_strategy.c` / `bot_tactic.c` / `bot_motor.c` —
those are sections inside `bot.c`. If `bot.c` crosses 1500 LOC the
split goes navigation-first (it's the natural separable unit).

### Public surface

```c
// src/bot.h
#pragma once
#include "world.h"
#include "match.h"

typedef enum {
    BOT_TIER_RECRUIT  = 0,    // intro tier; large aim jitter, slow reactions
    BOT_TIER_VETERAN  = 1,    // steady; reasonable aim and routing
    BOT_TIER_ELITE    = 2,    // strong; clean aim, good pickup priorities
    BOT_TIER_CHAMPION = 3,    // top classical tier; near-frame-perfect
    BOT_TIER_TRAINED  = 4,    // M6+ stretch: PPO policy loaded from .onnx
    BOT_TIER_COUNT,
} BotTier;

typedef struct BotPersonality {
    BotTier  tier;
    float    aim_jitter_rad;        // per-shot Gaussian noise on aim
    float    aim_lead_frac;         // 0..1, how much we lead moving targets
    float    reaction_ticks;        // ticks of delay before reacting to enemy appearance
    float    awareness_radius_px;   // how far we "notice" enemies (LOS still required)
    float    pickup_priority;       // 0..1 weight on grab-pickup utility
    float    flag_priority;         // 0..1 weight on flag goals (CTF)
    float    grapple_priority;      // 0..1 weight on using grapple to reach vertical targets
    float    aggression;            // 0..1, biases pursue vs retreat utility
    uint8_t  knows_full_map;        // false → only nodes within awareness become routes
    uint8_t  uses_powerups;         // false → ignores POWERUP_* spawners
} BotPersonality;

typedef struct BotMind {
    int              mech_id;
    BotPersonality   pers;
    uint8_t          goal;           // BotGoal — current strategy goal
    int              goal_target_mech; // -1 if none
    int              goal_target_node; // navigation node, -1 if none
    int              path[16];       // node IDs of the current route, head at [0]
    uint8_t          path_len;
    uint8_t          path_step;
    int              reaction_ticks_remaining;
    uint64_t         last_replan_tick;
    pcg32_t          rng;            // per-bot, seeded from world.rng at init
} BotMind;

typedef struct BotSystem {
    BotMind          minds[MAX_MECHS];
    int              count;
    void            *nav;            // BotNav* — opaque pointer to the nav graph
} BotSystem;

// Build navigation graph from level (call after map_build).
void bot_init(BotSystem *bs, World *w);

// Per-tick step: writes mech.latched_input for every bot under control.
// Cheap helper — under 100 µs at 32 bots (budget §"Performance").
void bot_step(BotSystem *bs, World *w, Game *g, float dt);

// Attach a bot mind to an existing mech slot (call AFTER mech_create).
void bot_attach(BotSystem *bs, int mech_id, BotTier tier);

// Release a mind on mech destruction (slot reset).
void bot_detach(BotSystem *bs, int mech_id);

void bot_destroy(BotSystem *bs);

// Personality default for a tier — used by both bake and in-game spawn.
BotPersonality bot_personality_for_tier(BotTier tier);
```

Note the signature `bot_step(BotSystem*, World*, Game*, float)` —
the Game pointer is needed for CTF state (mirrors how ctf_step
takes it). Strategy queries can read match.mode, lobby slots, etc.

## Layer 1 — navigation graph

### Why not a navmesh

Standard 2D navmeshes (Recast-style) presume freely-walkable convex
regions. Our world has Verlet-skeleton mechs that *jump*, *jet*, and
*grapple* — none of which are simple "edge to edge" walks. We want
the navigation layer to encode those traversal verbs explicitly.

Quake III's AAS (Area Awareness System) is the right shape: precompute
**areas** + **reachabilities** between them, where each reachability
names the verb used. Jean-Paul van Waveren's AAS divides the world
into convex polyhedra and tags reachabilities as `walk`, `jump`,
`step`, `ladder`, `swim`, etc. A bot route is a sequence of
reachabilities; the motor knows how to *execute* each verb.

We do the 2D version of this.

### Nodes

A **node** is a placeable point in the world where a bot can stop
and stand. Generated at level load:

- Every `LvlSpawn` position → one node.
- Every `LvlPickup` position → one node.
- Every `LvlFlag.home_pos` → one node.
- Every **floor center** of a 4×N solid floor segment → one node
  per 4 tiles, so flat surfaces are sampled at ~128 px spacing.
- Every **platform top center** for tile-fill platforms (catwalks)
  → one node per 4 tiles.
- Every **polygon top vertex** that lies above empty tiles → one
  node placed 32 px above (a "slope summit" landmark).

A 100×40 map (Foundry) produces ~80-120 nodes; Citadel (200×100)
produces ~400-500. Capacity:

```c
#define BOT_NAV_MAX_NODES   512
```

Per-node data:

```c
typedef struct BotNavNode {
    Vec2     pos;             // world-space px
    uint16_t flags;            // ON_FLOOR / ON_PLATFORM / SPAWN / PICKUP / FLAG / SLOPE_TOP
    uint8_t  pickup_kind;      // PICKUP_* if flags & PICKUP, else 0
    uint8_t  pickup_variant;
    int8_t   flag_team;        // 1/2/-1 (none)
    uint16_t reach_first;      // index into BotNavReach[]
    uint16_t reach_count;
} BotNavNode;
```

### Reachabilities

A reachability is a directed edge between two nodes, with a verb
tag and a cost:

```c
typedef enum {
    BOT_REACH_WALK    = 0,   // walk along floor / platform
    BOT_REACH_JUMP    = 1,   // jump-arc reach (≤ 1 tile up or 4 tiles across)
    BOT_REACH_FALL    = 2,   // drop off an edge
    BOT_REACH_JET     = 3,   // requires fuel; arc lift
    BOT_REACH_GRAPPLE = 4,   // hook to a target, swing/zip
    BOT_REACH_ONE_WAY = 5,   // drop through a one-way platform
} BotReachKind;

typedef struct BotNavReach {
    uint16_t from_node, to_node;
    uint8_t  kind;
    uint8_t  required_fuel_q8;  // Q0.8 fraction of fuel_max (JET only)
    uint16_t cost_ms;            // estimated travel time
} BotNavReach;
```

### Building the graph

Generation runs once per `map_build` (level arena lifetime):

1. Walk the level looking for ON_FLOOR / ON_PLATFORM strips. Place
   floor/platform nodes at the discrete sample points.
2. For each polygon, inspect its highest vertex; if the area above
   it is empty for ≥ 3 tiles vertically, place a SLOPE_TOP node.
3. Add SPAWN / PICKUP / FLAG nodes from `level.spawns / pickups /
   flags`.
4. For every pair of nodes (n, m) with `|n.pos - m.pos| ≤ MAX_REACH_PX`
   (≈ 6 tiles), test reachability:
   - **WALK**: same floor surface, no obstacle between → cost ∝ distance.
   - **JUMP**: target ≤ 32 px above source, horizontal gap ≤ 128 px,
     parabola clear of solids → cost = distance × 1.1.
   - **FALL**: target below source, no solid in vertical channel → cost
     = horizontal × 0.9.
   - **JET**: requires fuel proportional to vertical rise (precomputed
     against `JET_THRUST_PXS2` constant). Cost = distance × 1.4.
   - **GRAPPLE**: target ≤ `GRAPPLE_MAX_REST_LEN` (300 px), line-of-
     sight clear, target is a ceiling tile or solid above the source →
     cost = distance × 1.05 + 100 ms (rope-pull time).
   - **ONE_WAY**: target one tile below a ONE_WAY tile → cost = 1.

Each reachability test is closed-form; build cost on Citadel-scale
maps is ~30 ms (dominated by O(N²) candidate pair enumeration; we
can add a spatial hash for big maps).

### Pathfinding

Standard A* over `BotNavReach`. Cost = sum of `cost_ms`. Heuristic
= Manhattan distance / `RUN_SPEED_PXS`. Bot keeps the 16-node
prefix of the current path; if displaced more than a node-spacing
from the next step, replans.

### Why the graph is the single biggest win

The crude bake bot fails because of *missing navigation*. Adding the
nav graph alone (without changing any other bot logic) gets bots to
upper catwalks, sky struts, and castle interiors — because the path
planner is allowed to *want* those nodes as targets. With the nav
graph wired and the wander target picking up nodes (not just spawns),
we expect coverage to jump from 7% to ~50% of map cells on the first
implementation.

## Layer 2 — strategy (utility scorer)

Runs every 6 ticks (~10 Hz) per bot. Scores N candidate goals from
the current world state + bot personality. Picks the highest. Sticks
with it until a higher-scoring goal beats it by more than a
hysteresis margin (10%), to prevent flicker.

### Candidate goals

```c
typedef enum {
    BOT_GOAL_IDLE          = 0,
    BOT_GOAL_ENGAGE        = 1,   // fight a visible enemy
    BOT_GOAL_REPOSITION    = 2,   // move to better angle / cover
    BOT_GOAL_PURSUE_PICKUP = 3,   // walk to a pickup
    BOT_GOAL_GRAB_FLAG     = 4,   // CTF — pick up enemy flag
    BOT_GOAL_RETURN_FLAG   = 5,   // CTF — touch dropped friendly flag
    BOT_GOAL_DEFEND_FLAG   = 6,   // CTF — camp near friendly flag base
    BOT_GOAL_CHASE_CARRIER = 7,   // CTF — chase enemy holding our flag
    BOT_GOAL_CAPTURE       = 8,   // CTF — bring enemy flag to home base
    BOT_GOAL_RETREAT       = 9,   // health low, head to nearest health pack
} BotGoal;
```

### Utility curves (Veteran tier baseline)

Each goal has a scorer function `f(world, mind, ...) → float in [0,1]`.
Personality knobs scale specific scorers. Example:

```c
// "engage" — score peaks when there's a visible enemy at moderate range
static float score_engage(const World *w, const BotMind *m) {
    int enemy = nearest_visible_enemy(w, m);
    if (enemy < 0) return 0.0f;
    float d = mech_distance(w, m->mech_id, enemy);
    float close = clampf((1000.0f - d) / 800.0f, 0.0f, 1.0f);
    float health_factor = w->mechs[m->mech_id].health / w->mechs[m->mech_id].health_max;
    return close * (0.4f + 0.6f * health_factor) * m->pers.aggression;
}

// "pursue_pickup" — score peaks when a relevant pickup is nearby and
// the bot needs it
static float score_pursue_pickup(const World *w, const BotMind *m) {
    int spawner = nearest_relevant_available_pickup(w, m);
    if (spawner < 0) return 0.0f;
    float d = mech_distance_to_spawner(w, m->mech_id, spawner);
    float need = pickup_need_factor(w, m, spawner);   // 0..1
    float reachable = bot_nav_has_path(/*...*/) ? 1.0f : 0.0f;
    return reachable * need * (1.0f - clampf(d / 1500.0f, 0.0f, 0.8f)) * m->pers.pickup_priority;
}

// "grab_flag" — CTF: enemy flag is HOME or DROPPED + we're alive + healthy
static float score_grab_flag(const World *w, const BotMind *m, const MatchState *match) {
    if (match->mode != MATCH_MODE_CTF) return 0.0f;
    int enemy_flag = (w->mechs[m->mech_id].team == MATCH_TEAM_RED) ? 1 : 0;
    if (w->flags[enemy_flag].status == FLAG_CARRIED) return 0.0f;
    float dist = mech_distance_to_pos(w, m->mech_id,
                                      ctf_flag_position(w, enemy_flag));
    float close = clampf((3000.0f - dist) / 2400.0f, 0.0f, 1.0f);
    float healthy = w->mechs[m->mech_id].health / w->mechs[m->mech_id].health_max;
    return close * healthy * m->pers.flag_priority;
}
```

The full scorer list runs in linear time (~10 candidate goals × ~50 µs
each = 0.5 ms total at 32 bots, but each bot only runs 1/6 of ticks
so amortized cost is well inside budget).

## Layer 3 — tactic (small per-goal behavior tree)

Once the goal is chosen, the tactic layer drives the moves. Each goal
has a small (~5-10 node) behavior tree expressed as a plain switch
in C — we explicitly do *not* import a BT library.

### Example — BOT_GOAL_ENGAGE

```
sequence:
  acquire_aim(target_mech)
  if (in_grapple_range && grapple_node_above): try grapple-fire
  if (line_of_sight_clear):
    fire_active_slot
    if (under_fire AND health < 40%): retreat_one_step
    else: strafe_or_advance
  else:
    move_along_path(toward_target_node)
```

### Example — BOT_GOAL_PURSUE_PICKUP

```
sequence:
  follow_path_to(goal_target_node)
  if (at_node): pickup will auto-grab via pickup_step touch
```

### Example — BOT_GOAL_GRAB_FLAG

```
sequence:
  follow_path_to(flag_node)
  on_arrival: ctf_pickup fires server-side; switch to BOT_GOAL_CAPTURE
```

The trees are tiny and the leaves all set fields in a `BotWants`
struct that the motor reads:

```c
typedef struct BotWants {
    Vec2     move_target;       // where to head, world px
    Vec2     aim_target;         // where to aim, world px
    bool     want_fire;
    bool     want_jet;
    bool     want_jump;
    bool     want_grapple_fire;
    bool     want_grapple_release;
    bool     want_use;
    bool     want_reload;
    bool     want_swap;
    int      next_reach_kind;    // for the motor's jet/grapple gating
} BotWants;
```

## Layer 4 — motor (writes ClientInput)

The motor turns wants → buttons. This is where the physical knowledge
lives (how to climb a slope, when to release a jet pulse, how to aim
with a Mass Driver's arc).

Pseudocode for the per-tick motor:

```c
static ClientInput bot_motor(World *w, BotMind *m, const BotWants *wants) {
    ClientInput in = { .dt = 1.0f/60.0f };
    Vec2 pelv = mech_pelvis_pos(w, m->mech_id);

    /* Aim — apply jitter from personality. */
    Vec2 aim = wants->aim_target;
    float jit = m->pers.aim_jitter_rad;
    if (jit > 0.0f) {
        float a = pcg32_float01(&m->rng) * 2.0f * (float)M_PI;
        float r = pcg32_float01(&m->rng) * jit * 600.0f;
        aim.x += cosf(a) * r;
        aim.y += sinf(a) * r;
    }
    in.aim_x = aim.x; in.aim_y = aim.y;

    /* Move-direction: horizontal. */
    float dx = wants->move_target.x - pelv.x;
    if (fabsf(dx) > 16.0f) {
        in.buttons |= (dx > 0) ? BTN_RIGHT : BTN_LEFT;
    }

    /* Jet: explicit when the path needs JET, or chasing high targets. */
    if (wants->want_jet) in.buttons |= BTN_JET;

    if (wants->want_jump) in.buttons |= BTN_JUMP;
    if (wants->want_fire) in.buttons |= BTN_FIRE;
    if (wants->want_grapple_fire) in.buttons |= BTN_FIRE_SECONDARY;
    if (wants->want_grapple_release) in.buttons |= BTN_USE;
    if (wants->want_use) in.buttons |= BTN_USE;
    if (wants->want_reload) in.buttons |= BTN_RELOAD;
    if (wants->want_swap) in.buttons |= BTN_SWAP;

    return in;
}
```

### Aim model

The aim model is the single most-felt aspect of bot difficulty.

We use a **two-stage** model:

1. **Tracking** — every tick, slide `current_aim` toward `target_aim`
   at a per-tier maximum rate (`aim_slew_rad_per_tick`). This caps
   how fast the bot can *follow* a strafing target.
2. **Jitter** — on each fire tick, add a Gaussian noise sample
   (`aim_jitter_rad`). Caps single-shot accuracy.

Both are continuous parameters per tier. No branching code paths.

### Reaction time

`reaction_ticks` is enforced by *not updating* `goal_target_mech` for
N ticks after an enemy first appears in the awareness radius. Recruit
tier reacts in 30 ticks (500 ms); Champion in 2 ticks (33 ms).

### Lead estimation

Hitscan vs projectile: for projectile weapons the motor estimates lead
using `target_velocity × projectile_time_of_flight`. Per-tier
`aim_lead_frac` scales the lead from 0 (no lead — bullets miss
moving targets) to 1 (perfect lead, frame-accurate).

## Difficulty tiers

Concrete parameter table — these are starting numbers to tune in
playtest:

| Tier | aim_jitter_rad | aim_slew/tick | reaction_ticks | awareness_px | pickup_pri | flag_pri | grapple_pri | aggression | knows_full_map | uses_powerups |
|---|---|---|---|---|---|---|---|---|---|---|
| Recruit  | 0.18 (≈10°)  | 0.04 | 30 | 500  | 0.20 | 0.10 | 0.00 | 0.30 | false | false |
| Veteran  | 0.08 (≈4.5°) | 0.10 | 9  | 900  | 0.55 | 0.50 | 0.15 | 0.55 | true  | false |
| Elite    | 0.03 (≈1.7°) | 0.18 | 4  | 1300 | 0.80 | 0.75 | 0.45 | 0.75 | true  | true  |
| Champion | 0.005 (≈0.3°)| 0.32 | 2  | ∞    | 1.00 | 1.00 | 0.85 | 0.95 | true  | true  |
| Trained  | (from .onnx — replaces motor + tactic layers entirely) | | | | | | | | | |

Knob ranges for reference (from the existing tunables in CURRENT_STATE):

- `RUN_SPEED_PXS = 280` (px/s)
- `JET_THRUST_PXS2 = 2200`
- `JET_DRAIN_PER_SEC = 0.60`
- `GRAPPLE_MAX_REST_LEN = 300` (px)
- `GRAPPLE_RETRACT_PXS = 800`

Aim-jitter degrees → radians: 10° = 0.175 rad; 5° = 0.087; 1.7° =
0.030; 0.3° = 0.0052. Reaction tick count to milliseconds at 60 Hz:
30 = 500 ms (Recruit), 9 ≈ 150 ms (Veteran), 4 ≈ 67 ms (Elite),
2 ≈ 33 ms (Champion — one render frame at 60 fps).

### Why continuous parameters, not branching

Branching `if (tier == RECRUIT) ...` paths become a maintenance
nightmare. With continuous knobs we can A/B test a "Veteran with
Elite aim" or a "Recruit with Champion grapple priority" without
forking the code path. Tiers are just preset bundles.

## Per-mode bot behavior

The strategy layer's scorer set varies slightly per mode:

### FFA

Goals available: IDLE, ENGAGE, REPOSITION, PURSUE_PICKUP, RETREAT.
The CTF goals are scored 0. A simple deathmatch loop.

### TDM

Same goals as FFA, but `nearest_visible_enemy` filters by team. Plus
a "team-mate awareness" — bots ENGAGE more aggressively when a
teammate is nearby (within 400 px), inspired by Soldat's
group-courage shape.

### CTF

All goals enabled. Carrier penalty (per `06-ctf.md`: half-jet, no
secondary) is reflected in the scorer for BOT_GOAL_ENGAGE while
carrying (drops to 30% of baseline — carriers should run, not fight).
Defenders score BOT_GOAL_DEFEND_FLAG higher when no teammate is at
the base.

## Integration points

### With the bake-test harness

`tools/bake/run_bake.c` already constructs each bot's
`ClientInput` itself. Replace its `bot_tick` function with a call
to `bot_step(&bs, &g.world, &g, dt)` after spawning the mechs. Each
bake bot gets a tier from a CLI flag (default Veteran).

Expected impact: 7% → 50%+ heatmap coverage on Foundry; ≥80% pickup
grab rate within 60 s; CTF flag captures in Crossfire/Citadel within
2-3 minutes.

### With the lobby + match flow

The host config gets a new section:

```ini
# soldut.cfg
bots = 4              # how many bots to fill the lobby with
bot_tier = veteran    # recruit / veteran / elite / champion / trained
```

When the host starts a round and there are fewer human peers than
`min_players`, the lobby spawns bot slots up to the bot count. Bot
slots have negative slot IDs (e.g., `-(i+1)` so `-1` is the first
bot) and aren't sent in lobby-network state to peers; they exist
only on the host. Server-side, bot mechs are created in the same
`lobby_spawn_round_mechs` flow with a flag that calls
`bot_attach(mech_id, tier)`.

Clients see bot mechs exactly like remote-human mechs — same
snapshot, same EntitySnapshot wire format. Bots are server-only
state; the wire doesn't care which controller drives a mech's
`latched_input`.

### Single-player mode

`game.auto_start_single_player` already exists. The single-player
title path bootstraps with `bots = 7, bot_tier = veteran` so a new
player gets a populated practice arena immediately. Tier is
selectable from the title screen.

## Performance budget

Target: under **0.5 ms/tick** for 32 bots collectively, which means
about 15 µs per bot per tick.

Breakdown:

| Pass | Frequency | µs / bot | µs / tick (32 bots) |
|---|---|---|---|
| Motor | every tick | 3 | 96 |
| Tactic | every tick | 4 | 128 |
| Strategy scorer | every 6 ticks | 50 / 6 ≈ 8 | 256 |
| A* replan | on goal change | 200 amortized over 60 ticks ≈ 3 | 96 |
| Nav graph build | once per `map_build` | — | 0 (one-time 30 ms) |
| Total | | ~18 µs | ~576 µs ≈ 0.58 ms |

We accept ~0.58 ms on the high end. If we're over budget after first
implementation, the strategy scorer is the biggest lever — drop it
to every 12 ticks (~5 Hz) and the total halves.

## Optional: self-play training (M6+ stretch)

Once classical is shipped and the in-game bot mode is playable, the
self-play path becomes attractive. Outline:

### Training environment

A new entry point `./soldut --train` that:

1. Spawns N (e.g., 16) parallel `Game` instances in worker threads.
2. Each instance runs a 5-minute match with 4 bots (the trainee + 3
   opponents drawn from the policy zoo).
3. Each tick collects (observation, action, reward) for the trainee.
4. Observations: bot's pelvis pos / vel, nearest 4 enemies' pos / hp,
   nearest 8 nodes' pos / pickup_kind / availability, ammo, fuel,
   active_slot. Total ~64 floats.
5. Actions: 13-bit button mask + (aim_x, aim_y) — flatten to a
   discrete-15-class movement head + a continuous-2D aim head. Two
   separate policy heads sharing an encoder.
6. Reward: +10 per kill, +1 per pickup grab, +50 per flag capture,
   -5 per friendly-fire damage, +0.01 per tick alive, -1 per death.

### PPO + curriculum + self-play

Use a small PyTorch model (MLP, ~200 KB of params) trained with PPO
(`stable-baselines3` or a hand-rolled C/PyTorch trainer; we don't
ship the trainer in the game binary). Curriculum:

1. **Stage 1**: 1v1 on Foundry against `BOT_TIER_RECRUIT`. Reward
   for survival + occasional kill. ~10M timesteps.
2. **Stage 2**: 4-bot FFA on Foundry/Reactor against Veteran. ~30M.
3. **Stage 3**: Self-play against past checkpoints (policy zoo of
   the last 8 checkpoints, sampled randomly each match). ~50M.
4. **Stage 4**: CTF on Crossfire/Citadel with team-play reward
   (assists, returns, captures). ~50M.

Recent research (MDPI 2024) shows **curriculum learning outperforms
behavior cloning by 23.67% in average victories** for FPS bots, and
self-play is essential to avoid overfitting to fixed opponents
(*Reinforcement Learning as an Approach to Train Multiplayer FPS Game
Agents*, 2024).

DeepMind's Quake III CTF agents (2019) demonstrate that self-play +
distributed training produces team-coordination behavior without
explicit reward shaping. Their reward was just "team score delta";
the team play emerged from the multi-agent training dynamics.

### Inference at runtime

Trained policy exports to ONNX (~200 KB). Runtime loads via
[`onnxruntime`](https://onnxruntime.ai) — single static library, C
API, MIT license. We vendor it under `third_party/onnxruntime/`
(growing the dependency list from 4 to 5 — write down in
`01-philosophy.md`).

The `BOT_TIER_TRAINED` motor replaces the classical layers 2-4 with
a forward pass through the ONNX policy. Layer 1 (nav graph) still
generates and is exposed to the policy via the observation vector.

Inference cost: ~30 µs per bot per tick (200 KB model, ~50K FLOPs).
Fits in budget.

### What this buys us

- An opponent that genuinely improves with training time.
- A "play vs the latest" tier that grows as we train.
- A research lane for the project — Soldut becomes a benchmark.

What it costs:
- 1-3 weeks of trainer engineering (Python tooling, env wrapper).
- ~1 GPU-week of training time per major version.
- The onnxruntime dependency (small, but tracked).
- A maintenance burden for the trainer (the env shape must follow
  any sim/protocol changes).

### Why we don't ship the trainer in the game

The trainer is a development tool, not a game feature. Players load
ONNX policies; they don't train them. Same shape as `cook_maps` —
build-time-only.

## Done when

- `src/bot.{c,h}` exists. `BotSystem`, `bot_init`, `bot_step`,
  `bot_attach`, `bot_detach`, `bot_destroy` are wired.
- `tools/bake/run_bake.c` uses `bot_step` instead of its inline
  `bot_tick`; the heatmap coverage on every shipped map exceeds
  40% of playable cells at Veteran tier over a 60 s bake.
- A single-player practice mode is selectable from the title screen
  and spawns 7 Veteran bots on a chosen FFA map.
- A 4v4 CTF match against a mix of bots on Crossfire completes with
  at least one capture per side.
- `bot_personality_for_tier(BOT_TIER_*)` returns the table above;
  per-tier behavior is visibly distinct (Recruit misses; Champion
  doesn't).
- The classical implementation fits the 0.5 ms/tick budget at 32
  bots (measured via the existing `PROF_BLOCK` if we add one — or
  by inspection if we don't).
- `documents/11-roadmap.md` §"Stretch goals (post-v1)" updates the
  Bots line to point at this doc and acknowledges Champion is the
  v1 top tier; Trained is the M6+ stretch.

The PPO self-play path is a **separate done-when** for M6+:

- `tools/train/` exists, headless `./soldut --train` runs N parallel
  envs, ONNX export works.
- A trained policy beats Champion in head-to-head over 100 matches
  on Foundry.
- `BOT_TIER_TRAINED` loads `assets/bot/policy.onnx` at startup and
  uses it for one full FFA round.

## Trade-offs to log

These should land in `TRADE_OFFS.md` as the bot module ships:

- **No human-readable navmesh debug overlay.** The nav graph is built
  in memory; the editor doesn't visualize it. Designers can't
  preview "where will the bot route?" without running a bake.
  Revisit when bake iteration becomes a routine designer workflow.
- **Aim model is two-stage (slew + jitter); no recoil-aware
  prediction.** A bot doesn't account for its own weapon's recoil
  when picking burst-fire vs single-fire. Tolerated; doesn't read
  visibly wrong against most weapons.
- **No team formation behaviors at v1.** Bots don't pair up or
  cover each other. Acceptable for a 4v4 baseline; revisit when
  team play is a player complaint.
- **The 16-node path-prefix cap means very long routes truncate.**
  Bots replan when they reach the end of a prefix; on Citadel
  base-to-base routes can require 25+ nodes which means a mid-route
  replan. Tolerated; bake should show this as a brief pause rather
  than visible jitter.
- **No bot voice / chat.** Lobby chat shows bot names but they're
  silent. Tolerated; chatter is M6 polish.
- **Recruit tier's aim jitter is uniform per-shot, not
  weapon-aware.** A Recruit holding a Rail Cannon shouldn't miss
  more than they would with a Sidearm (the Rail is supposed to be
  the precision weapon). Tolerated at first ship; eventually the
  jitter should scale by weapon's spread.

## References

The architecture borrows directly from the Quake III Arena bot
(Mr. Elusive / Jean-Paul van Waveren, 2001) — specifically the AAS
navigation system + layered command structure. We adapt it to 2D
Verlet-skeleton mechs by adding GRAPPLE and JET reachability verbs.

Recent FPS-bot research informs the PPO self-play stretch:

- *Reinforcement Learning as an Approach to Train Multiplayer FPS
  Game Agents* (MDPI Technologies, 2024) — curriculum learning beats
  behavior cloning by ~23%.
- *A Modular Reinforcement Learning Framework for Iterative FPS
  Agent Development* (MDPI Electronics, January 2026) — separable
  movement / combat / strategy nets.
- *Human-level performance in 3D multiplayer games with population-
  based reinforcement learning* (DeepMind, Quake III CTF, Science
  2019) — proof that self-play + multi-agent training discovers
  team play without reward shaping.

Implementation references:

- *The Quake III Arena Bot* (Jean-Paul van Waveren, 2001) — the
  canonical thesis on AAS + layered bot architecture.
- *Programming Game AI by Example* (Mat Buckland, 2005) — utility
  AI scorer patterns we use for the strategy layer.
- *Behavior Trees in Robotics and AI* (Colledanchise & Ögren, 2020)
  — why we use a hand-rolled switch instead of a BT library.
