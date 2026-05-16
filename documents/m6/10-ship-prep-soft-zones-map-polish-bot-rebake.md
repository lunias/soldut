# M6 P10 — Ship-Prep: Soft Ambient Zones, Map Polish, Bot Rebake

**Status:** PLAN — write phase done 2026-05-16; implementation begins next session.
**Target ship:** 2026-05-17 morning, Windows .exe distributed to friends.
**Branch:** create `lunias/m6-p10-ship-prep` off `main`; merge via PR once green.

This document is a self-contained prompt to a future Claude session. Read it
top-to-bottom before touching code. The implementation order in §6 is the
load-bearing one — phases later in the list can be cut for time, phases
earlier in the list cannot.

---

## 0. Scope and non-goals

**In scope (priority-ordered):**

1. **Soft ambient-zone boundaries.** The user explicitly flagged that the
   hard rectangular edge of `AMBI_ZERO_G` / `AMBI_ACID` / `AMBI_FOG`
   overlays "breaks immersion" in-game. The editor's hard-edge preview is
   correct (designers need to see exact bounds); the in-game render must
   feather the edge.
2. **Windows ship-readiness verification.** `make windows` builds clean,
   `Soldut.exe` boots, host-and-join LAN sanity check, asset zip.
3. **Bake rebake.** Re-run `make bake-all` post-changes to confirm no
   regressions vs the 2026-05-15 baseline in `build/bake/`. If bots
   underperform on atmospherics-heavy maps (CITADEL / SLIPSTREAM / AURORA),
   add minimal hazard awareness (§4.2).
4. **Map content polish.** Targeted: fix theme duplication
   (CONCRETE × 2 → CONCRETE + OVERGROWN; NEON × 2 stays — Aurora is
   differentiated by DUST weather), add RAIN to one map (currently unused
   weather mode), drop 2–6 `push_deco` calls per map so the world stops
   feeling empty.

**Non-goals (explicitly out of scope):**

- New maps, new chassis, new weapons.
- Wire-protocol changes (stay on `S0LK`). All map and atmosphere data ships
  in the existing `.lvl` + map-share pipeline.
- Editor changes beyond what §2 strictly requires (the editor's hard-edge
  preview is correct and stays).
- Per-map sprite-deco atlases under `assets/maps/<short>/decorations.png` —
  the runtime already has a layer-tinted placeholder fallback in
  `src/map_kit.c`; that's what ships. Real deco art is M6 P11+.
- Smarter bot AI beyond hazard *avoidance*. The "Quake III layered AI" in
  `src/bot.c` stays; we add an ACID-rect penalty in the nav-cost function
  and stop at that.
- Tag/release automation. The user is shipping by hand to friends; we
  produce the `.exe` + assets zip and stop.

**Hard gates for ship:**

- All asserting unit tests pass: `test-level-io`, `test-pickups`,
  `test-ctf`, `test-snapshot`, `test-spawn`, `test-prefs`, `test-map-*`,
  `test-grapple-ceiling`, `test-frag-grenade`, `test-mech-ik`,
  `test-pose-compute`, `test-damage-numbers`.
- Paired-process: `tests/net/run.sh 2p_basic`, `tests/net/run_3p.sh`,
  `tests/shots/net/run.sh 2p_basic`, `tests/shots/net/run_kill_feed.sh`.
- `make bake-all`: all 8 maps PASS verdict (informational thresholds — see
  §5.2 for what we actually check).
- `make windows` clean; `Soldut.exe` boots in WSL/Wine smoke test or
  confirmed on a Windows host.
- Pre-existing flake `tests/shots/net/run_ctf_walk_own_flag.sh` may stay
  red (documented in `CURRENT_STATE.md`; do not chase it for ship).

---

## 1. Context: where we are right now (2026-05-16 snapshot)

Read `CLAUDE.md` first if you haven't this session — it has the canonical
M6-state paragraph. Quick recap:

- **M6 P09 shipped.** Tile-flag visuals (`ICE`/`DEADLY`/`ONE_WAY`/
  `BACKGROUND`), per-map atmospherics (theme palette + sky gradient + fog +
  vignette + weather), and all 4 `AmbiKind` zones now have runtime visuals.
  `FOG` got both a shader uniform pass and a gameplay effect (haze).
  Editor and runtime read identical data through `LvlMeta`.
- **8 maps cooked** by `tools/cook_maps/cook_maps.c` per the §10.7 theme
  spec. All 8 PASS the May-15 bake.
- **Wire protocol unchanged** at `S0LK`. `.lvl` distribution carries the
  atmospherics block for free.

**State of the gameplay-affecting ambient bits (do not regress these):**

