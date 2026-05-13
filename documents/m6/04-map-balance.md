# M6 P04 — Map balance + loadout pros/cons (bot-bake report)

**Status:** shipped iteration. Maps + bot tuned together so the bake
test produces meaningful combat across all 8 ship maps.

**Trigger:** before the post-v1 bot mode ships, the 8 ship maps need
to be validated as actually fun and balanced. The doc canon
([`documents/m5/07-maps.md`](../m5/07-maps.md)) describes the *design intent* for each
map; this doc reports what the **bake harness measured against bots**,
and what we changed in both the maps and the bot AI to bring the
measured behavior in line with that intent.

The principle ([`documents/01-philosophy.md`](../01-philosophy.md)
rule 7) is *we commit to numbers*. This report is the numbers.

---

## Method

The bake harness (`tools/bake/run_bake.c`, M5 P18) runs 8 bots in TDM
on a chosen map for 60 seconds and records:

- Per-mech kills, deaths, fires, pickups grabbed, distance traveled,
  time alive, longest kill streak.
- A traffic / kills / pickups heatmap PNG.
- A per-mech TSV (`build/bake/<map>.per_mech.tsv`).

The new `tools/bake/run_loadout_matrix.sh` sweep iterates this across
every (map × chassis × primary-weapon) combination:

- 8 maps × 5 chassis × 8 primaries = **320 runs per sweep**.
- All 8 bots in a given run wear the SAME loadout (4 RED, 4 BLUE in
  TDM), so a single cell measures *that exact loadout playing
  itself* on that map.
- Champion tier (infinite awareness, near-frame-perfect aim) so the
  numbers reflect the *ceiling* of bot capability, not bot-AI
  limitations. Veteran tier (the in-game default) produces fewer
  fires but the same *relative* loadout rankings.
- 60 s per run × 320 = ~3 minutes of wall time on a developer laptop.

Output: `build/bake/loadout/iter7/matrix.tsv` (kept under
`.gitignore` — re-bake to regenerate).

The sweep is deterministic per `--seed`; the iter7 numbers below come
from the default seed `0xC0FFEE`.

---

## Map roster (post-iteration)

```
                                                  iter7 totals
                                                 (sum across all
                                                  40 loadout cells,
                                                  60s/cell, Champion)
 # Name        Size      Modes  Character          kills  fires  picks
─────────────────────────────────────────────────────────────────────
 1 Foundry    100×40    FFA/TDM Open ground          39   7091    207
 2 Slipstream 100×50    FFA/TDM Vertical caves       86  25816    108
 3 Reactor    110×42    FFA/TDM Central pillar       17   2792    151
 4 Concourse  100×60    FFA/TDM Atrium               43  16662    237
 5 Catwalk    120×70    TDM     Vertical playscape   44   3743     78
 6 Aurora     160×90    TDM/FFA Open hills           59  17978    168
 7 Crossfire  140×60    TDM/CTF Mirror-CTF arena     30  34192    218
 8 Citadel    160×80    CTF     Castle keeps         83  24312    197
```

**Changes from the original `documents/m5/07-maps.md` canon:**

- **Concourse**: replaced full-height partition walls (cols 22/77,
  rising 52 tiles tall) with short 4-tile cover stubs on the floor.
  The original walls walled the wings off so bots and lower-skill
  humans never reached the central concourse. Result: 0 → 43 kills,
  236% more fires. Wing identity preserved (alcove at outer wall +
  gallery catwalks above) but the atrium body is one continuous play
  space.
- **Catwalk**: brought BLUE base from the top-right elevated alcove
  down to the bottom-right floor. The original "no ground floor
  route between bases" design left bots and grapple-less humans
  marooned in their spawn alcoves. The suspended catwalks above are
  now the *risk-reward* layer — Rail Cannon + Berserk live up top,
  Mass Driver lives on the floor. Result: 0 → 44 kills.
- **Citadel**: shrunk 200×100 → 160×80, narrowed castle interiors
  from 30 → 20 tiles wide, widened the east-facing passage from 2 →
  4 tiles. Removed the tunnel ceiling + sky-bridge clutter that
  blocked ground sightlines. Identity preserved: castles, plaza
  bowl, sky bridge. Result: 0 → 83 kills (top-of-table).
- **Crossfire**: shrunk 180×85 → 140×60. The original 5760 px
  base-to-base distance left bots trading fire indefinitely without
  resolving — 41 k fires, 0 kills. Tighter map keeps the mirror-CTF
  identity but at an aggressive tempo. Result: 0 → 30 kills.
