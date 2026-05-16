# M6 P09 — Editor-Runtime Parity & Tile Atmospherics

**Goal**: every property the level editor lets a designer set must produce
a visible, consistent effect in the game on every client and a matching
physical effect on the server. Tile flags (`ICE`, `DEADLY`, `ONE_WAY`,
`BACKGROUND`) currently look identical to a plain `SOLID` tile in-game
even though the editor exposes them as discrete material choices and the
thumbnail painter renders each kind in a different color. Ambient zones
(`WIND`, `ZERO_G`, `ACID`, `FOG`) have no visual whatsoever — `FOG` has
no game effect at all. Decoration sprites round-trip through the `.lvl`
but the editor never exposes a sprite picker, scale, rotation, flip,
additive, or layer control, so every deco ships at placeholder defaults.
Map metadata (`blurb`, `background`, `music`, `ambient_loop`, `reverb`)
either round-trips dead or is consumed only by cooked maps. This P09
closes every one of those gaps in a single coordinated pass, then adds a
**per-map atmospherics layer** (sky gradient, fog density, vignette, sun
direction, weather mode) sourced through `LvlMeta.reserved[]` and a new
shader uniform block, so each map can read like a specific *place*
rather than the same gray-box level with different geometry.

This is a polish phase. The simulation behavior of the eight shipping
maps does not change unless the editor's data was a lie (the dead
`TILE_F_ONE_WAY` and `TILE_F_BACKGROUND` bits — fixed under §5). Every
other change is render-side and consumes data already replicated by the
existing `.lvl` → map-share → `level_load` pipeline.

Read this whole document end-to-end before opening a single file. §0–§2
are scope, §3–§9 are subsystem specifications with concrete code
shapes and `file:line` cites, §10–§13 are integration mechanics, §14
onward is the implementation flow and test plan.

---

## 0. Scope and non-goals

**In scope**

- **Tile-flag visual differentiation.** Every `TILE_F_*` bit (`SOLID`,
  `ICE`, `DEADLY`, `ONE_WAY`, `BACKGROUND`) must be visually distinct in
  the running game in both the atlas-path (`g_map_kit.tiles.id != 0`)
  and the M4 fallback path (`tiles.id == 0`). Distinction must hold on
  both the host UI client and remote clients — the existing `.lvl`
  distribution already guarantees both ends load identical bytes; the
  visual change is render-only.