- `AMBI_WIND`: `src/physics.c:850-940`, target-velocity push capped at
  `WIND_MAX_SPEED_PXS = 700` and `WIND_MAX_ACCEL_PXS2 = 1500`.
- `AMBI_ZERO_G`: `src/physics.c:930-938`, sets per-particle zero-gravity
  mask while inside the rect.
- `AMBI_ACID`: `src/mech.c:1576-1601`, 5 HP/s environmental damage via
  `mech_apply_environmental_damage`.
- `AMBI_FOG`: shader-side haze in `assets/shaders/halftone_post.fs.glsl`
  `zone_fog_density()` (the soft `exp(-r²/r²)` falloff already exists for
  FOG — we mirror its shape for ACID/ZERO_G in §2).

**Ship-readiness punch-list snapshot:**

- ✅ All 8 `.lvl` + thumbnails on disk
- ✅ 70 audio files (47 SFX + 1 servo + 7 music + 7 ambient + 8 atmosphere)
- ✅ Sprite atlases for all 5 chassis + weapons
- ✅ Shader present
- ✅ Cross-build script (`cross-windows.sh`) intact, no recent breakage
- ⚠️ Ambient zone hard-edge (this doc fixes it)
- ⚠️ All 8 maps have zero `push_deco` calls — world feels empty
- ⚠️ Bake baseline is from May 15; needs rebake after §2/§3 changes

---

## 2. Soft ambient-zone boundaries (PRIMARY — must ship)

### 2.1 Problem statement

The user said: *"I do not like the hard boundary for the ambient zone that
is rendered in the actual game, it is nice in the editor, but in the game
it breaks immersion."*

**Where the hard edges come from** (`src/atmosphere.c:617-684`):

- `AMBI_ZERO_G` at 627-631:
  - `DrawRectangleRec(r, {120,160,220,25})` — solid cyan rect fill
  - `DrawRectangleLinesEx(r, 1.0f, {180,220,255,80})` — opaque outline
- `AMBI_ACID` at 633-652:
  - `DrawRectangleRec(r, pulsing-green-tint)` — solid pulsing fill
  - Caustic scroll loop bounded to `[rect_x, rect_x + rect_w]`
  - `DrawRectangleLinesEx(r, 2.0f, {120,220,80,200})` — 2-px opaque outline
- `AMBI_FOG` at 654-666: disc is already soft, but the
  `DrawRectangleLinesEx(r, 1.0f, ...)` on line 665 kills the effect by
  painting a hard rect frame over the soft disc
- `AMBI_WIND` at 668-681: only a thin edge frame + arrow — already
  near-invisible in play, but the frame still pops; remove it from the
  runtime path

**Particle spawn** also clips hard to rect edges (`src/atmosphere.c:393-432`).
Spawn margins make the in-rect bias visible at the edge as a sharp
density step. Particles then drift OUT freely, which is fine — that's the
existing "leaky" behavior, and the soft-falloff overlay in this section
will make leaky drift read as intentional bleed-out.

### 2.2 Design: edge-feather overlay + interior particle spawn

**Two-part fix.** Keep the overlays (designers and players still need to
sense the zone), but soften both their edges and their particle spawn so
the volume reads as a region of atmosphere instead of a painted box.

**Part A — overlay feather, two implementation options:**

- **Option A1 (CPU multi-ring, lower risk):** draw the overlay as 5–8
  concentric rounded rects with alpha ramped down from inner full to outer
  zero. Roughly:
  ```c
  /* In atmosphere_draw_ambient_zones, per zone: */
  const int FEATHER_RINGS = 6;
  const float FEATHER_PX  = 32.0f;  /* total feather distance */
  for (int k = 0; k < FEATHER_RINGS; ++k) {
      float t = (float)k / (float)(FEATHER_RINGS - 1);   /* 0..1 */
      float grow = t * FEATHER_PX;
      Rectangle rk = {
          r.x - grow, r.y - grow,
          r.width + 2*grow, r.height + 2*grow
      };
      unsigned char ring_a = (unsigned char)(base_alpha * (1.0f - t));
      DrawRectangleRounded(rk, 0.20f, 4,
          (Color){tint.r, tint.g, tint.b, ring_a});
  }
  ```
  Cost: 6× the draw calls per zone per frame. At max 32 zones / map this
  is 192 `DrawRectangleRounded` per frame — well inside budget. Draw
  **outer-to-inner** so the BIGGEST low-alpha ring lands first and the
  inner brighter rects overpaint it. Drop the `DrawRectangleLinesEx`
  outline entirely from the runtime path (keep it in the editor — see §2.5).
