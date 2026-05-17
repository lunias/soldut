# M6 — Bot AI current state (post-P13 snapshot)

A point-in-time recap of how the bot AI behaves *right now* (commit
`b05a05f` on `main`, M6 P13 merged). This is the "if you pick up bot
work today, here's the shape you're inheriting" document — useful
when the next AI pass starts cold and the spec docs
(`13-bot-ai.md`, `m6/05-bot-ai-improvements.md`,
`m6/13-bot-ai-refresh.md`) describe intent rather than current
behavior.

Source-of-truth is still the code. When this doc disagrees with
`src/bot.c`, fix the doc.

## Architecture in a paragraph

`src/bot.{c,h}` is the whole module — ~3500 LOC, single header,
single implementation. Three layers run for every bot every tick,
plus a cross-cutting nav graph built once per round:

```
   nav graph (per round, level_arena)         BotPersonality (per tier, const table)
   ─ nodes (floor/spawn/pickup/flag/slope-top) ─ aim_jitter / aim_lead / slew / reaction
   ─ reachabilities (WALK/JUMP/FALL/JET/GRAPPLE) ─ awareness / pickup_pri / flag_pri
   ─ per-node visibility bitset                ─ grapple_pri / aggression / retreat_threshold
   ─ AMBI_ACID / AMBI_ZERO_G tags              ─ knows_full_map / uses_powerups
            │                                            │
            ▼                                            ▼
   strategy (10 Hz) — utility scorer picks ENGAGE / PURSUE_PICKUP /
        REPOSITION / RETREAT / GRAB_FLAG / CAPTURE / CHASE_CARRIER /
        DEFEND_FLAG / RETURN_FLAG, with role × goal weights
            │
            ▼
   tactic  (60 Hz) — per-goal switch fills BotWants
        { move_target, aim_target, fire, jet, jump, grapple_fire,
          use, reload, swap, throw_active, throw_offhand,
          throw_target_charge }
            │
            ▼
   motor   (60 Hz) — wants → ClientInput, applies aim slew + jitter,
        manages WFIRE_THROW charge dance, ACID emergency exit,
        ZERO_G jet hysteresis, jet-fuel lockout, stuck recovery
            │
            ▼
   mech.latched_input  → simulate_step  → world updates
```

## What each tier feels like

| Tier     | Jitter | Aim slew  | Reaction | Awareness | Pickup | Flag | Grapple | Aggression | Retreat | Powerups |
|----------|-------:|----------:|---------:|----------:|-------:|-----:|--------:|-----------:|--------:|---------:|
| Recruit  | 0.18 rad (≈10°) | 0.04 /tick | 30 t (0.50 s) | 500 px   | 0.20 | 0.10 | 0.00 | 0.30 | never | no |
| Veteran  | 0.08 (≈4.5°)    | 0.10       | 9 t  (0.15 s) | 900 px   | 0.55 | 0.50 | 0.15 | 0.55 | < 0.25 | no |
| Elite    | 0.030 (≈1.7°)   | 0.18       | 4 t  (0.07 s) | 1300 px  | 0.80 | 0.75 | 0.45 | 0.75 | < 0.30 | yes |
| Champion | 0.005 (≈0.3°)   | 0.32       | 2 t  (0.03 s) | ∞        | 1.00 | 1.00 | 0.85 | 0.95 | < 0.30 | yes |

(Personality table at `src/bot.c:113-174`. Visible delta in matrix
runs: Champion fires 51–148/match across the 8 ship maps; Recruit
fires 0–68. Differentiation is mostly aim quality + awareness; tier
movement style is identical modulo the grapple/jet decisions
gated by `grapple_priority` and `aggression`.)

## Strategy scoring — what wins, when

The scorer runs every 6 ticks per bot, computes one score per
candidate goal, picks the winner (with 10 % hysteresis to avoid
flicker). Per-role multipliers apply on top (CTF only).

Approximate ranges of the raw scores at a healthy bot mid-fight on
flat ground vs one enemy:

- **ENGAGE** — `close × (0.4 + 0.6 × hp_frac) × aggression × LOS_factor
  × weapon_fit`. Maxes at ~1.1 inside optimal weapon range with full
  HP + LOS clear at Champion. Drops to ~0.05 at 1800 px with a Pulse
  Rifle (out of effective range). LOS-blocked enemies get a 0.85×
  damp so REPOSITION still wins, but ENGAGE stays competitive.
