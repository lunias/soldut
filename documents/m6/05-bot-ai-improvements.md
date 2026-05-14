# M6 P05 — Bot AI improvements (behavior + tactics + team play)

**Status:** plan, not built. Branch `bot-ai-improvements` is set up
off `main`.

**Trigger:** post-merge of M6 P04 the bot AI is functional but
brittle. A 1v1 bake sweep (8 maps × 4 tiers, 60 s/match, single
seed) reveals that on 5 of 8 maps even Champion-tier bots produce
**zero fires** — they cannot find each other. Bots also have no
concept of cover, no concept of weapon-appropriate engagement
range, no team-role coordination in CTF, and the matrix is
dominated by Mass Driver (76 kills, next-closest 67) because of the
above gaps, not because of a deliberate balance choice.

This document is the **diagnosis + plan** for a single fresh Claude
session to pick up and implement. It is written so the
implementing session can pick it up cold, with no surrounding chat
context.

---

## 0 — Required reading first

Three project docs override every choice below if they disagree:

- [`documents/01-philosophy.md`](../01-philosophy.md) — how we
  write code here. Sean Barrett / Casey Muratori / Jonathan Blow
  lineage. Specifically: rule 1 (data layout is API design), rule
  2 (pure functions where possible), rule 3 (allocate once), rule
  5 (no premature reusable code), rule 7 (commit to numbers),
  rule 9 (one way to do each thing).
- [`documents/00-vision.md`](../00-vision.md) — pillar 1
  (**Movement is the protagonist**) and pillar 6 (it runs on a
  laptop). The bot must use jets / slides / grapples like a real
  player; we still owe sub-0.5 ms / tick at 32 bots.
- [`documents/13-bot-ai.md`](../13-bot-ai.md) — the bot module's
  design canon. Goal of this pass is to *finish* the layered
  classical AI to the level the doc describes, not replace it.

Two M6 docs are the immediate prior art — read them to know what
already shipped:

- [`documents/m6/04-map-balance.md`](04-map-balance.md) — the
  matrix sweep tooling, the per-map character briefs, the iter7
  numbers.
- The latest [`CURRENT_STATE.md`](../../CURRENT_STATE.md) entry
  (M6 P04 post-merge polish) — describes the current bot file's
  shape: nav graph, opportunistic fire, charge-weapon hold,
  jet-fuel hysteresis.

---

## 1 — TL;DR

We're adding **five capabilities** to `src/bot.{c,h}` and keeping
the rest of the architecture intact. The order below is the
recommended implementation order — each phase produces a
measurable improvement in the matrix.

| # | Capability                          | Adds            | Eliminates                                 |
|---|-------------------------------------|-----------------|--------------------------------------------|
| 1 | Per-node visibility precompute      | new nav data    | "no LOS → can't engage" failure on 5 maps  |
| 2 | Cover + engagement-position search  | tactic layer    | bots idle in spawn corner with LOS blocked |
| 3 | Weapon-range gating on ENGAGE       | strategy layer  | Riot Cannon firing 4 833 pellets per kill  |
| 4 | Aggression / health-aware retreat   | strategy layer  | bots fight to death even at 5% HP          |
| 5 | CTF team-role coordination          | strategy layer  | every bot independently rushes the flag    |

Plus one out-of-AI tweak:

| # | Capability               | Touches      | Eliminates                            |
|---|--------------------------|--------------|----------------------------------------|
| 6 | Mass Driver balance pass | `weapons.c`  | matrix-dominant weapon (76 vs 67)     |

Total: ~600 LOC of new C, ~80 LOC of nav-build, two new fields on
`BotMind`, no new pools, no new wire messages, no new third-party
deps. Sub-0.5 ms / tick budget remains intact.

---

## 2 — Diagnostic baseline (the data that motivates this pass)

### 2.1 — 1v1 bake sweep

Run on `bot-ai-improvements` branch tip (commit `0b271d1` plus
the bot improvements that landed in M6 P04 polish). One seed
`0xC0FFEE`, 60 s/match, default loadout via
`bot_default_loadout_for_tier`.

```
                                       fires  kills
recruit    foundry      FAIL                  0      0
recruit    slipstream   PASS                 34     1   (Trooper kills Scout)
recruit    reactor      FAIL                  0      0
recruit    concourse    FAIL                  0      0
recruit    catwalk      FAIL                  0      0
recruit    aurora       PASS                 36     1
recruit    crossfire    FAIL                  0      0
recruit    citadel      FAIL                  0      0
veteran    foundry      FAIL                  0      0
veteran    slipstream   PASS                 43     1
veteran    reactor      FAIL                  0      0
veteran    concourse    FAIL                  0      0
veteran    catwalk      FAIL                  0      0
veteran    aurora       PASS                  9     1
veteran    crossfire    FAIL                  0      0
veteran    citadel      FAIL                  0      0
elite      foundry      PASS                 33     0
elite      slipstream   PASS                163     0
elite      reactor      FAIL                  0      0
elite      concourse    FAIL                  0      0
elite      catwalk      FAIL                  0      0
elite      aurora       FAIL                  0      0   ← regression from V/R
elite      crossfire    FAIL                  0      0
elite      citadel      FAIL                  0      0
champion   foundry      PASS                 77     1
champion   slipstream   PASS                 36     1
champion   reactor      FAIL                  0      0
champion   concourse    FAIL                  0      0
champion   catwalk      FAIL                  0      0
champion   aurora       FAIL                  0      0   ← regression from V/R
champion   crossfire    FAIL                  0      0
champion   citadel      FAIL                  0      0
```

