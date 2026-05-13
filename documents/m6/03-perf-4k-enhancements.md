# M6 P03 — Perf at high desktop resolutions (the "4K" FPS dip)

**Status:** plan, not built. Branch `perf-4k-enhancements`.

**Trigger:** user reports an FPS dip when the window is maximised on a
3440×1440 ultrawide monitor. (Marketed as "4K-class" — strictly it is
ultrawide QHD, ~5.0 Mpx; true 4K UHD is 3840×2160, ~8.3 Mpx. The fix
must hold for both.)

This document is the **diagnosis + plan** for a single Claude session
to implement. It is written so the implementing session can pick it up
cold, with no surrounding chat context. Read it end to end before
touching code. The actual fix is a contained, mechanical change — the
bulk of this doc is the *why* and the *guard rails*.

---

## 0 — Required reading first

Two project docs override every choice below if they disagree:

- `documents/01-philosophy.md` — how we write code here. Sean Barrett /
  Casey Muratori / Jonathan Blow lineage. Specifically: rule 1 (data
  layout is API design), rule 3 (allocate once), rule 7 (commit to
  numbers), rule 9 (one way to do each thing).
- `documents/00-vision.md` — pillar 6: *"It runs on a laptop. Sixty
  frames per second on integrated graphics with 32 players in heavy
  combat. Memory budget under 256 MB resident. We do this by writing
  **less code, of higher quality, with explicit data layouts** — not by
  leaning on a black-box engine."*

The performance budget doc commits to this in numbers — and one line of
it is the load-bearing motivator for this entire plan:

> `documents/10-performance-budget.md:208` — *"**8K resolutions**.
> Internal resolution capped at 1920×1080; UI scales but world doesn't
> render at higher density."*

**The cap was specified at v1 and never built.** This document is what
finally builds it. Everything else here is corollary.

---

## 1 — What actually happens at 3440×1440 today

The frame pipeline (current, post-M6 P02):

1. `main.c:2112` — `platform_begin_frame(&pf)` reads
   `pf.render_w = GetRenderWidth()` and `pf.render_h = GetRenderHeight()`
   from raylib. On a maximised window with `FLAG_WINDOW_HIGHDPI` set
   (`platform.c:94`), these are the **physical-pixel** count of the
   backbuffer: **3440×1440 = 4.95 Mpx**.

2. `main.c:2612` / `:2670` — those `render_w/h` are passed straight
   into `renderer_draw_frame(..., sw=render_w, sh=render_h, ...)`.

3. `render.c:1327` — `ensure_post_target(sw, sh)` either creates or
   resizes `g_post_target` (a `RenderTexture2D`) to **sw × sh**. At 3440×1440
   that is a 4.95 Mpx, 8-bit RGBA, point-filtered off-screen target —
   ~19 MB of VRAM, recreated on every window-size change.

4. `render.c:1333–1336` — `BeginTextureMode(g_post_target);
   draw_world_pass(...); EndTextureMode();`. Every world-space draw —
   tiles, polys, decals, mechs, plumes, projectiles, fx, flags,
   background polys, foreground decorations, near parallax — rasterises
   into a 4.95 Mpx surface.

5. `render.c:1402–1413` — backbuffer pass:
   `BeginShaderMode(g_halftone_post); DrawTextureRec(g_post_target, ...)`.
   The full backbuffer (4.95 Mpx again) is shaded by
   `assets/shaders/halftone_post.fs.glsl`. **Every fragment** runs:

   - `frag_px = fragTexCoord * resolution` (`:109`)
   - `shimmer_offset(frag_px)` (`:79–106`): a loop over up to 16 hot
     zones, each iteration doing one `exp()`, two `sin()`, two `cos()`,
     a dot, and a couple of falloff branches.
   - One texture sample, the Bayer 8×8 threshold lookup, a luminance
     dot, a `step`, a `mix`. (`:108–135`)

6. HUD is drawn on the backbuffer *outside* the shader pass
   (`render.c:1414`) — that part is fine and stays at backbuffer res.

The two killers in the current pipeline:

### 1a — World rasterisation runs at 4.95 Mpx

Every tile (`render.c:214–229`), every poly (`:259–280`), every mech
sprite (`draw_mech_sprites`), every particle quad, every decal-layer
blit (`decal_draw_layer`), every plume quad
(`mech_jet_fx_draw_plumes`) rasterises into a 4.95 Mpx surface. With 16
mechs jetting and a typical map's ~2000 visible tiles, this is the
**baseline world fillrate** that scales **linearly** with backbuffer
pixel count.

Going from 1920×1080 (2.07 Mpx) to 3440×1440 (4.95 Mpx) is a **2.39×
increase** in world-fragment work. That alone moves us from 60 FPS to
~25 FPS on a GPU that was at 60 FPS at 1080p with no slack.

