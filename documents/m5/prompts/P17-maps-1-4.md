# P17 — Author maps 1-4 (Foundry, Slipstream, Reactor as `.lvl` + Concourse)

## What this prompt does

Converts the three M4 code-built maps to `.lvl` files using a one-shot exporter (the stub from P01). Authors a fourth new map — Concourse — using the editor (P04). Each map ships with its tile atlas, parallax kit, decoration set, audio kit (path references), spawn lanes, pickup placement (per the per-map specs in `documents/m5/07-maps.md`), slope vocabulary, and caves/alcoves.

Depends on P01 (.lvl format), P02 (slope physics + polygon collision), P04 (editor), P05 (pickups), P10/P12 (atlas + damage feedback), P13 (parallax + tile sprites), P14 (audio), P16 (asset content).

## Required reading

1. `CLAUDE.md`
2. **`documents/m5/07-maps.md`** §"Foundry", §"Slipstream", §"Reactor", §"Concourse"
3. `documents/m5/01-lvl-format.md` — the wire layout
4. `documents/m5/03-collision-polygons.md` — slope features
5. `documents/m5/04-pickups.md` — pickup placement guidelines
6. `src/maps.c` — `build_foundry`, `build_slipstream`, `build_reactor` (the source for the exporter)
7. `tools/cook_maps/cook_maps.c` — the stub from P01

## Concrete tasks

### Task 1 — Implement `tools/cook_maps/`

Per `documents/m5/01-lvl-format.md`'s shape:

```c
int main(int argc, char **argv) {
    init permanent + level arena;
    for each MAP_FOUNDRY/MAP_SLIPSTREAM/MAP_REACTOR:
        build the existing code-built map into a temporary World;
        add slope/cave/alcove polygons per the per-map briefs in 07-maps.md;
        place spawns + pickups per the brief;
        set META;
        level_save("assets/maps/<short>.lvl");
}
```

Run via `make cook-maps` (new top-level Makefile target). Produces 3 `.lvl` files.

The exporter does NOT use the editor. It synthesizes the World programmatically. The editor is for authoring NEW maps; the exporter handles the legacy carry-overs.

### Task 2 — Foundry slope/alcove augmentation

Per `documents/m5/07-maps.md` §"Foundry":

The M4 Foundry is mostly flat. Add (in the cook_maps exporter):