**Diagnosis from the data:**

- **Reactor / Concourse / Catwalk / Crossfire / Citadel** produce
  0 fires at EVERY tier — including Champion (which has infinite
  awareness). That means awareness isn't the issue; **LOS is the
  issue**. The bots can't see each other and can't reposition to
  fix that.
- **Foundry** works at Elite / Champion but fails at Recruit /
  Veteran. The wider-awareness tiers compensate for sub-optimal
  positioning; the narrow-awareness tiers don't.
- **Aurora regressed at high tiers** — Recruit / Veteran score
  kills, Elite / Champion don't. Likely cause: the `pursue_enemy`
  baseline (0.40 × aggression-factor) loses to PURSUE_PICKUP on
  Aurora's pickup-dense surface, AND the higher-tier wider paths
  make bots overshoot the engagement window. Worth re-confirming.
- **8-bot bakes are NOT broken** — `tools/bake/run_loadout_matrix.sh
  iter7` results from M6 P04 show all 8 maps PASS at Champion.
  The 1v1 case is harder than 8v8 because: (a) only one target to
  pursue, (b) no Brownian motion from a crowd, (c) the strategy
  scorer's PICKUP / REPOSITION pull is enough to hold a single bot
  in its spawn quadrant.

### 2.2 — What the bot does that's wrong

Walking `src/bot.c::run_strategy` end-to-end against the 1v1
Reactor case:

1. Bot 0 spawns at the LEFT edge (x = 192). Bot 1 spawns at the
   RIGHT edge (x = W - 192).
2. `score_engage` → 0 because `find_nearest_enemy` requires LOS
   clear; the pillar blocks every ray.
3. `score_pursue_pickup` → 0.55 × 0.7 × 0.8 = ~0.30 for a nearby
   AMMO_PRIMARY pickup. Wins.
4. Bot walks to its alcove, grabs AMMO. Pickup goes COOLDOWN.
5. `score_pursue_pickup` drops to next-best pickup. Bot wanders.
6. `score_pursue_enemy` returns 0.40 × (0.4 + 0.6 × 0.55) =
   **0.292** at Veteran — still loses to a fresh AMMO pickup.
