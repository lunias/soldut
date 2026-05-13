# M5 — The 8 ship maps

The content payload of M5. Each map is authored in the editor, exported as a `.lvl` file, and shipped under `assets/maps/`. The runtime loader is in [01-lvl-format.md](01-lvl-format.md); the editor is in [02-level-editor.md](02-level-editor.md); the systems they exercise are in [04-pickups.md](04-pickups.md), [05-grapple.md](05-grapple.md), [06-ctf.md](06-ctf.md).

This document is the brief for each map plus the bake-test that decides "ship" vs. "iterate."

> **M6 post-iteration note.** Four of the eight maps received geometry edits in M6 P04 (the bot-bake balancing pass): Concourse's wing partitions, Catwalk's base placement, Citadel's size, and Crossfire's size were all changed to make the maps playable against bots and balanced for the loadout matrix. The current ship state and the per-map bake findings (including which loadouts dominate where) are documented in [`../m6/04-map-balance.md`](../m6/04-map-balance.md). The briefs below describe the **original design intent** — they remain the source of truth for character, but the geometry numbers reflect the M5 cook output, not the M6 iteration. See the M6 doc for what actually shipped.

## The roster

| # | Name | Tile dims | World px | Modes | Air kit | Lean | Slope character |
|---|---|---|---|---|---|---|---|
| 1 | **Foundry** | 100×40 | 3200×1280 | FFA | industrial | ground | Mostly flat; one 30° hill at center, one 45° entry ramp per spawn |
| 2 | **Slipstream** | 100×50 | 3200×1600 | FFA | maintenance | vertical | Steep 60° drop chute basement→floor; angled ceilings on the upper catwalks redirect jet sideways |
| 3 | **Concourse** | 100×60 | 3200×1920 | FFA, TDM | atrium | mid | Two long 30° hills under the concourse roof — runners gain speed downhill into the central fight |
| 4 | **Reactor** | 110×42 | 3520×1344 | FFA, TDM | reactor | central | Bowl floor (30° on each side meeting at center); the pillar's underside is angled (45° overhangs) for jet-slide play |
| 5 | **Catwalk** | 120×70 | 3840×2240 | TDM | exterior | vertical | Aggressive: 60° external slopes you slide *down*, 45° catwalk surfaces you walk *up* slowly; angled overhead struts |
| 6 | **Aurora** | 160×90 | 5120×2880 | TDM, FFA | aurora | open | Two big "hills" (30° rises ~10 tiles wide), one valley (45° bowl); angled ceilings nowhere — Aurora is an open-sky map |
| 7 | **Crossfire** | 180×85 | 5760×2720 | TDM, **CTF** | foundry | symmetric | Mirror-symmetric ramps (30° entry to each base); 45° angled struts above the central battleground |
| 8 | **Citadel** | 200×100 | 6400×3200 | **CTF** | citadel | layered | All four kinds: bowl plaza (30°), steep rampart slopes (60°), angled castle ceilings (45° overhangs); full slope vocabulary |

Sizes match [07-level-design.md](../07-level-design.md) §"Map size targets." All maps fit in the 200×100 ceiling.

## Air kit

Each map declares a "kit" in its `LvlMeta`: a single string-table reference to a parallax background set + a music track + an ambient loop. Kits are reusable across maps with palette swaps. We ship 8 kits (one per map for v1; a stretch is to share kits across pairs), but the kits themselves are 8 visual variations of one hand-drawn industrial mech aesthetic.

[08-rendering.md](08-rendering.md) details the parallax + decoration art per kit. [09-audio.md](09-audio.md) details the music + ambient.

## Slopes, hills, valleys, angled ceilings — discipline across the maps

Per [03-collision-polygons.md](03-collision-polygons.md) §"Slopes, hills, valleys, angled ceilings", Soldut adopts the Soldat run-up-slow / slide-down-fast feel as a baked-in physics property of every surface. The map roster is intentionally biased toward exploring this vocabulary across the 8 ship maps:

- **Every map has at least one explicit slope feature.** No map ships as a pure rectangular tile-stack.
- **3 angles in active use**: 30° (gentle, full grip, walk normally), 45° (medium, runs visibly slow uphill, runs faster downhill), 60° (steep, you slide passively if you're not pressing forward).
- **Every angle appears on at least 2 maps.** The bake-test enforces this so a player who learns slope physics on Foundry has the same vocabulary on Citadel.
- **Angled ceilings appear on 5 of 8 maps.** They aren't a universal feature (Aurora is an open-sky map, Foundry is a tutorial), but they're prominent enough that the jet-slide-along-overhang move is a real part of the game's vocabulary.
- **One map per pair shares slope character with another** (Foundry/Concourse are gentle-slope maps; Slipstream/Catwalk are steep-slope-vertical maps; Reactor/Citadel use bowl floors; Aurora/Crossfire share open hill geometry). This pairing means a designer can iterate two-at-a-time on slope physics tuning.

The following per-map briefs are explicit about which slope features each map ships, and *why* — i.e., what those slopes do for the gameplay shape of that map.

## Caves, alcoves, and hidden nooks

The slope vocabulary above is the *gross* terrain shape; caves and alcoves are the *fine* terrain pattern. Every map ships at least 2 alcoves; cave-rich maps (Slipstream, Citadel) ship 6+. They're where the best pickups live — power-ups, JET_FUEL, ARMOR — and they're what makes a map feel *explored* rather than *traversed*.

### Sizing — the mech has to fit

The mech's standing height is **~64 px** from foot-Y to head-Y (chest at floor−40, head at floor−60, with a few pixels of bone-radius slack). An alcove that doesn't accommodate the mech in its standing pose isn't an alcove, it's a dead-end. Hard rules the editor validates on save:

| Property | Minimum | Comfortable | Notes |
|---|---|---|---|
| Interior height (floor → ceiling) | **3 tiles (96 px)** | 4 tiles (128 px) | 2-tile (64 px) interiors clip the head into the ceiling on jet entry |
| Interior width (mouth → back wall) | **2 tiles (64 px)** | 3 tiles (96 px) | 1-tile-deep "shelves" feel like wall decorations, not nooks |
| Mouth opening height | **2 tiles (64 px)** | 3 tiles (96 px) | The body has to clear the entry — narrower = the player slides in along a slope |
| Pickup centroid clearance | 16 px from any wall | 24 px | The 24-px-radius pickup trigger zone needs to not graze geometry |

The editor's polygon-and-tile validation pass refuses to save a map where any `LvlPickup` spawner sits inside an enclosure smaller than these minimums. (Validation walks each spawner's neighborhood, computes the bounding empty volume, and flags too-shallow / too-short alcoves.)