- 30° center hill (3 tiles wide × 1 tile tall) as a polygon.
- 45° spawn ramps to each spawn platform (replacing M4's vertical-jet-only access).
- 2 spawn alcoves (edge alcove archetype) holding AMMO_PRIMARY.
- Pickup placement per the brief: 4× HEALTH small, 2× HEALTH medium, 2× AMMO_PRIMARY, 1× ARMOR light, 1× WEAPON Auto-Cannon, 1× WEAPON Plasma Cannon, 1× JET_FUEL.
- META: name "Foundry", blurb, mode_mask = FFA, kit "industrial".

### Task 3 — Slipstream slope/cave augmentation

Per `documents/m5/07-maps.md` §"Slipstream":

The M4 Slipstream has 3 vertical layers. Add:

- 60° basement slide chutes from main floor to basement (replacing the M4 vertical drops).
- 2 ICE patches on upper catwalks (4-tile-wide ICE polygons).
- 3 angled overhead struts (45° OVERHANG polygons) above upper catwalks.
- 2 WIND ambient zones at slide-chute bottoms.
- 4-room basement cave network (as 4 connected alcoves) holding HEALTH large + ARMOR heavy + WEAPON Mass Driver + AMMO_PRIMARY.
- 2 upper-catwalk jetpack alcoves holding POWERUP berserk + JET_FUEL.
- META: name "Slipstream", blurb, mode_mask = FFA, kit "maintenance".

### Task 4 — Reactor slope/alcove augmentation

Per `documents/m5/07-maps.md` §"Reactor":

- Bowl floor (30° on each side).
- 45° pillar undersides as overhang polygons.
- 45° flanking ramps from floor to flanking platforms (replacing M4's vertical-jet-only).
- 2 spawn-side edge alcoves holding AMMO_PRIMARY.
- 2 pillar-underside slope-roof alcoves holding JET_FUEL × 2.
- Pickup placement per the brief.
- META: name "Reactor", blurb, mode_mask = FFA | TDM, kit "reactor".

### Task 5 — Author Concourse via the editor

Per `documents/m5/07-maps.md` §"Concourse":

Open `./build/soldut_editor`, build a new map matching the brief:

- Size 100×60 (3200×1920 px world).
- Two long 30° hills under the concourse roof.
- Wing-floor 30° valleys (one tile deep at lowest point).
- 4 cover columns in the concourse (decorative POLYs).
- 2 FOG ambient zones in the upper gallery.
- Spawn lanes: red on left wing entrance (8 spawns), blue on right wing entrance (8 spawns).
- Pickups: WEAPON Rail Cannon mid; HEALTH medium ×2 each wing; AMMO_PRIMARY ×2 each wing; WEAPON Riot Cannon (one wing); HEALTH small ×4 upper gallery; ARMOR light upper gallery (in jetpack alcove); POWERUP invisibility (alcove); AMMO_SECONDARY ×4 doorways.
- 4 alcoves: 2 wing-floor edge + 2 upper-gallery jetpack.
- META: name "Concourse", blurb, mode_mask = FFA | TDM, kit "atrium".

Save as `assets/maps/concourse.lvl`. Hand-craft the layout in the editor; ~2-4 hours of map-design work.

### Task 6 — Asset kits per map

Per `documents/m5/07-maps.md` §"Per-map asset checklist":

For each map (foundry, slipstream, reactor, concourse):

```
assets/maps/<short>/
├── <short>.lvl
├── thumb.png         # 256×144 — for vote picker (P09)
├── parallax_far.png  # from P16
├── parallax_mid.png
├── parallax_near.png
├── tiles.png         # kit tile atlas
└── decorations/      # per-map decoration sprites
```

If any kit asset isn't ready from P16, ship a placeholder PNG and document in the art log.

### Task 7 — Bake-test for each map

After the maps are saved, run the bake-test harness (P18 builds the harness; this prompt assumes it's available — if not, skip to P18 and come back):

```bash
./build/bake foundry --bots 8 --duration_s 600
```

Check the heatmap.png output. Per the per-map acceptance criteria in the briefs, iterate the layout if a map fails.

For P17, "good enough" for now is "the map plays without crashing." Full bake-test passing is P18.

## Done when

- `make cook-maps` produces `assets/maps/foundry.lvl`, `slipstream.lvl`, `reactor.lvl`.
- `assets/maps/concourse.lvl` exists, authored in the editor.
- All four maps load via `level_load` in the host's match flow.
- Each map has a parallax kit, tile atlas, audio kit (path references in META).
- Spawn lanes work: 8 mechs spawn distributed across the map.
- Pickups visible in-world (placeholder sprite art OK).
- The fallback to code-built maps in `maps.c` is now dead code — remove it (or keep as commented for safety).
- `make && tests/net/run_3p.sh` passes a TDM round on Reactor.

## Out of scope

- Maps 5-8 — P18.
- Bake-test harness implementation — P18 builds it; P17 only authors.
- Polish iteration on maps that fail bake-test — P18.

## How to verify

```bash
make
make cook-maps
./soldut --host 23073 cfg=foundry      # play foundry
./soldut --host 23073 cfg=concourse    # play concourse
```

Or just rely on the rotation in `soldut.cfg`:

```
map_rotation=foundry,slipstream,reactor,concourse
```

## Close-out

1. Update `CURRENT_STATE.md`: 4 maps shipped as `.lvl`.
2. Update `TRADE_OFFS.md`:
   - **Delete** "Hard-coded tutorial map" once cook_maps produces shippable .lvl files.
3. Don't commit unless explicitly asked.

## Common pitfalls

- **Slope polygon coordinates**: tile-aligned coords for slope polygons make collision more reliable. Test by walking up; if there's a snag, the slope polygon's vertices probably aren't aligned to a tile boundary.
- **Spawn point density**: 8 lanes per team for TDM/CTF; 16 lanes for FFA. Fewer = clustered spawns = telefrag risk.
- **Cave-room internal sightlines**: cave rooms with all 4 walls SOLID and a 1-tile mouth read as a kill-box for anyone inside — adjust the mouth width if a room's K/D ratio is broken in bake-test.
- **META string-table**: every string asset path goes through STRT. Don't inline raw strings in the META struct.
- **Concourse pickup spacing**: 18 pickups in a 100×60 map is roughly the upper bound. Per the editor's validation, two pickups can't be within 64 px of each other.
- **The cook_maps tool can't use the editor's polygon presets directly** — it has to call `level_save` against a manually-built World. So the exporter has to hand-construct the slope polygons, alcoves, etc.