7. Meanwhile the opportunistic-fire fallback only fires if LOS
   is clear (it isn't), so no shots happen.
8. After ~60 s the spawn-side pickups respawn one by one; the
   bot orbits its alcoves indefinitely.

**The root cause is structural:** the bot's strategy says "engage
when an enemy is LOS-clear" and "pursue when ENGAGE doesn't fire,"
but it has no way to compute "go to a position from which I CAN
see the enemy." Without that primitive, bots stuck on one side of
a wall never close.

---

## 3 — Research summary (prior art we're standing on)

### 3.1 — Quake III (Jean-Paul van Waveren, 2001) — the canonical reference

Quake III's bot library has FOUR layers:

1. **AAS (Area Awareness System)** — precomputed convex areas +
   reachabilities. ~50 % of the bot codebase. Stores walking,
   jumping, swimming, and **grapple** reach edges *and* per-area
   visibility data (which areas can see which from where).
2. **Goal layer** (`be_ai_goal.c`) — stack-based, fuzzy-weighted.
   Goals: level items (pickups), camp spots, map locations, roam
   waypoints. Each item has a per-class weight function and an
   *avoidance* timer so the bot doesn't re-target a pickup it
   just visited.
3. **State / situation layer** (`be_ai_chat.c`, `be_ai_weight.c`)
   — fight-vs-flee decisions, weapon selection, chat lines.
4. **Top-level FSM** (`ai_dmnet.c`) — `AINode_Battle_Fight`,
   `AINode_Battle_Chase`, `AINode_Battle_Retreat`, `Battle_NBG`
   (nearby goal during combat), `Seek_LTG` (long-term goal), etc.

Decision functions in `ai_dmq3.h`:

- `float BotAggression(bot_state_t *bs)` — 0..100, computed from
  HP + armor + ammo + weapon-matchup-vs-enemy.
- `int BotWantsToRetreat(bot_state_t *bs)` — comparing aggression
  vs a flee threshold.
- `int BotWantsToChase(bot_state_t *bs)` — comparing aggression
  vs a chase threshold.

**CTF coordination** lives in `ai_dmq3.c::BotCTFSeekGoals`:

- `LTG_RUSHBASE` — bot is carrying the flag, race home.
- `LTG_TEAMACCOMPANY` — escort a teammate carrying the flag.
- `LTG_GETFLAG` — attack the enemy flag.
- `LTG_DEFENDKEYAREA` — sit near our flag.
- `TEAMTASK_OFFENSE` / `TEAMTASK_DEFENSE` / `TEAMTASK_ESCORT` —
  the team-leader bot assigns roles; without a leader, each bot
  rolls per its `teamtaskpreference` weight.

### 3.2 — F.E.A.R. (Jeff Orkin, 2005) — GOAP

- ~70 goals × ~120 actions per character type. Designers assign
  subsets per enemy type.
- Plans are SHORT — typically 1–2 actions, rarely 3–4. Re-plans
  on world-state changes.
- A* over the action space (not over space). World state is a
  predicate set.
- Tactical patterns: cover use, blind fire, flank, throw grenade,
  flush out, retreat. Each is an action with preconditions +
  effects, NOT a goal.

We will NOT adopt GOAP. It is the right shape for "many tactical
options per situation"; our problem is "the bot needs to know
*where to be* to engage." Utility scorer + per-goal switch
remains the right shape for Soldut.

### 3.3 — Modern human-like bots (2024–2026)

- *Reinforcement Learning Applied to AI Bots in First-Person
  Shooters* (MDPI 2023) — curriculum learning + self-play
  outperforms behavior cloning by ~23 %.
- *Human-like Bots for Tactical Shooters Using Compute-Efficient
  Sensors* (Microsoft 2025, CSGO) — 225 visual rays + 8 audio
  directions + game-state, behavior-cloned policy. 30 % of human
  evaluators misclassified the bots as human; ~16 obs-action
  pairs/sec.
- *Counter-Strike Deathmatch with Large-Scale Behavioural
  Cloning* (Pearce et al, 2022) — behavior cloning from 5K hours
  of human Twitch streams.

These are out of scope for v1 (the doc's PPO self-play tier is
M6+ stretch and adds onnxruntime as a fifth third-party). The
takeaway we use: human players almost always *move while
shooting*, *break LOS when hurt*, and *take cover that geometry
provides*. Our bot needs the same primitives.

### 3.4 — Cover-aware utility AI

From Roblox AI dev forum + UE marketplace bot frameworks +
*Close Quarters Development* (gamedev.net, 2014):

- Precompute per-nav-node **visibility data** at level load — for
  each node, which other nodes can see it. ~24-byte direction
  bitmask per node is common (Q3 AAS shape).
- **Flank paths** = A* toward the target with cost penalty for
  exposed nodes in the enemy's firing cone.
- **Cover scoring** = breadth-first search from current position
  for the first node whose visibility set excludes the enemy.

### 3.5 — Weapon range gating

Standard practice across ARMA / CSGO / Halo bots:

- Discretize distance into close / medium / far buckets.
- Each weapon has an effective-range band; the bot only engages
  within that band.
- For spread weapons (Riot Cannon equivalent), the effective
  range is much shorter than `range_px` because pellets disperse;
  the bot wants to fire when ALL pellets land in the target's
  hitbox.

---

## 4 — The five work areas

### Phase 1 — Per-node visibility precompute (the load-bearing change)

**Problem:** the bot has no concept of "from which positions can I
see / be seen by the enemy?" Every reach-aware decision today
goes through `los_clear(world, A, B)`, which does a ray cast —
fine for one-off checks, but the strategy/tactic layers want
"give me a node FROM WHICH this enemy is visible," and we don't
have that.

**Approach:** at `bot_system_build_nav` time, after reachabilities
are emitted, compute per-node visibility. For each pair of nodes
(N, M), cast `level_ray_hits(N.pos, M.pos)`; if clear, mark M as
visible from N. Store as a bitset.

For Citadel-scale graphs (~225 nodes), the matrix is 225 × 225 =
50 625 entries × 1 bit = 6.3 KB. For Foundry (~60 nodes), 450 B.
Tiny. The build cost is the bigger concern — Citadel needs ~50 k
ray casts. Each ray cast is roughly 4 µs in our current `level_ray_hits`
(8-cell DDA + polygon broadphase) → 200 ms. **Too slow** for level
load. Mitigations, in order of effort:

1. **Distance prune**: skip pairs farther apart than
   `BOT_VIS_MAX_PX = 1600.0f`. Halves the work on big maps.
2. **Symmetry**: visibility is symmetric (LOS A↔B), so only test
   the upper triangle. Halves again.
3. **Per-pair cap**: visibility data is most useful for nearby
   nodes — within ~3 reach-hops. Cap at the first 32 nearest
   nodes per source (k-NN via the existing nav_grid spatial hash).

With those three optimisations, Citadel becomes 225 × 32 / 2 =
3 600 ray casts ≈ 14 ms. Fits in the 30 ms map-build budget the
M5 P18 doc commits to.

**Touchpoints:**

```c
// src/bot.c
struct BotNav {
    BotNavNode  nodes  [BOT_NAV_MAX_NODES];
    int         node_count;
    BotNavReach reaches[BOT_NAV_MAX_REACH];
    int         reach_count;
    /* M6 P05 — per-node visibility. `vis[i]` is a bitset of
     * which nodes are line-of-sight visible from node i. Only
     * the lower 32 nearest neighbours per node are tested; bit
     * positions are indexes into BotNavNode.vis_targets[] (a
     * per-node array of 32 target node ids). */
    uint32_t    vis_mask[BOT_NAV_MAX_NODES];
    int16_t     vis_targets[BOT_NAV_MAX_NODES][32];
    uint8_t     vis_target_count[BOT_NAV_MAX_NODES];
    float       level_w_px;
    float       level_h_px;
    int         tile_size;
};
```

Plus a new helper:

```c
/* True if nav node M is in node N's precomputed visibility set.
 * O(1) — bit test against the 32-bit mask. */
static bool nav_node_sees(const struct BotNav *nv, int n, int m);

/* Iterate the visible targets from node n, calling `cb` for each.
 * Used by engagement-position search. */
typedef void (*BotVisCb)(int target_id, void *user);
static void nav_visit_visible(const struct BotNav *nv, int n,
                              BotVisCb cb, void *user);
```

**Acceptance criteria:**

- `bot_nav: built N nodes, M reachabilities, K visibility edges
  on WxH map` log line shows nonzero K on every shipped map.
- New `tests/bot_nav_test.c` (or extend `tests/mech_ik_test.c`'s
  pattern): build the nav for `level_build_tutorial`, assert that
  the per-node visibility bitset is symmetric (if A sees B, B
  sees A) and that at least one node-pair has visibility.
- 30 ms map-build budget holds on Citadel (instrument with
  `mono_seconds()` around the call in `bot_system_build_nav`).

### Phase 2 — Cover-aware engagement positioning

**Problem:** when LOS to the enemy is blocked, today's
`tactic_engage` does:

```c
if (!los) {
    out->move_target = ep;            /* enemy pelvis */
    if (ep.y < mp.y - 64.0f) out->want_jet = true;
    return;
}
```

That's "walk in a straight line toward the enemy." On Reactor /
Concourse / Catwalk / Crossfire / Citadel that wall in the way
means the bot walks STRAIGHT INTO IT. The bot needs a position
that has LOS to the enemy AND is reachable.

**Approach:** introduce a `BotPositionPick` primitive used by
`tactic_engage` (and by Phase 4's avoidance). Given a target
position + a query type, return a nav node id.

```c
typedef enum {
    BOT_POS_ENGAGE,   /* visible from this node TO target */
    BOT_POS_COVER,    /* NOT visible from this node TO target */
    BOT_POS_FLANK,    /* visible to target, not on the direct line */
} BotPosQuery;

static int nav_pick_position(const struct BotNav *nv,
                             Vec2 from, int target_node,
                             BotPosQuery q,
                             float max_walk_dist_px);
```

Implementation: a BFS from `nav_nearest_node(from)` over WALK +
JUMP + JET reachabilities up to `max_walk_dist_px`, scored by:

- For BOT_POS_ENGAGE: prefer nodes that ARE in `target_node`'s
  vis_mask; sub-score by proximity to target (closer is better
  up to `weapon_optimal_range_px`, see Phase 3).
- For BOT_POS_COVER: prefer nodes that are NOT in `target_node`'s
  vis_mask; sub-score by proximity (we want close cover, not
  cross-map cover).
- For BOT_POS_FLANK: ENGAGE nodes whose direction from the
  target is ≥ 60° off the source's bearing-to-target.

Use BFS up to 12 expansions (= ~12 × 96 px = 1 km). Cap budget
so the scorer remains cheap; if no answer in the budget, return
-1 and the bot falls back to "walk toward enemy."

Then in tactic_engage:

```c
if (!los) {
    int eng_node = nav_pick_position(bs->nav, mp,
                                     mind->goal_target_node_for_enemy,
                                     BOT_POS_ENGAGE,
                                     1200.0f);
    if (eng_node >= 0) {
        /* Plan a path to eng_node — re-use bot_plan_path. */
        bot_plan_path(mind, bs->nav, start_node, eng_node);
        out->move_target = bs->nav->nodes[eng_node].pos;
    } else {
        out->move_target = ep;    /* fall back to existing behavior */
    }
    return;
}
```

Add a per-mind cached `engagement_node` so we don't re-pick every
strategy tick — refresh only when LOS to enemy changes or every
~2 s.

**Acceptance criteria:**

- 1v1 bake on Reactor at Veteran tier produces > 0 fires.
- 1v1 bake on Concourse / Catwalk / Crossfire / Citadel at
  Veteran tier each produce > 0 fires.
- Per-mech distance traveled column in the per-mech TSV shows
  bots actually moved (> 500 px over 60 s) on every map.

### Phase 3 — Weapon-range gating + per-weapon engagement style

**Problem:** Riot Cannon's iter7 stats — 48 329 fires, 10 kills,
4 833 fires-per-kill. The pellet cone disperses across the screen
because the bot fires from any distance regardless of effective
range. Same problem in milder form on Microgun (615 fires/kill).

**Approach:** define a per-weapon **effective engagement range**
table, separate from `Weapon.range_px` (which is the absolute
max). When the bot's nearest enemy is outside this range,
`score_engage` returns the LOWER of {current score, a damped
value} — the bot still wants the enemy, but PURSUE
(close-the-distance) wins over ENGAGE (fire from here).

The damped-engage score lets the bot still spray a few rounds at
long range when there's truly nothing else to do — but at much
lower priority. Soldat-style: a Heavy with Microgun at far range
isn't going to hit much but shouldn't be silent either.

```c
typedef struct WeaponEngagementProfile {
    float optimal_range_px;   /* sweet spot — full ENGAGE score here */
    float effective_range_px; /* outside this → damped score (0.25×) */
    float ideal_strafe_px;    /* preferred stand-off distance */
    bool  prefers_high;       /* sniper-style — pick elevated nodes */
} WeaponEngagementProfile;

static const WeaponEngagementProfile g_weapon_profiles[WEAPON_COUNT] = {
    [WEAPON_PULSE_RIFLE]   = { 600, 1200, 500, false },
    [WEAPON_PLASMA_SMG]    = { 350,  800, 350, false },
    [WEAPON_RIOT_CANNON]   = { 220,  450, 250, false },  /* tight! */
    [WEAPON_RAIL_CANNON]   = { 1200, 2400, 1100, true }, /* sniper */
    [WEAPON_AUTO_CANNON]   = { 700, 1400, 600, false },
    [WEAPON_MASS_DRIVER]   = { 700, 1600, 600, false },
    [WEAPON_PLASMA_CANNON] = { 600, 1300, 550, false },
    [WEAPON_MICROGUN]      = { 500, 1000, 450, false },
    /* Secondaries — bot rarely uses these as engagement weapons,
     * but Frag Grenades + Sidearm are the realistic fallbacks. */
    [WEAPON_SIDEARM]       = { 400,  900, 400, false },
    [WEAPON_BURST_SMG]     = { 350,  700, 350, false },
    [WEAPON_FRAG_GRENADES] = { 350,  700, 350, false },
    [WEAPON_MICRO_ROCKETS] = { 500, 1000, 500, false },
    [WEAPON_COMBAT_KNIFE]  = {  80,  140,  80, false },
    [WEAPON_GRAPPLING_HOOK]= {   0,    0,   0, false },  /* not for damage */
};
```

The numbers are guesses calibrated against `documents/04-combat.md`
ranges. Tune in playtest.

Use the profile in three places:

1. `score_engage` — multiply by a range-fit factor:
   - Inside optimal: 1.0
   - Optimal → effective: linear 1.0 → 0.5
   - Past effective: 0.15
2. `tactic_engage` — `ideal_dist` uses
   `g_weapon_profiles[active_weapon].ideal_strafe_px` instead of
   the hardcoded 400 px.
3. `nav_pick_position(BOT_POS_ENGAGE)` — sub-score by proximity
   to `optimal_range_px` rather than "closer is always better."

Plus: when `prefers_high == true` (Rail Cannon), the engagement
node picker biases toward nodes whose `pos.y < target.y - 64 px`.
That makes Snipers actually take vertical positions.

**Acceptance criteria:**

- Riot Cannon iter9 stats: fires-per-kill < 1500 (was 4 833).
- Bots with Rail Cannon visit Aurora's mountain peaks + Citadel's
  rampart catwalks (visible in heatmap as a "high band" of pickup
  / traffic).
- Mass Driver fires-per-kill unchanged (≈ 27) — the AOE weapon
  doesn't change behavior; we just tighten the firing window.

### Phase 4 — Aggression-based retreat + cover behavior

**Problem:** current `score_retreat` only fires below 30 % HP and
just walks to a far pickup. Bots fight to death at 5 % HP because
the retreat goal score linearly maxes out at 0.95 below 30 % but
ENGAGE also caps at ~0.95 (close × 0.4 + 0.6×health × aggression).
The damaged bot keeps firing because ENGAGE hasn't dropped enough.

**Approach:** copy Q3's idea — compute a per-bot **aggression**
score every strategy tick, and gate engage vs retreat against it.

```c
/* 0..1 score. 1 = "I want to fight." 0 = "I want to break
 * contact." Computed from health, armor, ammo, weapon match,
 * and time-since-last-damage. */
static float bot_aggression(const World *w, const BotMind *m, int mid);
```

Inputs:

- HP fraction (full weight)
- Armor fraction (half weight; armor is HP buffer)
- Ammo fraction of current active slot (low ammo → low aggression)
- Time since last damage taken (recently hurt → low aggression).
  Use `Mech.last_damage_taken` (already on the Mech struct).
- Personality `aggression` field (Recruit 0.30, Champion 0.95)
- Optional weapon-matchup bonus — if our weapon's optimal range
  matches enemy distance, aggression bumps.

When `bot_aggression < 0.30`:

- `score_engage` is heavily damped (×0.2)
- `score_retreat` is boosted to ~0.85
- `tactic_retreat` (new behavior) picks a `BOT_POS_COVER` node
  toward the nearest health/armor pickup
- The bot also drops to a *strafing* pattern instead of advancing

When `bot_aggression > 0.70`:

- `score_engage` is boosted ×1.2
- ENGAGE wins over PURSUE_PICKUP unless the pickup's `need_factor`
  is critical (low HP health pack, dry ammo)

**Touchpoints:**

```c
// src/bot.c
static float bot_aggression(...);          /* new */
static float score_engage(...) {           /* extend */
    ...
    float agg = bot_aggression(w, mind, mid);
    float weapon_fit = engagement_range_factor(...);   /* phase 3 */
    return close * health_factor * agg * weapon_fit;
}
static float score_retreat(...) {          /* extend — use agg */ }
static void tactic_retreat(...) {          /* rewrite */ }
```

**Acceptance criteria:**

- 1v1 bake at Recruit tier on Slipstream: bots that hit < 20 % HP
  break engagement (per-mech distance_traveled spikes,
  fires-per-tick drops to 0 for the wounded bot for ≥ 3 s).
- Per-mech `longest_streak` median improves vs. iter7 baseline —
  meaning bots that survive a fight don't immediately get killed
  in the next one because they retreated long enough to heal.
- Iter9 matrix `Heavy` chassis kill total > iter7's 68 (more
  sustained engagement, less suicide).

### Phase 5 — CTF team-role coordination

**Problem:** in CTF every bot independently scores `GRAB_FLAG`,
`CAPTURE`, `CHASE_CARRIER`, etc. With 4 bots per team on
Crossfire, all four want to rush the enemy flag at once — and our
flag is undefended. The iter7 matrix shows CTF maps producing
fires + kills, but designer-readable team play is missing: there's
no "Bot-3 is defending, Bot-1 is the runner" structure.

**Approach:** add a **team-role assigner** that runs at round
start + flag-state transitions, assigning each bot one of:

```c
typedef enum {
    BOT_ROLE_ATTACKER = 0,   /* rush enemy flag */
    BOT_ROLE_DEFENDER = 1,   /* camp near friendly flag */
    BOT_ROLE_FLOATER  = 2,   /* mid-map, support either side */
    BOT_ROLE_CARRIER  = 3,   /* assigned dynamically when this bot picks up flag */
} BotTeamRole;
```

Default split for 4 bots: 2 attackers, 1 defender, 1 floater. For
3 bots: 1A / 1D / 1F. For 8 bots: 4A / 2D / 2F.

The role assigner is a per-team server-side pass that walks the
in_use bot slots and writes `bs->minds[mech_id].team_role`. Runs:

- On round start (after `lobby_spawn_round_mechs`)
- When the friendly flag's status changes (dropped → carrier
  becomes the temporary attacker stack)