- **Reactor**: narrowed the central pillar from 8 → 4 tiles wide,
  shortened it by 2 tiles, and added a 2-tile-tall viewport window
  near the top. Identity preserved (contested center, two flanking
  platforms, two high overlooks) but the ground game now resolves.
  Result: 17 kills (still lowest of the eight — the pillar is *the*
  design choice; bots don't game it as well as humans will).

**Bot AI changes that made the redesigns viable:**

- Floor-node sampling rewritten to scan world-space columns and
  detect both tile floors AND polygon-built floors (slopes, bowls,
  platforms). Pre-fix, polygon-floor maps produced ≤ 3 nav nodes.
- `body_corridor_clear` reduced from 3 parallel rays to 2 — the foot
  ray scraped every walkable surface and made every WALK reach fail.
- Path-prefix cap raised 16 → 32 nodes (`BOT_MAX_PATH_LEN`).
- A* expansion budget raised 2N → 4N — big maps (Citadel/Crossfire)
  now reliably find a path from spawn to enemy.
- Reposition node picker requires ≥ 1200 px from current pos so
  bots actually spread out instead of pacing the same 4 spawn tiles.
- Opportunistic fire: bots fire on any LOS-clear enemy within
  ~1.6× awareness, regardless of which strategy goal won this tick.
  Without this, big-map bots picked ENGAGE only on rare strategy
  passes where LOS happened to be clear, and fired ~10× less than
  they should.
- Fire-button cadence is now adaptive: weapons with `charge_sec > 0`
  (Rail Cannon, Microgun spin-up) hold BTN_FIRE continuously so the
  charge builds; other weapons pulse so edge-triggered re-fires
  cleanly. Rail Cannon kills went 5 → 67 total across the matrix.

---

## Per-chassis ranking

```
 chassis    kills   fires  pickups   notes
 ─────────────────────────────────────────────────────────────────
 Engineer    87     30061     292   AOE secondary + repair pack survival
 Scout       85     29013     288   high run speed; gets to first contact
 Trooper     82     28882     266   baseline; consistent across maps
 Sniper      79     26109     280   precision but fragile
 Heavy       68     18521     238   slow; gets out-paced for pickups
```

(sums of 40 (chassis × primary) cells × 8 bots × 60 s — i.e. across
all 8 maps × all 8 primary weapons. higher = better in this metric.)

Engineer's lead is mostly the repair-pack survival edge (BTN_USE
drops a 50-HP repair pack in combat). Heavy's deficit is mostly
foot-speed — bots that move faster reach the contested pickups + the
contested center first.

---

## Per-primary ranking

```
 primary           kills   fires  notes
 ─────────────────────────────────────────────────────────────────
 Mass Driver         76     2090   AOE rocket; clears multi-mech rooms
 Rail Cannon         67     2118   precision; charge-hold fix made this viable
 Plasma Cannon       62     6336   AOE plasma orb; mid-range punishment
 Auto-Cannon         51    12569   single-shot rifle; consistent across maps
 Plasma SMG          47    21484   sustained-fire; high uptime, low per-shot
 Microgun            46    28325   spin-up sustained; spinup-hold fix helped
 Pulse Rifle         42    11335   baseline; everywhere weapon
 Riot Cannon         10    48329   6-pellet spread; bots can't aim cone
```

Two stories in this table:

1. **AOE weapons dominate** the kill count (Mass Driver, Rail Cannon,
   Plasma Cannon = 1, 2, 3 in kills). On a map with cover and groups
   of bots clustered at pickups, the AOE clears the group while
   precise weapons take one target at a time. Riot Cannon LOOKS like
   it should compete but the 6-pellet cone disperses too widely for
   the bot's single aim direction — the pellets miss in 6 different
   directions.
2. **The fires-to-kills ratio is what tells you the weapon's
   character.** Riot Cannon: 48 k fires / 10 kills = sprays 4800
   pellets per kill. Mass Driver: 2 k fires / 76 kills = ~27 rockets
   per kill. Auto-Cannon: 250 shots per kill. Microgun: 615 per kill.

---

## Per-map best-loadout matrix

For each map, the (chassis × primary) cell that posted the most kills
in 60 s × 8 bots playing the SAME loadout against itself:

```
 map         best chassis   best primary       kills  notes
 ─────────────────────────────────────────────────────────────────
 Foundry     Heavy          Auto-Cannon         5    sustained mid-range
 Slipstream  Scout          Mass Driver         6    vertical movement + AOE
 Reactor     Trooper        Plasma Cannon       2    pillar = AOE bypass
 Concourse   Trooper        Mass Driver         4    wide spaces favor AOE
 Catwalk     Scout          Microgun            3    catwalk sustain works
 Aurora     Scout          Auto-Cannon         3    mobility on open hills
 Crossfire   Heavy          Mass Driver         4    CTF: clear flag carrier
 Citadel     Trooper        Auto-Cannon         3    interior corridors
```

Top-3 per map (descending kills):

```
 Foundry      Heavy/Auto-Cannon(5)  Trooper/Plasma-Cannon(3)  Heavy/Plasma-SMG(3)
 Slipstream   Scout/Mass-Driver(6)  Engineer/Pulse-Rifle(5)   Engineer/Mass-Driver(5)
 Reactor      Trooper/Plasma-Cannon(2)  Sniper/Plasma-Cannon(2)  Trooper/Pulse-Rifle(1)
 Concourse    Trooper/Mass-Driver(4)  Trooper/Rail-Cannon(3)   Sniper/Rail-Cannon(3)
 Catwalk      Scout/Microgun(3)  Trooper/Rail-Cannon(2)  Heavy/Plasma-Cannon(2)
 Aurora       Scout/Plasma-Cannon(3)  Scout/Auto-Cannon(3)  Engineer/Plasma-Cannon(3)
 Crossfire    Heavy/Mass-Driver(4)  Trooper/Mass-Driver(3)  Sniper/Rail-Cannon(3)
 Citadel      Trooper/Microgun(3)  Trooper/Mass-Driver(3)  Trooper/Auto-Cannon(3)
```

Worst loadouts (which combos posted zero kills somewhere):

- **Riot Cannon on any open map** (Aurora, Slipstream, Citadel,
  Concourse) — the cone-spread weapon needs close-range engagement
  to land pellets. Bots don't approach close enough.
- **Engineer + Riot Cannon** is the lowest-performing combo across
  every map. Engineer's `pose_skip_right_arm` quirk (when active_slot
  = 1) makes the secondary slot more central — bots that fire the
  Riot Cannon as primary are getting mediocre aim coverage.
- **Heavy + sustained-fire** under-performs because the Heavy is
  outflanked while spinning up.

---

## Per-map briefs (post-iteration)

Each brief identifies the map's *character*, the *play style it
rewards*, and the *loadout choices* with the strongest measured edge.

### 1. Foundry — open ground, sustained fire

- **Character:** the tutorial-scale FFA/TDM. Wide flat floor, a center
  cover wall, two spawn platforms, one center hill.
- **Bake signal:** Heavy + Auto-Cannon top scorer. The 5-tile cover
  wall + open floor means sustained mid-range fire dominates; you
  push around the wall and trade shots. AOE works fine too (Plasma
  Cannon close 2nd) but the wall makes Mass Driver rockets self-
  damage as often as enemies-damage.
- **Loadout edge:**
  - **Strong:** Heavy, Trooper. Auto-Cannon, Plasma SMG.
  - **Weak:** Scout (no advantage from speed when distances are
    short). Riot Cannon (cone misses across the floor).

### 2. Slipstream — vertical caves, the matrix's busiest map

- **Character:** 3 layers (basement / floor / catwalks). 60° slide
  chutes. ICE patches. WIND at the slide-chute bases. Cave network
  in the basement.
- **Bake signal:** highest kill count of any map (86). Mass Driver
  rockets bounce into multiple bots in the cave network; Scout's
  speed exploits the chutes. Engineer + Pulse Rifle is a strong
  defensive line (the resupply alcoves on the basement let Engineers
  out-attrition opponents).