- **Option A2 (shader-based, lower draw count, higher complexity):** extend
  `assets/shaders/halftone_post.fs.glsl`'s `fog_zones[16]` pattern to a new
  `tint_zones[16]` with a rect parameterization (4 floats: cx, cy, half_w,
  half_h) + a per-zone color + per-zone feather. In the fragment shader:
  ```glsl
  uniform vec4  tint_zone_rect[TINT_ZONE_MAX];   /* xy=center, zw=half_extents */
  uniform vec4  tint_zone_color[TINT_ZONE_MAX];  /* rgb=tint, a=density */
  uniform float tint_zone_feather[TINT_ZONE_MAX];
  uniform int   tint_zone_count;
  ...
  float rect_soft_mask(vec2 p, vec4 rect, float feather) {
      vec2 d = abs(p - rect.xy) - rect.zw;
      float dist = max(d.x, d.y);   /* >0 outside, <0 inside */
      return 1.0 - smoothstep(0.0, feather, dist);
  }
  ```
  Cost: ~32 ops per fragment max, plus CPU-side `atmosphere_collect_tint_zones`
  call peer of the existing fog collector. Visually crisper, plays nicely
  with halftone dither.

**Recommendation: ship Option A1 for P10.** It's a one-file change in
`src/atmosphere.c`, no shader / no uniform plumbing, no risk of breaking
the fog pass we just shipped, and visually the difference between A1 and
A2 is small at the rounded-ring count we'll use. Option A2 is M6 P11.

**Part B — particle spawn margins:**

In `src/atmosphere.c:spawn_ambient_particle` (around line 379), replace
the hard `[minx, maxx] × [miny, maxy]` spawn rect with an *interior*
spawn rect inset by `PARTICLE_INSET_PX = 12.0f`, then spawn ~15% of the
particles in a *boundary ring* between inset and edge with reduced alpha.
The boundary-ring particles fade out the spawn-density edge:

```c
static void spawn_ambient_particle(FxPool *fp, FxKind k, const LvlAmbi *a,
                                    PCG32 *rng) {
    const float INSET = 12.0f;
    bool boundary = pcg32_float01(rng) < 0.15f;
    float inset = boundary ? 0.0f : INSET;
    float minx = (float)a->rect_x + inset;
    float maxx = (float)(a->rect_x + a->rect_w) - inset;
    float miny = (float)a->rect_y + inset;
    float maxy = (float)(a->rect_y + a->rect_h) - inset;
    ...
    if (boundary) {
        /* Boundary-ring particle: half alpha, half life. */
        p->color = (p->color & 0xFFFFFF00u) | ((p->color & 0xFFu) >> 1);
        p->life     *= 0.5f;
        p->life_max *= 0.5f;
    }
}
```

**Part C — caustic scroll dies inside the inset:**

The ACID caustic wave (`src/atmosphere.c:643-649`) iterates from
`x = 0 .. a->rect_w` and `DrawRectangle` at `a->rect_x + x`. Bound the loop
to `[INSET, rect_w - INSET]` and taper alpha at the start/end 16 px of the
range so the wave doesn't sharp-cut at the rect edges:

```c
const float INSET = 16.0f;
for (int x = (int)INSET; x < a->rect_w - (int)INSET; x += 8) {
    float edge_t = fminf(
        (float)(x - INSET) / 24.0f,
        (float)((a->rect_w - INSET) - x) / 24.0f);
    if (edge_t > 1.0f) edge_t = 1.0f;
    unsigned char a8 = (unsigned char)(180.0f * edge_t);
    ...
    DrawRectangle(a->rect_x + x, a->rect_y + yoff, 6, 4,
                  (Color){180, 255, 120, a8});
}
```

### 2.3 Per-zone visual targets after the fix

- **AMBI_ZERO_G:** soft cyan glow that visibly bleeds ~32 px past the
  stored rect on each side, no outline, motes spawn in inset interior +
  occasional boundary mote.
- **AMBI_ACID:** pulsing green glow with the same 32 px bleed, no
  outline, caustic wave tapers at edges, bubbles spawn from bottom inset
  edge with the boundary-margin mix.
- **AMBI_FOG:** kill the rect outline at `src/atmosphere.c:665` — the
  shader already produces the right disc. The CPU disc preview at line
  664 is fine, lower its alpha from 30 → 18 so it doesn't double-up.
- **AMBI_WIND:** remove the runtime overlay entirely (the streaks ARE
  the visual). Editor still gets the outline + arrow (see §2.5).

### 2.4 Code surgery surface

| File | Lines | Change |
| --- | --- | --- |
| `src/atmosphere.c` | 393-432 | Add `INSET` + boundary-ring mix to `spawn_ambient_particle` |
| `src/atmosphere.c` | 617-684 | Replace solid rects + outlines with feathered ring overlays; gate WIND overlay behind `is_editor`; kill FOG outline |
| `src/atmosphere.h` | top | Add `void atmosphere_draw_ambient_zones_editor(...)` peer if needed (see §2.5) |
| `tools/editor/render.c` | 83-95 | Keep existing hard-edge preview unchanged |