- **PURSUE_PICKUP** — `need_factor × proximity × pickup_priority`.
  Maxes at 0.8 for low-HP bot near a HEALTH pickup; nominal 0.25
  for an idle bot near generic ammo. Filtered by per-bot
  `abandoned_nodes` blacklist (10 s TTL on nodes the bot recently
  failed to reach).
- **PURSUE_ENEMY (REPOSITION baseline)** — flat `0.40 × (0.4 + 0.6 ×
  aggression)`. Champion = ~0.59, Recruit = ~0.21. Multiplied by a
  combat-famine boost that grows to 3× over 30 s of no-kill so
  scattered bots converge on each other.
- **RETREAT** — climbs from 0 to ~0.95 as `bot_aggression()` drops
  below the tier's `retreat_threshold`. Recruit's threshold is 0,
  so Recruits never retreat (intentional: the "bad on purpose" tier).
- **GRAB_FLAG / CAPTURE / CHASE_CARRIER / RETURN_FLAG / DEFEND_FLAG**
  — CTF-only, gated by `match.mode`, scaled by `flag_priority`.
  Multiplied by `g_role_goal_mult[role][goal]` (e.g., ATTACKER ×1.5
  on GRAB_FLAG, DEFENDER ×1.6 on DEFEND_FLAG).

The **opportunistic-fire** fallback in `bot_step` runs every tick
after the tactic dispatch: if any LOS-clear enemy exists within
1.6 × awareness (min 1200 px), `wants.want_fire` flips on regardless
of which goal won. That's why bots fire even when REPOSITION
dominates the scorer — and it's the path that triggers the off-hand
WFIRE_THROW for bots whose secondary is frag.

## WFIRE_THROW (frag grenade) — the M6 P13 path

Frag is currently the only `WFIRE_THROW` weapon. The bot path:

1. **Detection**: `bot_throw_slot(world, mid, &wpn)` returns 0 if
   primary is WFIRE_THROW, 1 if secondary is, else -1. With the
   default loadout for Veteran+ tier index 0 the bot has Frag in
   the secondary slot (Pulse Rifle primary).
2. **Charge target**: `bot_throw_target_charge(personality, dist)`
   maps (tier, distance) → 0..1 charge factor. Recruit caps at
   0.45, Champion at 1.00; intra-tier scales linearly across
   200..2000 px of throw distance. So Champion at 1800 px hits
   ~0.95; Recruit at 200 px throws at 0.20.
3. **Arc aim**: `bot_throw_arc_aim(world, mid, target, charge)`
   projects an above-target aim point compensating for projectile
   gravity drop minus the engine's 0.12 upward launch bias.
4. **Motor dispatch**:
   - Active-slot WFIRE_THROW + `want_fire` → hold BTN_FIRE while
     `m->throw_charge < target_sec`, release otherwise.
   - Off-hand WFIRE_THROW (auto-derived from `want_fire` + off-hand
     check inside `run_motor`) → hold BTN_FIRE_SECONDARY for the
     charge cycle AND suppress BTN_FIRE the entire time. The
     suppression is load-bearing: `mech_try_fire`'s throw gate
     requires `fire_cooldown <= 0` at the release edge, and even
     a 0.11 s Pulse Rifle cooldown overlaps the release window;
     without the suppress, `throw_charge` resets to 0 on a wasted
     release.

Champion-tier throws verified at ~0.95 charge / 2555 px/s in
`tests/shots/net/run_bot_frag_charge.sh` (5/5 PASS). The
post-throw refractory window (one throw at high charge, several
weak ones in the cooldown trailing edge) is **expected** and
documented as a follow-up — see "Known limitations" below.

## Atmospheric awareness (M6 P13)

`level.ambis[]` rects are read by both the nav build and the per-
tick motor:

- **AMBI_ACID** — nodes inside an acid rect get `BOT_NODE_F_AMBI_ACID`.
  Reachabilities whose destination is acid-tagged pay 4× cost so
  A* routes around 5 HP/s puddles when alternatives exist; bots
  still grit through when acid is the only viable path (Crossfire's
  slime channels). Per-tick motor adds an emergency-exit branch:
  `world_point_in_acid(pelvis)` → override LEFT/RIGHT toward the
  closer rect edge + force JET pulse (bypasses fuel-hysteresis
  lockout because HP is the more urgent budget). 5 HP/s drain stops
  the moment pelvis crosses the rect boundary.
