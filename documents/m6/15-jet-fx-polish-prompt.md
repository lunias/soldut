# M6 — Jet-pack FX polish prompt (handoff to a fresh Claude session)

Copy the block below into a fresh Claude conversation in this repo.
It's self-contained — the implementer should not need this surrounding
conversation.

---

I'm picking up a polish pass on the jet-pack FX in Soldut at
`/home/lunias/clones/soldut`. Branch `lunias/m6-jet-fx-polish` is
checked out off `main` (it carries one prep doc commit on top of the
M6 P13 bot AI merge). Read `CLAUDE.md` first.

The original spec for the jet FX is
`documents/m6/02-jetpack-propulsion-fx.md` — read it end-to-end. The
M6 P02 implementation shipped at commit `b283b6e` and got a
follow-up perf fix at `48760e5` ("M6 P02-perf — fix FPS regression
when jetting"). The user has played enough since then to report that
"we may have regressed in jet pack FX" and explicitly asked: *are we
using our jetpack assets?*

The hard constraint is **no performance regression**. The perf-fix
commit retired per-particle `BeginBlendMode` wrapping (each call is a
full GPU flush via `rlDrawRenderBatchActive()`) and switched to a
two-pass batched render with an 8-segment octagon stand-in
(`fx_draw_particle`) instead of `DrawCircleV`'s 36-segment fan. Any
texture-path you re-introduce must preserve those wins — wrap the
texture draws in a single BeginBlendMode pair per pass and batch
through raylib's auto-batcher.

## What the inventory pass turned up

Before opening any code, here's what's true on `main` right now
(verified by reading the files, not the docs):

1. **`mech_jet_fx_step` runs** (simulate.c:429) and spawns
   `FX_JET_EXHAUST` + `FX_GROUND_DUST` + paints scorch decals + fires
   SFX_JET_IGNITION_{CONCRETE,ICE} on the grounded → airborne edge.
2. **`mech_jet_fx_draw_plumes` runs** between decals and the mech
   loop (render.c:1682) and draws textured plume quads from
   `assets/sprites/jet_plume.png` (1193 bytes). The atlas IS used —
   confirmed via `DrawTexturePro` in mech_jet_fx.c:491.
3. **Heat-shimmer uniforms are wired** in `assets/shaders/halftone_post.fs.glsl`
   (`jet_hot_zones[16]`, `jet_hot_zone_count`, `jet_time`). The
   uniform set is gated on `mech_jet_fx_any_active`. It is
   **suppressed in shot mode** for screenshot determinism
   (render.c:1852: `bool any = !g_shot_mode && ...`).
4. **SFX_JET_BOOST fires** from local mech (mech.c:1215) and remote
   mech via snapshot replication (snapshot.c:748) on the
   `MECH_JET_BOOSTING` leading edge.
5. **`MECH_JET_BOOSTING` snapshot bit is replicated** (snapshot.c
   record at :145, apply at :744-757).
6. **Hot-reload is registered** for both `jet_plume.png` and
   `jet_dust.png` (main.c:2360-2362).

## What looks regressed or never landed

In priority order:

1. **`jet_dust.png` is loaded but NEVER drawn.** This is the most
   likely "are we using our jetpack assets?" miss. Look at
   `src/mech_jet_fx.c`:
   - `s_dust_atlas` declared at line 22.
   - `try_load_dust_atlas()` at lines 44-58 (loads, sets bilinear
     filter, logs success).
   - `mech_jet_fx_reload_atlases` hot-reload branch at lines
     577-584.
   - **Zero references to `s_dust_atlas` in any draw path.** Grep
     it across the whole repo — only the load / reload sites
     appear.
   - `FX_GROUND_DUST` particles render through `particle.c::fx_draw`
     case at line 713-718, which calls `fx_draw_particle(pos, size,
     color)` — that's `DrawCircleSector(8 segments)`. The textured
     dust sprite is never sampled.
   - The 311-byte `assets/sprites/jet_dust.png` is dead weight on
     disk.

   **What I'd want investigated**: is restoring the textured dust
   path worth it? The dust character (soft alpha cloud) reads worse
   as a flat octagon than as a gradient sprite. But it must batch
   through one BeginBlendMode pair across all dust particles — wrap
   the FX_GROUND_DUST loop in `particle.c::fx_draw` pass 1 in a
   single texture binding (textured quad per particle via
   `DrawTexturePro`, source = full atlas, dest = pos±size, tint =
   particle color). Don't reintroduce a per-particle blend mode
   call.

2. **`FX_JET_EXHAUST` also renders as bare octagons** (particle.c
   pass 2, line 849-852: `fx_draw_particle(pos, fp->size, cc)`).
   The plume sprite carries the visual weight but the additive
   particles are tiny flat shapes. A small bright-core texture
   (8×8 or 16×16 radial gradient) drawn similarly batched would add
   pop without breaking the per-particle cost ceiling.

   This is lower priority than (1) because the plume sprite + the
   ground dust improvements would together address most of the
   visible weakness. But worth a look.

3. **No `tests/shots/net/run_jet_fx.sh` wrapper.** The two `.shot`
   pairs exist:
   - `tests/shots/net/2p_jet_fx.{host,client}.shot` (Reactor, Burst
     vs Standard, scripted boost).
   - `tests/shots/net/2p_jet_fx_glide.{host,client}.shot` (Glide
     variant).

   But no runner to execute them and assert anything. Compare to
   `run_frag_charge.sh` / `run_frag_concourse.sh` /
   `run_bot_frag_charge.sh` for the runner shape — they:
   - launch host + client in parallel
   - wait for both to exit
   - grep host/client `.log` files for assertion-grade lines
     (SHOT_LOG entries like `spawn_throw`, `explosion at`, fire
     counts)
   - produce a side-by-side composite from `host.png + client.png`
     via `montage` (already pattern, just runs if `command -v
     montage`).

   Add `run_jet_fx.sh` + `run_jet_fx_glide.sh` (or fold both into
   one runner) and assert SHOT_LOG lines that prove:
   - The expected number of `FX_JET_EXHAUST` spawns landed (host
     log: grep `fx_spawn_jet_exhaust` or add a SHOT_LOG inside the
     spawn helper if absent).
   - The expected SFX cues fired (host log already has `jet_burst
     sfx=jet_boost` per the existing SHOT_LOG line at mech.c:1216
     — extend the same pattern to ignition).
   - The plume sprite was actually drawn (host log: add a tally if
     useful, OR rely on the side-by-side composite for visual
     proof).

4. **Shimmer is invisible in shot tests** by the `!g_shot_mode`
   gate at render.c:1852. This is deliberate (so screenshot diffs
   stay byte-identical) but means visual regressions in the shader
   never show up in CI-style runs. Consider an alternate path:
   accept a deterministic `jet_time` seeded from `world.tick` in
   shot mode (not `GetTime()`), so the shimmer is identical across
   runs of the same shot — that lets the shot tests capture the
   shimmer visually while remaining byte-stable.

5. **Particle pool capacity headroom is tight.** `MAX_BLOOD =
   10500` in `src/world.h:184`. The spec budget was 8000 with
   ~7680 worst-case Burst-boost. Since then M6 P09 added weather
   (1500 particles peak) and ambient zone particles. Worst combined
   case in a 16-player Burst lobby with snow weather + ACID zone
   should still fit, but **measure it under
   `tools/bake/run_loadout_matrix.sh` with a Burst-heavy primary
   loadout** to confirm there's no silent overflow (FxPool
   overwrites the oldest live slot silently).