### 1b — Post-process pass is *worst-case* expensive at full res

Per fragment, with `n` active hot zones, the shimmer cost is:

```
~ n × (vec4 load + dot + exp + 4× sincos + 2× mul) ≈ n × 60 ALU ops
```

For one mech jetting (`n=1`) at 3440×1440 that's:

```
4.95 Mpx × 60 ALU ≈ 297 MFLOP / frame   for shimmer alone
```

For 16 mechs jetting (TDM peak), each pixel iterates 16 times — even
with the `count` short-circuit landed in M6 P02-perf:

```
4.95 Mpx × 16 × 60 ALU ≈ 4.75 GFLOP / frame   for shimmer alone
```

The whole frame budget on a mid-range integrated GPU
(`documents/10-performance-budget.md:22`, Intel UHD 630, ~0.4 TFLOPs)
is **~6.6 GFLOPs / frame at 60 Hz**. The shimmer pass alone wants
~72 % of that budget at peak.

Plus the per-pixel halftone (~10 ALU + 1 sample = ~50 MFLOP at peak).
Plus the world draws. We are massively over budget — and the *cause*
is "we cap nothing; the backbuffer dictates the fillrate."

### 1c — MSAA 4× is also unbudgeted at high res

`platform.c:94` enables `FLAG_MSAA_4X_HINT`. MSAA stores 4 samples per
backbuffer pixel and resolves at present time. At 3440×1440 that is
**4× the colour-buffer memory** (19 MB → 76 MB) and ~4× the resolve
bandwidth. With the halftone screen masking aliasing anyway, this is
double-paying.

### 1d — Aesthetic regression at high res

Worth flagging because it argues for the *same* fix on artistic
grounds, not just performance:

`halftone_post.fs.glsl:64–73` defines an 8×8 Bayer matrix. At 1920×1080
the screen holds 32,400 cells — each cell reads as a distinct
print-screen dot. At 3440×1440 the screen holds 77,400 cells — the
dither becomes "fine-grain noise" rather than a halftone screen.
Likewise the shimmer noise frequencies on `:99–102` (`0.08`, `0.10`,
`0.21`, `0.19` cycles/px) are tuned for 1080p-class densities. At
3440×1440 the shimmer reads as a higher-frequency shimmer with less
visible distortion.

The aesthetic in `documents/m5/11-art-direction.md` is "Industrial
Service Manual" halftone print. That aesthetic *requires* a target
resolution near 1080p. The performance fix and the aesthetic fix are
the same fix.

---

## 2 — The fix in one line

**Render the world and the post-process pass into a capped
internal-resolution render target, then bilinear-upscale that target
to the backbuffer. HUD continues to draw at backbuffer resolution
on top.**

This is the documented design (`10-performance-budget.md:208`),
implemented. It is also the standard solution recommended by AMD,
NVIDIA, and every shipped 2D raylib title — see references in §8.

### 2a — Why this is right, not a hack

- **It honours pillar 6.** Frame cost stops growing with monitor
  resolution. A 4K monitor and a 1080p monitor pay the same world-draw
  cost. The mid-range-laptop integrated-GPU target stays achievable on
  any monitor.
- **It honours rule 1 + 7.** We commit to a number (internal cap = 1080
  lines) and we structure the data (one internal RT, one post RT) to
  make that number load-bearing.
- **It honours rule 9.** There is one path: world → internal RT → halftone
  pass → backbuffer. No "do this on high-DPI, do that on low-DPI" branching.
- **It honours rule 8 (no premature reuse).** Two render textures, ~80
  new lines in `render.c`, ~6 new lines in `platform.c`. No dependency,
  no library, no abstraction.
- **The aesthetic comes back.** Halftone dots and shimmer noise read at
  the resolution they were tuned for, regardless of the user's monitor.

### 2b — Why not just "render at native and optimise the shader"

We could chase the halftone+shimmer shader with cheaper math. But:

1. Even a *free* shader still leaves the world fillrate at 2.39×.
   Tile draws, mech sprite blits, decal blits — none of which we can
   easily downscale piecewise.
2. The shimmer aesthetic at high res is broken even if it were free.
3. The cap is documented. Implementing what was specified is cheaper
   than re-relitigating it.

A capped internal RT covers both problems with one mechanism.

### 2c — Why not FSR1 / NIS / proper temporal upscaling

Considered. Rejected for v1:

- **FSR1** (EASU + RCAS, AMD, MIT-licensed, single-shader-pair) is the
  best-quality stationary spatial upscaler available. It is designed
  for **crisp game art that needs detail preserved**. Our look is
  already halftone-screen-print — crunchy by design. The upscaled image
  will be re-broken by the halftone screen-pattern anyway. FSR1 buys
  little visible win and adds ~120 lines of shader code we own and
  maintain. **File this as a stretch goal.**