- **AMBI_ZERO_G** — nodes inside get `BOT_NODE_F_AMBI_ZERO_G`. The
  motor relaxes jet hysteresis inside zero-G: entry threshold 4 %
  (vs standard 10 %), re-engage floor 20 % (vs 40 %). Bot burns
  fuel freely when gravity is zeroed; thrust is full-effective so
  staying airborne IS the right move.
- **AMBI_WIND** — not tagged. Wind is a per-tick particle nudge
  (max 700 px/s push) that we can't usefully bake into static
  nav cost. Bots accept the slowdown when moving against wind.
- **AMBI_FOG** — not tagged. Render-only; bots see through fog
  (no per-bot vision model at v1).

## Per-weapon engagement profiles

`g_weapon_profiles[WEAPON_COUNT]` at `src/bot.c:1392-1408`. The
scorer applies `weapon_range_fit_factor(weapon_id, dist)` to
`score_engage`; the tactic layer uses `ideal_strafe_px` for the
"keep this stand-off range" behavior. `prefers_high` biases the
engagement-node BFS toward elevated cover (Rail Cannon = sniper).

| Weapon          | Optimal | Effective | Strafe | High? |
|-----------------|--------:|----------:|-------:|------:|
| Pulse Rifle     | 600     | 1200      | 500    | -     |
| Plasma SMG      | 350     | 800       | 350    | -     |
| Riot Cannon     | 220     | 450       | 250    | -     |
| Rail Cannon     | 1200    | 2400      | 1100   | yes   |
| Auto-Cannon     | 700     | 1400      | 600    | -     |
| Mass Driver     | 700     | 1600      | 600    | -     |
| Plasma Cannon   | 600     | 1300      | 550    | -     |
| Microgun        | 500     | 1000      | 450    | -     |
| Sidearm         | 400     | 900       | 400    | -     |
| Burst SMG       | 350     | 700       | 350    | -     |
| Frag Grenades   | **900** | **1700**  | **700**| -     |
| Micro-Rockets   | 500     | 1000      | 500    | -     |
| Combat Knife    | 80      | 140       | 80     | -     |
| Grappling Hook  | 0       | 0         | 0      | -     |

Frag was updated in M6 P13 to match the hold-to-charge throw range
(was 350/700/350 → 900/1700/700). Other rows held up from the
M6 P05 iter9 matrix.

## Default bot loadout

`bot_default_loadout_for_tier(bot_index, tier, out)` cycles
chassis and primaries by index so a populated lobby shows variety:

- Chassis: Trooper / Scout / Heavy / Sniper / Engineer (cycle).
- Primary: Pulse Rifle / Plasma SMG / Auto-Cannon / Riot Cannon /
  Plasma Cannon (cycle).
- Secondary: Frag Grenades when `tier >= Veteran && (index & 3) == 0`;
  Grappling Hook when `tier >= Elite && (index & 3) == 2`;
  Sidearm otherwise.
- Armor: Light. Jetpack: Standard.

Only ~1/4 of Veteran+ bots get frag at v1; the rest carry the
Sidearm fallback. (Bumping the frag share is a tuning knob; the
shape works for variety on 4v4 / 8v8 lobbies.)

## CTF team roles

`bot_assign_team_roles(bs, world)` runs at round start and on flag
state transitions. Per-team round-robin: 2 ATTACKER / 1 DEFENDER /
1 FLOATER for a 4-bot team; smaller teams pro-rate. CARRIER is
dynamic — overrides `team_role` while `ctf_is_carrier(mid)` is
true and forces score weights to "run home, don't fight."

## Known limitations (not fixed in P13, queued)

These are deliberate trade-offs or follow-ups — none are blockers
for v1 ship but each is a likely tuning lever for the next AI pass.

