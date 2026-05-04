# P18 — Maps 5-8 (Catwalk / Aurora / Crossfire / Citadel) + bake-test harness

## What this prompt does

Authors the remaining four maps in the editor (Catwalk / Aurora / Crossfire / Citadel — each with its slope vocabulary, caves/alcoves, pickup placement, audio kit, parallax kit). Builds the bake-test harness (`tools/bake/`) — a headless 8v8-bot run with heatmap output that verifies per-map acceptance criteria. Iterates each map until it passes.

This is M5's content close-out and the milestone's final ship gate.

Depends on every previous prompt. Realistic time: 2-4 days of authoring + 1 day of bake-test iteration.

## Required reading

1. `CLAUDE.md`
2. **`documents/m5/07-maps.md`** §"Catwalk", §"Aurora", §"Crossfire", §"Citadel" + §"Bake test (across all maps)"
3. `documents/m5/06-ctf.md` — for Crossfire/Citadel CTF requirements
4. `documents/m5/05-grapple.md` — for Catwalk's grapple beats
5. `tests/headless_sim.c` — model for the bake harness

## Concrete tasks

### Task 1 — Bake-test harness `tools/bake/`

```
tools/bake/
├── run_bake.c            # main — spawns N bots, runs duration_s, dumps CSVs + heatmap
├── bot_ai.c              # crude wandering bot
├── heatmap.c             # CSV → PNG composite
└── Makefile
```

Per `documents/m5/07-maps.md` §"Bake test":

```c
int main(int argc, char **argv) {
    parse args: map short_name, --bots N, --duration_s S
    init engine (no window — headless like tests/headless_sim.c)
    level_load assets/maps/<short>.lvl
    spawn N bots per team (or N total in FFA)
    run simulate() for S seconds
    dump:
        kills.csv     (every kill event, pos + tick)
        deaths.csv    (every death, pos + tick)
        pickups.csv   (every pickup grab, spawner_id + tick)
    heatmap.c composites all three onto a top-down map render → heatmap.png
}
```

Bot AI: wander toward random spawn; aim at any enemy in line-of-sight within 800 px; shoot. No flag heuristic, no pickup priority. ~150 LOC.

`tools/bake/run_all.sh` runs the harness on every shipped map and emits a summary table.

### Task 2 — Author Catwalk

Per `documents/m5/07-maps.md` §"Catwalk":

- 120×70 tiles (3840×2240 px).
- TDM-primary; mode_mask = TDM.
- 60° external slide-down slopes connecting the catwalks.
- 45° catwalk surfaces (every catwalk slightly sloped).
- 5 angled overhead struts (45° OVERHANG polygons) paired with grapple anchors.
- 2 spawn alcoves (flat) at lower base + upper base.
- 3 mid-catwalk overhead jetpack alcoves holding JET_FUEL + ARMOR light + POWERUP berserk.
- 2 WIND ambient zones at the highest catwalks.
- Pickups per the brief.

### Task 3 — Author Aurora

Per `documents/m5/07-maps.md` §"Aurora":

- 160×90 tiles.
- TDM/FFA; mode_mask = TDM | FFA.
- Two 30° hills (~10 tiles wide) — real hills, not platforms.
- 45° central pit valley (~30 tiles wide; 4-tile basin at bottom).
- 6 floating overhead struts (grapple anchors only).
- 1 ZERO_G ambient zone at the very top.
- 30+ POLY decorations of distant skyline silhouettes.
- 2 hill-side edge alcoves + 2 mountain-peak jetpack alcoves.
- Pickups per the brief.

### Task 4 — Author Crossfire (first CTF map)

Per `documents/m5/07-maps.md` §"Crossfire":

- 180×85 tiles.
- TDM | CTF; mode_mask both.
- Mirror-symmetric layout (red on left, blue on right).
- 30° entry ramps to each base.
- 45° angled struts above central mid.
- 2 FLAG records (one per team).
- 8 spawn lanes per team.
- 2 base resupply alcoves (4×3 each, holding HEALTH + AMMO + ARMOR + WEAPON Pulse Rifle).
- 2 flank-tunnel cave segments (alcove room mid-tunnel).
- 2 central-high jetpack alcoves holding POWERUP invisibility.
- Pickups per the brief.

### Task 5 — Author Citadel