- When a bot dies — its role is reassigned to a teammate

Role-aware score adjustments in `run_strategy`:

| Role     | GRAB_FLAG | CAPTURE | DEFEND_FLAG | CHASE_CARRIER | RETURN_FLAG | ENGAGE |
|----------|-----------|---------|-------------|---------------|-------------|--------|
| Attacker | 1.5×      | 1.5×    | 0.3×        | 1.0×          | 1.0×        | 1.0×   |
| Defender | 0.3×      | 0.3×    | 1.6×        | 1.3×          | 1.4×        | 1.0×   |
| Floater  | 0.7×      | 0.7×    | 0.7×        | 1.0×          | 1.2×        | 1.2×   |
| Carrier  | n/a       | 2.0×    | 0×          | 0×            | 0×          | 0.3×   |

The carrier-while-carrying matrix already exists implicitly via
`ctf_is_carrier`; we just formalize it.

**For DEFENDER specifically**: introduce a new `score_defend_flag`
that's nonzero whenever the role is set AND the friendly flag is
HOME. The score should DROP if an enemy is detected near the flag
(in which case ENGAGE takes over for the defender too — they're
intercepting). Use the existing `score_chase_carrier` for the
"flag taken" case.

A `tactic_defend_flag` picks a `BOT_POS_ENGAGE` node within ~300
px of the friendly flag with LOS to common approach paths. The
bot stands there, scans, fires on visible enemies. Use the Phase
1 visibility data to find this node.