### 2.5 Editor vs runtime split

The editor draws ambient zones in `tools/editor/render.c:83-95` via its
own hard-edge code path. That's untouched — designers still need exact
bounds visibility. So the change to `atmosphere_draw_ambient_zones` is
runtime-only and the editor's own renderer doesn't call it.

**Verify by inspection:** grep for `atmosphere_draw_ambient_zones`:
- `src/render.c:renderer_draw_frame` is the only runtime caller
- the editor's render loop in `tools/editor/render.c` paints zones
  inline; it does NOT call into `src/atmosphere.c`

If a future P11 wants both renderers to share code, add a `bool soft`
parameter and let the editor pass `false`. For P10 the editor path is
*already* separate; no API change.

### 2.6 Test plan for §2

- **Visual shot test (new):** `tests/shots/m6_p10_soft_zones.shot`
  - `seed 1 1`
  - Load CITADEL (has 2 WIND zones + DUST weather)
  - `spawn_at` near a WIND zone edge
  - Capture at tick 0 (zone unentered), tick 30 (mech approaching edge),
    tick 90 (mech inside)
  - `contact_sheet m6_p10_soft_zones_cs cols 3 cell 480 270`
  - Eyeball-verify: no visible rect outline in the frames
- **Visual shot test (new):** `tests/shots/m6_p10_acid_feather.shot`
  - Use a custom map via `paint_ambi acid` directive (already exists from
    P09 — `src/shotmode.c`) on Slipstream
  - Capture mech walking through a placed ACID zone
  - Verify the caustic wave tapers and the green glow has visible bleed