### Placement archetypes

The 8 ship maps use four alcove patterns. Each map picks 1–3 from the menu:

#### A. Edge-of-map alcoves (2 tiles deep, on the floor against the outer wall)

The default. Cuts into the side wall at floor level; the mech walks straight in. Holds AMMO, HEALTH small/medium, AMMO_SECONDARY. Cheap to author, reads as "supply locker."

Examples: Foundry's spawn-side alcoves, Reactor's flank alcoves, Crossfire's red/blue base alcoves.

#### B. Jetpack alcoves (high on the side wall, 3+ tiles up from the floor)

Reachable only by jet. The mouth faces the play area; the interior is 3-4 tiles deep, 3 tiles tall. Holds the higher-tier pickups: ARMOR (any), POWERUP (any), WEAPON pickups.

The "jetpack into an alcove to grab a powerup" feel is the load-bearing pattern. Risk/reward shape: jetting up exposes you to the whole map's sightlines; the alcove walls give you cover only after entry. Time-window for "I see them jetting toward the alcove" is ~1 second — long enough to react with a Mass Driver.

Examples: Concourse's gallery alcoves, Catwalk's overhead alcoves above each upper catwalk, Aurora's mountain-peak alcoves.

#### C. Cave systems (linked alcove network — only on cave-style maps)

A series of connected alcoves with internal passages. Used on cave-themed maps (Slipstream, Citadel). The mech can navigate the inside as a small dungeon — picks up multiple items in sequence, gets ambushed at a choke. Caves are the only place the map has *hidden* sightlines (the player inside a cave isn't visible from the outside until they emerge).

Sizing: each cave room follows the alcove minimums; passages between rooms are 3 tiles tall × 2 tiles wide. Total cave footprint is typically 8–12 tiles across.

Examples: Slipstream's basement cave network, Citadel's tunnel systems.

#### D. Slope-roof alcoves (45° angled overhang creates a triangle-shaped nook)

Built from one floor + one 45° slope ceiling. The interior is taller at the entrance (4 tiles) and shorter at the back (1 tile). Pickup sits at the tall entrance. Visually distinct from edge alcoves — reads as "industrial overhang." The angled ceiling makes jetting *out* of the alcove sideways-and-up natural, integrating the slope-physics vocabulary.

Examples: Reactor's alcoves under the pillar overhangs, Citadel's castle-rampart nooks.

### Balance and contestability

A pickup in an alcove is *more contested* than one in the open, because:

- The path in is committed; you can't disengage mid-jet.
- The interior is a kill-box if an enemy is between you and the mouth.
- The respawn cue audio (per [04-pickups.md](04-pickups.md)) telegraphs the alcove's state to anyone within ~800 px.

The right pickups for alcoves are therefore the *high-stakes* ones: power-ups, ARMOR, JET_FUEL (for the Catwalk-style "I jetted in here for the fuel I'll need to jet out"), the rarer WEAPON pickups (Rail Cannon, Mass Driver). A small-health-pack-in-an-alcove is anti-pattern: the reward doesn't justify the commitment. The editor's pickup-validation pass warns on this combination.

### Across-the-map distribution

Each map gets at minimum:

- 2 edge alcoves (one near each spawn cluster), each holding AMMO or HEALTH small.
- 1 jetpack alcove holding a POWERUP or ARMOR.

Cave-style maps add 4–6 cave rooms beyond that. Slope-roof alcoves stack on top of the existing slope features.

### Editor presets

The polygon palette grows three alcove presets ([02-level-editor.md](02-level-editor.md) §"Polygon tool" gets a corresponding line):

- **Edge alcove** (2 tiles deep × 3 tiles tall, opens horizontally) — drops 4 SOLID polygons forming the C-shape.
- **Jetpack alcove** (3 wide × 3 tall × 3 deep, mouth facing down/inward) — drops the polygon set that scoops the geometry from the side wall.
- **Slope-roof alcove** (4 wide × 4 tall at entrance, 1 tall at back) — drops one floor + one 45° slope-ceiling polygon.

Cave systems aren't a single preset; the editor offers a "cave block" preset that drops a single alcove room, and the designer chains them.

## Per-map briefs

Each brief contains:

- **Intent** — what the map is *for*. The single sentence the designer holds in mind during authoring.
- **Flow** — the 3+ paths from spawn to combat.
- **Slopes, hills, valleys, ceilings** — the slope vocabulary this map uses and why.
- **Caves and alcoves** — count, placement, what each holds.
- **Pickup placement** — what goes where, at a high level.
- **Special features** — ambient zones, grapple anchors, audio kit notes.
- **Bake-test acceptance** — what the heatmap must show.

### 1. Foundry — small FFA

```
Size: 100 × 40 tiles (3200 × 1280 px)
Modes: FFA (default)
Pickup count: 12
```

**Intent**: the entry-level map. A new player joining a public FFA server should play this in their first round; it's the layout people land on for "let me try the controls."

**Flow**:
- Ground floor (main): wide path with two cover columns at ~1/3 and ~2/3 of the map width.
- Spawn platforms (left, right): elevated 4-tile platforms that funnel into the ground floor.
- Mid-cover wall: a single 5-tile-tall wall at the map center; you can shoot around it but not through. Each side has a small alcove with a pickup.

Differs from M4's `build_foundry` only in: a small **30° hill** in the center over the ground (3 tiles wide, 1 tile tall — a tutorial-scale "this is what a hill feels like") and **45° entry ramps** to each spawn platform (replacing M4's vertical-jet-only access). New players can now walk into the map without learning jet first.