- **NIS** (NVIDIA) — same story, NVIDIA-only at the GPU-driver level.
- **Temporal upscalers (DLSS/FSR2+/XeSS)** — require motion vectors,
  history buffers, jitter — way over our complexity budget and not
  philosophy-compatible.

**Recommendation: bilinear upscale.** Cheap, raylib-native, hides the
non-integer scale aliasing, looks correct under halftone. Leave an
`upscaler` enum hook for future FSR1 if someone wants it.

---

## 3 — The implementation plan, file-by-file

Six contained phases. Each is independently testable. Land them in
order; do not interleave.

### Phase 1 — Config + plumbing (no behaviour change yet)

**Goal:** Carry an `internal_h` number through to `renderer_draw_frame`.
After this phase, `internal_h == window_h` everywhere; nothing renders
differently.

1. **`src/platform.h:40-45`** — add to `PlatformConfig`:
   ```c
   int internal_h;   // 0 = match window_h (no cap); else cap line count
   ```

2. **`src/platform.h:47-52`** — add to `PlatformFrame`:
   ```c
   int internal_w, internal_h;   // capped render-target dims
   ```

3. **`src/platform.c:80-141`** — store `cfg->internal_h` in `g_cfg`.
   Log it next to the `%dx%d (msaa=4x highdpi)` line (`:138`) so the
   number is visible on every launch.

4. **`src/platform.c:149-156`** — extend `platform_begin_frame`:
   ```c
   int cap = g_cfg.internal_h;
   if (cap <= 0 || cap > out->render_h) {
       out->internal_w = out->render_w;
       out->internal_h = out->render_h;
   } else {
       out->internal_h = cap;
       // Aspect-match the internal width to the window so FOV is preserved.
       out->internal_w = (out->render_w * cap + out->render_h / 2) / out->render_h;
   }
   ```
   The rounded-to-nearest formula keeps the aspect ratio within ±0.5 px
   and avoids a stretched image. Ultrawide windows get a proportionally
   wider internal RT (e.g. 3440×1440 → internal 2580×1080), not a
   16:9 letterbox.

5. **`src/main.c:1960-1966`** — set `pcfg.internal_h = game.config.internal_res_h;`
   (default 1080; see step 6). Pass through to `platform_init`.

6. **`src/config.c`** + **`src/config.h`** — add `int internal_res_h;`
   to the config struct, parse key `internal_res_h` from
   `soldut.cfg`, default to **1080**.

7. **`src/prefs.c`** + **`src/prefs.h`** — add `int internal_res_h;`
   to `UserPrefs`, persist across launches. Default 1080.

8. **`src/main.c:2612-2671`** — change `renderer_draw_frame(...,
   pf.render_w, pf.render_h, ...)` to `renderer_draw_frame(...,
   pf.internal_w, pf.internal_h, pf.render_w, pf.render_h, ...)`.

9. **`src/render.h:47-53`** — change the `renderer_draw_frame`
   signature to take both:
   ```c
   void renderer_draw_frame(Renderer *r, World *w,
                            int internal_w, int internal_h,
                            int window_w,   int window_h,
                            float interp_alpha,
                            Vec2 local_visual_offset,
                            Vec2 cursor_screen,
                            RendererOverlayFn overlay_cb,
                            void *overlay_user);
   ```

10. **`src/render.c:1305-1429`** — accept the new parameters. **Do not
    yet change pipeline behaviour.** Just rename the existing `sw/sh`
    to the new names internally so the rest of the function compiles.

**Checkpoint after Phase 1:** game builds, runs identically. F3 / diag
overlay (`main.c:1717-1740`) prints "FPS N" same as before. Shot tests
are pixel-identical. All `make test-*` targets pass. No visible change.

### Phase 2 — Internal render target, shader pass at internal res

**Goal:** World draws to `g_internal_target` at internal res, halftone
shader runs at internal res into `g_post_target`, blit upscales to
window. Most of the perf win lives here.

1. **`src/render.c:50-52`** — declare a second target:
   ```c
   static RenderTexture2D g_internal_target = {0};
   static int             g_internal_target_w = 0;
   static int             g_internal_target_h = 0;
   ```

2. **`src/render.c:88-108`** — rename `ensure_post_target` to
   `ensure_internal_targets(int iw, int ih)`. It now manages **both**
   RTs at `iw × ih`. Both are point-filtered. `g_post_target` is also
   sized `iw × ih` (not `sw × sh` any more — the halftone pass runs at
   internal res). Both are recreated on size change.

3. **`src/render.c:1240-1303`** — `draw_world_pass`: leave its body
   unchanged but document that it must be called with `sw=internal_w`,
   `sh=internal_h`. The screen-space parallax (`:1247`, `:1302`) uses
   `sw/sh`, so it must see internal pixels.

