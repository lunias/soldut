# P13 — Parallax + tile sprites + HUD final art + TTF font + halftone post + decal chunking

## What this prompt does

Lands the rest of the rendering kit not covered elsewhere:

1. 3-layer parallax background per map (sky/mid/near).
2. Tile sprites — replace M4's flat 2-tone rectangles with kit-specific tile atlases.
3. Free polygon rendering (filled triangles per `kind`).
4. Decoration sprites (`LvlDeco` records).
5. HUD final art — bars, kill-feed icons, weapon icons, crosshair via the HUD atlas.
6. TTF font (Atkinson Hyperlegible + VG5000 + Steps Mono) replacing `GetFontDefault()`.
7. Halftone post-process fragment shader.
8. Decal-layer chunking for big maps (>4096 px).
9. Asset hot reload (dev builds).

Depends on P10 (chassis atlas runtime).

## Required reading

1. `CLAUDE.md`
2. `documents/06-rendering-audio.md`
3. **`documents/m5/08-rendering.md`** — the spec
4. `documents/m5/11-art-direction.md` §"The five hard constraints", §"The halftone post-process shader", §"Per-map kit assets"
5. `documents/m5/01-lvl-format.md` §"DECO", §"META"
6. `src/render.c` — current `draw_level`, `renderer_draw_frame`
7. `src/decal.{c,h}` — current single-RT splat layer
8. `src/ui.c` — text rendering via `GetFontDefault()`
9. `src/hud.c` — current HUD draws

## Concrete tasks

### Task 1 — TTF font

Vendor 3 fonts under `assets/fonts/`:
- `Atkinson-Hyperlegible-Regular.ttf` (~150 KB, OFL)
- `VG5000-Regular.ttf` (Velvetyne, OFL)
- `Steps-Mono-Thin.ttf` (Velvetyne, OFL)

In `src/platform.c::platform_init`, after window init:

```c
g_ui_font_body    = LoadFontEx("assets/fonts/Atkinson-Hyperlegible-Regular.ttf", 32, NULL, 0);
g_ui_font_display = LoadFontEx("assets/fonts/VG5000-Regular.ttf", 48, NULL, 0);
g_ui_font_mono    = LoadFontEx("assets/fonts/Steps-Mono-Thin.ttf", 32, NULL, 0);
SetTextureFilter(g_ui_font_body.texture, TEXTURE_FILTER_BILINEAR);
SetTextureFilter(g_ui_font_display.texture, TEXTURE_FILTER_BILINEAR);
SetTextureFilter(g_ui_font_mono.texture, TEXTURE_FILTER_BILINEAR);
```

Replace `GetFontDefault()` calls in `src/ui.c::ui_draw_text` with the appropriate font. For now, default to body; ui code can opt in to display/mono via a font enum if needed.

### Task 2 — Halftone post shader

Per `documents/m5/11-art-direction.md` §"The halftone post-process shader":

Write `assets/shaders/halftone_post.fs.glsl`. ~40 LOC of GLSL 330. Bayer 8×8 dither + halftone screen at ~30% density.

Wire into `src/render.c::renderer_draw_frame`'s post-process slot:

```c
BeginShaderMode(g_halftone_post);
DrawTextureRec(framebuffer.texture, ...);
EndShaderMode();
```

The framebuffer is the existing post-process render target. If the codebase doesn't have one yet, add it.

### Task 3 — 3-layer parallax

Per `documents/m5/08-rendering.md` §"3-layer parallax":

Add `Parallax` struct to render state:

```c
typedef struct {
    Texture2D far_layer, mid_layer, near_layer;
} Parallax;
```

Load from `assets/maps/<short>/parallax_far.png` etc. when a map loads (via `map_build` after `level_load`).

In `renderer_draw_frame`, before `BeginMode2D`:

```c
draw_parallax_layer(camera, far_layer,  0.10f);   // ratio 0.10 — almost static
draw_parallax_layer(camera, mid_layer,  0.40f);
// near is drawn after mechs (foreground) at 0.95f
```

Each layer drawn twice (wraparound). 6 draw calls total.

### Task 4 — Tile sprites per kit

Per `documents/m5/08-rendering.md` §"Tile sprites":

Add `Texture2D tile_atlas` to Level (or a per-map-kit cache). Load from `assets/maps/<short>/tiles.png` at level load.