Per `documents/m5/07-maps.md` §"Citadel":

- 200×100 tiles (6400×3200 px) — the biggest map.
- CTF-primary; mode_mask = CTF.
- 30° plaza bowl (60 tiles wide).
- 60° castle ramparts.
- 45° angled castle ceilings.
- 30° tunnel grades.
- 2 FLAG records.
- 12+ grapple anchors.
- 4 WIND ambient zones at plaza height.
- 2 ACID zones at tunnel ends.
- 10 caves/alcoves total (2 castle dungeons of 3 rooms each + 2 castle slope-roof nooks + 2 tunnel choke alcoves + 2 sky-bridge overlook alcoves).
- Pickups per the brief — including the rare POWERUP godmode in tunnel choke alcoves.

### Task 6 — Map vote thumbnails

Each new map needs a 256×144 `thumb.png` for the P09 vote picker. Quickest path: render the map in the editor at zoom-out, screenshot, scale to 256×144. Or generate via Pipeline 3-style ComfyUI for a stylized thumbnail. Quality bar is low (functional, identifiable); polish is M6.

### Task 7 — Bake-test iteration

For each of the 8 maps (4 from P17 + 4 here):

```bash
./build/bake <short> --bots 8 --duration_s 600
# inspect heatmap.png
# if any per-map acceptance criterion fails (per documents/m5/07-maps.md briefs):
#   open editor, adjust, save, re-bake
```

Iterate until all maps pass acceptance.

## Done when

- `tools/bake/` exists; `make bake` builds it; `tools/bake/run_all.sh` runs against all 8 maps.
- All 8 `.lvl` files exist under `assets/maps/<short>/`.
- All 8 maps pass their per-map bake-test acceptance criteria (or, if some fail, document the gap as a TRADE_OFFS entry rather than blocking ship).
- A 4v4 LAN playtest run on Crossfire / Citadel plays a CTF round end-to-end.
- An 8-player FFA on Aurora plays without crashes.
- Map thumbnails for all 8 maps.

## Out of scope

- Polish iteration on maps that pass bake-test but feel underwhelming — that's M6.
- New mode types (KOTH, asymmetric assault) — explicitly out of scope per the design canon.
- Procedural maps — out of scope.
- Performance optimization on Citadel-sized maps if the bake-test reveals frame-time issues — escalate to a separate fix pass.

## How to verify

```bash
make
make cook-maps                  # ensures P17 maps still build
make bake
./tools/bake/run_all.sh
# inspect heatmaps for all 8 maps; iterate until all pass
```

Then a full LAN playtest:

```bash
./tests/net/run_3p.sh   # confirm 3-player smoke still works on the new maps
```

## Close-out

1. Update `CURRENT_STATE.md`: M5 milestone complete. Bullet "M5 — Maps & content" → `Status: shipped <date>`.
2. Update `TRADE_OFFS.md`: sweep one final time. Any pre-disclosed entries from earlier prompts get full entries. Anything unresolved gets a clear "revisit when" trigger.
3. Update `documents/11-roadmap.md` M5 section: mark every checkbox done.
4. Add a final row to `documents/art_log.md` per map.
5. Update `assets/credits.txt` with all final assets.
6. Don't commit unless explicitly asked.

## Common pitfalls

- **CTF symmetry on Crossfire**: mirror left and right within ~5% tolerance. Bake-test will detect asymmetric kill rates.
- **Citadel's 10 alcoves**: keep the validator from rejecting any of them. Each must be ≥3 tiles tall × ≥2 deep.
- **Aurora's open-sky parallax**: this is the one map without a ceiling. Make sure the world-edge ceiling taper still works (existing code).
- **Bake-test bots are crude**: don't expect them to use the grapple. Catwalk's "needs grapple" beats won't be exercised by the bots; mark these as needing manual playtest.
- **Performance on Citadel**: 200×100 tiles × per-particle slope-aware physics × 32 mechs = the worst case. If frame time breaks 16.6 ms, profile and either reduce constraint iterations off-camera or sleep-aggressively.
- **Map files larger than the 500 KB cap**: Citadel might push it. If it does, decide whether to relax the cap or trim decoration count.
- **The fallback to code-built maps in `maps.c`** can be deleted now that all 8 maps ship as `.lvl`. Or leave as commented-out for emergency recovery. Document the decision.