**Touchpoints:**

```c
// src/bot.h
typedef struct BotMind {
    ...
    uint8_t   team_role;   /* BotTeamRole */
} BotMind;

// src/bot.c
static void bot_assign_team_roles(BotSystem *bs, const World *w);
static float score_defend_flag(const World *w, const BotMind *m,
                               int mid, MatchModeId mode, int *out_node);
static void  tactic_defend_flag(...);

// main.c — call bot_assign_team_roles in start_round after
// lobby_spawn_round_mechs, and in ctf_step when flag state changes.
```

**Acceptance criteria:**

- 4v4 Crossfire bake at Champion shows non-overlapping bot roles:
  per-mech logs include the assigned role; defenders' heatmap
  density centers near their flag, attackers' density centers
  near the enemy flag, floaters distribute mid-map.
- Captures actually happen — `flags.csv` shows at least one
  HOME→CARRIED→HOME transition per side over a 5-minute bake.
- Defenders' kill-per-mech ratio is higher than attackers' (they
  fight on home turf with cover).

### Phase 6 — Mass Driver balance pass (out-of-AI tweak)

**Problem:** iter7 matrix has Mass Driver at 76 kills, next-best
Rail Cannon at 67. The MD's AOE radius + low fire rate combo
makes it the optimal weapon for the bot's "group fight at a
pickup" pattern.