6. **The dust atlas's `BILINEAR` filter is set on load** (mech_jet_fx.c:55)
   but never sampled. The asset itself was authored to spec —
   confirmed: `file assets/sprites/jet_dust.png` reports 16×16 RGBA8
   (matches the spec art note). `jet_plume.png` is 32×96 RGBA8, also
   matching the spec. So the assets aren't placeholders — the dust
   one is a real authored sprite that the draw path silently dropped
   when the M6 P02-perf fix collapsed everything to
   `fx_draw_particle`. Restoring the texture sample is what unlocks
   the authored work.

## What to verify before touching code

Run these to capture the baseline:

```bash
make
# Smoke the existing paired shot tests so the baseline images exist.
./soldut --shot tests/shots/net/2p_jet_fx.host.shot &
HOST=$!
./soldut --shot tests/shots/net/2p_jet_fx.client.shot &
CLI=$!
wait $HOST $CLI
ls build/shots/net/2p_jet_fx.host/  # confirm the captured PNGs
# Look at the per-tick log to see what spawn paths fired.
grep -E "jet|spawn" build/shots/net/2p_jet_fx.host/*.log | head -40
```

Then read the existing files in this order:
- `documents/m6/02-jetpack-propulsion-fx.md` — full spec.
- `src/mech_jet_fx.c` (585 lines) — the module.
- `src/particle.c` lines 660-865 — the two-pass batched fx_draw
  where the texture path would re-land.