**Slopes, hills, valleys, ceilings**:
- **Center hill** — 30° gentle, 3 tiles wide × 1 tile tall, polygon SOLID. Run-over feels normal but visible. Teaches "the floor isn't always flat."
- **Spawn ramps** — 45° slopes on both sides of each spawn alcove. You walk into the round on a slope that slows you slightly; the muscle-memory builds in 30 seconds.
- **No angled ceilings.** Foundry is the tutorial; ceilings are flat to keep the jet behavior unambiguous.

**Caves and alcoves** (2 total):
- **Spawn alcoves** — one per spawn side. Edge alcove archetype (2 tiles deep × 3 tiles tall against the outer wall, just past the spawn ramp). Holds AMMO_PRIMARY. Tutorial-scale, easy walk-in.
- **No jetpack alcoves.** Foundry is the tutorial map; learning to jet *into* a nook is M2-week-2 material. The cover wall's top platform fills the "ARMOR is high up" role without an enclosed alcove.

**Pickups**:
- 4× HEALTH small (one per quadrant, two on the hill)
- 2× HEALTH medium (mid-map, near the cover wall)
- 2× AMMO_PRIMARY (in the spawn alcoves — see above)
- 1× ARMOR light (top of the cover wall — requires a jet hop)
- 1× WEAPON Auto-Cannon (left of the cover wall, contested mid-map)
- 1× WEAPON Plasma Cannon (right of the cover wall)
- 1× JET_FUEL (top of the center hill — accessible by walking, not jet-only)

**Special features**: none. This is the baseline. Tutorial register.

**Bake-test acceptance**:
- 12 pickups all grabbed at least once during the 10-min bot run.
- Both spawn quadrants have ≤2× the death rate of the central area.
- No dead zones — every tile-row in the central column gets walked at least 4× by the 16 bots over the run.

### 2. Slipstream — vertical FFA

```
Size: 100 × 50 tiles (3200 × 1600 px)
Modes: FFA (default)
Pickup count: 14
```

**Intent**: the mech moves up. Three vertical layers — basement, main floor, catwalks — with jet hops between them.

**Flow**:
- Basement (lower 4 rows): tunnel with two cover blocks; covered, slow, contains best pickups.
- Main floor: split by a small central island — no through-path on the ground; you have to drop into the basement or jet to the catwalks.
- Upper catwalks: two long platforms left and right, connected by a beam at the very top.

Differs from M4's `build_slipstream`: the basement-to-floor transitions are now **steep slide chutes** (60°, 4 tiles tall) that drop you into the basement at speed; the upper catwalks have **angled overhangs** above them that redirect jet thrust horizontally.

**Slopes, hills, valleys, ceilings**:
- **Basement slide chutes** — two 60° SOLID polygons sloping from the main-floor edge into the basement. You can stand at the top, stop pressing forward, and slide down without input — the ICE polygons that used to be on the upper catwalks now live here too, making the slide more pronounced. Climbing back up the chute by walking is *very slow* (steep + low grip = uphill walking is at <30% speed); jetting up is the intended path.
- **Upper-catwalk ICE patches** — 4-tile-wide ICE polygons on each catwalk; running across them at speed risks sliding off the edge. The original "joke of the map."
- **Angled overhead struts** — three 45° angled OVERHANG polygons above the upper catwalks. Jet straight up under one, your head deflects sideways along the underside. Gives the player a "slide-jet" maneuver that hides them from a mid-floor sniper.
- **2× WIND ambient zones at the slide-chute bottoms** — sideways push during the slide adds skill (you exit the chute drifting toward one wall).