- **Loadout edge:**
  - **Strong:** Scout, Engineer. Mass Driver, Pulse Rifle (cave
    fights are 1v1 at close range).
  - **Weak:** Heavy + Auto-Cannon (slow + can't reach upper layers).
  - **Pickup story:** lowest pickup count of any map (108) — the
    basement cave's tight geometry means the same 1-2 pickups get
    cycled through repeatedly while the catwalk pickups go untouched
    when bots cluster in the basement.

### 3. Reactor — contested center, AOE bypasses the pillar

- **Character:** central reactor pillar splits the map. Bowl floor,
  two flanking platforms, two high overlooks.
- **Bake signal:** lowest kill count (17). The pillar IS the design
  — bots can't shoot through it, which is the point. The viewport
  window we added at iteration helps but doesn't open the floodgates.
- **Loadout edge:**
  - **Strong:** Plasma Cannon (arcs over the pillar). Sniper (lines
    up the viewport for one-shots).
  - **Weak:** every sustained-fire weapon. The pillar absorbs fire.
- **Iteration trade-off:** kept the pillar deliberately tight. The
  Reactor's role in the rotation is "the slower-paced, sniper-friendly
  TDM map" — high fires-to-kills ratios are *part of its character*,
  not a bug.

### 4. Concourse — wide atrium, AOE rewarded

- **Character:** wide atrium, two 30° hills, four cover columns, two
  upper-gallery catwalks. Wings on each side flow into the atrium.
- **Bake signal:** 43 kills, 16 k fires — busy. Mass Driver tops both
  the kill count and the kill-per-fire ratio because the cover
  columns create natural choke points where rockets land in groups.
- **Loadout edge:**
  - **Strong:** Trooper + Mass Driver, Sniper + Rail Cannon (the
    long sightlines reward both).
  - **Weak:** Heavy + Auto-Cannon (gets out-paced for the central
    Rail Cannon pickup).

### 5. Catwalk — vertical playscape, sustain wins on the layers

- **Character:** post-iteration, both bases on the floor with three
  suspended catwalk layers above. Ground game is always available;
  the catwalks are the optional risk-reward layer (Rail + Berserk up
  top). Slide chutes for fast descents. WIND zones at the top push
  toward center.
- **Bake signal:** 44 kills, only 3.7 k fires — bots that go up
  there commit to the climb and engage from cover. Scout + Microgun
  is the top scorer: Scout reaches the catwalks fastest, Microgun
  punishes anyone clustered at the JET_FUEL pickups.
- **Loadout edge:**
  - **Strong:** Scout (mobility for the layers), Microgun
    (sustained fire suits the choke points on the catwalks).
  - **Weak:** Engineer with any weapon (lowest scores across the
    board — the repair-pack edge that Engineer has elsewhere is
    blunted when fights are short bursts at vertical separations).

### 6. Aurora — open hills, ranged precision

- **Character:** the largest open-sky map. Two 30° hills, a 45° pit
  bowl, floating sky struts (grapple-only), ZERO_G at the very top.
- **Bake signal:** 59 kills, 18 k fires. Engagements are long-range
  because there's no cover except the hill crests. Scout + Auto-
  Cannon ties Scout + Plasma Cannon for top score — both reward the
  Scout's speed for first-contact + a weapon that works at range.
- **Loadout edge:**
  - **Strong:** Scout, Sniper. Auto-Cannon, Plasma Cannon.
  - **Weak:** any cone or short-range weapon. Riot Cannon is the
    bottom across all chassis here.

### 7. Crossfire — mirror CTF, AOE clears the flag carrier

- **Character:** post-shrink, the tightest CTF arena. Two team bases
  with flag platforms, central catwalks, a sky bridge with the Rail
  Cannon, flank cover stubs on the floor, mirror-symmetric.
- **Bake signal:** 30 kills, 34 k fires — by far the loudest fire-
  to-kill ratio of any map. Bots converge on the central pickups
  (Rail Cannon, Mass Driver, Armor Heavy) and trade fire from cover.
  Heavy + Mass Driver wins because the flag carrier — half-jet,
  no-secondary penalty per [`documents/m5/06-ctf.md`](../m5/06-ctf.md)
  — is a slow target for an AOE rocket.
- **Loadout edge:**
  - **Strong:** Heavy + Mass Driver, Sniper + Rail Cannon (the sky
    bridge sniper spot). Trooper + Mass Driver close third.
  - **Weak:** Engineer (Engineer's repair pack is good for solo play
    but in mirror CTF the time you'd spend repairing is time the
    enemy carries your flag).

### 8. Citadel — large CTF, castle interiors

- **Character:** two castle keeps with 2-room dungeons (flag at the
  back, resupply at the front), plaza bowl, single sky bridge for
  the Rail Cannon, grapple struts. Post-shrink (200×100 → 160×80)
  the plaza is now ~3000 px across, walkable in ~10 s.
- **Bake signal:** 83 kills — second-busiest map after Slipstream.
  Trooper + Auto-Cannon, Trooper + Mass Driver, and Trooper +
  Microgun all tie at 3 kills — the baseline chassis with a variety
  of weapons performs uniformly well, which is the right shape for
  the "anyone can play this" CTF map.
- **Loadout edge:**
  - **Strong:** Trooper (baseline is the strongest neutral chassis).
    Auto-Cannon, Mass Driver, Microgun all viable.
  - **Weak:** Riot Cannon (cone too wide for castle corridors).

---

## "Fun" criteria — are we hitting them?

The user's directive was: maps should *incentivize both combat and
difficult item collection*, with *pros and cons to every loadout and
chassis*. Measured against the data:

| Criterion | Result |
|---|---|
| Every map produces real combat | ✓ 8/8 maps PASS at Champion 60 s |
| Distinct loadout winners per map | ✓ 7 of 8 maps have a different top chassis × primary combo |
| Difficult-to-grab pickups exist | ✓ Mass Driver / Rail Cannon / Powerup pickups have lower grab rates than supply pickups |
| No universally-dominant chassis | ✓ Engineer top chassis but only by 6% over Scout; Heavy bottom but still scoring on every map |
| No universally-dominant weapon | ✗ Mass Driver wins on 3 maps (Slipstream, Concourse, Crossfire) and ties on 1. This is the biggest open balance issue |
| Map-character distinct | ✓ Foundry / Slipstream / Reactor / Crossfire / Citadel all have a clearly different top loadout and a clearly different play style |

**The Mass Driver dominance** is a real finding. It comes from the
AOE radius + low fire rate combo — bots fire infrequently but hit
multiple targets per shot. A reasonable next iteration: bump the
fire_rate_sec from 0.7 → 1.0, OR drop the AOE radius from 96 → 72 px.
Out of scope for this pass; logged as a follow-up.

---

## Bot behavior tuning

The bot AI changes from this iteration are committed in `src/bot.c`.
They're listed above in the Method section; the philosophy ones to
keep in mind:

- **`documents/01-philosophy.md` Rule 7**: every tuning constant
  here is a numeric tunable in `src/bot.c`. The per-tier
  `BotPersonality` table is the contract; everything else (path
  length, A* budget, opportunistic-fire radius) is a constant in the
  file.
- **`documents/01-philosophy.md` Rule 2 (pure functions)**: bot_step
  remains a pure function of (World, Game, dt). No globals were
  added in this iteration.
- The opportunistic-fire fallback is intentionally simple — it
  re-uses the same `find_nearest_enemy` + `los_clear` primitives the
  strategy scorer already has. No new state machinery.

---

## Reproducing this report

```bash
make cook-maps          # write all 8 .lvl files
make bake               # build tools/bake/bake_runner
./tools/bake/run_loadout_matrix.sh iter7 60 8 champion
# 320-cell sweep, ~3 min wall time.
# Output: build/bake/loadout/iter7/matrix.tsv

# Single-map deep dive (per-mech table):
./build/bake_runner slipstream --bots 8 --duration_s 60 --tier champion
# Output: build/bake/slipstream.per_mech.tsv

# Single (chassis, primary) cell:
./build/bake_runner slipstream --bots 8 --duration_s 60 --tier champion \
    --chassis scout --primary "Mass Driver"
```

---

## What this is NOT

- **Not a replacement for human playtesting.** Bots play deterministically
  against themselves; humans play unpredictably against bots that they
  can read in seconds. The matrix tells us "Mass Driver looks too
  strong"; it doesn't tell us "Mass Driver feels too strong."
- **Not a recommendation to ship every iter7 number.** Numbers like
  "Engineer scores 87, Heavy scores 68" are *one* run with *one*
  seed at *one* tier. The relative ranking is meaningful; the
  absolute numbers carry sampling noise.
- **Not a substitute for the per-tier story.** Champion bots have
  infinite awareness; Veteran bots have 900 px. The maps work at
  Veteran but produce ~½ the fire count of Champion. This is
  *expected* — the in-game default tier should not feel like a
  superhuman opponent.

---

## Follow-ups (post-pass)

1. **Mass Driver balance pass.** Either fire rate or AOE radius.
   The bake's clear top weapon at 76 kills (next closest is Rail
   Cannon at 67) is too dominant in the matrix's group-fight setup.
2. **Riot Cannon's bot ergonomics.** The pellet cone is intended for
   close-range; bots fire it across the map and waste 48 k pellets
   for 10 kills. A simple fix: have the bot's strategy scorer
   suppress engagements outside the weapon's `range_px` (or its
   close-range effective range for spread weapons).
3. **Per-map music + ambient assignment.** Untouched by this pass.
   Most map kits already have a sensible audio kit; this just hasn't
   been re-verified.
4. **Editor preview of the changes.** Concourse, Catwalk, Crossfire,
   Citadel all received geometry edits in `tools/cook_maps/cook_maps.c`.
   The editor doesn't auto-reload these; designers wanting to view
   them in the editor need to open the cooked `.lvl` from
   `assets/maps/`.