Replace `draw_level` with `draw_level_tiles` that DrawTextureRec's the tile sub-rect per `LvlTile.id`.

### Task 5 — Free polygon rendering

Per `documents/m5/08-rendering.md` §"Free polygons":

`draw_polys` walks `level->polys` and `DrawTriangle`s each. Color by `kind` per the doc. BACKGROUND-kind polygons drawn in a separate pass after mechs.

### Task 6 — Decoration sprites

Per `documents/m5/08-rendering.md` §"Decoration sprites":

`draw_decorations(L, layer)` walks `level->decos`, renders sprites at `(pos_x, pos_y)` with `scale_q`, `rot_q`, layer-based draw order. ADDITIVE flag uses `BeginBlendMode(BLEND_ADDITIVE)`.

Decoration atlas: `assets/sprites/decorations.png` (1024×1024, all decorations across all maps share one atlas).

### Task 7 — HUD final art

Per `documents/m5/08-rendering.md` §"HUD final art":

HUD atlas: `assets/ui/hud.png` (256×256).

Replace `DrawRectangle`-based bars in `hud.c` with the atlas-based `draw_bar_v2` per the spec. Replace text-only kill feed entries with weapon-icon variants.

Crosshair: small atlas sprite, tinted by bink (`tint = bink > 0.5 ? red : pale_cyan`).

### Task 8 — Decal layer chunking

Per `documents/m5/08-rendering.md` §"Decal-layer chunking":

When level dimensions > 4096 px, chunk the decal RT into 1024×1024 tiles. Lazy-allocate chunks on first paint.

```c
typedef struct {
    int x_chunks, y_chunks;
    RenderTexture2D *chunks;
    bool *chunk_dirty;
} DecalLayer;
```

`decal_paint_blood` computes which chunk(s) the splat overlaps and queues the paint per-chunk. `decal_flush_pending` flushes per-chunk.

### Task 9 — Hot reload

Per `documents/m5/08-rendering.md` §"Hot reload of assets":

Dev-builds-only mtime watcher polling every 250 ms across:
- `assets/sprites/*.png`
- `assets/parallax/*.png`
- `assets/ui/*.png`
- `assets/maps/*.lvl`
- `assets/sfx/*.wav`, `assets/music/*.ogg`
- `assets/fonts/*.ttf`

`src/hotreload.{c,h}` (new module). On change: unload old + load new.

Disabled in release builds (`#ifdef DEV_BUILD`).

## Done when

- `make` builds clean.
- Game launches with the new fonts (text looks sharper than M4's bilinear-default).
- Parallax background visible on a map (with placeholder PNGs if real art not ready).
- Tile sprites render (with placeholder atlas if not ready).
- Free polygons render in their flat colors.
- HUD bars use atlas sprites.
- Halftone post applies; visible at 30% density.
- Citadel-sized map's decal layer chunks correctly; memory under 80 MB.
- Hot-reloading a `.png` while the game runs picks up changes within 250 ms (dev build).

## Out of scope

- Atlas content — P15/P16 generates.
- Audio hot reload (handled here for files but the audio module is P14).
- Menu / lobby screen art passes (M6 polish).

## How to verify

```bash
make
./soldut
# Check fonts, halftone post, tile/parallax/decoration draws.
```

## Close-out

1. Update `CURRENT_STATE.md`: rendering kit complete.
2. Update `TRADE_OFFS.md`:
   - **Delete** "Default raylib font (no vendored TTF)".
3. Don't commit unless explicitly asked.

## Common pitfalls

- **Font loading order**: `LoadFontEx` must be called after `InitWindow`. Check `platform_init` ordering.
- **Halftone shader uniforms**: pass `resolution` + `halftone_density`; don't hard-code.
- **Parallax wraparound math**: drawing the layer at `(scroll, 0)` and `(scroll + width, 0)` works only if `scroll` is in the right sign convention. Test with the camera moving both directions.
- **Decal-layer chunking on M4-sized maps**: don't chunk for maps under 4096 px; the per-tile dispatch is overhead.
- **Hot reload of textures**: raylib's `LoadTexture` returns a fresh GL ID; existing references to the old ID become stale. Update via the in-place pointer pattern in `documents/m5/08-rendering.md`.
- **Decoration atlas rotation pivot**: rotation around centroid is what looks right; not top-left.