**Approach:** small numeric tweak. The lever options are:

A. `Weapon.fire_rate_sec` 0.7 → 1.0 — slower fire, less DPS
B. `Weapon.aoe_radius`     96 → 72 — smaller blast, fewer multi-hit kills
C. Both at half magnitude.

I recommend (C) — splits the impact 50/50. Numbers in
`src/weapons.c`.

**Acceptance criteria:**

- Iter9 matrix sweep at Champion 60 s × 320 cells: Mass Driver
  kill total within ±10 % of Rail Cannon's. No weapon dominates
  by more than ~15 % of the matrix median.
- Per-map best-loadout matrix from iter7 shows Mass Driver
  winning 3 / 8 maps. Post-fix: Mass Driver wins ≤ 2 / 8.

---

## 5 — Implementation phases (recommended order)

The order matters because Phase 1 unlocks Phases 2 + 4 + 5.

```
 ┌────────────────────────────────────────────┐
 │ Phase 1 — per-node visibility precompute   │  load-bearing
 └─────────────────┬──────────────────────────┘
                   │
       ┌───────────┴─────────────┐
       ▼                         ▼
 ┌──────────┐              ┌────────────┐
 │ Phase 2  │              │ Phase 5    │
 │ cover    │              │ CTF roles  │
 │ engage   │              │ + defend   │
 └────┬─────┘              └─────┬──────┘
      │                           │
      ▼                           │
 ┌──────────┐                     │
 │ Phase 3  │                     │
 │ weapon   │                     │
 │ ranges   │                     │
 └────┬─────┘                     │
      ▼                           │
 ┌──────────┐                     │
 │ Phase 4  │                     │
 │ aggression│                    │
 │ retreat   │                    │
 └────┬─────┘                     │
      ▼                           │
 ┌─────────────────────────────────┐
 │ Phase 6 — Mass Driver balance   │
 │ (small numeric tweak in         │
 │  src/weapons.c — last so it's   │
 │  measured against the FIXED AI) │
 └─────────────────────────────────┘
```

Test each phase with a small bake before moving to the next. Run
the full 320-cell matrix sweep ONLY at the end (iter9) — that's
the comparison against iter7 for the report.

---

## 6 — Test plan

### 6.1 — Phase-1 acceptance

```bash
make cook-maps && make bake
# Inspect the log output for the visibility count:
./build/bake_runner reactor --bots 2 --duration_s 5 --tier veteran 2>&1 | \
    grep "bot_nav:"
# Expected: a line ending with "K visibility edges" where K > 100.
```