- **Post-throw refractory window**: after a max-charge frag throw
  the bot re-acquires the off-hand throw intent the very next
  tick, but `m->throw_charge` is gated on `fire_cooldown <= 0`
  (frag's 0.6 s = 36 ticks). During the cooldown the bot holds
  BTN_FIRE_SECONDARY uselessly; when cooldown clears the held
  button starts accumulating, BUT the bot's position has often
  shifted enough that `dist > 1900` or `dist < 200` and
  `want_throw_offhand` flips off mid-accumulation. Result: 1 good
  throw at full charge, then 2–4 weak (~0.05 charge) lobs as the
  intent oscillates. Fix would add per-mind throw-commit
  hysteresis (keep the intent latched once accumulation starts).
- **Aim lead frac uses a fixed 800 px/s projectile speed** in the
  tactic layer's lead formula. Frag's effective speed at full
  charge is 2800 px/s — the bot leads too much for thrown weapons.
  Visible at Champion tier (perfect 1.0 lead) where grenades
  consistently land slightly ahead of moving targets.
- **No bot-team coordination beyond CTF role weights**. Bots don't
  pair up, focus-fire, or call out flanks. Acceptable for the v1
  PvE / LAN bot mode; the doc canon
  (`documents/13-bot-ai.md` §"What we will NOT do") explicitly
  out-of-scopes this.
- **No bot voice / chatter**. Lobby chat lists bot names; bots are
  silent in play.
- **Recruit-tier aim jitter is weapon-agnostic**. A Recruit holding
  a Rail Cannon should miss less than one holding a Pulse Rifle
  (Rail is the precision weapon), but jitter is a single per-tier
  scalar. Tolerated; matrix runs don't show Rail being
  underpowered enough to chase.
- **No bot navmesh debug overlay** in the editor. Designers can't
  preview routes without running a bake. Worth picking up when
  bake iteration becomes a routine designer workflow.
- **Side-by-side tier-differentiation visual composites** weren't
  built into a formal artifact in M6 P13. The `tools/bake/run_1v1_sweep.sh
  m6p13` table shows the differentiation in fire counts
  (Champion 51–148, Recruit 0–68) but a per-tick visual diff
  would be cleaner evidence.

## Sweep numbers — what we see in practice

`tests/net/run_bot_playtest.sh` (4 Elite bots, 60 s TDM, observing
client) on all 8 ship maps post-P13:

```
aurora     8 kills    catwalk    7
citadel    7          concourse  5
crossfire  3          foundry    11
reactor    4          slipstream 2
```

`tools/bake/run_1v1_sweep.sh all 30 m6p13` (1v1 fires per match,
30 s, no respawn — so kills require landing lethal damage in 30 s
from one mech against one mech):

| Tier     | Foundry | Slipstream | Reactor | Concourse | Catwalk | Aurora | Crossfire | Citadel |
|----------|--------:|-----------:|--------:|----------:|--------:|-------:|----------:|--------:|
| Recruit  | 25      | 53         | 68      | 0         | 26      | 21     | 48        | 20      |
| Veteran  | 0       | 22         | 69      | 0         | 24      | 0      | 23        | 13      |
| Elite    | 65      | 46         | 68      | 29        | 36      | 8      | 41        | 20      |
| Champion | 71      | 51         | 106     | 8         | 46      | 32     | 40        | 148     |

Tier differentiation is visible in fire counts. The 0-fire entries
at Veteran on Foundry / Concourse / Aurora are 1v1 LOS-gap edge
cases — Veteran's narrower awareness (900 px) misses some opening
positions that Elite (1300) and Champion (∞) catch. Doesn't
reproduce in 4v4 (the playtest counts).

## File map

- `src/bot.h` — public surface, types, BotPersonality, BotMind,
  BotTeamRole, BotSystem. 280 lines.
- `src/bot.c` — implementation, ~3500 lines. Sections in order:
  tunables / personality table / nav graph (nodes / reaches /
  visibility / ambi tags) / world helpers / strategy scorer /
  tactic layer / motor / public API.
- `documents/13-bot-ai.md` — design canon.
- `documents/m6/05-bot-ai-improvements.md` — first M6 pass
  (visibility precompute, cover-aware engagement, weapon range
  gating, aggression-based retreat, CTF role coordination, Mass
  Driver balance).
- `documents/m6/13-bot-ai-refresh.md` — second M6 pass
  (WFIRE_THROW handling, atmospheric awareness).

## Tests

- `make test-bot-nav` — nav-build assertions (node count > 0,
  visibility symmetric, at least one visible + one blocked pair)
  across all 8 maps.
- `tests/net/run_bot_playtest.sh` — 4 bots + 1 observing client
  on configurable map / mode. Asserts plumbing (accept, nav
  build, role assignment) + reports mech_kill count.
- `tests/shots/net/run_bot_frag_charge.sh` — Champion bot frag
  throw verification on Aurora.
- `tools/bake/run_1v1_sweep.sh` — 1v1 fires-per-tier matrix
  across all 8 maps.
- `tools/bake/run_loadout_matrix.sh` — 8v8 per-weapon kill matrix
  (used for M6 P05 iter7 / iter9 weapon balance).