4. **`src/render.c:1305-1429`** — rewrite the body in this order:

   ```c
   ensure_internal_targets(internal_w, internal_h);

   decal_flush_pending();

   if (g_halftone_loaded) {
       // 1) World pass into internal_target at internal res.
       BeginTextureMode(g_internal_target);
           ClearBackground((Color){12,14,18,255});
           draw_world_pass(r, w, alpha, local_visual_offset,
                           internal_w, internal_h);   // <-- internal!
       EndTextureMode();

       // 2) Halftone pass: internal_target -> post_target at internal res.
       //    The shader sees `resolution = internal_w/h` and runs
       //    internal_w * internal_h times per frame — half of native at 3440x1440.
       Vector2 res = { (float)internal_w, (float)internal_h };
       SetShaderValue(g_halftone_post, g_halftone_loc_resolution,
                      &res, SHADER_UNIFORM_VEC2);
       // ... density + jet uniforms unchanged ...
       // Hot-zone screen coords must also be in internal-pixel space —
       // see step 5 below.

       BeginTextureMode(g_post_target);
           ClearBackground((Color){0,0,0,0});
           BeginShaderMode(g_halftone_post);
               Rectangle src = { 0, 0, (float)internal_w, -(float)internal_h };
               DrawTextureRec(g_internal_target.texture, src,
                              (Vector2){0,0}, WHITE);
           EndShaderMode();
       EndTextureMode();

       // 3) Backbuffer: bilinear upscale post_target to window-sized
       //    destination, aspect-preserving letterbox (almost always a
       //    no-op letterbox since internal_w is computed to match the
       //    window aspect — but handle the edge case where someone forces
       //    an internal resolution that doesn't match).
       BeginDrawing();
           ClearBackground(BLACK);

           float sx = (float)window_w / (float)internal_w;
           float sy = (float)window_h / (float)internal_h;
           float scale = (sx < sy) ? sx : sy;
           float dw = internal_w * scale;
           float dh = internal_h * scale;
           float dx = (window_w - dw) * 0.5f;
           float dy = (window_h - dh) * 0.5f;

           // Switch to bilinear for the upscale; preserve the point
           // filter on g_internal_target (so the halftone pass samples
           // sharp source pixels).
           SetTextureFilter(g_post_target.texture, TEXTURE_FILTER_BILINEAR);

           Rectangle src = { 0, 0, (float)internal_w, -(float)internal_h };
           Rectangle dst = { dx, dy, dw, dh };
           DrawTexturePro(g_post_target.texture, src, dst,
                          (Vector2){0,0}, 0.0f, WHITE);

           // HUD continues to draw at window res, on top, no shader.
           hud_draw(w, window_w, window_h, cursor_screen, r->camera);
           if (overlay_cb) overlay_cb(overlay_user, window_w, window_h);
           audio_draw_mute_overlay(window_w, window_h);
       EndDrawing();
   } else {
       // Fallback when the shader file is missing: same shape as Phase 1
       // (i.e., still respect internal_w/h — draw world to internal_target,
       // then a no-shader bilinear blit to the backbuffer). The "no
       // halftone + no internal RT" original path is dead.
       BeginTextureMode(g_internal_target);
           ClearBackground((Color){12,14,18,255});
           draw_world_pass(r, w, alpha, local_visual_offset,
                           internal_w, internal_h);
       EndTextureMode();
       BeginDrawing();
           ClearBackground(BLACK);
           // ... same letterbox math + bilinear blit ...
           hud_draw(w, window_w, window_h, cursor_screen, r->camera);
           if (overlay_cb) overlay_cb(overlay_user, window_w, window_h);
           audio_draw_mute_overlay(window_w, window_h);
       EndDrawing();
   }
   ```