**Caves and alcoves** (6 total — Slipstream's the cave-style map):
- **Basement cave network (4 rooms)** — connected alcoves at the basement floor. The Mass Driver lives in the centermost room (a 3×3-tile cave); two side rooms hold ARMOR heavy and HEALTH large; one tunnel-end room holds AMMO_PRIMARY. Passages between rooms are 3 tiles tall × 2 tiles wide. The whole network is reachable from the slide-chute exit; you slide down, drift on the WIND, end up in one of the rooms. Ambushes inside are 1v1 close-range fights — the basement is where the **Riot Cannon** earns its keep. Pickup audio (Mass Driver respawn cue) telegraphs the room state from up to 800 px.
- **Upper-catwalk jetpack alcoves (2)** — one above each upper catwalk, at the far end. Reachable only by jet from the catwalk surface. 3 wide × 3 tall × 3 deep. One holds POWERUP berserk (90 s respawn — the rare prize); the other holds JET_FUEL (so you can jet back out). The angled overhead struts above the catwalks make the entry path natural — you jet up, deflect sideways along the strut, land in the alcove mouth.

**Pickups** (placement detail above; categories summarized):
- Basement cave network: HEALTH large, ARMOR heavy, AMMO_PRIMARY, WEAPON Mass Driver
- Main floor: 3× HEALTH small, AMMO_PRIMARY×1
- Upper catwalks: JET_FUEL × 1 (in alcove), POWERUP berserk (in alcove)
- Top beam: WEAPON Rail Cannon (high reward, exposed)

**Special features**: see slopes above. ICE + WIND + caves make this the most "physics-readable" map in the rotation — playtesters who get the slope feel here transfer it cleanly elsewhere.

**Bake-test acceptance**:
- All three layers have non-zero death heatmap density.
- The Rail Cannon spot has at least 4 grabs over the bot run (reward for jet skill).
- No layer dominates >50% of total kills.

### 3. Concourse — medium FFA / TDM

```
Size: 100 × 60 tiles (3200 × 1920 px)
Modes: FFA, TDM
Pickup count: 18
```

**Intent**: a big open atrium. Long sightlines for snipers, but cover everywhere within 400 px. The mid-rangle map.

**Flow**:
- Central concourse (main floor): wide, with periodic columns. The Rail Cannon central sightline.
- Side wings (left and right): narrow corridors with their own pickups; opens onto the concourse via 3 doorways each.
- Upper gallery: catwalks above the wings, accessible by jet. Vantage points — "rooftop of the wing."

**Slopes, hills, valleys, ceilings**:
- **Two long 30° hills** under the concourse — the floor isn't flat from spawn to mid; it gently rises and falls. Runners coming from spawn gain speed downhill into the central Rail Cannon fight. This is the gentlest slope vocabulary on any map.
- **Wing-floor valleys** — each side wing has a single 30° bowl (one tile deep at the lowest point) that funnels chase-fights into a small contested basin.
- **No angled ceilings.** Concourse's roof is high-vaulted and flat — the gallery is the play layer above the floor; FOG ambient zones obscure sightlines without touching physics.

**Caves and alcoves** (4 total):
- **Wing-floor edge alcoves (2)** — one in each side wing, opening from the wing's bowl basin. Edge alcove archetype, 2 tiles deep × 3 tiles tall. Holds HEALTH medium and AMMO_PRIMARY. The bowl's slope means you tend to slide *into* the alcove during a chase — designed-in.
- **Upper gallery jetpack alcoves (2)** — one alcove above each wing's gallery catwalk, mouth facing down. Reachable only by jet from the gallery surface. 3 wide × 3 tall × 3 deep. One holds ARMOR light (the gallery's contested vantage); the other holds POWERUP invisibility (the sneaky-flank-from-the-gallery prize). FOG ambient zones obscure the alcove mouths from below — you have to *know* they're there.