- **Polygon kind visual hygiene.** Fix the `POLY_KIND_DEADLY` fill at
  `src/render.c:335` (currently `{80,200,80}` green — clashes with the
  editor's `{200,80,60}` red at `tools/editor/render.c:19` and the
  thumbnail's `{200,80,60}` at `src/map_thumb.c:41`). Add a directional
  chevron stripe to `POLY_KIND_ONE_WAY` polys on the passable face. Add
  an animated glint band to `POLY_KIND_ICE` and a warning-hatch overlay
  to `POLY_KIND_DEADLY`.
- **Ambient-zone visuals.** Each of the four `AmbiKind` values
  (`AMBI_WIND`, `AMBI_ZERO_G`, `AMBI_ACID`, `AMBI_FOG`) gets a
  rect-scoped visual that announces what the zone does. Particle-based
  for `WIND`/`ACID`/`ZERO_G`; shader-based for `FOG`. `AMBI_FOG` also
  gets its game effect (previously inert — only declared in the enum).
- **Dead-bit physics wiring.** `TILE_F_ONE_WAY` and `TILE_F_BACKGROUND`
  currently only function through the *polygon* analogs at
  `src/physics.c:599-600`; the tile collision branch never reads them.
  Wire both through `collide_map_one_pass` so the editor's tile-flag UI
  stops lying.
- **Editor exposure of currently-hidden settable fields.** Add UI
  controls in `tools/editor/` for every field that the runtime reads but
  the editor never lets a designer set: `LvlTile.id` (atlas sprite
  index), `LvlDeco.{sprite_str_idx, layer, scale_q, rot_q, flags}`,
  `LvlAmbi.{strength_q, dir_x_q, dir_y_q}`, `LvlMeta.{blurb_str_idx,
  background_str_idx, music_str_idx, ambient_loop_str_idx,
  reverb_amount_q}`.
- **Per-map atmospherics.** Promote 8 of `LvlMeta.reserved[9]`'s slots
  to named atmosphere fields (`theme_id`, `sky_top_rgb565`,
  `sky_bot_rgb565`, `fog_density_q`, `vignette_q`, `sun_angle_q`,
  `weather_kind`, `weather_density_q`). Pure additive — `LvlMeta` stays
  32 B, no `LVL_VERSION` bump, old `.lvl` files read zeros → defaults.
- **Theme palette table.** A hand-tuned `g_themes[THEME_COUNT]` `const`
  C table provides per-theme defaults (backdrop color, ICE tint, DEADLY
  tint, FOG color, sky gradient) so a designer only sets `theme_id` for
  the common case, and overrides each field individually for the rare
  case.
- **Weather**: a four-mode runtime (`SNOW`, `RAIN`, `DUST`, `EMBERS`)
  driven by `LvlMeta.weather_kind` + `weather_density_q`. Particles
  spawn in screen-space (cheap), live in a new `FxKind` family
  (`FX_WEATHER_*`) on the existing `FxPool` (capacity bump
  8500 → 10500 — see §9).
- **Shader extension.** The existing
  `assets/shaders/halftone_post.fs.glsl` accepts five new uniforms
  (`sky_top`, `sky_bot`, `fog_density`, `fog_color`, `vignette_strength`)
  plus a `fog_zones[16]` array peer of the existing
  `jet_hot_zones[16]` for rect-scoped `AMBI_FOG` volumes. Each new
  uniform follows the existing `<= 0.001 → pass-through` pattern at
  `halftone_post.fs.glsl:115` so a zero-init map costs zero shader ops.
- **Sourced assets.** Tile sets, decorations, fog/dust/caustic
  textures, and particle sprites pulled from Kenney.nl + OpenGameArt
  (CC0 only) using the same `tools/audio_inventory/source_map.sh`
  pattern P19 used for SFX. Concrete URLs in §10.
- **Editor parity tests + paired-process atmosphere tests + new shot
  tests visualizing every tile flag, every poly kind, every ambient
  zone, every theme, every weather mode.** Detail in §13.
- **Documentation.** `CURRENT_STATE.md`, `CLAUDE.md` status line,
  `TRADE_OFFS.md` retirements (5 entries — see §15), and the status
  footnote at the end of this document.

**Out of scope**

- **`LvlPickup.flags` semantics** (`PICKUP_FLAG_CONTESTED`/`_RARE`/
  `_HOST_ONLY`). The bits are designed but never defined as enum
  values. Out of scope here; tracked as a separate `TRADE_OFFS.md`
  entry.
- **`LvlPoly.bounce_q` / `LvlPoly.group_id`.** Dead reserved-for-v1
  fields. Editor exposes nothing; runtime reads nothing. Leave them
  dead until destructible-geometry work picks them up post-M6.
- **`LvlSpawn.flags` (`PRIMARY`/`FALLBACK`).** Same — designed,
  unused. Out of scope.
- **Bumping `LVL_VERSION`.** Every schema change here either fits in
  existing `reserved[]` or extends already-shipped fields' semantics.
  Old `.lvl` files keep loading; new `.lvl` files keep loading on the
  pre-P09 binary (unknown bytes read as zero = "use default"). Wire
  protocol id stays `S0LK`.
- **Bumping `MapKit` to per-map atmospherics directories.** The theme
  table lives in C; per-map override fields live in `LvlMeta`. We do
  not add a `.atmosphere` file per map. (If a future map needs an
  effect this table doesn't support, add the override field then.)
- **Replacing the `FxPool` with per-particle-kind ring buffers.**
  Pool capacity bump from `8500` to `10500` is the only sizing change.
  If the worst-case budget in §9 holds, the single pool is still
  correct.
- **Network-replicating particle state.** Atmospherics + weather are
  pure visual functions of `LvlMeta` (already replicated via P08 map
  share) + wall-clock time. The two clients drift in their RNG
  particle positions and that is intentional — these are screen-decor,
  not gameplay state.
- **Editor preview of atmospherics inside the editor's own draw
  pass.** The editor uses a simpler debug renderer
  (`tools/editor/render.c`). Adding the full shimmer/halftone/fog
  pipeline to the editor is a bigger lift; the editor previews
  atmosphere via the existing F5 test-play (which spawns a real game
  process). This is a deliberate choice — see §11.5.

**Why this fits M6 polish, not a new milestone**

The pieces are small and the wire format already carries the data.
M5 P14–P19 stood up the audio layer; M6 P01–P04 closed the
visual-vs-physical gap for mech bodies (IK, jet exhaust, damage
numbers) and got the renderer pipeline into a stable internal-RT
shape (P03). The level-data side has been the loudest remaining
"this UI control does nothing" surface — the editor's tile palette is
a checkbox grid that visually does nothing in-game; the ambient zones
have no announcing visual; map themes are 8 instances of the same
flat-gray fallback. Closing this is one ship.

---

## 1. The parity gap (current state inventory)

This section is the why. The audit cites are from the actual code at
the time this document was written. If anything below disagrees with
the code at implementation time, the code wins — re-audit and update
this section before starting work.

### 1.1 Tile flags

The five `TILE_F_*` bits at `src/world.h:843-851` are exposed in the
editor as a 5-checkbox palette at `tools/editor/editor_ui.c:174-183`.
Serialization at `tools/editor/level_io.c:230` packs them into
`LvlTile.flags`. The runtime collision step reads `TILE_F_SOLID` at
`src/physics.c:403`, `TILE_F_ICE` at `src/physics.c:367`, and
`TILE_F_DEADLY` at `src/mech.c:1523`. **`TILE_F_ONE_WAY` and
`TILE_F_BACKGROUND` have ZERO read sites for the tile path** — they
fire only via the `POLY_KIND_*` translation at `src/physics.c:599-600`.

Visually: `src/render.c::draw_level_tiles` at `src/render.c:288-325`
keys exclusively on `(t->flags & TILE_F_SOLID)` (gate) and `t->id`
(atlas sub-rect). **Zero per-flag visual variation** in either branch.
A `TILE_F_ICE` tile and a `TILE_F_DEADLY` tile and a plain `TILE_F_SOLID`
tile draw identical pixels. (The editor's own renderer at
`tools/editor/render.c:19-23` and the thumbnail painter at
`src/map_thumb.c:38-41` both *do* paint per-flag colors — the game just
discards that art direction.)

### 1.2 Polygon kinds

The five `PolyKind` values at `src/world.h:852-858` are exposed at
`tools/editor/editor_ui.c:198-209`. `draw_polys` at `src/render.c:332-359`
distinguishes by fill color:

- SOLID `{32,38,46}` ✓
- ICE `{180,220,240}` ✓
- DEADLY `{80,200,80}` **🟥 GREEN — clashes with editor `{200,80,60}` and thumb `{200,80,60}`**
- ONE_WAY `{80,80,100,200}` — alpha 200, no directional hint
- BACKGROUND → handled separately in `draw_polys_background` at
  `src/render.c:365-375`, single tint `{28,32,40,153}` ✓

Edge stroke is the same `{180,200,230}` 2 px for every kind. There is
no glint on ICE, no hatch on DEADLY, no chevron on ONE_WAY.

### 1.3 Ambient kinds

`AmbiKind` at `src/world.h:863-868`. Editor exposes a four-button
palette at `tools/editor/editor_ui.c:309-320` and serializes via
`LvlAmbi` (16 B). Runtime:

- `AMBI_WIND` — wired at `src/physics.c:770-783` (per-tick velocity
  nudge from `strength_q` × `(dir_x_q, dir_y_q)`)
- `AMBI_ZERO_G` — wired at `src/physics.c:783-790` (mask gravity)
- `AMBI_ACID` — wired at `src/mech.c:1547-1559` (5 HP/s damage)
- **`AMBI_FOG` — INERT.** Zero references in `src/`. Loads, persists,
  ignored.

Visually: **zero**. There is no `draw_ambient_zones` pass anywhere.
A WIND zone is invisible until you step into it and your particles
drift. A FOG zone is invisible because nothing reads the enum.

### 1.4 Decorations

`LvlDeco` at `src/world.h:908-916` has `scale_q`, `rot_q`,
`sprite_str_idx`, `layer`, `flags` (`FLIPPED_X`, `ADDITIVE`). The
runtime renderer `draw_decorations` at `src/render.c:493-541` consumes
all of them — sub-rect picked via `deco_src_rect_for` at
`src/render.c:464-470`, layer dispatched 4 times in `draw_world_pass`
at `src/render.c:1335,1336,1341,1375`, flags honored at
`src/render.c:514,522`.

**The editor exposes ZERO controls for any of this.** `ToolCtx`
hardcodes `e.layer = 1`, `e.scale_q = 32767` (≈1.0), `e.rot_q = 0`,
`e.flags = 0`, `e.sprite_str_idx = c->deco_sprite_str` at
`tools/editor/tool.c:24,349-353`, and `deco_sprite_str` itself is
never written by any UI path. Every editor-placed deco ships
identical: layer 1, scale 1, rotation 0, sprite 0, flags 0. The
manifest-vs-atlas binding is also broken — `deco_src_rect_for` hashes
the `sprite_str_idx` byte *offset* rather than the resolved string,
so the string content the editor would interpret (when wired up)
never reaches the artist's atlas naming.

### 1.5 Map metadata

`LvlMeta` at `src/world.h:934-943`:

- `name_str_idx` — wired (`src/maps.c:152-174` reads display name)
- `mode_mask` — wired (lobby filters by FFA/TDM/CTF eligibility)
- `blurb_str_idx` — **dead** (commented out at `src/maps.c:306`)
- `background_str_idx` — **dead**
- `music_str_idx` — wired in `src/audio.c:719`, **but the editor has
  no UI to set it** — only `tools/cook_maps/cook_maps.c` populates it
- `ambient_loop_str_idx` — same: wired in `src/audio.c:725`, no
  editor UI
- `reverb_amount_q` — **dead**
- `reserved[9]` — 18 bytes free, never touched

### 1.6 Map-kit slots

`MapKit` at `src/map_kit.h:35-41` carries only four textures —
`parallax_far`, `parallax_mid`, `parallax_near`, `tiles` — plus a
short_name identifier. No tint, no clear color, no fog density, no
weather kind, no sky gradient. Load path
`map_kit_load` at `src/map_kit.c:38-83` probes
`assets/maps/<short>/{parallax_far,parallax_mid,parallax_near,tiles}.png`.
All four assets are currently **absent on disk** for every shipping
map (only `assets/maps/foundry/` exists and is empty). So in practice
every map runs the M4 flat-fill fallback for every layer.

### 1.7 Backdrop

Hardcoded `{12,14,18,255}` at `src/render.c:1456,1472`, both the
window clear and the internal-RT clear. No per-map override path.

### 1.8 The full dead-data inventory

| Where | Field | Status |
|---|---|---|
| `LvlTile.id` | atlas sprite index | wired in render, editor never sets (always 0) |
| `LvlTile.flags` ICE / DEADLY | physics flag | wired in physics, **no render variation** |
| `LvlTile.flags` ONE_WAY | tile flag | **dead in physics + render** (poly version works) |
| `LvlTile.flags` BACKGROUND | tile flag | **dead in physics + render** (poly version works) |
| `LvlPoly.bounce_q` | restitution | dead (no editor UI, no runtime read) |
| `LvlPoly.group_id` | destructible group | dead (reserved-for-v1) |
| `LvlSpawn.flags` PRIMARY/FALLBACK | priority bits | dead (no enum values, no read) |
| `LvlPickup.flags` CONTESTED/RARE/HOST_ONLY | spawn bits | dead (no enum values, no read) |
| `LvlDeco.scale_q` / `rot_q` / `sprite_str_idx` / `layer` / `flags` | all of it | wired in render, **editor exposes nothing** |
| `LvlAmbi.strength_q` / `dir_x_q` / `dir_y_q` | physics tuning | wired in physics, **editor hardcodes defaults** |
| `LvlAmbi.kind == AMBI_FOG` | enum | **fully dead** |
| `LvlMeta.blurb_str_idx` | UI text | dead (editor input exists, runtime ignores) |
| `LvlMeta.background_str_idx` | parallax id | dead |
| `LvlMeta.music_str_idx` | music track | wired in audio, **no editor UI** |
| `LvlMeta.ambient_loop_str_idx` | ambient loop | wired in audio, **no editor UI** |
| `LvlMeta.reverb_amount_q` | room reverb | dead |
| `LvlMeta.reserved[9]` | 18 bytes | unused space — **becomes atmospherics** |
| Backdrop color | hardcoded | **no per-map override** |

P09 closes everything in **bold** above. The italicized rows
(`bounce_q`, `group_id`, `Spawn.flags`, `Pickup.flags`) stay
deliberately out of scope.

---

## 2. Wire format & schema decisions

### 2.1 `.lvl` schema additions — additive only

`LvlMeta.reserved[9]` (18 bytes free) gets promoted to eight named
fields, plus one trailing reserved word for the next pass:

```c
typedef struct {
    uint16_t name_str_idx;
    uint16_t blurb_str_idx;
    uint16_t background_str_idx;
    uint16_t music_str_idx;
    uint16_t ambient_loop_str_idx;
    uint16_t reverb_amount_q;          /* Q0.16 */
    uint16_t mode_mask;                /* FFA=1, TDM=2, CTF=4 */
    /* P09 — promoted from reserved[9]. Old .lvl reads zero → defaults
     * via theme table at src/atmosphere.c:g_themes[0]. */
    uint16_t theme_id;                 /* AtmosphereTheme enum; 0 = "default" (CONCRETE) */
    uint16_t sky_top_rgb565;           /* RGB565 sky gradient top    — 0 = use theme */
    uint16_t sky_bot_rgb565;           /* RGB565 sky gradient bottom — 0 = use theme */
    uint16_t fog_density_q;            /* Q0.16 global fog density   — 0 = none */
    uint16_t fog_color_rgb565;         /* RGB565 fog tint            — 0 = use theme */
    uint16_t vignette_q;               /* Q0.16 vignette strength    — 0 = none */
    uint16_t sun_angle_q;              /* Q0.16 sun angle [0,1) → [0, 2π) */
    uint16_t weather_kind;             /* WeatherKind enum; 0 = none */
    uint16_t weather_density_q;        /* Q0.16 weather density      — 0 = none */
    uint16_t reserved[1];              /* keep one for future use */
} LvlMeta;
_Static_assert(sizeof(LvlMeta) == 32, "LvlMeta must stay 32 bytes");
```

**Forward-compat**: pre-P09 binary reads new `.lvl` file → those bytes
land in `reserved[9]` which it never reads. **Backward-compat**: P09
binary reads pre-P09 `.lvl` → all new fields are zero → atmospherics
falls through to theme `0` (CONCRETE, neutral gray, no fog, no
vignette, no weather). No `LVL_VERSION` bump required.

The eight existing maps cooked by `tools/cook_maps/cook_maps.c` get
recooked under §10.5 with sensible theme assignments — the recook
produces new `.lvl` bytes (different CRC32), so the cached map files
on existing user installs invalidate naturally through P08's
content-addressed cache.

### 2.2 `LvlAmbi` semantic extension — no schema change

`LvlAmbi` (16 B) stays as is; the `strength_q` / `dir_x_q` / `dir_y_q`
trio is per-kind reinterpreted:

| Kind | `strength_q` | `dir_x_q` | `dir_y_q` |
|---|---|---|---|
| `AMBI_WIND` | wind speed (px/s, Q0.16 × 200) | wind dir x | wind dir y |
| `AMBI_ZERO_G` | gravity scale (1.0 = full negate) | unused | unused |
| `AMBI_ACID` | damage scale (1.0 = 5 HP/s) | unused | unused |
| `AMBI_FOG` | fog density (Q0.16) | fog tint R/G hint (split byte) | fog tint B/A hint (split byte) |

Editor UI exposes sliders + the existing dir-vector drag (the WIND
case keeps its current shape; the others enable/disable rows in the
palette per kind). See §6.

### 2.3 `LvlTile.id` — fully exposed

The runtime already keys the atlas sub-rect off `t->id` at
`src/render.c:299-305`. Editor adds a 64-entry tile-id grid picker
(8×8, matching the atlas layout) at `tools/editor/editor_ui.c`. The
default stays `id = 0` (auto: flag-derived sprite, see §4); the
designer can override.

### 2.4 `LvlDeco` — fully exposed

Editor adds:
- A sprite picker (16×16 grid sampling `assets/sprites/decorations.png`
  if present; placeholder grid otherwise)
- Layer dropdown (0/1/2/3)
- Scale slider (0.25–4.0)
- Rotation slider (0–360°)
- Flip X toggle
- Additive toggle

A new `g_deco_manifest[]` const C table at `src/render.c` resolves
sprite names → atlas sub-rects (replacing the `deco_src_rect_for`
hash-of-offset stopgap at `src/render.c:464-470`). Manifest entries:
`{ "pipe_short", { 0, 0, 64, 32 } }`, etc. The editor reads the same
manifest via `tools/editor/deco_manifest.c` (one-file include of the
same C table — kept as a single source of truth).

### 2.5 `LvlMeta` text fields — fully exposed in META modal

Editor adds:
- `blurb` text input (single line, ≤96 chars, interned to STRT)
- `background` dropdown (theme-derived preview swatch)
- `music` dropdown (lists `assets/sfx/music_*.ogg`)
- `ambient_loop` dropdown (lists `assets/sfx/ambient_*.ogg`)
- `reverb_amount` slider (0–1.0)

Plus a full **Atmospherics** panel for the new fields — see §7.

### 2.6 Wire protocol id

`S0LK` stays. **No runtime wire-format bytes change.** The `.lvl`
distribution layer (P08 `MapDescriptor` + map-share chunks) already
serves arbitrary `.lvl` bytes, so the new fields ride for free.

---

## 3. Editor-runtime parity (Phase A — 1 day)

The smallest-footprint phase. Pure editor + tiny runtime additions
to consume the now-set fields. Ships before the visual phases so the
visual phases have real data to render.

### 3.1 New `ToolCtx` fields

`tools/editor/tool.h`: extend `ToolCtx` with the missing per-tool
state:

```c
typedef struct ToolCtx {
    /* existing fields ... */
    /* P09 — exposed per-tool state */
    int      tile_id;            /* TILE tool: atlas sub-rect index, default 0 */
    int      deco_layer;         /* DECO tool: 0..3, default 1 */
    int      deco_sprite_str;    /* DECO tool: STRT offset for sprite name */
    float    deco_scale;         /* DECO tool: world scale, default 1.0 */
    float    deco_rot_deg;       /* DECO tool: rotation, default 0 */
    bool     deco_flipped_x;     /* DECO tool: horizontal flip */
    bool     deco_additive;      /* DECO tool: additive blend */
    float    ambi_strength;      /* AMBI tool: kind-dependent strength */
    float    ambi_dir_deg;       /* AMBI tool (WIND): direction, deg */
    int      meta_theme_id;      /* META modal: AtmosphereTheme */
    /* etc — full list in §7 */
} ToolCtx;
```

### 3.2 Editor UI extensions

Concrete file:line plan:

- `tools/editor/editor_ui.c:174-183` — augment TILE palette with a
  `tile_id` grid picker (sample `assets/sprites/tiles_default.png`
  for thumbnails)
- `tools/editor/editor_ui.c:186-240` — already paints poly-kind
  palette; no change to UI shape, just the kind colors should match
  the new in-game colors (§5)
- `tools/editor/editor_ui.c:297-321` — extend AMBI palette: kind
  buttons + per-kind sliders (WIND: strength + dir; ZERO_G: strength;
  ACID: strength; FOG: density + tint)
- New DECO palette panel — currently `main.c:441-442` paints
  `ui_draw_empty_palette` for DECO. Replace with a real palette:
  sprite picker grid + layer dropdown + scale/rot sliders + flip +
  additive toggles
- `tools/editor/editor_ui.c:332-402` (META modal) — extend with the
  text/dropdown/slider rows from §2.5, and add the full Atmospherics
  panel (§7.5)

### 3.3 Editor tool-c writeback

Update `tools/editor/tool.c:tile_press/tile_drag` (`tool.c:58/66`) to
write `t->id = c->tile_id` (currently always 0). Update
`tools/editor/tool.c:349-353` (DECO place) to write the full struct
from `ToolCtx`. Update `tools/editor/tool.c:317-324` (AMBI drag) to
use the slider values instead of hardcoded `16384/32767/0`.

### 3.4 Runtime consumption changes (none yet for visuals)

This phase consumes only the **already-wired but unset** fields. No
new render code. The deco_layer drop-down picks layer 0/1/2/3 and the
existing dispatch at `src/render.c:1335,1336,1341,1375` already
draws them. Scale/rot/flip/additive are already honored. The
`music_str_idx`/`ambient_loop_str_idx`/`reverb_amount_q` already feed
`src/audio.c:719,725`. So this phase is "the editor writes more
bytes; the runtime starts using them for free."

### 3.5 Tests for Phase A

- Extend `test-level-io` to round-trip every new editor-set field
  (existing `LvlMeta` test gets new assertions; new `LvlAmbi`
  strength assertions)
- New `tests/test_editor_paths.c` smoke: spawn editor in scripted
  mode, place a TILE with `tile_id=7`, a DECO with `layer=3
  scale=2.0 rot=90 flipped_x=true`, an AMBI with `strength=0.7
  dir=(0.7, -0.7)`, save, reload, assert bytes match
- Extend `test-map-share` to confirm the new fields survive the
  chunk → CRC32 → reassembly path (existing test; just needs new
  assertions on the post-receive `LvlMeta`)

---

## 4. Tile visual differentiation (Phase B — 1 day)

The headline. Each `TILE_F_*` flag must produce a visually distinct
pixel both with and without an atlas. The visual must read at
1920×1080 (default window since wan-fixes-13) and stay legible at
the M6 P03 capped 1080p internal target.

### 4.1 Flag → visual mapping (canonical, used by both paths)

A new `src/atmosphere.{c,h}` module owns a const table mapping
flag combinations to:

```c
typedef struct TileMaterial {
    Color   base;             /* main fill */
    Color   accent;           /* highlight stripe / hatch / chevron */
    uint8_t pattern;          /* PAT_NONE | PAT_ICE_GLINT | PAT_DEADLY_HATCH | PAT_ONE_WAY_CHEVRON | PAT_BACKGROUND_ALPHA */
} TileMaterial;

extern const TileMaterial g_tile_materials[16];  /* indexed by (flags >> 1) & 0xF — see g_tile_mat_index */
```

Plus `tile_material_for(uint16_t flags, int theme_id)` which:

1. Picks the dominant flag for visual purposes (priority: DEADLY >
   ICE > ONE_WAY > BACKGROUND > SOLID)
2. Looks up the base color in the theme palette `g_themes[theme_id]`
3. Picks the pattern from the flag

Result: ICE tiles always read cyan-glinted, DEADLY tiles always read
red-hatched, ONE_WAY tiles always have a chevron arrow, BACKGROUND
tiles read alpha-blended even in the fallback. The theme palette
controls the *base* color so a NEON-themed map reads neon and a
BUNKER-themed map reads bunker, but the flag still announces its
material.

### 4.2 Atlas-path rendering

Update `draw_level_tiles` at `src/render.c:288-325` so the atlas
branch (`has_atlas == true`):

1. Still picks the `t->id` sub-rect from the atlas (existing
   behavior — keeps designer art visible)
2. Additionally tints the draw with the material `base` color
   (multiplied via the `colDiffuse` of `DrawTexturePro`'s 5th arg).
   ICE tiles get a cool cyan multiplier, DEADLY tiles a warm red one
3. Calls a new `draw_tile_overlay(x, y, ts, material)` after each
   tile's `DrawTexturePro` to paint the per-flag pattern (chevron,
   hatch, glint) on top

### 4.3 Fallback-path rendering

When `tiles.id == 0` (the common case today since no `tiles.png`
ships), the existing 2-tone checkerboard at `src/render.c:312-323`
becomes a per-material checkerboard. The 2-tone variation stays for
within-flag variety, but the base color is `material.base` and the
overlay is `material.pattern`. So even on a fresh checkout with no
art, the editor's flag choices read in-game.

### 4.4 The four overlay patterns

All four are pure-procedural, no texture required:

- **`PAT_ICE_GLINT`**: a `sin(time*1.5 + (x+y)*0.04)`-driven 4 px
  diagonal highlight strip drawn as a 1 px wide `DrawLineEx` on each
  ICE tile, alpha modulated by the sin. Adds a slight "wet" sheen
  without a real shader pass.
- **`PAT_DEADLY_HATCH`**: a 45° hatch of 2 px amber-red stripes
  drawn via `DrawLineEx` across the tile (4 stripes per tile at
  `ts/4` spacing). Static — no animation.
- **`PAT_ONE_WAY_CHEVRON`**: a single white chevron `>` glyph
  pointing in the passable direction (always upward for tile-flag
  ONE_WAY since tiles drop down — see §5.1). Drawn via 2
  `DrawLineEx` calls forming a `^` centered on the tile.
- **`PAT_BACKGROUND_ALPHA`**: render at 60% alpha (modify the
  existing `WHITE` tint to `{255,255,255,153}`). No overlay glyph.

### 4.5 Performance budget

Worst case: 110 × 60 = 6,600 tiles (Concourse). Each overlay is 2–4
DrawLineEx calls = at most ~26k draw calls/frame for tile overlays.
raylib batches `DrawLineEx` into the default batcher. M6 P03's
internal-RT cap means these draws hit the capped 1920×1080 surface,
not the 4K backbuffer. Profile with the existing perf overlay
(`--perf-overlay`) and the existing `tests/shots/perf_bench.shot`
script. If overlays cost >1 ms / frame at stress, gate `PAT_ICE_GLINT`
on `theme_id == THEME_ICE_SHEET` only (other themes don't need it).

### 4.6 Tile id atlas

A new shipped asset `assets/sprites/tiles_default.png` (256×256, 8×8
grid of 32×32) gives designers a baseline tile vocabulary even when
no per-map kit ships. Sourced from Kenney Pixel Platformer (§10.1).
This becomes `g_tile_default_atlas` in `src/map_kit.c`, loaded once
at `platform_init` time alongside `g_weapons_atlas` /
`g_decorations_atlas`. The per-map `g_map_kit.tiles` continues to
override it when present.

---

## 5. Polygon visuals + dead-bit physics (Phase B continued)

### 5.1 Fix `POLY_KIND_DEADLY` color

`src/render.c:335`: change
`Color poly_dead = (Color){ 80, 200, 80, 255 };` →
`Color poly_dead = (Color){ 200, 60, 60, 255 };`. Now consistent
with `tools/editor/render.c:19` and `src/map_thumb.c:41`.

### 5.2 Add ONE_WAY chevron

`src/render.c:332-359`: extend `draw_polys` to detect the passable
edge of each `POLY_KIND_ONE_WAY` triangle. The passable edge is the
one whose normal points **upward** (`poly->normal_y[e] < 0`). Paint
a row of small chevron glyphs along that edge using
`DrawLineEx` — 1 chevron per ~24 px of edge length. The chevron
points in the passable direction (the normal).

The same code path also drives the simulation fix: in
`src/physics.c::collide_map_one_pass`, when a tile or poly is
`ONE_WAY`, allow particle traversal in the direction of the normal
and block in the opposite. Today this is wired for polys via
`src/physics.c:717-721` but **not** for tile-flag ONE_WAY. §5.4
fixes the tile side.

### 5.3 Add ICE glint + DEADLY hatch to polys

Same `g_tile_materials` pattern from §4.1 reused — call a new
`draw_poly_overlay(poly, material)` after each triangle fill. ICE
polys get a sin-driven highlight strip across the longest edge;
DEADLY polys get a hatch overlay clipped to the triangle (via
`BeginScissorMode` set to the triangle's AABB, plus a per-pixel
discard — cheaper: just paint the hatch across the AABB and accept
small overdraw outside the triangle since DEADLY polys are usually
narrow strips anyway).

### 5.4 Wire dead tile flags through `collide_map_one_pass`

`src/physics.c:collide_map_one_pass`: extend the per-tile check
to honor `TILE_F_ONE_WAY` (only block contacts whose impact normal
points downward — i.e., the particle is falling onto the tile from
above; sideways and upward contacts pass through). Honor
`TILE_F_BACKGROUND` (skip the collision entirely — the tile is
decorative). Mirror the existing `POLY_KIND_ONE_WAY` /
`POLY_KIND_BACKGROUND` logic at `src/physics.c:599-600,717-721`.

This is the only **simulation behavior change** in P09. It only
affects maps that use these flags on tiles today, which (per the
audit) is zero — no shipping map uses tile-flag ONE_WAY or
BACKGROUND because the editor's UI exposes the checkbox but the
runtime ignored it. So no behavior regression on the eight shipping
maps. New editor-authored maps gain a working tile palette.

### 5.5 Tests for Phase B

- New `tests/shots/m6_tile_flag_differentiation.shot`: single-process
  shot, sets up a map with one row of each flag combination,
  contact-sheet composites a frame showing all five
- New `tests/shots/m6_poly_kind_differentiation.shot`: same, for
  POLY_KIND_*
- New `tests/shots/net/2p_tile_one_way.{host,client}.shot` +
  `run_tile_one_way.sh`: paired-process, host walks a mech down
  through a row of `TILE_F_ONE_WAY | TILE_F_SOLID` tiles, asserts
  both host and client log identical traversal events (one
  pass-through SHOT_LOG line per tile crossing). Catches any
  server↔client divergence in the new collision branch.
- Recolor regressions: existing thumb test if any (search
  `test-map-thumb`); update reference images for DEADLY.

---

## 6. Ambient zone visuals (Phase C — 1 day)

A new pass `draw_ambient_zones(world, time)` invoked from
`renderer_draw_frame` after the poly pass and before the mech pass.
Walks the level's ambient zones and dispatches per `kind`.

### 6.1 `AMBI_WIND`

Visual: per-tick spawns 0–4 streak particles at the rect edge facing
**away from** `(dir_x, dir_y)`. Streaks are 4 px long alpha-fading
lines drifting in `+dir` at the rect's `strength_q` × 200 px/s. Live
0.4–0.8 s (RNG range), die naturally (no tile collision check —
they're decorative).

Particle backed by a new `FX_WEATHER_WIND_STREAK` `FxKind`. Spawn
count `n = floor(strength_q * 4)` per tick, capped at 4. For 8
shipping zones at max strength = 32 spawns/tick × 0.6 s avg life =
~1150 particles steady-state. Comfortably under the §9 budget.

### 6.2 `AMBI_ZERO_G`

Visual: a slow upward dust mote drift inside the rect. Particles
spawn at a uniform rate from random points in the rect (1 spawn/tick
per zone), drift at 8 px/s upward with ±2 px/s horizontal jitter,
live 1.5 s. Each particle is a 1 px dot drawn with `DrawPixel` at
60% alpha. Color: cool blue-white `{180, 200, 230, 153}`. Adds a
"things are floating" hint without simulating.

Backed by `FX_WEATHER_ZEROG_MOTE`. ~30 particles per zone × 8
zones = 240 steady-state.

### 6.3 `AMBI_ACID`

Three layered visuals:

- **Caustic surface band** at the rect top edge: a 4 px tall strip
  of animated caustic texture (see §10.4 for asset) UV-scrolling at
  20 px/s. Drawn additive at 50% alpha.
- **Bubble particles** rising from the rect bottom: spawn 1–2/tick
  per zone, rise at 12 px/s with sinusoidal horizontal sway, live
  1.0 s, fade in last 0.3 s. Drawn as 3 px additive green disks.
- **Pulsing green tint** inside the rect: a `DrawRectangle` call
  with `Color{120, 220, 80, alpha}` where `alpha = 30 +
  20*sin(time*1.5)`.

Backed by `FX_WEATHER_ACID_BUBBLE`. Budget ~80 particles total.

### 6.4 `AMBI_FOG`

Visual: a shader-side fog volume. The CPU side fills a 16-slot
`fog_zones[16]` uniform array peer of the existing `jet_hot_zones[16]`
at `src/render.c:88-102`. Each slot carries `(rect_center_x,
rect_center_y, rect_radius, density)` in screen space (rect → bounding
disc; cheap and the rounded shape reads as natural fog). The shader
adds, per fragment, a per-zone alpha-weighted lerp toward the zone's
fog color (looked up from `LvlMeta.fog_color_rgb565` or the
zone-overridden fog hint from §2.2).

The shader code lives in `assets/shaders/halftone_post.fs.glsl`
alongside the existing shimmer pass (§7.4 details the shader
extension). FOG also **earns its game effect** — for now, the only
game effect is the visual; no AI vision/damage changes. (If a future
pass wants fog to obscure detection, it lives outside P09.)

### 6.5 Tests for Phase C

- New `tests/shots/m6_ambient_zones.shot`: single-process, contact
  sheet showing WIND, ZERO_G, ACID, FOG zones side by side
- Extend `tests/shots/m6_ambient_zones.shot` to use the reactor or
  catwalk map (per the user's `feedback_map_choice` memory —
  parallax-light maps for visual tests)
- New `tests/shots/net/2p_ambient_visuals.{host,client}.shot` +
  `run_ambient_visuals.sh`: paired-process. Host and client should
  render the four zone visuals with the same *count* of particles
  (within ±15% RNG tolerance) and the same fog screen color (exact
  match — fog is shader-derived from .lvl bytes only).

### 6.6 Audio integration

Each ambient zone now also drives a positional audio loop when a
local mech is inside it:

- WIND → reuse `SFX_ENV_WIND_GUST` (already in the M5 P19 manifest)
- ZERO_G → quiet sub-bass drone `SFX_ENV_ZEROG_HUM` (new manifest
  entry, sourced from `kenney_sci-fi-sounds/lowFrequency_drone_*.ogg`
  via the existing inventory pipeline)
- ACID → low bubble loop `SFX_ENV_ACID_BUBBLE` (new manifest entry,
  sourced from `opengameart` bubbling water loops — see §10.6)
- FOG → silent (it's just visual obscurance)

Per-zone audio handle stored on a new `AmbientZoneAudio` state
struct, updated from `simulate.c` ambient zone tick.

---

## 7. Per-map atmosphere (Phase D — 1 day)

### 7.1 Theme enum + palette table

New `src/atmosphere.h`:

```c
typedef enum {
    THEME_CONCRETE  = 0,  /* default: neutral gray industrial */
    THEME_BUNKER    = 1,  /* warm browns, low light, dust */
    THEME_ICE_SHEET = 2,  /* cool blues, glints, snow */
    THEME_NEON      = 3,  /* purples + cyans, dim ambient */
    THEME_RUST      = 4,  /* oranges + iron, embers */
    THEME_OVERGROWN = 5,  /* greens + earth, dust motes */
    THEME_COUNT
} AtmosphereTheme;

typedef struct ThemePalette {
    Color  backdrop;              /* window/RT clear color */
    Color  sky_top;               /* gradient top (used when LvlMeta.sky_top_rgb565 == 0) */
    Color  sky_bot;               /* gradient bottom */
    Color  tile_solid;            /* default solid material base */
    Color  tile_ice;              /* ICE material base */
    Color  tile_deadly;           /* DEADLY material base */
    Color  fog_color;             /* default fog tint */
    float  vignette;              /* default vignette strength */
    int    default_weather_kind;  /* default weather (0 = none) */
    float  default_weather_density;
} ThemePalette;

extern const ThemePalette g_themes[THEME_COUNT];
```

Per-map `LvlMeta` fields override theme defaults when non-zero.

### 7.2 Sky gradient

Replaces the hardcoded `{12,14,18,255}` clear at `src/render.c:1456,1472`.
New `draw_sky_gradient(top, bot)` paints a vertical gradient to the
internal-RT (so it lives inside the M6 P03 capped pipeline). Single
quad with vertex colors — raylib supports this via `DrawRectangleGradientV`.
Costs nothing measurable.

If a map has parallax_far/mid/near set (`MapKit`), they draw on top of
the sky gradient as today. Sky gradient becomes the "depth zero"
backdrop.

### 7.3 Vignette

Shader-side, folded into `halftone_post.fs.glsl`. Per-fragment
`vignette = pow(distance_from_center, 2) * vignette_strength`,
darkening the edges. Standard postprocessing recipe.

### 7.4 Shader uniform extensions

`assets/shaders/halftone_post.fs.glsl` gains:

```glsl
uniform vec3  sky_top;            /* (r, g, b) — for sky-portion fragments only */
uniform vec3  sky_bot;
uniform float fog_density;        /* global fog density, 0 = off */
uniform vec3  fog_color;          /* tint */
uniform float vignette_strength;  /* 0 = off */
uniform vec4  fog_zones[16];      /* (cx, cy, radius, density) per AMBI_FOG zone */
uniform int   fog_zone_count;
uniform float atmos_time;         /* monotonic seconds, peer of jet_time */
```

The shader's fragment main extends to:

1. Read source pixel (existing)
2. Sample shimmer (existing)
3. Apply global fog: `mix(src, fog_color, fog_density)`
4. Apply zone fog: for each `fog_zones[i]`, accumulate
   `density * exp(-dist² / radius²)` and `mix` toward `fog_color`
   weighted by accumulated density
5. Apply halftone (existing)
6. Apply vignette: `result *= (1 - vignette_strength * pow(d, 2))`

Each new uniform follows the existing `<= 0.001` short-circuit
pattern at `halftone_post.fs.glsl:115` so a zero-init map costs zero
shader ops.

Uniform locations cached in `src/render.c` at the same site that
caches the existing shimmer uniforms (`src/render.c:52-60,88-102`).
Per-frame `SetShaderValue` calls live in the same block that sets the
shimmer uniforms today (`src/render.c:1488-1530`).

### 7.5 Editor Atmospherics panel

New panel in the META modal at `tools/editor/editor_ui.c`. Layout
(modal grows to ~640 × 720 px):

```
[Atmospherics]
  Theme:           [CONCRETE ▾]    [Preview swatch]
  Sky top:         [color picker]  (or "use theme")
  Sky bottom:      [color picker]
  Fog density:     [────●────────] 0.00
  Fog color:       [color picker]
  Vignette:        [─●──────────] 0.10
  Sun angle:       [─────●──────] 180°
  Weather:         [NONE ▾]
  Density:         [────────────] 0.00
```

The theme dropdown sets the swatch + defaults for the other rows.
Each subsequent row is editable independently. "Use theme" buttons
restore the theme default.

### 7.6 Tests for Phase D

- New `tests/shots/m6_atmosphere_themes.shot`: single-process, loads
  the same geometry 6 times with 6 different themes, contact-sheet
  composites all 6 frames showing the per-theme look
- Extend `test-level-io`: round-trip new `LvlMeta` atmosphere fields
- New `tests/shots/net/2p_atmosphere_parity.{host,client}.shot` +
  `run_atmosphere_parity.sh`: paired-process, host loads a custom
  `.lvl` with non-default atmospherics (theme = NEON, fog = 0.4,
  vignette = 0.3, weather = EMBERS, density = 0.5), client downloads
  via P08 + applies, both screenshots should be visually identical
  (per-pixel diff < 5% RGB delta after a 2-second warm-up to let
  weather RNG converge)

---

## 8. Weather (Phase E — 1 day)

Four modes; each is a particle spawner driven by
`LvlMeta.weather_kind` + `weather_density_q`. Particles spawn in
screen-space (cheap — independent of map size) and live ~2–4 seconds.

### 8.1 Snow

`FX_WEATHER_SNOW`: spawn `(density * 8)` particles/tick at random x
along the top of the screen, drift down at 30 px/s with mild
horizontal wind (sin-modulated). 2–3 px white dots at 70% alpha.

### 8.2 Rain

`FX_WEATHER_RAIN`: spawn `(density * 16)` particles/tick at random x
along the top, fall at 400 px/s with slight tilt. 4 px long thin
white lines (`DrawLineEx` width 1, alpha 60%). On tile contact (cheap
`level_point_solid` check), spawn a tiny 4-particle splash via the
existing `fx_spawn` API.

### 8.3 Dust

`FX_WEATHER_DUST`: spawn `(density * 4)` particles/tick at random
screen points, drift slowly (5 px/s avg) in a random direction, live
3 s. 1–2 px warm tan dots at 40% alpha. Best on RUST and BUNKER
themes.

### 8.4 Embers

`FX_WEATHER_EMBER`: spawn `(density * 6)` particles/tick at random x
along the bottom of the screen, drift upward at 15 px/s with
sinusoidal horizontal sway, live 2.5 s. Glowing additive red-orange
3 px disks. Best on RUST theme.

### 8.5 Pool budget

Worst case: density = 1.0 RAIN at 60 Hz = 16 × 60 × 0.5 s avg life =
~480 active rain particles. Doubling for safety (and other weather
running simultaneously, which never happens per LvlMeta but the
budget should still cover): ~1,000 weather particles.

Current `FxPool` cap: 8500 (M6 P04). Worst-case combat draws: ~4,500
particles. Headroom: 4,000. Bump `FxPool` capacity 8500 → 10500 to
absorb worst-case (weather + combat + ambient zone particles
overlapping). This is the *only* sizing change in P09.

### 8.6 Performance

Particle count for any single map is bounded by the LvlMeta-set
density. Render cost is `DrawPixel` / `DrawLineEx` / additive disc —
all are batched. Profile via the existing perf overlay; if a stress
case exceeds 1 ms / frame, halve the density caps on the worst
offender.

### 8.7 Tests for Phase E

- New `tests/shots/m6_weather_modes.shot`: single-process, contact
  sheet of SNOW / RAIN / DUST / EMBERS at density 0.5
- New `tests/shots/m6_weather_stress.shot`: single-process at
  density 1.0 SNOW + RAIN simultaneously, with the perf overlay on;
  asserts frame time < 16.6 ms in the SHOT_LOG perf line
- Extend `test-snapshot` round-trip: weather is purely
  client-derived from LvlMeta — no snapshot changes — but confirm
  the `LvlMeta` fields round-trip through `snapshot_apply`'s
  level-descriptor path

---

## 9. FxPool extension

```c
typedef enum {
    /* existing kinds ... */
    FX_DAMAGE_NUMBER     = ...,  /* M6 P04 */
    /* P09 — environmental/atmosphere kinds */
    FX_AMBIENT_WIND_STREAK,
    FX_AMBIENT_ZEROG_MOTE,
    FX_AMBIENT_ACID_BUBBLE,
    FX_WEATHER_SNOW,
    FX_WEATHER_RAIN,
    FX_WEATHER_DUST,
    FX_WEATHER_EMBER,
    FX_KIND_COUNT
} FxKind;
```

`FxPool` capacity: `8500 → 10500`. The bump touches one `#define` in
`src/particle.h` and is reflected in `CURRENT_STATE.md`. No
per-particle struct size change — all the new kinds reuse the
existing `FxParticle` layout (pos, vel, life, color, kind, plus the
`angle`/`ang_vel` fields M6 P04 added; weather/ambient particles
ignore those fields).

New per-kind branches in `src/particle.c::fx_update` and
`fx_draw`. Pattern matches M6 P02's `FX_JET_EXHAUST` /
`FX_GROUND_DUST` and M6 P04's `FX_DAMAGE_NUMBER` branches.

---

## 10. Asset sources

Every asset below is CC0 or zlib (raylib examples). No CC-BY-SA, no
non-commercial restrictions. Sourcing pipeline: extend the existing
`tools/audio_inventory/source_map.sh` pattern into a sibling
`tools/sprite_inventory/source_map.sh` that fetches the listed
asset packs, copies the needed sub-files into `assets/sprites/`,
and writes per-source attribution into `CREDITS.md`.

### 10.1 Tile sets

- **Kenney — Pixel Platformer** (CC0) —
  https://kenney.nl/assets/pixel-platformer — primary tile vocabulary;
  base SOLID/ICE/sand variants
- **Kenney — Pixel Platformer Industrial Expansion** (CC0) —
  https://kenney.nl/assets/pixel-platformer-industrial-expansion —
  pipes/machinery for INDUSTRIAL theme tiles
- **Kenney — Pixel Platformer Blocks** (CC0) —
  https://kenney.nl/assets/pixel-platformer-blocks — material blocks
  (use for theme-specific tile palettes)
- **OpenGameArt — Sci-fi platformer tiles 32×32** (CC0) —
  https://opengameart.org/content/sci-fi-platformer-tiles-32x32 —
  crisp 32px metallic for NEON theme

Process: pick 64 tiles from these packs that map to our flag matrix
(4 base × 16 variants), composite into `assets/sprites/tiles_default.png`
(256×256, 8×8 of 32×32). Per-theme override atlases ship under
`assets/maps/<short>/tiles.png` as today.

### 10.2 Decorations

- **Kenney — Background Elements** (CC0) —
  https://kenney.nl/assets/background-elements
- **Kenney — Background Elements Redux** (CC0) —
  https://kenney.nl/assets/background-elements-redux
- **Kenney — Nature Kit** (CC0) — https://kenney.nl/assets/nature-kit

Composite into `assets/sprites/decorations.png` with a manifest at
`src/decorations_manifest.c` (replaces the
`deco_src_rect_for` hash-of-offset stopgap at `src/render.c:464-470`).

### 10.3 Particles / overlays

- **Kenney — Particle Pack** (CC0) —
  https://kenney.nl/assets/particle-pack — fog/dust/glow textures
- **Kenney — Smoke Particles** (CC0) —
  https://kenney.nl/assets/smoke-particles — fog texture for the
  shader fog volume
- **OpenGameArt — 700+ Noise Textures** (CC0) —
  https://opengameart.org/content/700-noise-textures — shader
  lookup textures for shimmer and fog noise

### 10.4 Effects textures

- **OpenGameArt — Caustic Textures** (CC0) —
  https://opengameart.org/content/caustic-textures — ACID surface
  caustic
- **OpenGameArt — Water Caustics Effect (Small)** (CC0) —
  https://opengameart.org/content/water-caustics-effect-small —
  16-frame animated loop, ACID alternate

### 10.5 Shader recipe references (raylib, zlib)

- **Fog rendering** —
  https://github.com/raysan5/raylib/blob/master/examples/shaders/shaders_fog_rendering.c
  (3D-targeted; the GLSL ports cleanly to 2D)
- **Palette switch** —
  https://github.com/raysan5/raylib/blob/master/examples/shaders/shaders_palette_switch.c
  (per-tile-type tinting recipe)
- **Bloom** (GLSL330) —
  https://github.com/raysan5/raylib/blob/master/examples/shaders/resources/shaders/glsl330/bloom.fs
  (acid glow / ember glow)
- **Texture waves** —
  https://www.raylib.com/examples/shaders/loader.html?name=shaders_texture_waves
  (acid surface UV animation)
- **Postprocessing harness** —
  https://github.com/raysan5/raylib/blob/master/examples/shaders/shaders_postprocessing.c
  (vignette + chromatic aberration scaffold)
- **Spotlight** —
  https://github.com/raysan5/raylib/blob/master/examples/shaders/resources/shaders/glsl330/spotlight.fs
  (sun direction lit-decal recipe)

### 10.6 New SFX entries

- **`SFX_ENV_ZEROG_HUM`** — sourced from
  `kenney_sci-fi-sounds/lowFrequency_drone_*.ogg` via the existing
  `tools/audio_inventory/source_map.sh`
- **`SFX_ENV_ACID_BUBBLE`** — search opengameart.org for "bubbling
  water loop CC0"; verify license before commit

### 10.7 Map theme assignment recook

Update `tools/cook_maps/cook_maps.c` to set `LvlMeta.theme_id` per
map. Suggested assignment:

| Map | Theme | Weather |
|---|---|---|
| foundry | INDUSTRIAL | none |
| slipstream | ICE_SHEET | SNOW @ 0.3 |
| reactor | RUST | EMBERS @ 0.4 |
| concourse | CONCRETE | none |
| catwalk | NEON | none |
| aurora | NEON | DUST @ 0.2 |
| crossfire | CONCRETE | none |
| citadel | BUNKER | DUST @ 0.3 |

These are starting points — the implementer can adjust based on how
the visuals read after Phase D ships. `make cook-maps` regenerates
all eight `.lvl` files with the new metadata; existing user installs
re-download via P08 transparently (content-addressed cache
invalidates on the new CRC32).

---

## 11. Server / client / single-process / editor parity

The user explicitly asked for "client and server" parity. This
section unpacks what that means at each runtime:

### 11.1 Dedicated server (headless)

The standalone `--dedicated` server runs no renderer. `decal_init` is
already gated standalone-only per wan-fixes-16. The new
`atmosphere_init` / weather spawner / shader uniform calls follow the
same gate: skip if no GL context. Server reads the full `.lvl`
including new `LvlMeta` fields; the only **simulation** effect P09
adds is the `TILE_F_ONE_WAY` / `TILE_F_BACKGROUND` collision fix at
§5.4, which runs on the server too (it's `src/physics.c`, shared
code). Server replicates the same state it always has — no new wire
fields.

### 11.2 Client (UI process)

Loads `.lvl` either from local disk or via P08 map-share. Both load
paths run `level_load` at `src/level_io.c`, which populates the
augmented `LvlMeta`. The client's render pass consumes the new fields
via the new `g_atmosphere` state struct (`src/atmosphere.c`),
populated once on `map_build`. Per-frame uniform updates feed the
shader.

### 11.3 In-process server (wan-fixes-16 host mode)

Same as two-process: the in-process server thread reads `.lvl` for
physics; the UI client thread reads `.lvl` independently for visuals.
No shared memory between threads; the `.lvl` bytes on disk are the
sync mechanism. Both ends see the same atmospherics.

### 11.4 Single-process (shotmode / --test-play)

One thread does both physics and rendering. The atmospherics state
populates on `map_build` and feeds the same render code. Shot tests
that don't care about atmospherics get a deterministic look by
defaulting `theme_id = 0` and zero density/weather (the existing
`build_fallback` path produces this naturally).

### 11.5 Editor test-play (F5)

F5 spawns the game with `--test-play <path>`. The game loads the
edited `.lvl` end-to-end, applying the full atmospherics pipeline.
**This is the editor's preview mechanism.** The editor itself does
not render atmospherics inside its own draw pass — that would
duplicate the renderer. F5 → real game → real visuals.

A new ESC-to-return-to-editor flow (currently the test-play spawns
a fresh game process that the editor doesn't track; the user has to
manually close the test-play window) is **out of scope** for P09.
Tracked as a future trade-off.

### 11.6 Determinism vs. RNG

Particle positions for weather and ambient zones are seeded from
`World.rng` (the existing PCG) keyed by tick number. Two clients at
the same tick with the same `World.rng` seed produce identical
particle positions. The `World.rng` seed is part of `INITIAL_STATE`
(wan-fixes-era), so it already replicates. **The paired-process
atmosphere parity test (§7.6) leans on this** — host and client
should produce identical screenshots within RNG tolerance.

If determinism turns out to be intractable (driver float order,
particle creation order under packet jitter), fall back to a
parallel `Atmosphere.rng` seeded from a hash of `tick + level_crc32`
on both ends, so the seed converges even if `World.rng` drifts.

---

## 12. Renderer integration — concrete file plan

### 12.1 New module: `src/atmosphere.{c,h}`

```
src/atmosphere.h:
  - AtmosphereTheme enum + ThemePalette struct + g_themes[] extern
  - TileMaterial struct + g_tile_materials[] extern + tile_material_for()
  - WeatherKind enum
  - Atmosphere struct (current resolved per-map state)
  - atmosphere_init_for_map(const Level*, const LvlMeta*) — populates g_atmosphere
  - atmosphere_tick(World*, float dt) — weather particle spawn
  - atmosphere_draw_sky() — sky gradient pass
  - atmosphere_draw_ambient_zones(const Level*, double time) — zone visuals
  - atmosphere_collect_fog_zones(vec4 out[16], int *out_count) — uniform fill
  - g_atmosphere (the live state)

src/atmosphere.c:
  - g_themes hand-tuned table (THEME_COUNT entries)
  - g_tile_materials hand-tuned table (16 entries indexed by flag combo)
  - Weather spawners (one function per kind, each ~30 LOC)
  - Sky gradient draw
  - Ambient zone draw dispatch
```

### 12.2 `src/render.c` changes

- `renderer_draw_frame` (`src/render.c:1456-1545`): call
  `atmosphere_draw_sky()` before the M6 P03 internal-RT begin (sky
  paints into the internal RT); call `atmosphere_draw_ambient_zones()`
  after `draw_polys` and before mech draw; collect fog zones into
  `fog_zones[16]` uniform; set the new shader uniforms in the
  existing per-frame block at `src/render.c:1488-1530`
- `draw_level_tiles` (`src/render.c:288-325`): per-flag material
  lookup + overlay paint
- `draw_polys` (`src/render.c:332-359`): DEADLY color fix + chevron
  for ONE_WAY + glint/hatch overlays
- New `draw_weather()` called after the internal-RT upscale blit and
  before HUD (same layering rationale as M6 P04's damage numbers —
  weather lives at sharp window pixels)

### 12.3 `assets/shaders/halftone_post.fs.glsl` changes

Add the §7.4 uniforms + per-fragment logic. Keep every new effect
behind a `<= 0.001` short-circuit so default maps cost zero extra
shader work. The shimmer pass from M6 P02 stays unchanged.

### 12.4 `src/physics.c` changes

`collide_map_one_pass`: honor `TILE_F_ONE_WAY` and
`TILE_F_BACKGROUND` per §5.4. Pattern matches the existing
`POLY_KIND_ONE_WAY` block at `src/physics.c:717-721`.

### 12.5 `src/simulate.c` changes

Call `atmosphere_tick(world, dt)` after the existing FX step.
Weather particle spawn is per-tick CPU work; the per-tick budget is
covered in §8.5.

### 12.6 `src/world.h` changes

Promote `LvlMeta.reserved[9]` to the 8 named atmosphere fields per
§2.1.

### 12.7 `src/particle.{c,h}` changes

Add the new `FxKind` values per §9. Add per-kind update branches in
`fx_update`. Add per-kind draw branches in `fx_draw`. Bump
`FX_POOL_CAP` from 8500 to 10500.

### 12.8 `src/map_kit.c` changes

Load `assets/sprites/tiles_default.png` once at startup; expose as
`g_tile_default_atlas` for the renderer to use when
`g_map_kit.tiles.id == 0`.

### 12.9 `src/audio.c` changes

Add the new env loop SFX entries per §6.6. Drive per-zone audio from
`atmosphere_tick`.

### 12.10 `tools/editor/*` changes

Per §3 — new palette panels, ToolCtx fields, tool.c writeback paths.
The editor's own debug renderer at `tools/editor/render.c` stays
flat-colored (see §11.5 — F5 is the preview mechanism).

### 12.11 `tools/cook_maps/cook_maps.c` changes

Set `LvlMeta.theme_id` and `weather_kind`/`weather_density_q` per
the §10.7 table. `make cook-maps` regenerates all eight `.lvl`s.

---

## 13. Test plan

### 13.1 Unit / assert tests

- **`test-level-io`** — extend with assertions for every new
  `LvlMeta` field (theme, sky colors, fog, vignette, sun, weather)
  round-tripping through `level_save` → `level_load`
- **`test-map-share`** — extend with new-field-survival assertion
  through chunk → CRC32 → reassemble
- **`test-snapshot`** — no change required (atmospherics are
  level-level, not per-tick wire data); add one defensive assertion
  that `LvlMeta` fields survive `INITIAL_STATE` replication
- **New `test-atmosphere-defaults`** — boots without a `.lvl`, asks
  for theme = 0, verifies `g_atmosphere` matches `g_themes[0]` field
  for field

### 13.2 Shot tests (single-process visual review)

Write each shot test on a parallax-light map per the user's
`feedback_map_choice` memory (reactor / slipstream / catwalk /
citadel / concourse — never foundry):

- `tests/shots/m6_tile_flag_differentiation.shot` — 5-tile row of
  each flag combination, contact sheet (4 cols × 2 rows)
- `tests/shots/m6_poly_kind_differentiation.shot` — 5 poly kinds
  side by side
- `tests/shots/m6_ambient_zones.shot` — WIND/ZERO_G/ACID/FOG zones
  on reactor
- `tests/shots/m6_atmosphere_themes.shot` — same geometry rendered
  6 times with theme_id 0..5
- `tests/shots/m6_weather_modes.shot` — SNOW/RAIN/DUST/EMBERS
- `tests/shots/m6_weather_stress.shot` — SNOW + RAIN at density 1.0
  with `--perf-overlay`; asserts frame time
- `tests/shots/m6_deco_full_controls.shot` — places decos at
  multiple layers / scales / rotations / flip / additive, contact
  sheet shows each variant

### 13.3 Paired-process tests (host + client visual + sim parity)

Per the user's `feedback_networked_tests` memory — multiplayer
features require paired networked shot tests:

- `tests/shots/net/2p_tile_one_way.{host,client}.shot` +
  `run_tile_one_way.sh` — host walks through `TILE_F_ONE_WAY` tiles;
  both sides assert identical pass-through SHOT_LOG events
- `tests/shots/net/2p_ambient_visuals.{host,client}.shot` +
  `run_ambient_visuals.sh` — host and client render same ambient
  zone particle count (±15% RNG tolerance) + identical fog screen
  color
- `tests/shots/net/2p_atmosphere_parity.{host,client}.shot` +
  `run_atmosphere_parity.sh` — custom .lvl, host + client
  screenshots match within tolerance

Per the user's `reference_paired_shot_spawns` memory, client
positioning in these tests uses host's `peer_spawn SLOT WX WY`
directive (not client `spawn_at` which gets overridden by snapshots).

### 13.4 Process cleanup (always)

Per the user's `feedback_test_cleanup` memory: every new test
harness must kill+verify the spawned game processes before exiting
the tool call. Existing `tests/shots/net/run.sh` already does this;
new `run_*.sh` scripts must follow the same pattern.

### 13.5 Audio volume (always)

Per the user's `feedback_shot_test_volume` memory: shot tests
default audio volume ≤ 30%. The new env-loop SFX from §6.6 must
default low.

### 13.6 No CI

Per `TRADE_OFFS.md` "No CI for physics correctness", none of these
run in CI. The developer runs them locally before opening the PR.
PR description includes the contact-sheet PNG outputs inline (per
the user's `feedback_test_screenshots` memory — show the intent
visually, don't trust PASS).

---

## 14. Implementation phases (build order)

Each phase is independently shippable behind its own PR. They build
on each other but each phase produces visible polish even if the
later phases never ship. If the implementer needs to pause partway
through, the partial state is shippable.

### Phase A — Editor parity (~1 day)
- §3 in full
- Tests: `test-level-io` extensions, `test_editor_paths.c` smoke,
  `test-map-share` extensions
- Ship: PR titled "M6 P09 Phase A — editor exposes settable fields"

### Phase B — Tile + poly visual differentiation (~1 day)
- §4 + §5 in full
- New module `src/atmosphere.{c,h}` skeleton (just the tile material
  table — themes/weather are Phase D)
- Wire `TILE_F_ONE_WAY` / `TILE_F_BACKGROUND` collision fix (§5.4)
- Tests: `m6_tile_flag_differentiation.shot`,
  `m6_poly_kind_differentiation.shot`, `2p_tile_one_way` paired
- Ship: PR titled "M6 P09 Phase B — tile + poly visual hygiene"

### Phase C — Ambient zone visuals (~1 day)
- §6 in full
- New `FxKind`s for ambient particles (§9)
- New env-loop SFX (§6.6, §10.6)
- Tests: `m6_ambient_zones.shot`, `2p_ambient_visuals` paired
- Ship: PR titled "M6 P09 Phase C — ambient zones get visuals"

### Phase D — Per-map atmosphere (~1 day)
- §7 in full
- Theme palette table + shader extension
- `LvlMeta` field promotion (§2.1) + cook_maps recook (§10.7)
- Tests: `m6_atmosphere_themes.shot`, `2p_atmosphere_parity` paired
- Ship: PR titled "M6 P09 Phase D — per-map atmospherics"

### Phase E — Weather + sourced assets (~1 day)
- §8 in full
- New `FxKind`s for weather (§9)
- Asset sourcing pipeline (`tools/sprite_inventory/source_map.sh`)
  per §10
- Default tile atlas ship (§4.6)
- Tests: `m6_weather_modes.shot`, `m6_weather_stress.shot`
- Ship: PR titled "M6 P09 Phase E — weather + sourced atmospherics"

If the work needs to compress, Phases A+B can ship as one PR (the
"parity" half) and Phases C+D+E as another (the "atmospherics"
half). Phases C/D/E individually are too coupled to split further.

---

## 15. Documentation updates

### 15.1 `CURRENT_STATE.md`

Append a new "Atmospherics" tunables section:

```
## Atmospherics (M6 P09)

g_themes[]                    src/atmosphere.c    6 themes
weather density caps:
  SNOW spawn / tick           per density × 8
  RAIN spawn / tick           per density × 16
  DUST spawn / tick           per density × 4
  EMBER spawn / tick          per density × 6
FxPool capacity               10500 (was 8500)
new FxKinds                   7 (4 weather + 3 ambient)
new shader uniforms           5 globals + fog_zones[16] + atmos_time
```

### 15.2 `TRADE_OFFS.md` retirements

Delete the following entries (each describes state P09 fixes):

- "AMBI_FOG enum is inert" (if present — if not, none retired here)
- "Tile flag UI controls have no in-game visual effect"
- "DEADLY polys render green not red"
- "Decorations ship at placeholder defaults; editor exposes no controls"
- "No per-map atmosphere / sky / fog / weather"
- "TILE_F_ONE_WAY does not function on tiles"
- "TILE_F_BACKGROUND does not function on tiles"

(Audit `TRADE_OFFS.md` at implementation time — only delete entries
that genuinely exist in the file. The list above describes the
*observed* gaps; some may not have been recorded as formal trade-offs
yet. New entries to *add*: none — every limitation here is fixed.)

### 15.3 `CLAUDE.md` status line

The opening paragraph at the top of `CLAUDE.md` describes the
current ship state. P09 amends it to read (insert after "M6 P04 ships
flying damage-number glyphs..." block):

```
**M6 P09** (2026-MM-DD) — Editor-runtime parity + tile atmospherics:
every TILE_F_* flag, every PolyKind, every AmbiKind now produces a
visually distinct in-game effect; AMBI_FOG earns its visual via a
shader fog volume; TILE_F_ONE_WAY and TILE_F_BACKGROUND collision
finally wired through `collide_map_one_pass`; LvlMeta.reserved[9]
promoted to 8 named atmospherics fields (theme_id, sky gradient
colors, fog density, fog color, vignette, sun angle, weather kind,
weather density); 6 themes + 4 weather modes (SNOW/RAIN/DUST/EMBERS)
shipped; halftone shader extended with 5 new uniforms + fog_zones[16]
peer of jet_hot_zones[16]; FxPool capacity 8500 → 10500; editor gains
full DECO controls (sprite picker / layer / scale / rot / flip /
additive) + full META atmospherics panel; tile_default.png + curated
decorations.png composited from Kenney + OpenGameArt CC0 sets. Asset
sourcing through new tools/sprite_inventory/source_map.sh. Protocol
id stays S0LK (no wire-format changes). LVL_VERSION stays at current
(LvlMeta additive — old .lvl files read zeros = use defaults). See
documents/m6/09-editor-runtime-parity-and-atmospherics.md.
```

### 15.4 `documents/m5/08-rendering.md`

The "Per-map kit assets" section at `documents/m5/08-rendering.md` needs
a paragraph appended describing the P09 atmospherics layer that runs
*on top of* the per-map kit (the kit still owns parallax + tile atlas;
atmosphere owns sky/fog/vignette/weather; these compose cleanly).

---

## 16. Risks and trade-offs to track

- **Shader complexity creep.** Five new uniforms + a fog-zones loop
  inside `halftone_post.fs.glsl`. Each is short-circuited at the
  `<= 0.001` pattern but the shader is growing. Profile on the
  4K-windowed perf stress (§13) — if cost exceeds the P03 budget
  recovery, split atmospherics into its own second pass to keep the
  halftone shader simple.
- **Weather + ambient particle budget.** Worst case is roughly 1,500
  particles concurrent. Pool bumped accordingly. If a future map
  uses density 1.0 weather + 16 ambient zones simultaneously,
  revisit.
- **Editor UI density.** The META modal grows from ~480 px tall to
  ~720 px tall to fit the Atmospherics panel. Verify it fits on a
  720p editor window (the editor's default). If not, scroll the
  modal contents (raygui supports this) — do not split atmospherics
  into a separate modal because designers iterate atmosphere
  alongside name/mode.
- **Theme assignment art-direction.** The §10.7 theme assignments
  are starting points. The implementer should iterate visuals
  per-map and adjust the table based on what reads best. Update
  this document's §10.7 with whatever ships.
- **Tile material visual subtlety.** ICE glint is a sin-driven
  highlight, not a full reflection shader. If the look is too subtle
  at 1080p, increase the glint width or saturation rather than
  jumping to a per-tile reflection — the cost trade-off matters at
  6,600 tiles.
- **Old maps look unchanged.** Pre-P09 `.lvl` files read zero in the
  new atmospherics fields → fall to theme 0 (CONCRETE, no fog, no
  weather). This is **the correct default** — old maps shouldn't
  suddenly snow because the engine grew weather. Designers opt in
  via `make cook-maps` (the eight shipping maps recook to opt
  themselves into atmospherics per §10.7).

---

## 17. Files touched

Expected file list (audit at PR time — extend if a phase needs
something not listed):

**New**
- `src/atmosphere.h`, `src/atmosphere.c`
- `src/decorations_manifest.c` (replaces hash-of-offset stopgap)
- `tools/sprite_inventory/source_map.sh` (sister of audio_inventory)
- `tools/editor/deco_manifest_view.c` (editor's read of the manifest)
- `assets/sprites/tiles_default.png` (composited from Kenney)
- `assets/sprites/decorations.png` (composited from Kenney)
- `assets/sprites/caustic_acid.png` (OpenGameArt caustics)
- `assets/sprites/fog_noise.png` (OpenGameArt 700 noise textures)
- `assets/sfx/env_zerog_hum.ogg`, `assets/sfx/env_acid_bubble.ogg`
- 7 new shot scripts under `tests/shots/`
- 3 new paired shot scripts + 3 run_*.sh under `tests/shots/net/`

**Modified**
- `src/world.h` (LvlMeta promotion)
- `src/level_io.{c,h}` (no schema change but new field tests need accessors)
- `src/render.c` (per-flag tile/poly visuals + new draw passes + uniforms)
- `src/physics.c` (tile-flag ONE_WAY/BACKGROUND wiring)
- `src/simulate.c` (atmosphere_tick call)
- `src/particle.{c,h}` (new FxKinds, pool cap bump)
- `src/map_kit.{c,h}` (default tile atlas load)
- `src/audio.c` (new env-loop SFX entries + per-zone audio)
- `assets/shaders/halftone_post.fs.glsl` (new uniforms + fog volume)
- `tools/editor/editor_ui.c` (TILE id picker, DECO palette, AMBI
  sliders, META text/dropdowns/atmospherics panel)
- `tools/editor/tool.h` (ToolCtx extensions)
- `tools/editor/tool.c` (writeback paths)
- `tools/editor/level_io.c` (writeback paths)
- `tools/cook_maps/cook_maps.c` (per-map theme assignment)
- `tests/test_level_io.c` (new field assertions)
- `tests/test_map_share.c` (new field survival assertions)
- `Makefile` (new test targets, asset composition steps,
  `make sprite-inventory` peer of `make audio-inventory`)
- `CLAUDE.md`, `CURRENT_STATE.md`, `TRADE_OFFS.md`
- `documents/m5/08-rendering.md` (composition note)

---

## 18. Status footnote

This section is filled in post-ship. Template:

> **Shipped 2026-MM-DD.** Phases A–E landed in NN PRs. Final
> measured deltas: FxPool peak X particles (cap 10500); shader added
> N ms per frame at 4K stress (was 0); shipped tile_default.png at
> Wx H; new SFX manifest entries: env_zerog_hum, env_acid_bubble.
> Outstanding items deferred to future passes: ___. Trade-off
> entries retired: ___. Trade-off entries added: ___ (if any).
