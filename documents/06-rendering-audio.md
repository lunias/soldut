# 06 — Rendering & Audio

This document specifies how the game looks and sounds — and what we do directly with raylib vs. what we layer on top. raylib handles a lot for free; we just have to use it correctly.

## raylib version we use

We vendor **raylib 5.5** (already present at `/home/lunias/clones/asset/third_party/raylib`), built as a static lib (`libraylib.a`) per target platform. We compile against it as a C library; we do not pull in the C++ wrappers or the higher-level "raylib_extras."

Public API surface we rely on: `rcore` (window, input, time), `rlgl` (low-level batch + shader), `rshapes` (lines, triangles, polygons), `rtextures` (Image / Texture2D), `rtext` (TTF + atlases), `raudio` (miniaudio wrapper).

We do **not** use `rmodels` (3D). It compiles in but we don't call it.

## The render loop

```c
BeginDrawing();                          // clears framebuffer
    BeginTextureMode(splat_layer);       // RT for permanent decals (drawn rarely)
        // (only when a new decal is being baked in)
    EndTextureMode();

    BeginMode2D(camera);                 // world-space camera
        draw_background_parallax();      // 3 layers
        draw_decals();                   // splat layer composited
        draw_world_geometry();           // map polygons + tile sprites
        draw_corpses();                  // dead mechs (sleeping or settling)
        draw_mechs_alive();              // live mechs
        draw_projectiles();              // bullets, grenades
        draw_particles();                // blood, sparks, smoke
        draw_pickups();
    EndMode2D();

    BeginShaderMode(post_fx);            // optional post: vignette, scanline, hit-pulse
        DrawTexturePro(framebuffer, ...);
    EndShaderMode();

    draw_hud();                          // screen-space, no camera
    draw_chat();
    draw_kill_feed();
EndDrawing();
```

raylib's begin/end pairing is **strict** and **stack-like**. Forgetting an `End*` results in a black screen and no GL error. We wrap each pair in scoped helpers in dev builds with assertions.

## Camera

`Camera2D { offset, target, rotation, zoom }`. Per frame:

```c
camera.target = lerp(camera.target, focus_point, 0.18f);    // smoothed follow
camera.offset = (Vector2){ screen_w * 0.5f, screen_h * 0.5f };
camera.zoom   = match_zoom + recoil_zoom_kick + jet_pullback;
camera.rotation = screen_shake_angle;
```

`focus_point` is a weighted blend of: local mech position (1.0), aim direction (0.3 lookahead), nearest enemy (0.05). Smoothing prevents jittery follow.

Screen shake is a small low-frequency sine driven by an "intensity" scalar that decays each frame. Hits, explosions, and big-weapon firing add to intensity.

## Drawing the mechs

Each mech is **N polygon parts** (~12–16 per mech), each rendered as a sprite or an n-gon, **rotated and translated** to align with its bone in the skeleton:

```c
void draw_part(MechPart *p, Particle *head, Particle *tail) {
    Vector2 mid = { (head->pos.x + tail->pos.x) * 0.5f,
                    (head->pos.y + tail->pos.y) * 0.5f };
    float angle = atan2f(tail->pos.y - head->pos.y,
                         tail->pos.x - head->pos.x) * RAD2DEG;
    DrawTexturePro(p->sprite,
                   (Rectangle){0,0, p->sprite.width, p->sprite.height},
                   (Rectangle){mid.x, mid.y, p->draw_w, p->draw_h},
                   p->origin,        // origin offset along the bone
                   angle,
                   p->tint);
}
```

For the **polygon plates** (mechs explicitly look polygonal) we render either:
- A baked sprite of the polygon (one per chassis, pre-baked at load).
- Or, for small parts and live polygons during dismemberment, `rlBegin(RL_TRIANGLES)` + `rlVertex2f` directly with a flat color.

The flat-shaded polygon look comes from how the sprites are drawn (no normal mapping, no texture detail, hand-drawn line-work seams). We do not do per-pixel lighting on mechs.

## Draw order