5. **`src/mech_jet_fx.c:522-563`** — `mech_jet_fx_collect_hot_zones`
   converts world → screen with `GetWorldToScreen2D(..., *cam)`. The
   camera target/offset live in **internal-pixel space** under the new
   pipeline (because `update_camera` sets `r->camera.offset = sw/2,
   sh/2` and the renderer's `sw/sh` are now internal). So `sc.xy` is
   already in internal pixels — **correct, no change needed**. Add a
   comment confirming this so the next reader doesn't "fix" it.

6. **`src/render.c:128-137`** — `renderer_init` is called at startup
   with `GetScreenWidth/Height` (`main.c:2050`). Change `main.c:2050`
   to pass the *initial* internal size derived the same way as
   `platform_begin_frame` step 4. (Or simpler: do `renderer_init(&rd,
   1920, 1080, ...)` — the camera offset is overwritten every frame in
   `update_camera`, so the init value only matters for the first
   `screen_to_world` call before the first frame.)

7. **`src/render.c:146-201`** — `update_camera` already takes `sw/sh`
   as parameters and writes `r->camera.offset = (Vector2){sw*0.5f,
   sh*0.5f}` each frame. Called from `renderer_draw_frame:1314` with
   the renamed `internal_w/h`. **Correct.** Just verify after the
   refactor that the `sw/sh` passed in is in fact `internal_w/h`.

8. **`src/render.c:139-142`** — `renderer_screen_to_world` calls raylib
   `GetScreenToWorld2D` using `r->camera`. The camera is in internal
   coords now. Cursor positions arriving from `main.c:2612` are
   **window coords** (raw `GetMousePosition`). Need to convert
   window→internal before calling `renderer_screen_to_world`. Add a
   helper:

   ```c
   // In renderer_draw_frame, just before update_camera:
   Vec2 cursor_internal = {
       (cursor_screen.x - dx_letterbox) / scale,
       (cursor_screen.y - dy_letterbox) / scale,
   };
   r->last_cursor_screen = cursor_internal;
   r->last_cursor_world  = renderer_screen_to_world(r, cursor_internal);
   ```

   The `dx_letterbox/dy_letterbox/scale` are the same numbers
   computed in step 4 for the backbuffer blit. Factor them out into a
   small `static inline` helper at the top of `render.c` so the
   computation lives in one place.

9. **`src/main.c`** — search for any other consumer of "cursor in
   world space" that bypasses `renderer_screen_to_world`. The
   simulation's `ClientInput.aim_x/y` (filled in `platform_sample_input`
   at `platform.c:177-179`) is the cursor in **window pixels**. The
   simulate path converts via `mech_aim_dir` which uses a stored world
   aim — the conversion happens once per tick at `main.c:~2400-2500`
   in `process_input`. Find the call site and apply the same
   window→internal correction before passing to the world aim.

   **This is the single sharpest regression vector.** A wrong cursor
   conversion makes the player's aim drift away from where the cursor
   points. Verify with the existing `tests/shots/m5_weapon_held.shot`
   (aim alignment) and a manual play test before declaring done.

**Checkpoint after Phase 2:** Maximise on the 3440×1440 monitor; FPS
should jump from current to ≥60. Halftone dots read at 1080p density.
Aim cursor lines up with the world point under the cursor.

### Phase 3 — Drop MSAA on the backbuffer

**Goal:** Recover ~1 ms / frame and ~57 MB of VRAM by deleting an
unused anti-aliaser.

1. **`src/platform.c:94-95`** — remove `FLAG_MSAA_4X_HINT` from the
   `flags` mask. Adjust the log line accordingly.

The world now goes through a point-filtered internal RT and a bilinear
upscale. MSAA on the backbuffer only ever helped triangle edges drawn
*directly* on the backbuffer; under the new pipeline that's HUD-only,
and the HUD is axis-aligned rectangles + bilinear-sampled text glyphs
— neither of which MSAA improves visibly. The halftone screen on the
world hides whatever sub-pixel aliasing remains.

If a play-test shows visible jagginess, the answer is **not** to bring
MSAA back: it's to enable raylib's `SUPPORT_MSAA` on the **internal
target** using `rlLoadFramebuffer` (deeper raylib API) and accept the
small fillrate hit there. Even then, the halftone pass mostly masks
it. We can postpone that decision until someone complains.

**Checkpoint after Phase 3:** Same visual quality, ~1 ms / frame
faster, ~57 MB less VRAM (76 MB MSAA backbuffer → 19 MB single-sample
backbuffer).

### Phase 4 — Verification & measurement

**Goal:** Numbers in CURRENT_STATE.md. Don't merge on faith.

1. **Measurement protocol** — use the existing always-on diag overlay
   (`main.c:1717-1740` `draw_diag`, prints FPS via `GetFPS()`). For
   apples-to-apples numbers, also temporarily log a per-frame total
   ms using `GetTime()` deltas around the main loop body
   (`main.c:2100-2710`).