**Pickups** (placement detail above):
- Concourse mid: WEAPON Rail Cannon (heavily contested center, at the bottom of the second hill so charging downhill puts you on top of it)
- Side wings: HEALTH medium ×2 each (in alcoves), AMMO_PRIMARY ×2 each (in alcoves), WEAPON Riot Cannon (one wing's basin)
- Upper gallery: HEALTH small ×4 (open catwalk), ARMOR light (in alcove), POWERUP invisibility (in alcove, slow respawn)
- Doorways: AMMO_SECONDARY ×4 (rapid hot-spot fights)

**Special features**:
- 4× decorative columns in the concourse (POLY records, one polygon each).
- 2× FOG ambient zones in the upper gallery (visual effect; doesn't affect physics).
- TDM spawn lanes: red on left wing entrance, blue on right wing entrance (8 spawns each side).

**Bake-test acceptance (FFA)**:
- Concourse + wings + gallery each get >25% of total deaths (no quadrant <10%).
- The Rail Cannon's mid spot has both kills (someone holding it) and deaths (someone trying to take it) in roughly equal measure.

### 4. Reactor — medium FFA / TDM

```
Size: 110 × 42 tiles (3520 × 1344 px)
Modes: FFA, TDM
Pickup count: 16
```

**Intent**: a contested center. The reactor pillar is a 8×12 block in the middle; both teams approach it from the wings. Plays as a wider "Foundry" with a strong contested midpoint.

**Flow**:
- Floor: open on both sides of the pillar.
- Pillar: blocks all sightlines through the map center; you must commit to a side.
- Flanking platforms: elevated, on each side of the pillar; jet up for height advantage.
- High overlooks: 2 small platforms above the pillar's height (jet-only access).

**Slopes, hills, valleys, ceilings**:
- **Bowl floor** — the entire floor between the side walls is a 30° bowl meeting at a flat 4-tile basin under the pillar. Players coming from either spawn accelerate downhill into the contested center; backing away toward your own spawn is uphill (visibly slower).
- **Pillar undersides angled** — the reactor pillar's bottom corners are 45° angled overhangs. A player jetting up under the pillar gets pushed sideways along the underside instead of hard-stopping — turns the underside into traversable space rather than a no-go zone.
- **Flanking ramps** — 45° SOLID polygons from the floor to each flanking platform. Running up the ramp is slower than running down it; the speed differential biases combat downhill toward the center.
- **No angled ceilings other than the pillar.** The world ceiling is the existing flat taper.

**Caves and alcoves** (4 total):
- **Spawn-side edge alcoves (2)** — one on each outer wall at floor level, behind the spawn point. 2 tiles deep × 3 tiles tall. Holds AMMO_PRIMARY. Pure resupply.
- **Pillar-underside slope-roof alcoves (2)** — formed by the bowl floor + the 45° pillar overhangs above. 4 tiles wide at the entrance (under the overhang), 1 tile tall at the back where it meets the pillar. The JET_FUEL canisters live here; reaching them is a jet-slide along the overhang, which is the map's signature movement vocabulary. Entering and exiting is asymmetric — you slide *out* along the angled ceiling but you have to jet *under* it to enter, which is the kind of skill-expression beat the design lives for.

**Pickups** (placement detail above):
- Pillar base, both sides: WEAPON Plasma SMG (twin pickups, exposed)
- Spawn alcoves: AMMO_PRIMARY × 2
- Pillar-underside alcoves: JET_FUEL × 2 (jet-slide entry)
- Floor mid: HEALTH small × 4 (one per quadrant, exposed)
- Flanking platforms: HEALTH medium × 2, ARMOR light × 2
- High overlooks: WEAPON Mass Driver (north), WEAPON Rail Cannon (south) — both heavily contested

**Special features**:
- Reactor core decoration: animated additive sprite at the pillar's base (purely visual, sells the "active reactor" theme).

**Bake-test acceptance**:
- Both flanks get equivalent density (TDM mirror — mirror to a 5% tolerance).
- High overlooks each get 5+ grabs of their power weapon (reward for jet skill).

### 5. Catwalk — TDM, grapple-heavy

```
Size: 120 × 70 tiles (3840 × 2240 px)
Modes: TDM (primary)
Pickup count: 20
```

**Intent**: vertical TDM. Two team bases at opposite *heights* of the map; no ground-floor route between them. You traverse by catwalk + jet + grapple.

**Flow**:
- Lower base (red, bottom-left): floor-level spawn alcove with cover.
- Upper base (blue, top-right): elevated spawn at ~70% of map height.
- Connecting catwalks: 4 platforms across the map at different heights, with gaps between them too wide to jet without good timing.
- **Grapple anchors**: 6 designated grapple-friendly tiles (visually identifiable by overhanging pipes, marked `GRAPPLE_HINT` in editor metadata) that let you swing to the next platform faster than jetting.

**Slopes, hills, valleys, ceilings**:
- **External slide-down slopes** — three 60° external slopes connect the catwalks; you can slide down them at sprint+gravity speed (the fastest movement in the game). Sliding off a 60° catwalk and into a fight at terminal speed is the signature move of this map.
- **Catwalk surfaces** — every catwalk is a 45° gentle slope (not flat) so walking *across* a catwalk is slightly uphill toward the next; running *back* is slightly downhill. Encourages always-forward play.
- **Angled overhead struts** — five 45° angled OVERHANG polygons above the central catwalks. Each grapple anchor is paired with an angled strut — the strut redirects your jet sideways toward where you're trying to grapple, so jet+grapple combos chain cleanly.
- **No flat floors except spawn alcoves.** The map is *intentionally* slope-heavy.

**Caves and alcoves** (5 total):
- **Spawn alcoves (2)** — flat, edge alcove archetype, 3 tiles deep × 3 tiles tall. Holds HEALTH medium ×3 and AMMO_PRIMARY ×2 each side. The two flat-floor refuges in an otherwise slope-heavy map.
- **Mid-catwalk overhead jetpack alcoves (3)** — one above each of three central catwalks, paired with a grapple anchor. 3 wide × 3 tall × 3 deep. Mouth faces down; the angled strut redirects jet entry. Holds (sequenced from inner to outer) JET_FUEL, ARMOR light, POWERUP berserk. The grapple-into-alcove play is the highest-skill maneuver on any map: hook a strut, swing into the alcove mouth, grab the powerup, slide back out via the angled-ceiling jet trick. **This is the map's signature beat.**

**Pickups** (placement detail above):
- Lower base alcove: HEALTH medium × 3, AMMO_PRIMARY × 2 (red side resupply)
- Upper base alcove: HEALTH medium × 3, AMMO_PRIMARY × 2 (blue side resupply)
- Mid-catwalk alcoves: JET_FUEL × 1 (innermost), ARMOR light × 1 (middle), POWERUP berserk × 1 (outermost)
- Mid catwalks open: HEALTH small × 4, JET_FUEL × 3 (open, on catwalk surfaces)
- Highest catwalk: WEAPON Rail Cannon (exposed)
- Lowest gap: WEAPON Mass Driver (suicide pickup; you have to jet up afterwards)

**Special features**:
- 6× grapple-anchor decorations (sprite + a comment in the editor's META "this is a grapple beat"). Functionally identical to other tiles for collision; the visual cue is the gameplay primitive.
- 2× WIND ambient zones at the highest catwalks (sideways push, makes the Rail Cannon spot trickier).

**Bake-test acceptance**:
- Players using the grapple loadout reach the upper base in <8 s on average; without grapple, average is >12 s.
- Mid catwalks have heat-map density between 60–90% of total deaths.
- Neither base dominates kills (both within 20% of each other).

### 6. Aurora — large TDM / FFA

```
Size: 160 × 90 tiles (5120 × 2880 px)
Modes: TDM, FFA
Pickup count: 26
```

**Intent**: open arena. Big sky, distant vista, lots of cover at ground level. Plays differently on FFA (chaos) vs. TDM (territorial).

**Flow**:
- Central pit: a 30-tile-wide depression in the floor; you have to jet to cross it (or grapple).
- Western and Eastern hills: long ramps up to elevated platforms with small bunkers.
- Sky (above the central pit): grapple-only — overhead struts you can swing across.
- Mountain peaks: 2 elevated towers at the map's top corners, requiring multi-step jet ascent.

**Slopes, hills, valleys, ceilings**:
- **Two big 30° hills** — the western and eastern hills are *real* hills, ~10 tiles wide each, that the player walks/runs over. The hill summits hold the Auto-Cannon and Plasma Cannon. Running over the hill toward the central pit gains speed.
- **Central pit valley** — a 45° bowl ~30 tiles wide. The bottom is flat (4-tile basin) for the pickup; the slopes are climbable but slow.
- **No angled ceilings.** Aurora is an open-sky map; there are no overhangs above the play area. The 6 grapple struts are *floating* anchors in the sky, not ceiling polygons.
- **30+ decorative POLY silhouettes** form a distant city skyline at the parallax-far layer.

**Caves and alcoves** (4 total):
- **Hill-side edge alcoves (2)** — one carved into each big hill's outer face, near the spawn-side end. Edge alcove archetype, 3 tiles deep × 3 tiles tall. Holds AMMO ×2 each side (= AMMO ×4 total). Walking *out* of the alcove drops you into the downhill run toward the central pit.
- **Mountain-peak jetpack alcoves (2)** — one in each of the two corner mountain-peak structures. The peak summit is exposed (Rail Cannon there); the alcove is *behind* the peak, mouth facing inward (toward the map). Reachable only by jet up the *back* of the peak. 3 wide × 3 tall × 4 deep. One holds HEALTH large + POWERUP berserk; the other holds HEALTH large only. Risk: jetting up the back of the peak exposes you to the entire opposing peak's Rail Cannon for ~1.5 s.

**Pickups** (placement detail above):
- Central pit: HEALTH small × 3 (encourage jumping in), JET_FUEL × 2
- Western hill summit (exposed): WEAPON Auto-Cannon, ARMOR heavy
- Eastern hill summit (exposed): WEAPON Plasma Cannon, ARMOR heavy
- Hill-side alcoves: AMMO × 4 (resupply on the way to combat)
- Sky struts (exposed, grapple-only): WEAPON Mass Driver, HEALTH small × 2
- Mountain peaks (exposed summit): WEAPON Rail Cannon × 2
- Mountain-peak alcoves: HEALTH large × 2, POWERUP berserk × 1

**Special features**:
- 6× grapple-friendly overhead struts (visual decoration for the gameplay primitive).
- 1× ZERO_G ambient zone at the very top of the map (encourages floating, makes the Rail Cannon shot easier).
- 30+ POLY decorations of distant skyline silhouettes (visual depth).

**Bake-test acceptance**:
- Central pit + hills + sky + peaks each get >15% of total deaths.
- Mass Driver in the sky is grabbed at least 6 times over the bot run (rewards skill).

### 7. Crossfire — TDM / **CTF** (first CTF map)

```
Size: 180 × 85 tiles (5760 × 2720 px)
Modes: TDM, CTF
Pickup count: 30
```

**Intent**: the introductory CTF map. Mirror-symmetric: red base on the left, blue base on the right, a wide central battleground between them. Designed to teach CTF flow.

**Flow**:
- Red base (far left): flag base on a small elevated platform; spawn alcove behind it; resupply pickups inside.
- Blue base (far right): mirror.
- Central battleground: ~70 tile-wide combat zone with multi-layer architecture — 2 catwalks, 2 sky bridges, plenty of cover.
- Two **flank routes**: tunnels under the central battleground (low path); rooftops above (high path with grapple anchors).

**Slopes, hills, valleys, ceilings**:
- **30° entry ramps** — each base has a 4-tile-wide ramp from the central battleground up to the flag platform. Carriers escaping with the flag have to *climb the ramp* (slow), but defenders chasing them have to *climb the ramp* too — defensive symmetry.
- **45° angled struts above central mid** — three angled OVERHANG polygons above the mid-map. Combatants jetting up under them get redirected sideways, which makes mid-air dodge feel responsive.
- **Mirror-symmetric** — the slope vocabulary on the red side is the exact mirror of the blue side. CTF symmetry depends on this.

**Caves and alcoves** (6 total — symmetric, 3 per side):
- **Base resupply alcoves (2)** — one in each team base, opening from the base's interior. Edge alcove archetype, 4 tiles deep × 3 tiles tall (slightly bigger than the standard so it can hold the resupply set). Holds HEALTH medium × 4, AMMO × 4, ARMOR light, WEAPON Pulse Rifle for that team. Carriers running *into* their base for capture pass right by the alcove; defenders camping the alcove can resupply mid-fight.
- **Flank-tunnel cave segments (2)** — each flank tunnel is a 6-tile-long covered passage with one alcove room mid-tunnel. Holds HEALTH small × 2 and AMMO_SECONDARY × 2 each. The alcove forces the tunnel-runner to commit to a brief stop — defenders can intercept here.
- **Central-high jetpack alcoves (2)** — one above each side of the central battleground, mouth facing center. Reachable by jet from the central catwalks. 3 wide × 3 tall × 3 deep. Each holds POWERUP invisibility (90 s respawn). Asymmetric *prize* — only one team usually grabs theirs per cycle. The classic "cloak-and-grab-the-flag" play.

**Pickups** (placement detail above):
- Red base alcove: HEALTH medium × 4, AMMO × 4, ARMOR light, WEAPON Pulse Rifle
- Blue base alcove: mirror
- Flank-tunnel alcoves (one per side): HEALTH small × 2, AMMO_SECONDARY × 2
- Central low: HEALTH large, ARMOR heavy, JET_FUEL × 4 (exposed)
- Central mid: WEAPON Mass Driver, WEAPON Rail Cannon (exposed, contested)
- Central-high alcoves (one per side): POWERUP invisibility

**Special features**:
- 6× grapple anchors on the rooftops.
- 2× FLAG records (red on left, blue on right).
- Symmetric spawn lanes: 8 red on the left, 8 blue on the right.
- META mode_mask = `MATCH_MODE_TDM | MATCH_MODE_CTF`.

**Bake-test acceptance (CTF)**:
- Average time-to-capture: 60–90 s (slow enough to be hard, fast enough to score).
- 30%+ of round time has at least one flag carried.
- Flag returns roughly equal red and blue (mirror symmetry validated).

### 8. Citadel — XL **CTF**

```
Size: 200 × 100 tiles (6400 × 3200 px)
Modes: CTF (primary)
Pickup count: 32
```

**Intent**: the "endgame" CTF map. Big, layered, with deep team-defensible architecture. Grappling and jet skill are not optional.

**Flow**:
- Two team castles: each has a flag base inside a defensible structure.
- Inner courtyards: the spawn area for each team, with all resupply pickups.
- Outer walls: elevated catwalks circling the courtyards; defenders camp here.
- Central plaza: the "neutral zone" where attackers from both teams meet. Filled with cover.
- Sky bridges: 2 high-altitude bridges crossing the plaza; grapple-friendly.
- Underground tunnels: 2 low-route tunnels connecting the bases through the plaza; slow but covered.

**Slopes, hills, valleys, ceilings**:
- **Plaza bowl** — the entire central plaza is a 30° bowl, 60 tiles wide, meeting at a flat 8-tile basin holding the Mass Driver. Anyone running into the plaza accelerates downhill toward the Mass Driver — the most contested spot is a *physics attractor* that pulls fights toward it.
- **Steep castle ramparts** — 60° outer slopes on each castle wall. Defenders can slide *down* a rampart at speed to engage attackers; attackers walking *up* a rampart move at <30% speed. Strongly favors defense, which is the right shape for CTF castles.
- **Angled castle ceilings** — the castle interiors have 45° angled rooflines (overhang polygons). A defender jetting up inside their castle deflects sideways under the roofline; the move is "jump-up-jet-along-roof to a defensive vantage."
- **Tunnel grades** — the underground tunnels gently rise and fall (30° grades). Carriers in the tunnel can *slide down* into the plaza basin from the tunnel exit, ambushing the Mass Driver fight from below.
- **Full slope vocabulary appears**: 30° (plaza, tunnels), 45° (castle ceilings), 60° (ramparts). Citadel is the only map with all three.

**Caves and alcoves** (10 total — Citadel is the cave-richest map):
- **Castle-interior cave systems (2 networks, 3 rooms each)** — each castle has a small dungeon inside: a flag-room (where the FLAG entity sits, ~4 wide × 4 tall), a side resupply room (HEALTH medium × 6, AMMO × 6, ARMOR ×2, WEAPON Pulse Rifle + Plasma SMG resupply), and a defender's perch (small alcove with view of the castle entrance, AMMO_SECONDARY × 2). Passages between are 3 tall × 2 wide. Defenders camp the network; attackers must penetrate it.
- **Castle slope-roof nooks (2)** — formed by each castle's angled roofline + the floor below. Slope-roof archetype, 4 wide at entrance × 1 at back. Each holds ARMOR reactive (rare). The entry is the angled-ceiling jet-slide move; you ride the roofline laterally, drop into the alcove. Fits the "fortress-defensive vantage" theme.
- **Tunnel choke alcoves (2)** — one in each underground tunnel, near the midpoint. Edge alcove archetype with a slight 30° slope at the entrance (inheriting the tunnel's grade). 3 deep × 3 tall. Holds **POWERUP godmode** (180 s respawn). High-stakes — the slow grade means you can't exit fast, and a defender at the choke trades the godmode user 5 s of god-time for the chance to control the entire tunnel.
- **Sky-bridge overlook alcoves (2)** — small alcoves on the underside of each sky bridge, mouth facing down toward the plaza. Reachable only by grapple. 3 wide × 3 tall × 2 deep. Holds JET_FUEL each. The grapple-onto-sky-bridge-and-into-alcove combo is the most acrobatic move in the game.

**Pickups** (placement detail above):
- Castle cave networks: HEALTH medium × 6, AMMO × 6, ARMOR × 2, WEAPON Pulse Rifle + Plasma SMG, AMMO_SECONDARY × 2 (per side)
- Castle slope-roof nooks: ARMOR reactive × 2 (one per castle)
- Plaza basin: WEAPON Mass Driver (at the lowest point of the bowl)
- Plaza mid: WEAPON Rail Cannon, HEALTH large, JET_FUEL × 4 (exposed)
- Sky-bridge alcoves: JET_FUEL × 2 (grapple-only)
- Tunnel choke alcoves: POWERUP godmode × 2 (very rare, very high stakes)

**Special features**:
- 2× FLAG records (one per castle).
- 12+ grapple anchors across the sky bridges and outer castle walls.
- 4× WIND ambient zones at the plaza height (varied directions, randomizes long-distance shots).
- 2× ACID zones (small) at the tunnel ends (5 HP/s — punishment for camping the choke).

**Bake-test acceptance (CTF)**:
- Average time-to-capture: 90–150 s.
- Both teams' captures within 30% of each other (symmetric).
- Tunnels see 20%+ of total traffic (encouraging the low route).
- Plaza Mass Driver is grabbed at least 8 times over the 10-min bake-test.

## Bake test (across all maps)

The acceptance criteria above call out per-map heatmap targets. The infrastructure is shared:

```c
// tests/bake/run_bake.c — new test harness
//
// Usage: ./build/bake <map_short_name> [--bots N] [--duration_s S]
//   Spawns 8 bots per team (16 total in TDM/CTF; 16 single-team in FFA),
//   runs a real `simulate()` loop for `duration_s` seconds,
//   logs every kill event + every pickup grab + every flag event,
//   dumps three CSV files: kills.csv, deaths.csv, pickups.csv,
//   then a heatmap.png compositing all three.
```

Bot AI is intentionally crude: wander toward a random spawn point, aim at any enemy in line-of-sight (within 800 px), shoot. No flag-running heuristic, no pickup priority. This generates noisy data but the heatmap shape is informative — dead zones and spawn imbalance show up loud.

```bash
# Run the bake test on every map, in parallel:
$ ./tools/bake/run_all.sh
[1/8] foundry      ✓  12 pickups grabbed (12 unique)  spawn imbalance 1.1×  no dead zones
[2/8] slipstream   ✓  14 pickups grabbed (14 unique)  spawn imbalance 1.0×  no dead zones
[3/8] concourse    ✗  18 pickups grabbed (16 unique)  ARMOR-light at upper-gallery: 0 grabs
[4/8] reactor      ✓  ...
...
```

A map that fails its acceptance criteria (e.g., Concourse's upper gallery armor never grabbed) is **iterated in the editor** — move the pickup, re-run, repeat — until it passes. We do this loop in week 4 of the milestone.

The editor has a built-in "run bake test" button (Ctrl+B) that calls the bake harness and renders the heatmap atop the editor's view. Closes the iteration loop tightly.

## Vote picker UI (M4 carry-forward)

The lobby UI gets a three-card map vote modal at round end (currently a TRADE_OFFS entry: "Map vote picker UI is partial"). Implementation:

- Server picks 3 random maps from the rotation (excluding the just-played one) when entering MATCH_PHASE_SUMMARY.
- The vote candidates are broadcast via the existing `LOBBY_VOTE_STATE` message (already wired).
- Summary screen surfaces a 3-card panel: each card shows map name, blurb, screenshot (loaded from `assets/maps/<short>_thumb.png`), and a "Vote" button.
- Each player can vote once. Real-time tally updates via the existing `LOBBY_VOTE_STATE` rebroadcasts.
- 15 s vote timer; ties broken by random.
- Winner is auto-applied to next round's `match.map_id` via the existing `lobby_vote_winner` path.

UI in `src/lobby_ui.c::summary_screen_run` — adds ~150 LOC for the modal panel.

Resolves the M4 trade-off entry "Map vote picker UI is partial."

## Host controls (M4 carry-forward)

The kick/ban backend is wired (NET_MSG_LOBBY_KICK, NET_MSG_LOBBY_BAN; `lobby_ban_addr`). M5 surfaces it in the player list:

- In the lobby's player-list rows, when the local user is the host, hovering a non-host row shows two small buttons: `[Kick]` `[Ban]`.
- Click → modal confirmation: "Kick PlayerName?" / "Ban PlayerName? (persists across host restarts)".
- Confirmed → `net_client_send_kick(slot)` / `net_client_send_ban(slot)`.

Plus `bans.txt` persistence (the M4 trade-off "bans.txt not persisted"):

- On server start: load `bans.txt` from the binary's directory. Format is one ban per line: `IP_HEX NAME`. ~50 LOC in `lobby.c`.
- On `lobby_ban_addr`: append to `bans.txt`. (Not durable to a crash mid-write; documented as accepted.)
- Resolve the M4 trade-off entries.

UI in `src/lobby_ui.c::player_row` — adds ~80 LOC for the row buttons + a small modal helper.

## Per-map asset checklist

Each map ships with these assets in `assets/`:

```
assets/
├── maps/
│   ├── <name>.lvl                    # the level data
│   └── <name>_thumb.png              # 256×144 thumbnail for the vote picker
├── parallax/
│   └── <kit>_far.png                 # 1024×512 sky/distant
│   └── <kit>_mid.png                 # 1024×512 mid-distance
│   └── <kit>_near.png                # 1024×512 foreground silhouettes
├── music/
│   └── <kit>.ogg                     # ~2 MB OGG-Vorbis
└── sfx/
    └── ambient_<kit>.ogg              # 30 s ambient loop
```

8 maps × 3 parallax PNGs (1.5–3 MB each) = ~20 MB.
8 music tracks × ~2 MB = ~16 MB.
8 ambient loops × ~0.5 MB = ~4 MB.
8 thumbnails × ~50 KB = ~0.4 MB.
8 .lvl files × ~50 KB = ~0.4 MB.
**Total per-map content footprint: ~40 MB.**

That's the bulk of the binary's <50 MB ship target ([10-performance-budget.md](../10-performance-budget.md)). Tight but workable.

## Done when

- All 8 `assets/maps/<name>.lvl` files are checked in.
- Each map's bake-test passes its acceptance criteria.
- Map thumbnails (`<name>_thumb.png`) exist and are referenced from the editor's META + the vote picker UI.
- Vote picker UI works end-to-end: round ends → 3 cards appear → players vote → winner becomes next round's map.
- Host-controls (kick/ban buttons + bans.txt persistence) work end-to-end; the M4 trade-off entries are deleted.

## Trade-offs to log

- **Hand-authored over procedural.** No procedural generation (per the design canon).
- **Fixed pickup placement, not "designer placeholder + tuner."** A designer who wants to A/B two pickup positions has to ship two versions of the map. We don't have placement-by-rule tooling.
- **Bake-test bot is intentionally crude.** Wander + shoot; no flag heuristic. We accept biased heatmaps as long as they catch dead zones.
- **No map-specific gameplay overrides.** A map can't say "my pickups respawn 50% slower" except via the per-spawner `respawn_ms` override (which is enough).
- **Map thumbnails are static PNGs**, not in-engine renders. We could render them at editor save time; we don't, to keep the editor's dependency on raylib's framebuffer state simple.