### 6.2 — Phase-2 acceptance (1v1 sweep — the core test)

Save a script `tools/bake/run_1v1_sweep.sh` (extend the existing
matrix script with `--bots 2`). Run before each phase and compare
fires + kills across the matrix.

```bash
for tier in recruit veteran elite champion; do
  for m in foundry slipstream reactor concourse catwalk \
           aurora crossfire citadel; do
    ./build/bake_runner $m --bots 2 --duration_s 60 --tier $tier \
        2>&1 | grep "PASS\|FAIL"
  done
done
```

Target after Phase 2: at least Veteran tier produces > 0 fires on
all 8 maps. Champion produces ≥ 1 kill on all 8.

### 6.3 — Phase-3 acceptance (weapon range)

```bash
# Riot Cannon specifically — confirm fires-per-kill dropped.
./build/bake_runner crossfire --bots 8 --duration_s 60 \
    --tier elite --primary "Riot Cannon" 2>&1 | grep "fires\|kills"
```

Target: matrix-wide Riot Cannon fires-per-kill < 1 500 (was 4 833).

### 6.4 — Phase-4 acceptance (retreat)

Add a SHOT_LOG line in `tactic_retreat` and `bot_aggression` when
aggression drops below 0.30. Run a 60 s Slipstream Recruit bake;
grep the log for retreat-entry events. Target: ≥ 1 retreat event
per bot over the run.

### 6.5 — Phase-5 acceptance (CTF roles)

```bash
./build/bake_runner crossfire --bots 8 --duration_s 120 \
    --tier elite 2>&1 | grep "role\|capture"
cat build/bake/crossfire.flags.csv     # should show captures
```

Target: ≥ 1 capture per team in a 2-minute bake. Per-bot heatmap
density shows defenders clustered near their base, attackers near
the enemy base.

### 6.6 — Phase-6 acceptance (Mass Driver balance)

Full matrix re-sweep:

```bash
./tools/bake/run_loadout_matrix.sh iter9 60 8 champion
```

Compare `iter9/matrix.tsv` per-primary aggregates vs iter7's:
Mass Driver should drop from 76 → ~60–65, Rail Cannon should
hold ~67. Aim: no weapon ≥ 15 % above the median.

### 6.7 — Regression suite

Every phase must pass:

```bash
make test-level-io test-pickups test-snapshot test-spawn \
     test-prefs test-map-chunks test-map-registry test-ctf \
     test-mech-ik test-pose-compute
bash tests/net/run_3p.sh
```

Net regression matters specifically for Phase 5 (CTF role
assignment is server-side; clients see role-driven behavior
through ordinary snapshot stream).

---

## 7 — Risks + things NOT to do

### Risks

- **Visibility precompute can blow the map-build budget on a
  Citadel-scale graph.** Mitigation: k-NN cap (32 nearest), upper-
  triangle, distance prune. If still slow, lower
  `BOT_VIS_TARGETS_PER_NODE` from 32 to 16.
- **Cover-aware engagement can produce oscillation** — bot picks
  engage_node, walks halfway, LOS opens, ENGAGE fires, then LOS
  closes again, repeat. Mitigation: cache `engagement_node` per
  mind with a ≥ 2 s refresh interval (re-uses the existing
  `last_replan_tick`).
- **Aggression-based retreat can produce permanent flee loops**
  if a bot's HP regen path is interrupted. Mitigation: a hard
  floor at 0.20 aggression below which the bot waits for an
  AVAILABLE health pickup before retreating further (= no
  re-flee, just stand and reload).
- **CTF role-assignment vs late-joining humans**: the role
  assigner runs at round start + flag transitions. If a human
  joins mid-round, no role gets assigned to the bot that
  "supposed" to be a defender. Mitigation: role-recompute on any
  team-composition change too (lobby_add_slot / lobby_remove_slot
  for the team).

### Things NOT to do

- **Don't pull in onnxruntime / PyTorch / any RL.** The PPO
  self-play tier is M6+ stretch (per
  `documents/13-bot-ai.md`). Keep the classical path.
- **Don't introduce a behavior-tree library.** Switch statements
  in C per philosophy rule 9 (one way to do each thing).
- **Don't add new wire messages for this pass.** Everything
  stays server-side; clients render bots through the existing
  EntitySnapshot path.
- **Don't widen `BotPersonality` with per-weapon fields.** Keep
  the personality table small + use the global
  `g_weapon_profiles` table for per-weapon behavior.
- **Don't add a "voice line" / chat system.** Bots are silent
  per `documents/13-bot-ai.md` §"What we will NOT do."
- **Don't break the iter7 matrix tooling.** The bake sweep needs
  to keep working so we can compare iter9 numbers to iter7's.
- **Don't try to make Recruit-tier bots GOOD.** Recruit is the
  *intro tier* — 0.30 aggression, 30-tick reaction, 0.18 rad
  jitter. Bad-on-purpose. Most acceptance criteria target
  Veteran + tier; Recruit just needs to not be completely broken.

---

## 8 — Open questions for the user (resolve before / during impl)

These are decisions the implementer should bring to the user
before locking in:

1. **The `g_weapon_profiles` numbers in Phase 3 are guesses.**
   Should we calibrate them against `documents/04-combat.md`'s
   range table, or against the matrix's measured fires-per-kill
   ratio? My recommendation: matrix-derived (data over docs), but
   the user has the design-canon authority.