- **Network parity (extend existing):** add an `at 60 shot before.png`
  and `at 90 shot after.png` to `tests/shots/net/run_atmosphere_parity.sh`
  paired script. The new feathered overlay must render identically on
  host + client (it's pure render-side, so this should be free).
- **No regressions:** all M6 P09 shot tests
  (`m6_p09_tile_flags.shot`, `m6_p09_ambient_zones.shot`,
  `m6_p09_atmosphere_themes.shot`, `m6_p09_tile_oneway_collide.shot`)
  must still produce the same PASS/FAIL status. Visual diffs OK
  (overlays change); functional asserts unchanged.

### 2.7 Acceptance criteria for §2

- Walking into / out of a ZERO_G zone in CITADEL or AURORA shows no
  visible rect edge — the cyan glow fades.
- Standing 24 px outside a fog/acid zone: a faint hint of the zone's
  color shows on screen.
- The editor's zone preview is unchanged (rects with hard outlines).
- All shot tests render and the contact sheets are visually approved by
  a human pass before merge.

---

## 3. Map content polish (SECONDARY — cut for time if needed)

### 3.1 Theme & weather hygiene

Current usage (from cook_maps audit):

| Map | Theme | Weather | Fog | Issue |
| --- | --- | --- | --- | --- |
| foundry | INDUSTRIAL | none | 0 | OK |
| slipstream | ICE_SHEET | SNOW 0.30 | 0 | OK |
| reactor | RUST | EMBERS 0.40 | 0.12 | OK |
| concourse | **CONCRETE** | none | 0 | dup |
| catwalk | NEON | none | 0.08 | OK |
| aurora | NEON | DUST 0.20 | 0.05 | dup (NEON × 2) but differentiated by weather |
| crossfire | **CONCRETE** | none | 0 | dup |
| citadel | BUNKER | DUST 0.30 | 0.10 | OK |

**OVERGROWN is unused. RAIN is unused.** Both are real palettes/modes
that ship in the runtime. Use them.

**Proposed changes** (in `tools/cook_maps/cook_maps.c`):

- **CONCOURSE** → switch theme to OVERGROWN, add light RAIN
  (density 0.18): a covered-but-leaky atrium reads believably as
  overgrown + drizzly without changing any geometry. Update the
  `set_meta_atmos(..., THEME_OVERGROWN, sky_top=rgb(110,140,90),
  sky_bot=rgb(70,100,60), fog_density=0.10, fog_color=rgb(140,170,110),
  vignette=0.20, weather_kind=2 /*RAIN*/, weather_density=0.18)`.
- **CROSSFIRE** stays CONCRETE (it's the canonical clean CTF arena —
  the lack of weather + low vignette is intentional for competitive
  reads). No change.
- **CATWALK** keep NEON; add light RAIN at 0.10 to differentiate from
  AURORA more clearly. Cyberpunk rain on neon catwalks is on-genre.
- **AURORA** stays NEON + DUST.

After this pass: 6 unique themes (1 still unused — OVERGROWN moved to
CONCOURSE), all 5 non-NONE weather modes used (SNOW, RAIN×2, DUST×2,
EMBERS — RAIN now active on 2 maps).

### 3.2 Decoration pass

All 8 maps have **zero `push_deco` calls** today. The runtime layer-tinted
placeholder fallback in `src/map_kit.c` will render rectangles where they
go. That's better than empty but not great. For P10:

- Add 4–8 `push_deco` calls per map at visually-prominent locations
  (background pillars, foreground railings, hanging signs, debris).
- Use the existing `push_deco(d, layer, x, y, scale_q, rot_q, flipped_x,
  additive, sprite_name)` signature.
- Layer guidance:
  - Layer 0–1: deep background (parallax with map_kit's parallax_far)
  - Layer 2: foreground decoration drawn after tiles, before mechs
  - Layer 3: silhouette/foreground overlay
- Sprite-name strings hash into the deco atlas. Since we have no
  authored atlas, names are decorative — pick semantic strings
  (`"pipe_v"`, `"railing"`, `"sign_left"`, `"crate_stack"`). The hash
  picks a 64×64 sub-rect at random; placeholder fallback paints
  layer-tinted boxes.

**Minimum deliverable for P10:** 2-3 deco calls per map at known-safe
positions (above floor, away from spawn, away from pickup spawns). If
the placeholder rendering looks bad in a smoke test, **cut the deco
pass** and ship without it. We are not blocking the release on this.

### 3.3 Ambient-zone usage check

After §2 softens zone visuals, some currently zone-less maps may benefit
from a small ambient pass:

- **FOUNDRY:** 0 zones. Add a single FOG zone at low density (0.15) in
  the rear gallery (right alcove area, ~30 tiles in × 8 tiles tall) so
  the new soft-bleed effect has somewhere to show.
- **REACTOR:** 0 zones. Add a single ACID pool at the bottom of the
  central bowl (4 tiles deep, 12 tiles wide along the floor) — game
  mechanic AND visual sell of the new feather. This DOES change
  gameplay (5 HP/s damage if you fall in) — verify bots still PASS the
  bake after.
- **CROSSFIRE:** 0 zones. Add 2 small WIND zones flanking the central
  catwalk pushing AWAY from center to add a small "navigate the wind"
  decision when crossing. Strength 0.4 (gentle).

**These are net-new gameplay tweaks.** If bake regresses, REMOVE the
new zones rather than chasing it with bot AI. Map design is reversible;
bot work is not.

### 3.4 Recook + verify

After §3.1–§3.3 changes:
```bash
make cook-maps
ls -la assets/maps/*.lvl                    # all 8 present
git diff assets/maps/                       # confirm bytes changed
make test-level-io                          # 9 atmosphere round-trip asserts
make test-map-share                         # host→client .lvl streaming
make test-atmosphere-parity                 # 10 paired-process asserts
```

---

## 4. Bot AI: rebake, then conditional hazard awareness (TERTIARY)

### 4.1 Step 1 — rebake the May-15 baseline first

**Do this BEFORE writing any bot code.** It's possible the bake passes
fine on the new map content and we don't need bot work.

```bash
make bake-all
```

Outputs land in `build/bake/<map>.summary.txt` + `<map>.heatmap.png`.
Compare to the May-15 baseline (also in `build/bake/`):

- May-15 baseline kill counts (from `build/bake/*.summary.txt`):
  foundry=8, slipstream=7, reactor=15, concourse=2, catwalk=13,
  aurora=7, crossfire=14, citadel=13
- May-15 fires: foundry=404, slipstream=253, reactor=611, concourse=223,
  catwalk=683, aurora=534, crossfire=961, citadel=675
- Verdict for all 8: PASS

**Regression rule of thumb:** kills within ±50% of baseline, fires
within ±30%, no map drops to verdict=FAIL. Significantly fewer kills on
REACTOR (where we may have added ACID) would indicate bots are dying to
the hazard repeatedly. Significantly fewer kills on CROSSFIRE (where we
may have added WIND) would indicate bots fail to cross.

**If all 8 maps PASS within tolerance:** skip §4.2 entirely. Ship.

**If 1+ maps regress:** §4.2 is the minimum hazard-awareness fix.

### 4.2 Step 2 — minimum hazard awareness (CONDITIONAL)

The bot AI in `src/bot.c` is a layered Quake III-style utility scorer
(strategy 10 Hz, tactic + motor every tick). It currently has **zero
reads of `world.level.ambis`** — bots don't know hazards exist. Verified
by `grep -n "ambi\|AMBI" src/bot.c` returning empty.

**Minimum patch — ACID rect penalty in nav cost:**

The nav graph in `src/bot.c` precomputes per-node walkability. Add a
post-pass that flags any node whose floor sample lies inside an
`AMBI_ACID` rect, and bumps that node's traversal cost in the A*
heuristic. Approximate location:

```c
/* In bot_build_nav (or wherever nav nodes are scored), after the
 * existing per-node setup, add: */
for (int n = 0; n < nav->node_count; ++n) {
    BotNavNode *nd = &nav->nodes[n];
    float wx = nd->pos.x, wy = nd->pos.y;
    for (int zi = 0; zi < L->ambi_count; ++zi) {
        const LvlAmbi *a = &L->ambis[zi];
        if ((AmbiKind)a->kind != AMBI_ACID) continue;
        if (wx < a->rect_x || wx > a->rect_x + a->rect_w) continue;
        if (wy < a->rect_y || wy > a->rect_y + a->rect_h) continue;
        nd->traverse_cost_mul = 8.0f;   /* A* will route around */
        break;
    }
}
```

The `traverse_cost_mul` field needs to be added to `BotNavNode` (default
1.0) and consumed in the A* edge cost. Search `bot.c` for the existing
nav-cost function — there's almost certainly a `cost = dist(a, b)`
somewhere; multiply it by `dst_node->traverse_cost_mul` before adding to
g-score.

**Do NOT add wind / zero-G awareness.** WIND only shifts kinematics
slightly (max 700 px/s push); bots can rubber-band through it without
strategy. ZERO_G zones are vertical-mobility opportunities, not
hazards — leaving bots ignorant means they don't exploit them, which
is fine for a friend-shipping build.

### 4.3 Step 3 — re-rebake after §4.2

```bash
make bake-all
```

Verify the regressing maps recover. If they don't, **revert the
hazard-awareness patch and remove the new ACID zone from REACTOR (§3.3)
instead.** Bake correctness gates the map change, not the bot change.

### 4.4 Research notes if §4.2 is insufficient

If the trivial cost-multiplier patch doesn't help and you need to go
deeper, these are the search terms / references to consult online:

- "utility AI hazard avoidance" — the canonical pattern is a per-tick
  utility scorer that includes "predicted damage in next N seconds"
  as a negative score component.
- "navigation mesh annotation hazard" — Recast/Detour uses
  per-poly area_id with area_cost tables. Our nav is a graph, not a
  mesh, but the pattern is the same: tag nodes, multiply cost.
- "Quake III bot AI" + "fl_aas_areainfo" — id's bot system tags areas
  with `AREA_LAVA` / `AREA_SLIME` and the goal-seek skips them unless
  the bot has invulnerability. Closest analog to what we need.
- valvedev wiki / "TF2 nav mesh attributes" — `NAV_MESH_AVOID` is the
  pattern name.

Do NOT spend more than 90 minutes on research-and-implement for §4.2/§4.4
together. Past 90 minutes, prefer the §3.3 revert path. We are shipping
to friends, not Steam.

---

## 5. Windows .exe ship verification (GATE)

### 5.1 Build

```bash
make distclean        # wipe third_party builds to flush stale caches
make raylib && make enet
make                  # host build smoke
make windows          # → build/windows/Soldut.exe + SoldutEditor.exe
```

`cross-windows.sh` wipes and rebuilds `third_party/{raylib,enet}` for
the Windows target then restores the host build at the end. If it
errors midway, run `make distclean && make raylib && make enet` to
recover host libs before retrying.

**Verify outputs:**
- `build/windows/Soldut.exe` exists and is non-empty
- `build/windows/SoldutEditor.exe` exists and is non-empty
- `build/windows/assets/` mirrors the host `assets/` tree (the script
  copies it)
- DLL-free build (`zig cc -target x86_64-windows-gnu` statically links)

### 5.2 Smoke test

**WSL/Wine path (preferred — same machine):**
```bash
wine build/windows/Soldut.exe --shot tests/shots/m1_walk_right.shot
ls build/shots/   # confirm PNGs landed
```

**Manual path (Windows host):** copy the `build/windows/` tree to a
Windows machine, double-click `Soldut.exe`, verify:
- Title screen renders, fonts load, atmosphere visible
- Host Server → start match → walk + fire — no crash
- Lobby → join self via 127.0.0.1 with a second instance — both render
  each other, atmosphere parity visible

### 5.3 Asset zip

```bash
cd build/windows
zip -r ../soldut-windows-2026-05-17.zip . -x "*.pdb"
cd ../..
ls -lh build/soldut-windows-2026-05-17.zip
```

Verify size is reasonable (expect ~25–40 MB given audio + atlases).

### 5.4 Optional release tag

User said "ship to friends" — likely no Git tag needed. If they want
one:
```bash
git tag -a v0.6.10-friends -m "M6 P10 friends-build"
git push origin v0.6.10-friends
```
Coordinate with user before pushing tags.

---

## 6. Implementation order and time budget

Time estimates assume the implementing Claude session has 5–7 hours to
spend before the user's morning ship deadline.

| Phase | Effort | Cut if | Output |
| --- | --- | --- | --- |
| 1. Branch + read this doc + read M6 P09 doc | 15 min | never | `lunias/m6-p10-ship-prep` branch live |
| 2. **§2 soft zones — Part A1 (overlay ring)** | 60 min | never | `src/atmosphere.c` edited, 1 shot test |
| 3. **§2 soft zones — Part B (particle inset)** | 30 min | never | spawn function patched, 1 shot test |
| 4. **§2 soft zones — Part C (caustic taper)** | 20 min | never | ACID caustic loop bounded |
| 5. **§5 Windows build + smoke** | 45 min | never | `Soldut.exe` boots clean |
| 6. **§4.1 bake rebake (before any bot work)** | 15 min | never | `build/bake/` summaries fresh |
| 7. §3.1 theme/weather hygiene | 30 min | bake red | cook_maps.c edited, recook |
| 8. §3.3 ambient zone adds (REACTOR ACID etc) | 30 min | bake red | cook_maps.c edited |
| 9. §3.4 recook + level-io test | 15 min | bake red | maps stamped |
| 10. §4.2 bot ACID avoidance | 90 min | §4.1 PASS | `src/bot.c` edited |
| 11. §4.3 re-rebake | 15 min | §4.2 cut | all 8 PASS |
| 12. §3.2 decoration calls | 45 min | time | cook_maps.c deco edits |
| 13. Re-recook + final bake | 15 min | time | final `.lvl` set |
| 14. §5.3 ship zip + verify | 15 min | never | `soldut-windows-*.zip` |
| 15. PR + merge | 15 min | never | merged to main |

**Hard order constraints:**
- Soft zones (phase 2-4) must happen first — it's the user's explicit
  ask and the most visible win.
- Windows build verification (phase 5) right after, so we discover
  build breakage early.
- Bake rebake (phase 6) gates whether §3.3 zone adds and §4.2 bot work
  happen.

**Phases that are safe to drop:**
- §3.2 decorations (phase 12) — placeholder rects ship if needed.
- §3.3 new ambient zones beyond what's already there — if bake gets
  worse, just don't add them.
- §4.2 bot ACID avoidance — only needed if bake regresses.

---

## 7. Test plan

### 7.1 Required green tests before merge

```bash
make test-level-io          # .lvl round-trip + atmosphere 9-field asserts
make test-pickups
make test-ctf
make test-snapshot
make test-spawn
make test-prefs
make test-map-share
make test-map-registry
make test-grapple-ceiling
make test-frag-grenade
make test-mech-ik
make test-pose-compute
make test-damage-numbers
make test-atmosphere-parity   # M6 P09 10/10 paired assertions
./tests/net/run.sh 2p_basic
./tests/net/run_3p.sh
./tests/shots/net/run.sh 2p_basic
./tests/shots/net/run_kill_feed.sh
./tests/shots/net/run_frag_grenade.sh
```

### 7.2 New visual artifacts (commit alongside .lvl changes)

- `tests/shots/m6_p10_soft_zones.shot` + its contact sheet PNG in
  `build/shots/`
- `tests/shots/m6_p10_acid_feather.shot` + contact sheet
- Updated `build/bake/*.summary.txt` and `*.heatmap.png` for all 8
  maps — eyeball-verify the heatmaps look reasonable before commit
- If §3.3 added new ACID zone to REACTOR, the heatmap will have a
  dark column where the acid pool is — that's expected

### 7.3 Tests that are allowed to stay red

- `tests/shots/net/run_ctf_walk_own_flag.sh` (3 asserts) — pre-existing
  flake documented in `CURRENT_STATE.md`. Do not touch.

### 7.4 What to do if the shot tests look wrong

The user's complaint is subjective ("breaks immersion"). The shot tests
prove the new feathered overlay renders and is consistent across host
and client. Final visual approval is the user looking at the contact
sheets before merge. If the user thinks the feather is too wide or too
narrow, the only tuning knob is `FEATHER_PX` in §2.2 Part A1 (default
32 — try 16 for narrower, 48 for wider). Re-run shot tests, regenerate
contact sheet, repeat until user says "good."

---

## 8. Rollback plan

Each phase is independently revertable; this is intentional.

- **§2 rollback:** revert `src/atmosphere.c` to current head. Hard
  edges return; no other system depends on the feathered rendering.
- **§3 rollback:** revert `tools/cook_maps/cook_maps.c` and `make
  cook-maps`. All 8 .lvls return to their May-15 state.
- **§4 rollback:** revert `src/bot.c` and the `BotNavNode` struct
  change. Bots stop seeing ACID (back to current behavior).
- **§5 rollback:** if the Windows build is broken, no rollback —
  ship a Linux build or postpone the friends-ship by a day. The user
  knows this risk; do not attempt to fix Windows cross-build problems
  past 90 minutes of effort.

If we get all the way to merge and the user is unhappy with one phase
but happy with the others: split the PR. Each phase is in
independently-committable pieces (atmosphere.c is its own commit,
cook_maps.c is its own commit, bot.c is its own commit). User can
cherry-pick.

---

## 9. Definition of done

This phase is shippable when:

1. ☐ A user walks into / out of a ZERO_G or ACID zone in CITADEL and
   does NOT see a sharp rectangular edge.
2. ☐ The editor still shows hard-edged zone rectangles (designer-facing
   preview unchanged).
3. ☐ All asserting tests from §7.1 are green (flake from §7.3 may stay
   red).
4. ☐ `make bake-all` is green; per-map kill / fire counts within ±50% /
   ±30% of May-15 baseline; no map drops to verdict=FAIL.
5. ☐ `build/windows/Soldut.exe` exists, boots, can host + join a
   loopback match without crash.
6. ☐ `build/soldut-windows-2026-05-17.zip` exists and is under 50 MB.
7. ☐ Branch merged to `main`; user has the zip file or knows where to
   find it.

---

## Appendix A: File-and-line reference card

| Concern | File | Lines (current head) |
| --- | --- | --- |
| Ambient zone overlay rendering | `src/atmosphere.c` | 617-684 |
| Ambient zone particle spawn | `src/atmosphere.c` | 379-432 (the static `spawn_ambient_particle` + dispatch) |
| Ambient zone tick & spawn rates | `src/atmosphere.c` | 517-552 |
| Fog zone shader collection | `src/atmosphere.c` | 689-719 |
| Fog shader pass (model for §2 Option A2) | `assets/shaders/halftone_post.fs.glsl` | 149-193 |
| Editor zone preview (DO NOT TOUCH) | `tools/editor/render.c` | 83-95 |
| `LvlAmbi` struct | `src/world.h` | 945-952 |
| WIND physics push | `src/physics.c` | 850-940 |
| ACID damage tick | `src/mech.c` | 1576-1601 |
| Map cooker (FOUNDRY) | `tools/cook_maps/cook_maps.c` | 303-425 |
| Map cooker (SLIPSTREAM) | `tools/cook_maps/cook_maps.c` | 434-610 |
| Map cooker (REACTOR) | `tools/cook_maps/cook_maps.c` | 619-769 |
| Map cooker (CONCOURSE) | `tools/cook_maps/cook_maps.c` | 780-956 |
| Map cooker (CATWALK) | `tools/cook_maps/cook_maps.c` | 974-1126 |
| Map cooker (AURORA) | `tools/cook_maps/cook_maps.c` | 1136-1356 |
| Map cooker (CROSSFIRE) | `tools/cook_maps/cook_maps.c` | 1366-1534 |
| Map cooker (CITADEL) | `tools/cook_maps/cook_maps.c` | 1545-1731 |
| Theme palette | `src/atmosphere.c` | 26-132 |
| Bake harness | `tools/bake/run_bake.c` | entire file; bot tick at 817 |
| Bot AI entry point | `src/bot.c` | `bot_step` (search) |
| Bot nav build | `src/bot.c` | `bot_build_nav` (search) |
| Windows cross-build | `cross-windows.sh` | entire script |

## Appendix B: What we are intentionally NOT doing

To save the next session from rabbit holes — these were considered and
rejected:

- **Per-map sprite-deco atlases.** Real deco art belongs to a content
  phase (M6 P11+). Placeholder rects ship.
- **Adding RAIN sound effects.** RAIN weather adds visual + friction
  but no audio. Existing ambient loops cover it.
- **Editor live-preview of feathered zones.** Editor still uses its
  own hard-edge code. Designers want exact bounds.
- **A new ambient-zone shape (circle, polygon).** `LvlAmbi` stays
  rect-only. Soft feather is enough.
- **Bot pathing through ZERO_G / WIND.** Out of scope for this ship.
  Documented in `TRADE_OFFS.md`'s existing bot entries.
- **CI gating on the bake.** Stays informational per the existing
  TRADE_OFFS entry "Bake-test verdict is informational, not gating
  (P18)".
- **Wire protocol changes.** Stays `S0LK`. Anything that would touch
  the wire is a deferred phase.

---

End of plan. Implementing session: start with §6 phase 1 (branch off),
then §2 in order. Phase 6 (bake rebake) is the decision point for
whether the second half of the day is map polish or bot work.