- `src/render.c` lines 1670-1880 — draw order + shimmer uniform
  push.
- `assets/shaders/halftone_post.fs.glsl` — shimmer pass.

## Concrete deliverables I'd want from this session

In priority order, each independently mergeable:

1. **Restore the dust texture path** (or delete the dead load code
   + asset if not worth restoring). If restoring: must batch through
   one BeginBlendMode pair, must match the spec's "alpha-blend, not
   additive" rule for dust. Add a `run_jet_fx.sh` regression that
   visually composites host+client and asserts the host log shows
   at least N `FX_GROUND_DUST` spawns during ignition.

2. **Optionally** add a tiny exhaust-core texture if the dust path
   work proves the batched-texture approach is cheap. Same batching
   rule. Use the same atlas-load pattern as plume / dust.

3. **Add `run_jet_fx.sh` + `run_jet_fx_glide.sh`** runners that
   exercise the existing `.shot` pairs and produce side-by-side
   composites. Mirror the shape of `run_frag_concourse.sh`. Assert
   on the SHOT_LOG lines we can rely on (`jet_burst sfx=jet_boost`
   already exists at mech.c:1216).

4. **Decide on shot-mode shimmer determinism.** Either leave it
   off (and document why in CURRENT_STATE.md), or seed `jet_time`
   from `world.tick` in shot mode so the shimmer is captured but
   stable across runs. User preference.

5. **Replace placeholder atlases if they were placeholders.**
   `jet_plume.png` (1193 B) and `jet_dust.png` (311 B) are
   suspicious sizes — check the dimensions with `file`/`identify`.
   If they're 32×96 / 16×16 placeholders, ship hand-authored
   gradients per the spec art notes.

6. **Re-validate the perf budget** under the worst-case load (16
   Burst chassis, all boosting). Use `tools/bake/` to drive a
   high-pressure scenario; check that fx pool doesn't overflow
   silently and the frame budget holds.

## Things NOT to do

- Don't bring back per-particle `BeginBlendMode` — that's the bug
  the M6 P02-perf fix was for. The user reports they'd rather have
  flat circles at 60 FPS than gradients at 30 FPS.
- Don't introduce a new FX pool. The shared `FxPool` is the right
  shape; just bump capacity (or, better, measure first to confirm
  the bump is needed).
- Don't re-architect `mech_jet_fx.c`. The module shape (per-tick
  step driven by `MECH_JET_*` bits + per-tier `JetFxDef` + chassis
  nozzle table) is correct. The work is asset / draw-path / test
  polish, not redesign.
- Don't add wire-format changes. The spec verified at §10 that all
  needed state is already replicated.
- Don't ship without a `tests/net/run.sh` + the existing paired
  frag-shot suite passing. The list is in CLAUDE.md.

## Out of scope (queue as separate work)

These were already logged as trade-offs by M6 P02 and shouldn't be
folded into this pass:

- Per-decal scorch fade (no per-splat age tracking in the decal RT).
- Per-particle networking for client/host visual identity.
- Shimmer 16-zone hard cap.

## Commit + PR

CLAUDE.md's git protocol applies (separate commits per logical
phase, no `--no-verify`). PR title shape: "M6 — jet-pack FX polish".
Update CURRENT_STATE.md with what changed at end of pass.

The branch is already set up; the first commit on the branch is the
bot-AI recap doc — leave it alone, it's unrelated background.

---

End of prompt.