2. **CTF role split**: 2A/1D/1F for 4-bot teams matches Q3's
   default, but the user might prefer 1A/2D/1F for "defensive
   meta" or 3A/0D/1F for "aggressive meta." Worth asking.
3. **Cover-aware retreat at low HP — should it path to the
   NEAREST cover, or the SAFEST cover (cover behind which the
   nearest pickup also lies)?** The latter chains retreat + heal
   naturally; the former is faster.
4. **Phase 6 — Mass Driver balance**: option C (50/50 split on
   fire-rate + AOE radius), option A (fire-rate only), or option
   B (AOE only)? User preference for "feel."
5. **Should Recruit tier ever retreat?** A Recruit with 0.30
   aggression that retreats at < 20 % HP is going to spend most
   of its match running. Maybe Recruit retreat is disabled (the
   tier is "bad at survival" by design) and only Veteran + can
   retreat. My recommendation: per-tier `retreat_threshold` knob
   in BotPersonality.

---

## 9 — Reproduction commands

```bash
# Branch the implementer is starting from.
git checkout bot-ai-improvements
git log --oneline -3
# Expected top commit: "Lobby bot UX rewrite + fuel-aware jet (#46)"

# Build + test baseline
make
make test-level-io test-pickups test-snapshot test-ctf test-spawn

# 1v1 baseline (this is what motivates Phase 2)
for tier in veteran elite champion; do
  for m in foundry slipstream reactor concourse catwalk \
           aurora crossfire citadel; do
    printf "%-10s %-10s " "$tier" "$m"
    ./build/bake_runner $m --bots 2 --duration_s 60 --tier $tier \
        2>&1 | grep -E "^bake\[.*\]:.*(PASS|FAIL)" | tail -1
  done
done

# Matrix baseline (this is the iter7 reference for Phase 6)
./tools/bake/run_loadout_matrix.sh iter7-reference 60 8 champion
# Tag it iter7-reference so iter9 can diff cleanly.

# Per-mech inspection after each phase
./build/bake_runner reactor --bots 2 --duration_s 60 --tier elite
# Look at:
#   build/bake/reactor.per_mech.tsv
#   build/bake/reactor.heatmap.png
#   build/bake/reactor.kills.csv
```

---

## 10 — File map for the implementer

Files this plan will touch:

| File                             | Phase     | What changes                                  |
|----------------------------------|-----------|-----------------------------------------------|
| `src/bot.h`                      | 1, 4, 5   | New `team_role` field; visibility decls       |
| `src/bot.c`                      | 1–5       | Visibility build, pick_position, aggression, role assign |
| `src/weapons.c`                  | 6         | Mass Driver `fire_rate_sec` + `aoe_radius`    |
| `src/main.c`                     | 5         | Call `bot_assign_team_roles` in `start_round` |
| `src/ctf.c`                      | 5         | Call role-reassign on flag state change       |
| `tests/bot_nav_test.c` (new)     | 1         | Visibility symmetry assertion                 |
| `tools/bake/run_1v1_sweep.sh` (new) | 2      | The 1v1 acceptance test                       |
| `documents/m6/05-bot-ai-improvements.md` | (this file) | History + future-self reference   |
| `CURRENT_STATE.md`               | end       | Prepend M6 P05 entry summarising the pass     |
| `TRADE_OFFS.md`                  | end       | Per-weapon profile numbers are guesses; carrier-role behavior is single-strategy; etc. |
| `documents/m6/04-map-balance.md` | end       | Cross-link footnote pointing at iter9         |

---

## References

The architecture borrows from:

- *The Quake III Arena Bot* (Jean-Paul van Waveren / Mr. Elusive,
  2001) — AAS + goal stack + battle/seek/retreat node FSM. The
  canonical reference; sources at
  [fabiensanglard.net/quake3/a.i.php](https://fabiensanglard.net/quake3/a.i.php),
  paper at
  [fabiensanglard.net/fd_proxy/quake3/The-Quake-III-Arena-Bot.pdf](https://fabiensanglard.net/fd_proxy/quake3/The-Quake-III-Arena-Bot.pdf),
  source on
  [github.com/id-Software/Quake-III-Arena/blob/master/code/botlib](https://github.com/id-Software/Quake-III-Arena/blob/master/code/botlib).
- *Three States and a Plan: The AI of F.E.A.R.* (Jeff Orkin,
  GDC 2006) — GOAP. We don't adopt GOAP but borrow the
  "preconditions + effects + short plans" framing for the
  tactical layer.
  [gdcvault.com/play/1013282](https://gdcvault.com/play/1013282/Three-States-and-a-Plan)
- *Human-like Bots for Tactical Shooters Using Compute-Efficient
  Sensors* (Microsoft 2025, CSGO/Valorant-style) — multimodal
  sensor design + Turing-test evaluation methodology.
  [arxiv.org/abs/2501.00078](https://arxiv.org/html/2501.00078v1)
- *Reinforcement Learning Applied to AI Bots in First-Person
  Shooters: A Systematic Review* (MDPI Algorithms, 2023).
  Confirmed that PPO + curriculum + self-play is the modern
  SOTA — out of scope here.
  [mdpi.com/1999-4893/16/7/323](https://www.mdpi.com/1999-4893/16/7/323)
- *Surfacer: 2D-platformer AI and pathfinding* (Godot asset
  library) — platform-graph trajectory-edge model. Our nav
  graph already uses this shape; the visibility precompute
  extension is new.
  [godotengine.org/asset-library/asset/968](https://godotengine.org/asset-library/asset/968)
