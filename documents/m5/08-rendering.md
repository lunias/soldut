# M5 — Rendering & art

The visual layer that retires the M1–M4 capsule mech / silent world prototype. M5 ships:

- Mech sprite atlases (5 chassis) replacing the thick-line-bone rendering in `src/render.c::draw_mech`.
- 3-layer parallax backgrounds per map.
- Decoration sprites in the world (decorative non-collision elements).
- Final HUD art: bars, kill-feed icons, weapon icons, crosshair.
- TTF font replacing the bilinear-filtered raylib default.
- Decal-layer chunking for big maps (>4096 px).
- Asset hot-reload during dev.

The audio layer is in [09-audio.md](09-audio.md). The level format that references these assets is in [01-lvl-format.md](01-lvl-format.md).

## What we're keeping vs. replacing

| Subsystem | M4 state | M5 state |
|---|---|---|
| Mech body | Capsule-line-bones (`draw_mech`) | Sprite atlas per chassis |
| Tile rendering | Solid 2-tone rectangles per tile | Sprite atlas per kit (industrial set) |
| Background | Solid dark-grey clear | 3-layer parallax PNGs |
| HUD | raylib defaults + crosshair lines | Atlas-based icons + bars |
| Font | `GetFontDefault()` + bilinear filter | Atkinson Hyperlegible TTF |
| Decals | One whole-level RT layer | Chunked 1024² when level >4096 px |
| Particles | Atlas (already at M4) | Same |
| Projectiles | DrawCircleV + trail | Same (the projectile look is fine; not blocking ship) |

Nothing breaks; M4's render path is the fallback when an asset isn't present. The "default" behavior degrades to the M4 look.

## Mech sprite atlases

Per the research, the right pattern is **one 1024×1024 atlas per chassis**. Each chassis has 12 visible parts plus 5 stump caps; pack them into a single texture; bind once per chassis per frame.

**The full per-part anatomy — including overlap zones, exposed-end discipline, pivot data, stump caps, damage decals, hit-flash, and smoke-from-damaged-limbs — is specified in [12-rigging-and-damage.md](12-rigging-and-damage.md).** That doc is the contract between the art pipeline ([11-art-direction.md](11-art-direction.md)) and the renderer; this section here covers atlas packaging only.

### Atlas layout

```
soldut_<chassis>.png    # 1024×1024 RGBA8 per chassis (5 files)

Sub-rects (sizes include the overlap-zone padding; see 12-rigging-and-damage.md
for the rules on overlap zones and exposed-end discipline):
  torso             (~56×72  + overlap padding)
  head              (~40×40)
  arm_upper_l/_r    (~32×80  +14 px parent overlap)
  arm_lower_l/_r    (~28×72  +12 px elbow-side overlap)
  hand_l/_r         (~16×16  +4 px wrist overlap)
  leg_upper_l/_r    (~36×96  +16 px hip-side overlap)
  leg_lower_l/_r    (~32×88  +14 px knee-side overlap)
  foot_l/_r         (~32×24  +6 px ankle overlap)
  shoulder_plate_l/_r  (~56×40 — drawn at shoulder particle, covers upper-arm overlap)
  hip_plate            (~64×36 — drawn at pelvis, covers upper-leg overlap)
  stump_shoulder_l/_r  (~32×32 — drawn at parent particle on dismemberment)
  stump_hip_l/_r       (~32×32 — drawn at parent particle on dismemberment)
  stump_neck           (~32×32 — drawn at parent particle on dismemberment)
  jetpack              (~120×60 — only when jetpack ID != JET_NONE)
```

Each part is a single sprite — flat-shaded, hand-drawn line work, no texture detail. Per the design canon ([06-rendering-audio.md](../06-rendering-audio.md) §"Drawing the mechs"). Each part has an **origin offset** (the pivot — where the bone passes through) baked into its sub-rect entry. The 5 stump caps activate when their corresponding limb is dismembered; see [12-rigging-and-damage.md](12-rigging-and-damage.md) §"Dismemberment visuals".

### Atlas data file

```c
// assets/sprites/<chassis>.png + assets/sprites/<chassis>.atlas
//
// .atlas is a tiny binary file (or, equivalently, a small C struct
// shipped in code — we ship in code at v1):

typedef struct {
    Rectangle src;         // sub-rect into the atlas
    Vec2      origin;      // pivot, in src coords
    float     draw_w, draw_h;
} MechSpritePart;

typedef struct {
    Texture2D atlas;
    MechSpritePart parts[MECH_PART_RENDER_COUNT];
} MechSpriteSet;
```