Within a mech, draw order matters (back leg, back arm, body, front leg, front arm, head). We pre-sort parts at chassis-init time and use that ordering. Z-fighting is impossible (we're 2D), but visual layering must be deliberate.

Across mechs: dead bodies first (they're "behind" the action), then alive mechs sorted by Y position (lower Y draws first for depth illusion).

## Particles

We render thousands of particles per frame in heavy combat (blood, sparks, smoke, casings). All go through one batched draw:

```c
void draw_particles(ParticlePool *pool) {
    rlSetTexture(particle_atlas.id);
    rlBegin(RL_QUADS);
    for (int i = 0; i < pool->count; ++i) {
        // emit 4 vertices per particle
        // ...
    }
    rlEnd();
}
```

Single texture atlas (`assets/particles.png`, 256×256 max) holds blood, sparks, smoke, casings, muzzle-flash sprites. One texture switch + one batched draw = ~thousands of particles per millisecond.

For **additive** particles (muzzle flash, plasma trail, explosion spark) we end the alpha-blend batch, switch to additive, draw, then switch back. Two state changes per frame.

```c
BeginBlendMode(BLEND_ADDITIVE);
    draw_additive_particles();
EndBlendMode();
```

## Decals — the splat layer

Permanent blood, scorch, and impact marks live on a render-to-texture layer the size of the level (or chunked into 1024×1024 tiles for very large maps).

```c
RenderTexture2D splat_layer = LoadRenderTexture(level_w, level_h);
// once at level load, optional: blit current layer state if loading from save

// when a decal lands:
BeginTextureMode(splat_layer);
    DrawTexture(decal_tex, decal_pos.x, decal_pos.y, fade_color);
EndTextureMode();
```

The decal sprite is rendered into the splat layer **once** and forgotten. Drawing the splat layer back into the world is a single textured quad per visible chunk per frame. This is how we get thousands of decals at zero per-frame cost.

**Memory**: a 4096×4096 RGBA8 layer is 64 MB. For larger maps we chunk to 1024×1024 (4 MB each) and only allocate chunks that actually receive decals.

**Y-flip gotcha**: raylib's render textures come out Y-flipped. Standard pattern when drawing them back:

```c
DrawTextureRec(splat_layer.texture,
               (Rectangle){0, 0, splat_layer.texture.width, -splat_layer.texture.height},
               (Vector2){0, 0}, WHITE);
```

## Backgrounds and parallax

Three parallax layers behind the world:

| Layer | Speed factor | Content |
|---|---|---|
| Far | 0.15× camera | Sky, distant skyline, weather |
| Mid | 0.40× camera | Buildings, hills |
| Near | 0.85× camera | Foreground silhouettes |

Each layer is one large image (or a tile strip) drawn translated by `camera.target * factor`. Three sprite draws per frame total. Trivial cost.

## Lighting

We do **no dynamic lighting** at v1. Lighting is **baked** into the level art. Muzzle flashes, explosions, and impact sparks are just bright additive sprites — they read as light without simulating it.

If we add real lighting later, the candidate is **2D shadow casting** via raycasts from light sources to nearby polygon edges, accumulated into an additive light buffer. For a 2D shooter this is well-trodden but not free; we don't build it in v1.

## Shaders

Default raylib shader for almost everything. Custom shaders for:

1. **Post-process fragment shader** — vignette, slight color grading, subtle scanline, momentary "hit pulse" when local mech is shot.
2. **Plasma weapons** — color/UV scroll on the projectile sprite for that distinctive shimmer.
3. **Mech outline** when locally damaged — red rim shader for 100ms after taking a hit.

Shaders are GLSL 330 (matching raylib's default desktop GL backend). On macOS we are still on OpenGL via GLFW — Apple-deprecated but functional, capped at GL 4.1. We stay at GL 3.3-level features. There is no Metal backend.

## HUD

Drawn in screen space (after `EndMode2D`). Elements:

- Health/armor bar (bottom-left)
- Ammo + weapon (bottom-right)
- Jet fuel gauge (left edge, vertical)
- Crosshair (center, scales with bink/recoil)
- Kill feed (top-right)
- Mini-map (top-left, optional)
- Score / timer (top-center)
- Chat (lower-right)

We use `raygui.h` (already in our `third_party/`) for menus and lobby. raygui is single-header, depends only on raylib, gives us buttons, sliders, lists, text inputs. No custom UI framework — we let raygui handle the lobby and stop there.

## Frame pacing

```c
SetTargetFPS(0);                // we manage our own pacing
SetConfigFlags(FLAG_VSYNC_HINT); // vsync if available
```

We free-run rendering and let vsync gate. The simulation runs at fixed 60 Hz inside an accumulator loop:

```c
double accum = 0;
double last = GetTime();
while (!WindowShouldClose()) {
    double now = GetTime();
    double dt = now - last;
    last = now;
    accum += dt;
    while (accum >= TICK_DT) {
        sample_input();
        simulate(&world, &input, TICK_DT);
        accum -= TICK_DT;
    }
    float alpha = (float)(accum / TICK_DT);
    render(&world, alpha);   // interpolate between previous and current state
}
```

`alpha` is the interpolation factor between the previous and current simulated frames; render uses it to lerp every visible body so motion is smooth even at non-multiple frame rates. This is Glenn Fiedler's *Fix Your Timestep!* loop, and it's the right shape for a fixed-step network sim.

## Audio

raylib wraps **miniaudio**. Two abstractions we use:

- `Sound` — decoded into RAM, cheap to play repeatedly. Used for SFX (gunshots, footsteps, hits, explosions). Load once, play many.
- `Music` — streamed from disk. Used for the (optional) background score.

`AudioStream` is available for raw PCM synthesis; we don't use it.

### Sample design

Each gun has 2–4 firing variants (slight pitch/timbre changes) chosen randomly to avoid the "machine-gun loop" effect. Hit sounds vary by surface (metal vs concrete vs flesh).

### Mixing

We compute pan and volume per-frame from `(listener - source)` and call `SetSoundVolume` / `SetSoundPan`. raylib does NOT spatialize automatically (we have `MA_NO_ENGINE` set in raylib's miniaudio config), so this is on us. Simple model:

```c
float dx = source.x - listener.x;
float dy = source.y - listener.y;
float dist = sqrtf(dx*dx + dy*dy);
float pan = clampf(dx / 800.0f, -1.0f, 1.0f);     // full pan past 800 px
float vol = clampf(1.0f - dist / 1500.0f, 0.0f, 1.0f); // silence past 1500 px
SetSoundPan(s, pan);
SetSoundVolume(s, vol * source.priority);
```

For ducking: when a *very* loud event fires (Mass Driver), we briefly attenuate other sounds by 6 dB for 300 ms. Implemented as a global volume scalar on the music + ambient channel.

### Sound aliases

`LoadSoundAlias` lets multiple instances of one sample play simultaneously without copying the wave data. Critical for gunshots — we load the Pulse Rifle sample once, alias it 8 times, and play overlapping shots from many sources.

```c
Sound base = LoadSound("assets/sfx/pulse_rifle.wav");
for (int i = 0; i < 8; ++i) pulse_rifle_aliases[i] = LoadSoundAlias(base);
```

### Audio thread

miniaudio runs the actual mix on its own thread. **Don't call raylib draw or load functions from inside `SetAudioStreamCallback`** — that thread doesn't own the GL context. We don't use `SetAudioStreamCallback` for the game; raylib's mix callback handles everything.

## Asset loading

At startup:

1. Read `assets/manifest.txt` — flat list of every asset path + type.
2. For each entry, load the appropriate raylib resource (`LoadTexture`, `LoadFontEx`, `LoadSound`).
3. Store handles in a tightly-packed array indexed by stable string IDs hashed at build time.

Total startup load: target **<2 seconds** on a spinning HDD. Asset count target: **<200 files** at v1 (textures + sounds + fonts).

Hot reload: a tiny file watcher (`src/hotreload.c`) polls mtimes every 250 ms in dev builds. When a watched asset changes, we:

1. Free the old raylib resource.
2. Reload from disk.
3. Patch the handle in place.

Hot reload of **textures** and **sounds** works seamlessly. Hot reload of **fonts** works but you lose any cached glyph atlas state. Hot reload of **shaders** works in raylib (`UnloadShader` + `LoadShader`).

## Memory budgets

| Category | Target | Notes |
|---|---|---|
| Textures | 80 MB | Mech sprites, particles, UI, parallax. Mostly RGBA8. |
| Splat layer | 16 MB | Decal RT chunks |
| Audio | 30 MB | Sound effects in RAM (most are <100 KB each); music streamed |
| Fonts | 4 MB | One font, 3 sizes, atlased |
| World state | 8 MB | Particles, mechs, projectiles, decals |
| Network buffers | 4 MB | ENet + our own queues |
| Misc | 14 MB | Strings, raygui state, etc. |
| **Total** | **156 MB** | Comfortable on 512 MB systems |

Hard cap at startup: **256 MB**. If we exceed, we fail loudly with a memory report.

## What we are NOT doing

- **No 3D rendering.** rmodels stays unused. `Camera3D` stays unused.
- **No skeletal mesh deformation.** Single-bone-per-polygon. Rigid skinning is the contract.
- **No dynamic lighting / shadows** at v1. Baked light + additive sprite "lighting effects."
- **No SDF text rendering.** raylib's `LoadFontEx` with a reasonable size is enough for our HUD. SDF only if HUD scaling becomes a problem.
- **No video playback.** Title screens are static. Cutscenes are out of scope.
- **No external scene serialization** (FBX, glTF). We have one file format per asset type, baked from our pipeline.
- **No middleware audio** (FMOD, Wwise). raylib + miniaudio is enough.
- **No threaded asset loading.** Startup is short enough.
- **No HDR.** SDR + tone-mapped post.

## References

- raylib API at `/home/lunias/clones/asset/third_party/raylib/src/raylib.h` (581 RLAPI symbols).
- raymath (`raymath.h`, header-only) for `Vector2*`, `Lerp`, etc.
- rlgl (`rlgl.h`, header-only) for low-level batched draws.
- miniaudio (vendored inside raylib) — official docs at miniaud.io.
- raygui (`raygui.h`, single-header) for lobby UI.
- Glenn Fiedler — *Fix Your Timestep!* for the render-interpolation loop.

See [reference/sources.md](reference/sources.md) for URLs.