2. **Test conditions:**
   - **Solo bake on Foundry**, 16 bots (when bot AI lands; until then,
     human + 1 dummy on `--test-play assets/maps/foundry.lvl`).
   - **Three resolutions:** window 1920×1080, window 2560×1080,
     window maximised at native (3440×1440 on the user's monitor).
   - **Two scenarios per resolution:** idle (no jets) and worst-case
     (player holding jet + W, plumes painting + shimmer firing).

3. **Acceptance criteria:**

   | Window      | Before (today) | After (target) |
   |---|---|---|
   | 1920×1080, idle      | 60 FPS lock (vsync) | 60 FPS lock (no regression) |
   | 1920×1080, worst     | 60 FPS lock          | 60 FPS lock |
   | 2560×1080, idle      | likely <60          | 60 FPS lock |
   | 2560×1080, worst     | likely <60          | 60 FPS lock |
   | 3440×1440, idle      | dropped (the bug)   | 60 FPS lock |
   | 3440×1440, worst     | dropped (the bug)   | ≥55 FPS, no perceptible drop |

   "60 FPS lock" means vsync-bound (`SetTargetFPS(0)` plus
   `FLAG_VSYNC_HINT`). Below vsync = something is wrong.

4. **Regression checklist — every item must pass before merge:**
   - [ ] `make` (host native) builds clean with `-Wall -Wextra -Wpedantic -Werror`.
   - [ ] `make windows` cross-builds clean (`cross-windows.sh`).
   - [ ] `make test-physics`, `test-level-io`, `test-pickups`,
         `test-ctf`, `test-snapshot`, `test-spawn`, `test-prefs`,
         `test-map-chunks`, `test-map-registry`, `test-map-share`,
         `test-editor`, `test-spawn-e2e`, `test-meet-named`,
         `test-meet-custom`, `test-ctf-editor-flow`,
         `test-grapple-ceiling`, `test-frag-grenade` — all pass.
   - [ ] `make shot SCRIPT=tests/shots/walk_right.shot` — PNG output
         **byte-identical** to pre-fix (shot mode opens its own window
         at 1280×720, where internal_h cap == window_h, so the internal
         RT is 1:1 with the window — pipeline should produce identical
         pixels).
   - [ ] `tests/shots/m5_chassis_distinctness.shot`,
         `m5_weapon_held.shot`, `m5_drift_isolate.shot` — same
         byte-identical bar.
   - [ ] `tests/net/run_3p.sh` — log-assertion pass.
   - [ ] `tests/shots/net/run.sh 2p_dismember` —
         paired-screenshot byte-identical.
   - [ ] `tests/shots/net/run_frag_grenade.sh` and
         `run_kill_feed.sh` — pass.
   - [ ] **Aim test:** play `--test-play assets/maps/foundry.lvl`
         maximised at 3440×1440, fire at a fixed world target. The
         bullet trace must land where the cursor visually points
         to within ≤2 px.
   - [ ] **Visual A/B:** `make host-overlay-preview` and
         `lobby-overlay-preview` look correct (no letterbox bars in
         the UI screens, since lobby/menu render at window res with
         no internal-RT path — see Phase 5 step 3).
   - [ ] **Resize stress:** maximise → restore → drag-resize for 5
         seconds → maximise — no crash, no GL errors in the log, FPS
         stays at vsync.

5. **Document the new budget breakdown** in
   `documents/10-performance-budget.md` once Phase 1–3 ship. Replace
   the "8K resolutions" bullet at `:208` with the actual mechanism:

   > "Internal resolution capped at `internal_res_h` (default 1080);
   > world + post-process run at the capped resolution; bilinear
   > upscale to window. HUD draws at window resolution on top."

### Phase 5 — Cover the regression vectors

Each of these is a place where the Phase 2 refactor could silently
break a screen. Walk through them deliberately.

1. **Lobby / menu screens** (`title_screen_run`, `host_setup_screen_run`,
   `browser_screen_run`, `connect_screen_run`, `lobby_screen_run`,
   summary screen — `main.c:2193 ff`). All of these are passed
   `pf.render_w, pf.render_h` (window coords). They do **not** go
   through `renderer_draw_frame` and do **not** use the internal RT.
   **Verify** they still pass window coords (not internal) so the UI
   keeps drawing crisp at backbuffer res. If any of them got
   accidentally migrated to internal coords during Phase 1 step 8,
   revert that call site.

2. **Shot mode** (`src/shotmode.c`). Shot mode opens its own
   `platform_init` at a script-specified `window` (typically 1280×720).
   For pixel-byte-identical regression, the easiest rule:
   `internal_h = 0` (no cap) in shot mode. Override in
   `shotmode.c::run_shot_script` before its `platform_init` call.

3. **Dedicated server** (`--dedicated`). `decal_init` already bails
   when `!IsWindowReady()` (`decal.c:89`). The internal-RT code lives
   inside `ensure_internal_targets`, gated the same way (check via
   `IsWindowReady` and bail). The dedicated path **must not** create
   any RT. Verify via `tests/net/run_3p.sh`.

4. **Audio overlays** (`audio_draw_mute_overlay`, etc.). Drawn after
   `EndShaderMode`, at window res. Unchanged.

5. **Camera bounds clamp** (`render.c:181-191`). Uses
   `level_width_px / level_height_px` and `halfw = (float)sw /
   (2.0f * r->camera.zoom)`. With `sw = internal_w`, the clamp adjusts
   the camera so the visible internal-RT area never extends past the
   level edges. Math is correct, but confirm visually that the player
   can still see the level edges (no extra unreachable margin).

6. **F3 overlay text positioning** (`main.c:1722-1743`). Uses raw px
   coords on a backbuffer drawn at window res. Unchanged — looks
   identical at any window size.

7. **Reconcile snap visual offset** (`local_visual_offset` in
   `renderer_draw_frame`). It's a per-particle pixel offset added in
   world space (`particle_render_pos + visual_offset.x`). World space
   is independent of render resolution — unchanged.

8. **`tests/shots/net/run.sh` paired screenshots.** Spawns host +
   client via `--shot` scripts. Both windows are 1280×720, both have
   `internal_h = 0` (shot mode override), both paths render
   pixel-identical to pre-fix. ✓

### Phase 6 — Documentation + trade-off bookkeeping

Required by `documents/01-philosophy.md` rule 7 ("we commit to
numbers") and the project convention (`CLAUDE.md`: trade-offs are a
queue, not a changelog).

1. **`CURRENT_STATE.md`** — describe the new pipeline in the
   "Rendering" section. Numbers: measured FPS at the three test
   resolutions, before/after. New tunables: `internal_res_h` in
   `soldut.cfg` (default 1080), persisted in `soldut-prefs.cfg`.

2. **`TRADE_OFFS.md`** —
   - Add (if applicable post-implementation): "HUD draws at window
     res, world at internal res, so HUD glyphs read sharp on 4K
     monitors while world art reads at the halftone-tuned density.
     Trade-off: 1 px-aligned UI ↔ 1 px-aligned world is no longer the
     same px size. **Revisit if:** anyone reports UI/world alignment
     bugs (a HUD reticle that doesn't line up with the world cursor)."
   - **Delete:** any existing entry related to "halftone too fine at
     high res" or "shimmer too subtle at 4K" if present (none today,
     but Phase 4 may discover one).

3. **`documents/10-performance-budget.md`** — replace `:208` bullet
   (see Phase 4 step 5) and add a new sub-section under "Render
   budget detail" describing the internal-RT mechanism.

4. **`documents/11-roadmap.md`** — mark M6 P03 shipped with one line.

5. **`CLAUDE.md`** — update the M6 status paragraph at top to
   mention P03.

---

## 4 — Anti-patterns we explicitly reject

Per `01-philosophy.md` §"Anti-patterns we forbid", and tuned for this
specific refactor:

- **Don't add a `RenderConfig` struct.** The two numbers we need fit
  in `PlatformConfig` and `PlatformFrame`. A struct of one thing is a
  struct of nothing.
- **Don't gate the internal-RT path behind a `#ifdef
  ENABLE_INTERNAL_RES`.** It is the one path. Per rule 9. Delete the
  old "no internal RT" code paths once the new pipeline ships.
- **Don't introduce a `Resolution` typedef.** `int internal_w, int
  internal_h` is correct.
- **Don't write a `Scaler` interface to "support multiple upscalers
  in the future."** When FSR1 is wanted, swap the
  `DrawTexturePro(g_post_target, ...)` call for `DrawTexturePro(...,
  fsr_shader)`. Compression-oriented programming (`01-philosophy.md`
  §Casey Muratori): three call-sites would justify an abstraction;
  one does not.
- **Don't try to make MSAA work on the internal RT in v1.** Skip it.
  See Phase 3 for the reasoning. If quality is a problem later, the
  fix lives in `rlLoadFramebuffer` and is a separate session.
- **Don't write a "headless render benchmark."** The measurement
  protocol is the existing `draw_diag` FPS readout plus shot tests.
  Adding an offline benchmark is more code than it's worth and won't
  catch the regression that matters (the user's real monitor).
- **Don't widen the shader-loading branches.** The "halftone shader
  missing" fallback in Phase 2 step 4 keeps the same internal-RT
  pipeline; we just skip the shader pass. **Don't** preserve a "no
  internal RT + no shader" third path. That's two paths.
- **Don't hot-reload the internal resolution mid-frame.** A
  `soldut.cfg` value change requires a restart, like every other
  config key. If hot-tunability is wanted, it lives in a future M-pack
  config UI.

---

## 5 — Things to verify mid-implementation (sanity checks for the implementing Claude)

Each is a single test you can run from a shell or eyeball; if any
fails, **stop and re-check** the relevant phase step.

1. **After Phase 1**: `./soldut` launches, F3 / diag overlay shows
   the same FPS as before. The line `platform_init: 1920x1080
   (msaa=4x highdpi)...` in `soldut.log` now ends with
   `internal=1080`. Shot tests are byte-identical.

2. **After Phase 2**: At a 1920×1080 maximised window, FPS is
   unchanged (internal == window, pipeline is 1:1). At a 3440×1440
   maximised window, FPS climbs to vsync lock. Cursor crosshair
   visually overlaps the bullet origin on fire — bullets land where
   the cursor points.

3. **After Phase 3**: MSAA log line is gone. No new jaggy edges
   visible in any UI screen — halftone screen masks any subtle
   aliasing in the world. VRAM usage (taskmgr / `nvidia-smi` /
   `intel_gpu_top`) drops by ~55 MB at the moment of window
   maximise.

4. **After Phase 4**: numbers in `CURRENT_STATE.md` match the
   acceptance table in Phase 4 step 3. `make test-*` clean.

5. **End-to-end**: paired-dedi test
   (`tests/shots/net/run_frag_grenade.sh`) passes; visual A/B between
   `tests/shots/m5_chassis_distinctness.shot` PNGs from before and
   after is byte-identical (because shot mode uses
   `internal_h = 0`).

---

## 6 — A list of things this fix is NOT

So the implementing session doesn't get pulled into adjacent work:

- It is **not** a refactor of the halftone shader. The shader is
  untouched. The fix changes how big a surface the shader runs on, not
  what it does.
- It is **not** a tile-culling pass. `draw_level_tiles`
  (`render.c:209-246`) still iterates every tile in the level grid.
  That's a separate optimisation (off-screen tile culling) that has
  its own per-frame cost regardless of resolution. Not in scope.
- It is **not** a switch to an ECS / SoA refactor of the mech draw
  loop. The mech sprite path is fine; it just happened to be rasterising
  into a too-big surface.
- It is **not** a fix for the constraint-solver iteration count, the
  particle pool budget, or the snapshot rate. The CPU sim path is
  within budget today.
- It is **not** a Steam Deck pass. `10-performance-budget.md:206`
  says we don't target Steam Deck at v1 — the internal-RT path will
  help it incidentally, but we don't profile or test on it here.

If the implementing session notices that any of these adjacent things
*are* now in the critical path post-fix, write a new
`documents/m6/04-...md` document. Don't fold the work in.

---

## 7 — Hand-off summary for the next-session Claude

If you're the session implementing this: **read §3 carefully**. The
mechanical work is six phases, each isolated, each independently
testable. The single sharpest regression is **cursor coordinate
conversion** — get §3 Phase 2 step 8 + 9 right, and run the aim test in
Phase 4 step 4 before declaring done.

Concrete starting move: open `src/platform.h`, add the two int fields,
follow the chain. Don't pre-build anything. Don't refactor anything
else.

Two non-obvious things to remember while you work:

1. **Cursor coordinates** are the cardinal trap. The simulate step
   sees `ClientInput.aim_x/y` in window pixels (`platform.c:177-179`).
   The camera sees internal pixels (after Phase 2). One conversion,
   one place — same `dx_letterbox / scale` numbers as the backbuffer
   blit. Apply once, in the same function that computes the blit
   destination. Don't apply it twice.

2. **Shot tests are the regression backstop.** They run with
   `internal_h = 0` (no cap) → internal RT is 1:1 with the window
   backbuffer → pixel-identical to today. If a shot test diffs after
   your change, the bug is in your refactor, not in the test. Don't
   "fix" the test.

---

## 8 — References

Algorithm + implementation references actually used in this design:

- **raylib core_window_letterbox example** —
  `https://github.com/raysan5/raylib/blob/master/examples/core/core_window_letterbox.c`.
  The canonical raylib pattern for internal-render-target + aspect-
  preserving upscale. The `MIN(sw/iw, sh/ih)` scale, the centred
  destination rect, the virtual-mouse conversion — all directly
  applicable to §3 Phase 2 step 4 and step 8. Linked from the raylib
  examples gallery `https://www.raylib.com/examples/core/loader.html?name=core_window_letterbox`.

- **AMD FidelityFX Super Resolution 1.0 (FSR1)** — `https://gpuopen.com/fidelityfx-superresolution/`.
  MIT-licensed single-shader-pair spatial upscaler (EASU + RCAS).
  Considered for the upscale step and rejected for v1 because the
  halftone-screen aesthetic eats most of the detail FSR1 would
  preserve. File as a stretch goal if the bilinear-upscale look ever
  reads as objectionable.

- **Surma's Ditherpunk** + **Daniel Ilett's Obra Dinn writeup** — the
  references already cited in `halftone_post.fs.glsl:36–37`. Both
  describe the visual contract the shader was built against (an 8×8
  Bayer screen at ~1080p density). Useful context for the aesthetic
  motivation in §1d.

- **Linden Reid's heat-distortion shader tutorial** — the shimmer
  reference cited in `halftone_post.fs.glsl:36–37`. The shimmer's
  spatial frequencies on `:99–102` are tuned against ~1080p density;
  the internal-RT cap restores that intent.

- **`documents/10-performance-budget.md:208`** — the load-bearing
  internal commitment that everything else here is implementing.

---

**End of plan.** Implementation Phase 1 should start at
`src/platform.h:40` and the rest follows.