Loaded once at startup per chassis (`assets/sprites/trooper.png` etc.). Stored in `Game.permanent` arena.

Building atlases at v1: hand-pack via [rTexPacker](https://raylibtech.itch.io/rtexpacker) (raylib-tech's free tool). Output is the .png + a JSON we hand-translate into the C struct. Build-time tooling for "stb_rect_pack at compile time" is overkill for 5 chassis × 16 parts.

### Replacing draw_mech

The current `draw_mech` in `src/render.c` walks bones and draws thick lines. The M5 version walks **render parts** (a separate list per chassis, ordering: back leg, back arm, torso, front leg, front arm, head):

```c
// src/render.c — replaced
typedef struct MechRenderPart {
    int   part_a, part_b;       // PART_* indices for the bone segment
    int   sprite_idx;           // index into chassis's MechSpritePart[]
    float depth;                // for z-sort (back vs front leg)
} MechRenderPart;

static void draw_mech(const ParticlePool *p, const ConstraintPool *cp,
                      const Mech *m, const Level *L)
{
    const MechSpriteSet *set = mech_sprite_set(m->chassis_id);
    if (!set) {
        draw_mech_capsules(p, cp, m, L);   // M4 fallback
        return;
    }
    for (int i = 0; i < MECH_RENDER_PART_COUNT; ++i) {
        if (!bone_active_or_no_constraint(p, cp, ...)) continue;
        Vec2 a = particle_pos(p, m->particle_base + render_parts[i].part_a);
        Vec2 b = particle_pos(p, m->particle_base + render_parts[i].part_b);
        float angle = atan2f(b.y - a.y, b.x - a.x) * RAD2DEG;
        Vec2 mid = (Vec2){(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const MechSpritePart *sp = &set->parts[render_parts[i].sprite_idx];
        DrawTexturePro(set->atlas, sp->src,
                       (Rectangle){mid.x, mid.y, sp->draw_w, sp->draw_h},
                       (Vector2){sp->origin.x, sp->origin.y},
                       angle, m->is_dummy ? (Color){200,130,40,255} : WHITE);
    }
}
```

Capsule fallback (`draw_mech_capsules`) is the M4 path verbatim; we keep it as the no-asset/dev fallback so a fresh checkout without the atlases still renders.

### Dismemberment in the new path

When a limb's distance constraint is inactive (the mech has lost a limb), the `bone_active_or_no_constraint` check returns false for that limb's bones; the limb sprite continues to draw (its particles are still in the pool, still integrating, still bone-anchored to its own internal constraints) but the parent shoulder/hip plate's overlap region is no longer covering the limb's parent-side end. **Every limb sprite has a torn-edge "exposed end" pre-baked into its parent-side overlap region**, normally hidden by the parent plate; on dismemberment it becomes visible without a sprite swap. See [12-rigging-and-damage.md](12-rigging-and-damage.md) §"Exposed ends".

The **stump cap** sprite for the detached joint draws at the parent particle when the relevant `LIMB_*` bit is set in `m->dismember_mask`. Each chassis ships 5 stump caps in its atlas (shoulder L/R, hip L/R, neck). Hand-drawn; AI generators are bad at "torn-off mech-limb interior" — see [11-art-direction.md](11-art-direction.md) §"Pipeline 5 — Hand-drawn stump caps".

### Chassis-specific differences

Each chassis sprite set is generated via the ComfyUI pipeline ([11-art-direction.md](11-art-direction.md) §"Pipeline 1 — Mech chassis canonical T-pose") with chassis-specific style anchors and skeleton inputs:

- **Trooper**: balanced, medium-thick plates, dark blue-grey.
- **Scout**: thin, fast-looking plates, rounded edges, lighter colour.
- **Heavy**: thick block plates, warm orange-grey, jetpack mount visible.
- **Sniper**: medium plates, optic visor on head, slightly hunched torso.
- **Engineer**: medium plates, tool-belt strapping, utility vibe.

Players read silhouettes at a glance per the design pillar in [02-game-design.md](../02-game-design.md). Visible difference: Heavy's chest is ~30% larger than Trooper's; Scout's is ~30% smaller. Hitboxes are also scaled (already wired via `chassis.hitbox_scale`).

### Damage-feedback layers

The render path also handles three runtime layers on top of the static sprite assets — none of which require additional source art per part:

- **Hit-flash tint** — white-additive flash on the whole mech body for ~80–120 ms after a damage event lands. Cheap; ~1 byte per Mech, 1 branch per draw.
- **Persistent damage decals** — small dent/scorch decals projected onto each limb in sprite-local coordinates, rendered as overlay quads on top of each part. Per-limb ring of 16 decals; no per-mech RT.
- **Smoke from heavily damaged limbs** — particle emitter triggered when a limb's HP falls below 30% of max, rate proportional to damage intensity squared.

Specs: [12-rigging-and-damage.md](12-rigging-and-damage.md) §"Damage feedback — three layers".

## Tile sprites

The 32×32 tile draws are currently flat 2-tone rectangles. M5 replaces them with sprite atlases — one per kit:

```
assets/sprites/tiles_industrial.png   # 256×256 atlas, 8×8 grid of 32×32 tiles
assets/sprites/tiles_reactor.png      # same shape, different look
... (one per kit)
```

Each atlas is 64 sprites, indexed by `LvlTile.id`. The renderer picks the kit's atlas based on `LvlMeta`. Tile draws become:

```c
static void draw_level_tiles(const Level *L) {
    Texture2D atlas = level_tile_atlas(L);   // looked up via LvlMeta
    const float ts = (float)L->tile_size;
    for (int y = 0; y < L->height; ++y) {
        for (int x = 0; x < L->width; ++x) {
            const LvlTile *t = &L->tiles[y * L->width + x];
            if (!(t->flags & TILE_F_SOLID)) continue;
            int ax = (t->id % 8) * 32;
            int ay = (t->id / 8) * 32;
            DrawTexturePro(atlas,
                (Rectangle){(float)ax, (float)ay, 32.0f, 32.0f},
                (Rectangle){(float)x * ts, (float)y * ts, ts, ts},
                (Vector2){0, 0}, 0.0f, WHITE);
        }
    }
}
```

Edge tiles can use higher tile IDs to draw with adjacency-aware variants (e.g., a "floor with light dust" tile vs. "floor near a wall" tile). The editor's tile palette shows all 64 sprites per atlas; the designer paints intentionally.

## Free polygons (rendering)

Free polygons currently aren't drawn (the M4 build only has tiles). M5 renders them as flat-colored triangles per `kind`:

```c
static void draw_polys(const Level *L) {
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *p = &L->polys[i];
        if (p->kind == TILE_F_BACKGROUND) continue;     // skip; drawn elsewhere
        Color c;
        switch (p->kind) {
            case TILE_F_SOLID:   c = (Color){32, 38, 46, 255}; break;
            case TILE_F_ICE:     c = (Color){180, 220, 240, 255}; break;
            case TILE_F_DEADLY:  c = (Color){80, 200, 80, 255}; break;
            case TILE_F_ONE_WAY: c = (Color){80, 80, 100, 200}; break;
            default:             c = (Color){80, 80, 100, 255}; break;
        }
        Vector2 verts[3] = {
            {(float)p->v_x[0], (float)p->v_y[0]},
            {(float)p->v_x[1], (float)p->v_y[1]},
            {(float)p->v_x[2], (float)p->v_y[2]},
        };
        DrawTriangle(verts[0], verts[1], verts[2], c);
    }
}
```

For BACKGROUND polygons, render in a separate pass *after* the world-front-layer (foreground silhouettes that sit visually in front of mechs). They're `BLEND_ALPHA` mode, alpha 0.6, same color as the kit's near-bg layer.

## Decoration sprites (`LvlDeco`)

Decorations are atlas references: each `LvlDeco.sprite_str_idx` points at a path like `decals/pipe_horizontal.png`. They're atlased into one global decoration atlas (`assets/sprites/decorations.png`) at startup.

Render layers per `LvlDeco.layer`:

- `0`: far parallax (rendered before tiles)
- `1`: mid parallax (rendered before tiles)
- `2`: near parallax (rendered after tiles, before mechs)
- `3`: foreground (rendered after mechs and projectiles, with `BLEND_ALPHA`)

```c
static void draw_decorations(const Level *L, int layer) {
    for (int i = 0; i < L->deco_count; ++i) {
        const LvlDeco *d = &L->decos[i];
        if (d->layer != layer) continue;
        Texture2D tex = decoration_atlas;
        Rectangle src = decoration_src_rect_for(d->sprite_str_idx);
        Vector2 origin = {src.width * 0.5f, src.height * 0.5f};
        Color tint = (d->flags & DECO_FLIPPED_X) ? WHITE : WHITE;   // future: tint
        if (d->flags & DECO_ADDITIVE) BeginBlendMode(BLEND_ADDITIVE);
        DrawTexturePro(tex, src,
            (Rectangle){d->pos_x, d->pos_y,
                        src.width * d->scale_q / 32768.0f,
                        src.height * d->scale_q / 32768.0f},
            origin, d->rot_q * 360.0f / 256.0f, tint);
        if (d->flags & DECO_ADDITIVE) EndBlendMode();
    }
}
```

A 100-decoration map costs 100 DrawTexturePro calls per layer. raylib batches by texture, so all 100 collapse into a small number of GL draw calls when they share the atlas.

## 3-layer parallax

Per [06-rendering-audio.md](../06-rendering-audio.md) §"Backgrounds and parallax", we draw three parallax layers behind the world. M5 makes this real:

```c
typedef struct {
    Texture2D far_layer;     // sky/distant — 0.10× camera
    Texture2D mid_layer;     // mid-distance — 0.40× camera
    Texture2D near_layer;    // foreground silhouettes — 0.95× camera
    int       far_w, far_h;
    int       mid_w, mid_h;
    int       near_w, near_h;
} Parallax;

static void draw_parallax(const Parallax *px, Camera2D cam, int sw, int sh) {
    if (!px) return;
    // Far: ratio 0.10 — almost static.
    {
        float scroll = -cam.target.x * 0.10f;
        int   tile_w = px->far_w;
        int   start  = (int)floorf((scroll - sw / 2.0f) / (float)tile_w);
        int   stop   = (int)ceilf ((scroll + sw / 2.0f) / (float)tile_w);
        for (int i = start; i <= stop; ++i) {
            DrawTextureEx(px->far_layer,
                (Vector2){scroll - (float)i * tile_w, 0},
                0.0f, 1.0f, WHITE);
        }
    }
    // Mid: 0.40
    // Near: 0.95
    // (same shape with different ratios)
}
```

Each layer draws ≤2 textures per frame (wraparound). 3 layers × 2 = 6 draw calls per frame, batched. Trivial cost.

The ratios are pulled per-map from `LvlMeta` in [01-lvl-format.md](01-lvl-format.md). The M4 render loop's `ClearBackground` is replaced by `draw_parallax`.

### M6 P09 — atmosphere layer composes on top of the kit

The per-map kit above owns *static* art (parallax PNGs + tile atlas).
M6 P09 adds a second per-map source of truth that owns *runtime
atmosphere*: sky gradient, fog density, vignette, sun angle, weather
mode, theme palette. These compose cleanly with the kit:

1. **Sky gradient** (atmosphere) — painted first into the internal RT,
   replacing the hardcoded `{12,14,18}` clear.
2. **Parallax FAR / MID** (kit) — drawn over the gradient.
3. **Tiles + polys** — per-flag base color now sourced from the
   atmosphere theme palette (`g_themes[theme_id].tile_*`), so an
   ICE_SHEET map paints cool-blue ICE tiles + a RUST map paints
   warm-orange DEADLY tiles. Atlas-path multiplies the theme tint
   into `DrawTexturePro`'s colDiffuse; the no-atlas fallback uses
   the same theme tint as the checkerboard base color.
4. **Ambient zone overlays** (atmosphere) — drawn just before mechs.
5. **Decals / mechs / projectiles / FX** — unchanged.
6. **Parallax NEAR** (kit) — drawn after the world, before halftone.
7. **Halftone shader pass** (unchanged from P13) now also reads
   `fog_density`, `fog_color`, `fog_zones[16]` (AMBI_FOG volumes),
   and `vignette_strength` uniforms; each is short-circuited at
   `<= 0.001` so a zero-init map costs zero extra shader work.
8. **Weather particles** (atmosphere) — drawn at window resolution
   AFTER the internal-RT upscale blit, BEFORE the HUD. Screen-space
   `pos.x`/`pos.y` are stored as 0..1 normalized coords so they
   scale with window size without recompute on resize.

Wire format: per-map atmosphere fields live in `LvlMeta` (promoted
from `reserved[9]`). P08's `.lvl` distribution already replicates
them — no protocol bump.

Source: `src/atmosphere.{c,h}`, `documents/m6/09-editor-runtime-parity-and-atmospherics.md`.

## HUD final art

Replaces the `DrawRectangle` bars + line crosshair in `src/hud.c`.

### Asset list

```
assets/ui/hud.png             # 256×256 atlas — all HUD icons in one texture
  weapons/<id>.png            # 16×16 weapon icons (14 weapons)
  killfeed/<flag>.png         # 16×16 flag icons (HEADSHOT, GIB, etc.)
  ammo/full.png               # ammo dot
  health/heart_full.png       # heart icons
  jet/full.png                # jet fuel droplet
  pickup/<kind>.png           # 32×32 pickup icons (used in world too)
```

One atlas. Loaded once at startup.

### Bar style

```c
static void draw_bar_v2(int x, int y, int w, int h, float v, Color fg) {
    // Outline.
    DrawRectangleLines(x - 1, y - 1, w + 2, h + 2, (Color){0, 0, 0, 200});
    // Background fill.
    DrawRectangle(x, y, w, h, (Color){10, 12, 16, 200});
    // Foreground fill.
    int fill = (int)(v * (float)w);
    if (fill > 0) DrawRectangle(x, y, fill, h, fg);
    // Tick marks every 10% (for legibility at distance).
    for (int i = 1; i < 10; ++i) {
        DrawRectangle(x + (w * i / 10), y, 1, h, (Color){0, 0, 0, 100});
    }
}
```

1px outline, 1px shadow, flat fill, tick marks. No gradients (matches the "no textures" doc directive).

### Crosshair

```c
static void draw_crosshair_v2(Vec2 c, float bink_total) {
    // Atlas sub-rect for the crosshair sprite (16×16 transparent PNG with
    // four tick marks). Tinted by bink amount.
    float gap = 6.0f + bink_total * 12.0f;
    Color col = (bink_total > 0.5f)
              ? (Color){255, 80, 80, 255}    // red when very-shaky
              : (Color){200, 240, 255, 255}; // pale cyan otherwise
    DrawTexturePro(hud_atlas, crosshair_src,
        (Rectangle){c.x, c.y, 24.0f + gap, 24.0f + gap},
        (Vector2){12.0f + gap * 0.5f, 12.0f + gap * 0.5f},
        0.0f, col);
}
```

### Kill-feed icons

The kill-feed pulls weapon icons from the HUD atlas:

```c
// src/hud.c::draw_kill_feed — replace text-based weapon name with icon
static void draw_kill_feed_v2(...) {
    for (each kill):
        draw_player_chip_color(killer);
        draw_weapon_icon(weapon_id);     // atlas sub-rect
        draw_player_chip_color(victim);
        for each flag in flags:
            draw_flag_icon(flag);        // HEADSHOT / GIB / etc.
}
```

Same pulse-fade behavior as M4; just better visual density.

## TTF font

Currently `GetFontDefault()` + bilinear filter (M4 trade-off). M5 vendors **Atkinson Hyperlegible** at `assets/fonts/Atkinson-Hyperlegible-Regular.ttf` (~150 KB, OFL license, https://www.brailleinstitute.org/freefont/).

```c
// src/platform.c — at startup
Font g_ui_font;
g_ui_font = LoadFontEx("assets/fonts/Atkinson-Hyperlegible-Regular.ttf", 32, NULL, 0);
SetTextureFilter(g_ui_font.texture, TEXTURE_FILTER_BILINEAR);
```

`ui_draw_text` switches from `GetFontDefault()` to `g_ui_font`:

```c
// src/ui.c::ui_draw_text — replace
DrawTextEx(g_ui_font, text, (Vector2){x, y}, sz, sz / 10.0f, col);
```

One file, ~150 KB, MIT-licensed equivalent (OFL is not strictly MIT but is comparably permissive — public-domain-equivalent for distribution).

Why Atkinson Hyperlegible: free, designed for high-contrast legibility, looks great on the dark backgrounds we use, ships in a single regular weight that handles all our use cases (no need for bold or italic at v1).

Alternatives considered: Inter (Google Fonts, 200 KB), JetBrains Mono (~400 KB), monospace TTF — Atkinson is smaller and reads better at HUD sizes.

Resolves the M4 trade-off "Default raylib font (no vendored TTF)."

## Decal-layer chunking

The current `decal_init` in `src/decal.c` allocates one RT the size of the whole level. For Citadel (6400×3200), that's ~80 MB. **Over budget.**

M5 chunks the decal layer when the level exceeds 4096 px in either dimension:

```c
// src/decal.c — modified
#define DECAL_CHUNK_SIZE 1024

typedef struct {
    int x_chunks, y_chunks;
    RenderTexture2D *chunks;       // x_chunks * y_chunks
    bool *chunk_dirty;             // for selective flush
} DecalLayer;

static DecalLayer g_layer;

void decal_init(int level_w, int level_h) {
    if (level_w <= 4096 && level_h <= 4096) {
        // Old behavior: single RT.
        g_layer.x_chunks = g_layer.y_chunks = 1;
        g_layer.chunks = &single_rt;
        g_layer.chunks[0] = LoadRenderTexture(level_w, level_h);
        return;
    }
    g_layer.x_chunks = (level_w + DECAL_CHUNK_SIZE - 1) / DECAL_CHUNK_SIZE;
    g_layer.y_chunks = (level_h + DECAL_CHUNK_SIZE - 1) / DECAL_CHUNK_SIZE;
    g_layer.chunks = arena_alloc(/*level_arena*/, ...);
    for (int y = 0; y < g_layer.y_chunks; ++y) {
        for (int x = 0; x < g_layer.x_chunks; ++x) {
            g_layer.chunks[y * g_layer.x_chunks + x] =
                LoadRenderTexture(DECAL_CHUNK_SIZE, DECAL_CHUNK_SIZE);
        }
    }
}

void decal_paint_blood(Vec2 pos, float radius) {
    // Compute which chunk(s) the splat overlaps.
    int cx0 = (int)((pos.x - radius) / DECAL_CHUNK_SIZE);
    int cy0 = (int)((pos.y - radius) / DECAL_CHUNK_SIZE);
    int cx1 = (int)((pos.x + radius) / DECAL_CHUNK_SIZE);
    int cy1 = (int)((pos.y + radius) / DECAL_CHUNK_SIZE);
    // Add to pending in those chunks.
    for (int cy = cy0; cy <= cy1; ++cy) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            // Translate pos into chunk-local coords.
            ...
            queue_pending_in_chunk(cx, cy, local_pos, radius);
        }
    }
}
```

For a Citadel-sized level (200×100 tiles = 6400×3200 px): 7×4 = 28 chunks of 1024² RGBA8 = 4 MB each = 112 MB. Still over budget. So we additionally **lazy-allocate** chunks on first paint into them:

```c
static void ensure_chunk_allocated(int cx, int cy) {
    if (g_layer.chunks[cy * g_layer.x_chunks + cx].id != 0) return;
    g_layer.chunks[cy * g_layer.x_chunks + cx] =
        LoadRenderTexture(DECAL_CHUNK_SIZE, DECAL_CHUNK_SIZE);
}
```

Most matches stain only ~10–20 chunks (the high-traffic zones); lazy alloc keeps memory at ~80 MB worst-case, comfortably below the 80 MB texture budget. Documented as a soft cap.

## Hot reload of assets

In dev builds (`#ifdef DEV_BUILD`), a tiny mtime watcher polls every 250 ms across:

- All `.png` files in `assets/sprites/`, `assets/parallax/`, `assets/ui/`.
- All `.lvl` files in `assets/maps/`.
- All `.wav` and `.ogg` files in `assets/sfx/`, `assets/music/`.
- The font file.

```c
// src/hotreload.c — new module
typedef struct {
    char     path[256];
    time_t   mtime;
    void   (*reload_cb)(const char *path);
    bool     enabled;
} HotReloadEntry;

void hotreload_register(const char *path, void (*cb)(const char *));
void hotreload_poll(void);    // called once per frame
```

When a file changes, the matching reload callback fires:

- Texture reload: `UnloadTexture` + `LoadTexture`. raylib hands back the same `Texture2D` slot, but the underlying GL texture is fresh; references in atlases stay valid.
- Sound reload: `UnloadSound` + `LoadSound` for each alias.
- Level reload: `level_load` rebuilds `world.level`. If the round is active, push to clients via `LEVEL_RELOAD` event (host-only debug feature, see [02-level-editor.md](02-level-editor.md)).
- Music reload: `UnloadMusicStream` + `LoadMusicStream`; restart playback.
- Font reload: `UnloadFont` + `LoadFontEx`. Atlas re-baked.

Disabled in release builds. Gated by `g.hot_reload_enabled` for the F-key toggle in dev.

## Render order

The full draw call sequence per frame, in M5:

```c
// src/render.c::renderer_draw_frame — M5 shape
BeginDrawing();
    ClearBackground((Color){12, 14, 18, 255});

    // Parallax (3 layers): drawn under everything else, no Camera2D
    // (parallax does its own camera math).
    draw_parallax_far(...);
    draw_parallax_mid(...);

    BeginMode2D(camera);
        // Decoration layer 0 (far parallax) — already drawn above
        draw_decorations(L, /*layer*/0);

        // World tiles (sprite atlas)
        draw_level_tiles(L);

        // Free polygons (filled triangles)
        draw_polys(L);

        // Decoration layer 1 (mid)
        draw_decorations(L, /*layer*/1);

        // Decal splat layer
        decal_draw_layer();

        // Pickups
        draw_pickups(w);

        // Mechs (corpses first, then alive)
        draw_corpses(w);
        draw_mechs_alive(w);

        // Decoration layer 2 (near)
        draw_decorations(L, /*layer*/2);

        // Projectiles
        projectile_draw(w);

        // FX particles
        fx_draw(w);

        // Decoration layer 3 (foreground silhouettes, alpha-blend)
        draw_decorations(L, /*layer*/3);

        // CTF flags
        if (match.mode == MATCH_MODE_CTF) draw_flags(w);
    EndMode2D();

    // Parallax near (over the world, behind HUD)
    draw_parallax_near(...);   // optional; 0.95× camera, very subtle

    // HUD (screen space)
    hud_draw(w, sw, sh, cursor);
    if (overlay_cb) overlay_cb(...);
EndDrawing();
```

The parallax-near pass is optional — most maps don't use it (it's the layer that obscures sightlines). It defaults off in `LvlMeta`.

## Render-side interpolation alpha

The M4 build calls `simulate()` once per render frame; on a vsync-fast display (small windows, fullscreen on a 144 Hz monitor) sim runs faster than 60 Hz and per-tick caps don't scale ([TRADE_OFFS.md](../../TRADE_OFFS.md) entries `60 Hz simulation, not 120 Hz`, `No render-to-sim interpolation alpha`, `Vsync / frame-pacing leak`). With M5's damage-feedback layers (hit-flash tint at 80–120 ms, smoke from damaged limbs) and the new slope physics, this gap is visible. **A hit-flash that pops because of a snap-on-snapshot reads as a glitch, not a hit.**

M5 lands the canonical Glenn Fiedler accumulator + render-side alpha. See [13-controls-and-residuals.md](13-controls-and-residuals.md) §"Render-side interpolation alpha + fixed-step accumulator" for the full design rationale and the decision to leave the 60-vs-120 Hz tuning open.

### Per-particle previous-frame snapshot

```c
// src/world.h — added to ParticlePool
float *render_prev_x;     // pos_x at the start of the most-recent simulate tick
float *render_prev_y;     // (NOT to be confused with prev_x/prev_y which Verlet uses)
```

Names matter: `prev_x/prev_y` is the **Verlet** previous position (`pos - prev = velocity`); `render_prev_x/_y` is the **render snapshot** previous position (the value `pos_x` had at the start of the current simulate tick). They're decoupled — the Verlet `prev` updates inside the integrator; the render snapshot updates once per simulate tick.

```c
// src/simulate.c::simulate_step — at the top, before any work
for (int i = 0; i < w->particles.count; ++i) {
    w->particles.render_prev_x[i] = w->particles.pos_x[i];
    w->particles.render_prev_y[i] = w->particles.pos_y[i];
}
/* ... rest of simulate body unchanged ... */
```

One memcpy-equivalent per simulate tick. ~30 µs for 4096 particles. Inside slack.

### Renderer reads alpha + lerps

```c
// src/render.c — every particle read becomes a lerp
static inline Vec2 particle_render_pos(const ParticlePool *p, int i, float alpha) {
    return (Vec2){
        p->render_prev_x[i] + (p->pos_x[i] - p->render_prev_x[i]) * alpha,
        p->render_prev_y[i] + (p->pos_y[i] - p->render_prev_y[i]) * alpha,
    };
}
```

Every reader of `particle_pos` in `draw_mech`, `draw_held_weapon`, `projectile_draw`, `fx_draw` switches to `particle_render_pos(p, i, alpha)`. Projectiles, FX, and pickup positions get analogous prev-frame snapshots.

### Performance

- Per-tick: one snapshot pass (~30 µs).
- Per-frame: one cheap lerp per particle read. ~5000 reads × negligible cost = ~50 µs total. Inside slack.
- Memory: 4096 particles × 2 × 4 bytes = 32 KB extra in the particle pool.

### What this fixes

- Vsync-fast play (144 Hz display) no longer accelerates physics; sim is locked to 60 Hz regardless of render rate.
- Hit-flash tint and damage decals appear smoothly even when the renderer outpaces the simulator.
- The 120 Hz toggle becomes a one-line `TICK_DT` change once the accumulator is in place — actual rate decision deferred to playtest.

This resolves three [TRADE_OFFS.md](../../TRADE_OFFS.md) entries: `60 Hz simulation, not 120 Hz` (partial — accumulator in place; the *rate* stays 60), `No render-to-sim interpolation alpha`, `Vsync / frame-pacing leak`.

## Performance check

The added draw calls per frame:

- Parallax: 6 textured-quad draws (3 layers × 2 wraparound)
- Tiles: ~1500 visible at typical zoom; raylib batches into ~1 GL call.
- Polys: ~50 triangles per map; ~1 GL call (DrawTriangle uses raylib's batch).
- Decorations: ~100 per map, atlased; ~1 GL call.
- Mechs: 32 mechs × ~14 sprite parts = ~450 calls; raylib batches into ~5–10 GL calls.
- Pickups, projectiles, FX: as M4.
- HUD: ~30 atlas-based draws; ~1 GL call.

Total: ~10–20 GL draw calls per frame. Well within the 8 ms render budget on integrated GPUs.

The CPU cost is more interesting: 450 mech sprite calls × atlas binding is expensive without batching. We rely on raylib's CPU-side batch — it tracks the active texture and submits a single draw when the texture changes. Sequencing parts in the right order (per-chassis, in render order) keeps batches large.

## Asset budget

| Category | Size | Notes |
|---|---|---|
| 5 chassis atlases × 1024² RGBA8 | 20 MB | Mostly transparent; PNG compresses to ~2 MB each on disk |
| 8 tile atlases × 256² RGBA8 | 2 MB | One per kit |
| 1 decoration atlas × 2048² RGBA8 | 16 MB | Shared across all maps |
| 1 HUD atlas × 256² RGBA8 | 256 KB | Static |
| 24 parallax layers (8 maps × 3) × 1024×512 RGBA8 | 48 MB | Big — main asset budget chunk |
| Pickup atlas × 256² RGBA8 | 256 KB | |
| Particle atlas (existing) | 256 KB | |
| 1 TTF font | 150 KB | |
| 8 map thumbnails × 256×144 | 400 KB | |
| **Total textures in RAM** | **~88 MB** | Slightly over the 80 MB target — see below |
| **Total textures on disk (PNG-compressed)** | **~30 MB** | |

The 88 MB GPU textures slightly overshoots [10-performance-budget.md](../10-performance-budget.md)'s 80 MB target. Mitigation:

1. **Parallax layers shared across maps.** 8 visually distinct maps, but only 4 distinct parallax sets — each set is reused across 2 maps with palette tints. Drops parallax memory from 48 to 24 MB.
2. **Foreground decorations atlased into the same 2048² as everything decorative.** Already there.

Final budget: ~64 MB textures. Inside target.

## Done when

- The capsule-line mech rendering is gone. Fresh checkout with assets shipped → mechs draw as sprites.
- Each of the 8 maps loads its kit-specific tile atlas + parallax + decorations.
- HUD bars + kill-feed icons + crosshair use the HUD atlas.
- TTF font replaces `GetFontDefault()` everywhere; the M4 trade-off entry is deleted.
- Decal layer chunks correctly for Citadel-sized maps; memory under the budget.
- Hot reload of textures + level data works in dev builds.

## Trade-offs to log

- **Mech atlases hand-packed via rTexPacker, not at build time.** Documented above.
- **Parallax shared across map pairs** — we ship 4 sets, not 8. Two maps will look visually similar enough to be identifiable as a pair.
- **No animated sprites** in v1 (mech sprites are static; animation is via bone position). Animated decals (rotating warning lights, pulsing fire) are a v1.5 polish item.
- **Crosshair tints rather than swaps sprite at very-shaky.** Cheaper than two separate atlas entries.
- **No skeletal mesh deformation** — single bone per sprite, rigid skinning. Preserved from the canon.
- **Lazy-allocated decal chunks.** Small soft-cap; documented above.
