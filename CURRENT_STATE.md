# CURRENT_STATE — Where the build actually is

A living snapshot of debugging and playtesting. Updated as the build
moves. The design documents in [documents/](documents/) describe the
*intent*; this file describes the *current behavior* of the code that's
sitting on disk right now.

Last updated: **2026-05-03** (end of M1 debug pass).

---

## Milestones

| Milestone | Status                                  |
|-----------|-----------------------------------------|
| **M0**    | Done — see [README.md](README.md).      |
| **M1**    | Playable end-to-end. B1, B3, B4, B5, B6, B7 fixed 2026-05-03. Close-delay fixed same day. |
| **M2**    | Not started.                            |

---

## What works

A fresh checkout, `make`, `./soldut` opens a 1280×720 window onto the
tutorial level. From there:

- **Stand** — Pelvis pinned to standing height when grounded; spine,
  head, knees, feet all stable. No sag, no fold-over, no oscillation.
  The 120-tick idle test (`./build/headless_sim`, Phase 2) shows pelvis
  Y holding its spawn value to floating-point precision and Y-velocity
  reading 0.00.
- **Run** — `A`/`D` accelerates the whole body to ~280 px/s on the
  ground, ~98 px/s in the air (35% air control). Active braking when
  grounded with no input gives a snappy stop.
- **Jump** — `Space` applies a vertical impulse of 320 px on a fresh
  ground contact; locked out until next ground touch.
- **Jet** — `W` thrusts at 2200 px/s². Drains fuel at 0.6/sec. Regens at
  0.2/sec when grounded. Feels good in fullscreen; see "vsync /
  frame-rate" in [TRADE_OFFS.md](TRADE_OFFS.md) for the windowed-mode
  caveat.
- **Aim** — Crosshair follows the mouse in world space. Right arm pose
  is built from the aim vector each tick; the rest of the body is
  layout-consistent.
- **Fire** — `LMB` shoots the Pulse Rifle: 18 dmg, 110 ms cycle, 1.5 s
  reload, 30-round mag, 2400 px range. Tracer renders, recoil impulse
  pushes the chest back. Hitscan vs bone capsules.
- **Dummy** — Yellow target at (2400, 984). Stands stable (same anchor
  as the player). Takes damage. At 0 HP it transitions to ragdoll —
  pose drive drops and gravity does the rest. Left arm comes off when
  its limb HP drops to 0; the constraint connecting it to the shoulder
  is deactivated and the limb keeps Verlet-integrating as a free piece.
- **Blood / decals** — Blood particles spawn on hit, fall, and splat
  onto the persistent decal RT. Splats survive forever and stack.
- **Hit-pause** — A handful of ticks of frozen physics on a notable kill
  (the killshot itself, not chip damage). FX (blood) keeps falling
  during the freeze.
- **Screen shake** — Decays exponentially. Triggered by recoil and
  notable events.
- **HUD** — Health bar, jet fuel gauge, ammo counter, crosshair, kill
  feed. Drawn last, on top of everything.

The 30-second loop the M1 milestone calls for ("run, jet, shoot the
dummy until it falls apart — and grin") is reachable.

---

## Headless test

`tests/headless_sim.c` is a non-graphical scenario runner. It builds the
real `World`, runs `simulate()`, and dumps particle positions across
five phases:

1. **Spawn** — sanity check.
2. **Idle 2 s** — does the body fold under gravity + pose alone? No.
3. **Hold RIGHT 1 s** — does the mech move? Yes, ~280 px/s, body upright.
4. **Release** — does it stop? Yes, vel ≈ 0 in a tick or two.
5. **Hold JET 0.5 s** — climb rate, body shape under thrust.

Run: `make test-physics && ./build/headless_sim`. This is how we
catch physics regressions without firing up a window.

---

## M1 close-out blockers

Found via shot-mode runs of `tests/shots/m1_*.shot` on 2026-05-03.
PNGs referenced live under `build/shots/<script>/`.

### B1 — Crumpled-leg landing while running (FIXED 2026-05-03)

Repro: run off the spawn platform with `right` held throughout the
fall (`tests/shots/b1_dense.shot`). Pre-fix, the body landed at
tick ~100 in a permanently folded pose — knees buckled inward, body
sunk close to the floor — and stayed crumpled indefinitely
(`settled_t200` was identical to `land_t100`). Releasing `right`
mid-air made the body land cleanly (`tests/shots/b1_release.shot`),
which pinned the cause: `mech_post_physics_anchor` was gated on
`anim_id == ANIM_STAND` and quietly skipped during ANIM_RUN, so a
running landing never got the standing-height correction.

Fix in `src/mech.c::mech_post_physics_anchor`:

- Run the anchor for `ANIM_STAND` *and* `ANIM_RUN`. JET/FALL/DEATH
  still skip naturally (grounded is false, or m->alive is false).
- Translate knees by the same `dy_pelvis` as the rest of the upper
  body (preserves the thigh constraint at rest length).
- Snap knees to mid-chain only in `ANIM_STAND`. In `ANIM_RUN` we let
  the stride animation drive knee X swing.

Verified against `b1_dense`, `m1_big_fall`, `m1_stuck_recovery`, and
`walk_right` — all show clean landings and correct stride pose.

(My initial diagnostic note pointed at "stretched legs / body pinned
high" in the fall script. That turned out to be a misread caused by
the shot-mode camera using `GetFrameTime()`, which is ~1 ms per draw
in shot mode and made the smooth-follow camera lag far behind a
fast-moving player. Fixed alongside: `Renderer.cam_dt_override`,
shot mode pins it to 1/60.)

### B2 — Jet recovers from B1 (no longer relevant)

With B1 fixed, the body recovers without needing to jet. Recovery
script kept around because the user-facing intuition ("if it ever
looks weird, jet to reset") is still worth verifying we don't break.

### B3 — Rifle and forearm rendered through thin solids (FIXED 2026-05-03)

Repro: `tests/shots/b3_arm_clip.shot`. Walking up to the col-55 wall
with the cursor on the far side puts the rifle aim direction into
the wall. Pre-fix, the rifle barrel and forearm bones drew straight
through the tiles even though both endpoint particles were outside
them. Cause was rendering, not physics: `draw_mech` in `src/render.c`
emitted a raw `DrawLineEx` from elbow→hand and from hand→muzzle with
no awareness of level geometry.

Fix: new `draw_bone_clamped(level, a, b, ...)` in `src/render.c`.
Uses the existing `level_ray_hits` DDA traversal; if the segment
hits a solid, the line stops at the hit point (with a 0.02 pull-back
so we don't draw on the tile edge). Applied to the rifle barrel,
both forearms, both back-arm forearms. Other body bones (torso,
shoulder plate, hip plate, thighs, shins, neck) stay raw because
particle-vs-tile collision keeps those from crossing in practice.
`draw_mech` now takes a `Level *` parameter.

### B4 — Body wedged against world ceiling under sustained jet (FIXED 2026-05-03)

Repro: `tests/shots/b4_ceiling.shot`. The level has no explicit
ceiling tiles; `level_tile_at` returns `TILE_SOLID` for `ty < 0` so
out-of-bounds is treated as solid for collision. Sustained jet rose
the player into that boundary, where collision pushed every particle
back down while the next tick's jet pushed them up again — the
constraint solver tangled the chain trying to keep bones rigid
against the wedge.

Fix in `src/mech.c::apply_jet_force`: taper the thrust as the head
particle approaches y=0. New constants `JET_CEILING_TAPER_BEGIN=64`
and `JET_CEILING_TAPER_END=24` (px below ceiling). At >64 px below,
full thrust; between 64 and 24, linearly scaled; above 24, zero
thrust. Player can still cap their altitude near the ceiling but
can't hammer into it. After release, gravity pulls them back into
the level cleanly.

### Not yet verified

- **Dummy ragdoll glitch on death.** The kill scripts couldn't
  deliver shots to the dummy because the col-55 wall blocks both
  travel and line-of-sight from the spawn approach (air control is
  98 px/s, not enough to clear a 32 px wall in the time a 5-tile-tall
  jet allows). Need either a script that gets on top of the wall, a
  wider jet window, or a fresh layout — separate task.

### B5 — "Long red rectangle" trailing the dummy on death (FIXED 2026-05-03)

Repro: `tests/shots/m1_dummy_kill.shot` (uses the new `spawn_at`
directive to park the player on the dummy platform, fires until the
dummy drops). Pre-fix, the dead dummy left a thick red bone-shaped
line stretched from the corpse to the right wall — sometimes the
dummy "shot off" cleanly, sometimes the line was huge.

Two compounding causes, both fixed:

1. **Double-scaled kill impulse in `weapons.c`.** `weapons_fire_hitscan`
   passed `dir * (recoil_impulse * 4)` (= `dir * 4.8`) into
   `mech_apply_damage` as the "direction" argument. `mech_kill` then
   multiplied that again by `90` (the kill impulse magnitude),
   producing a 432 px instant displacement on the killshot particle.
   When the killshot landed on a dismembered limb (e.g. L_ELBOW after
   a left-arm tear), that lone particle had no body mass to absorb
   the kick and flew clear across the level until it pinned against
   the world wall (~750 px from the chest in the dump). Fix: pass the
   unit `dir` through `mech_apply_damage` as it always was meant to
   be — `mech_kill`'s `impulse=90.0` arg is the only scale.
2. **Kill impulse applied to the killshot part rather than the
   pelvis.** Even at 90 px the issue persisted whenever the killshot
   was on a disconnected fragment. Now `mech_kill` applies the
   impulse to `PART_PELVIS`, which is always tied to the rest of the
   body via active constraints — the body ragdolls as a unit and
   detached limb pieces stay where they were when severed.
3. **Renderer drew bones whose distance constraints had been
   deactivated.** Even after fixing the physics, the shoulder-plate
   bone (L_SHO ↔ R_SHO) was still being drawn between the
   left-behind L_SHOULDER and the body's R_SHOULDER, producing the
   same long-line silhouette in a different orientation. Fix: new
   `bone_constraint_active(cp, a, b)` helper in `src/render.c`; every
   `draw_bone_chk` call skips bones whose backing distance constraint
   is inactive. Dismembered limbs simply disappear visually now
   (their particles still integrate; they're just not drawn).

### B6 — Jet tunnels through 1-tile-thick platforms (FIXED 2026-05-03)

Three compounding sources of tunneling, fixed in three steps:

1. **Inside-tile escape priority pushed UP** — when a particle ended
   up inside a 1-tile-thick platform from below, the original
   `open_up > open_left > open_right > open_down` priority pushed it
   straight through to the top with its upward velocity intact. Fix
   in `collide_map_one_pass` (`src/physics.c`): pick the exit side
   based on whichever side the particle's `prev` was on. The
   neighbour-priority falls back when `prev` was already inside the
   tile across multiple ticks.

2. **Constraint relaxation popped a particle past the tile** — even
   with the inside-tile fix, constraint solver can apply a >r-px
   correction in one iter, putting a particle past the tile entirely
   so the close-point collision check doesn't fire either. Fix:
   close-point branch now also checks `prev` and pushes the particle
   back to the side it came from when `prev` and current are on
   opposite sides of the tile.

3. **Pose drive teleported particles through the tile** (the
   user-reported "head ends up on the other side" case). The HEAD
   pose target is `(pelvis_x, pelvis_y - 52)` with strength 0.7.
   When the body jets up under a 1-tile-thick platform, the gap
   between current head and pose target can be 50 px; the kinematic
   translate (which moves both `pos` and `prev` together to avoid
   Verlet velocity injection) then teleports the head 35 px in one
   tick — straight through a 32-px platform. Trace at the spawn
   platform: HEAD jumped from y=996 to y=956 between T50 and T51, a
   40-px instant move with no collision check between.
   
   Fix: new `physics_translate_kinematic_swept` (in `src/physics.c`)
   that ray-casts from the current pos to the target and clamps the
   move r+½ px shy of the first solid tile crossing. Called from
   `apply_pose_to_particles` in `src/mech.c`. Pose still pulls the
   body toward standing-pose-relative-to-pelvis, but a strong pull
   can no longer skip a body part across a thin solid.

The integrate step also gained a swept-collision check for forces
(jet, gravity, kill impulse) that drive a particle through a tile in
one tick, in case forces alone build enough per-tick step to tunnel.

### B7 — Feet skitter across the ground while running (FIXED 2026-05-03)

The previous run pose swung each foot ±14 px around the hip with a
sinusoid at ~1.4 Hz. With the body moving at 280 px/s = 4.67 px/tick,
no point on that sin curve had body-frame velocity equal to
`-RUN_SPEED`, so a "planted" foot was always being dragged forward
through the world by `body_velocity + sin'(t)·swing` rather than
sitting still — visible as a constant skitter / micro-slide across
the ground.

Fix in `mech.c` ANIM_RUN pose: replace the sinusoid with an explicit
two-phase walk cycle. Each foot alternates between **stance** (foot
linearly moves backward in body-frame from `+stride/2` to `-stride/2`
over the first half of the cycle) and **swing** (foot lifts in a
sin-arc and moves forward to the next plant point over the second
half). Stride is fixed at 28 px and the cycle frequency is tied to
the run velocity such that one stride's worth of body motion exactly
covers the stance phase: `cycle_freq = RUN_SPEED / (2 · stride)` =
5 Hz at 280 px/s. The right foot is offset by 0.5 cycles so the two
are out of phase. Result: the planted foot's world velocity is
exactly zero during stance, and the body pivots over it confidently.

### Close-window delay (FIXED 2026-05-03)

Hitting the X on the window had a noticeable beat before the process
exited. The slow steps were `CloseAudioDevice` (miniaudio teardown),
`CloseWindow` (`glfwTerminate` / Wayland connection close), and the
arena destroys (~60 MB of `free`s). None of that work is necessary —
the kernel reclaims memory, the GL context, and the audio device on
process exit. `main.c` now does `log_shutdown(); _exit(0);` after the
loop and the close is instant. The shotmode path still does its own
shutdown sequence because shot mode wants its log messages flushed
before the process leaves.

### Verdict

All identified M1 close-out blockers fixed. The known remaining
rough edges — body launches very fast on death (90 px/tick momentum
on the pelvis carries it across most of the level before settling),
dismembered limb stays visually invisible — are tunable but no longer
"a line stuck across the screen." Up next is content/feel work.

---

## Visual debugging — shot mode

`src/shotmode.c` implements a `--shot <script>` mode that drives the
real `simulate()` + renderer with a tick-stamped input script and
writes PNGs at chosen ticks. It's how we (or a tool) inspect motion
and physics without filming.

```bash
make shot                                    # tests/shots/walk_right.shot
make shot SCRIPT=tests/shots/your_case.shot
```

Script grammar lives in `src/shotmode.h`. Output: `build/shots/*.png`.
Fixed `dt = 1/60`, no wall-clock reads, RNG re-seedable from script —
re-runs are byte-identical.

---

## Recently fixed

The M1 implementation pass shipped, then we played it. These are the
real bugs we found and fixed, in chronological order — kept here so we
remember which classes of failure to look for next time.

### "My mech bounces around without me pressing any keys."

Three compounding bugs in the pose / drive / collision loop:

1. **Pose drive only updated `pos`, not `prev`.** Verlet reads
   `pos − prev` as velocity, so every pose lerp injected ghost velocity
   the next tick. Fixed by making pose drive a *kinematic translate*
   (update both `pos` and `prev` by the same delta) — see
   `physics_translate_kinematic()` and `apply_pose_to_particles()` in
   `src/mech.c`.
2. **Collision push didn't zero normal velocity.** Particles bounced off
   tile contacts because their post-push `prev` was on the other side of
   the tile. Fixed by adding `contact_position_only` (zero normal vel,
   keep tangential) for in-iteration contacts and `contact_with_velocity`
   (apply tangential friction) on the final iter only.
3. **Run force as `pos += force·dt`** accumulated velocity at 60× the
   intended rate. Fixed by using `physics_set_velocity_x()` to write the
   target velocity directly via `prev`.

### "I'm standing inside the platform. WASD doesn't move me."

- Run/jump/jet forces were applied to lower-body particles only. The
  upper body had no horizontal force, so the constraint solver fought
  the legs. Fixed by applying `apply_run_velocity`, `apply_jump`, and
  `apply_jet_force` to all 16 particles.
- Spawn was at the exact tile boundary. Added `foot_clearance = 4`
  (particle radius) when computing spawn Y so feet sit *on* the floor,
  not inside it.

### "Start/stop feels sluggish, mechs sink into the floor, dummy is folded in half."

- Single-pass constraint then single-pass collide didn't propagate. A
  foot held by the floor couldn't lift the body in the same tick — the
  pelvis sagged a little each frame instead. Fixed by interleaving
  constraint + collide inside one relaxation loop
  (`physics_constrain_and_collide`), 12 iterations.
- Particles inside a tile ping-ponged between adjacent solid tiles.
  Fixed with neighbour-aware exit (`collide_map_one_pass` checks
  up/left/right/down in priority order; pushes out toward the empty
  side).
- Active braking when grounded with no input — `apply_run_velocity`
  zeros the body's X velocity if there's no run input.

### Dummy's leg buckled to horizontal

This was the long one. The dummy at the far edge of the map was
mysteriously slumping over even though the player wasn't.

- We tried angle constraints. They had a modulo bug at the π boundary
  that injected huge corrections. Fixed the solver (use `acos(dot)`
  instead of an `atan2` difference) — but angle constraints **can't**
  prevent a leg from rotating to horizontal: the interior angle stays
  π whether the leg is upright or lying down. Removed.
- The pose target for `L_HAND` was far from the L_SHOULDER pose target.
  Pose was fighting the upper-arm + forearm chain on every tick. Fixed
  by dropping the L_HAND pose target entirely (the left hand dangles —
  see [TRADE_OFFS.md](TRADE_OFFS.md), no IK).
- The dummy's aim flipped between left and right as its chest sagged
  past the aim point. Fixed by making dummies skip the arm pose
  entirely (`if (!m->is_dummy)` around the aim-driven arm logic).
- The pelvis Y pose target was being read from the (already sagging)
  live pelvis. Foot-chain pose targets were derived from that, so they
  ended up under the floor; collision pinned them; the chain collapsed.
  Fixed by deriving the pelvis pose target from foot Y minus chain
  length when grounded — the body's *target* is always a standing one
  even if the live pelvis is briefly elsewhere.
- Even with that, soft pose strength wasn't enough; gravity sag still
  accumulated faster than the solver could correct it. Hardening the
  pose strength pumped velocity through the constraint solver (the
  classic "Verlet position-only correction → injected velocity" trap).
  Fixed by adding `mech_post_physics_anchor()`: after physics, if
  grounded and in `ANIM_STAND`, lift pelvis + upper body + knees to
  standing positions kinematically *and* zero their Y-velocity
  (`prev_y = pos_y`). Skipped during run/jump/jet/death so the body
  responds naturally to those.

### Pinned feet (`inv_mass = 0`) experiment

We tried pinning the feet during stand to anchor the body. That made it
explode upward during run because all the constraint corrections
landed on the only free end (the pelvis), creating a huge velocity
injection. Reverted; the post-physics anchor approach is what shipped.

---

## Known issues

These are real and reproducible. They aren't blockers for M1 (the loop
is playable and the milestone is met), but they're filed for future
attention. Each links to its longer-form entry in
[TRADE_OFFS.md](TRADE_OFFS.md).

- **Vsync / FPS mismatch** — Render runs at vsync rate (often >60 Hz in
  small windows); the simulation is locked to 60 Hz. The sim looks the
  same in either mode but jet *feels* different. The fix is render-side
  (use sim-tick interpolation alpha or accumulator with sub-step), not
  more physics tuning.
- **Left arm dangles** — No inverse kinematics yet. The left hand has
  no pose target; the upper-arm + forearm chain hangs from the
  shoulder. Looks weird with a two-handed rifle.
- **Knee snap during anchor** — `mech_post_physics_anchor` snaps knees
  to mid-thigh+shin. It's invisible at idle but would break a crouch
  cycle, which is why the anchor only runs in `ANIM_STAND`.
- **No render-to-sim interpolation alpha** — When the renderer outpaces
  the simulator, the latest sim state is drawn N times. A slight
  judder is visible on close inspection.
- **Only one chassis, one weapon, one dismemberable limb** — All by
  design for M1, but worth listing.

---

## Tunables (current values)

These are the numbers driving the feel. Documented here so we can A/B
them against playtest reactions.

| Source                    | Constant                        | Value    |
|---------------------------|---------------------------------|----------|
| `src/physics.h`           | `PHYSICS_GRAVITY_PXS2`          | 1200     |
| `src/physics.h`           | `PHYSICS_RKV` (Verlet damping)  | 0.99     |
| `src/physics.h`           | `PHYSICS_CONSTRAINT_ITERATIONS` | 12       |
| `src/mech.c`              | `RUN_SPEED_PXS`                 | 280      |
| `src/mech.c`              | `JUMP_IMPULSE_PXS`              | 320      |
| `src/mech.c`              | `JET_THRUST_PXS2`               | 2200     |
| `src/mech.c`              | `JET_DRAIN_PER_SEC`             | 0.60     |
| `src/mech.c`              | `AIR_CONTROL`                   | 0.35     |
| `src/mech.c` (Trooper)    | `health_max`                    | 150      |
| `src/mech.c` (Trooper)    | `bone_thigh` / `bone_shin`      | 18 / 18  |
| `src/mech.c` (Trooper)    | `torso_h` / `neck_h`            | 30 / 14  |
| `src/weapons.c` (Pulse)   | damage / cycle / reload         | 18 / 110ms / 1.5s |
| `src/weapons.c` (Pulse)   | mag / range                     | 30 / 2400 |

The simulation runs at **60 Hz** at M1 (not 120 Hz as
[documents/03-physics-and-mechs.md](documents/03-physics-and-mechs.md)
specifies). See "60 Hz vs 120 Hz" in [TRADE_OFFS.md](TRADE_OFFS.md).

---

## Playtest log

Running record of what the user (the only playtester right now) said,
what we did about it, and whether it was fixed. Most-recent first.

| Date | Report | Outcome |
|------|--------|---------|
| 2026-05-03 | Jet feels different in fullscreen vs windowed | Diagnosed as vsync/render-rate mismatch. Fix deferred to render-side interpolation. |
| 2026-05-03 | "Standing inside the platform, body folded, WASD doesn't move me" | Foot clearance + interleaved constraint/collide + full-body forces. **Fixed.** |
| 2026-05-03 | "Bounces around without input" | Kinematic pose translate + contact normal-vel zeroing + run force via prev. **Fixed.** |
| 2026-05-03 | "Dummy folded in half across the map" | Removed angle constraints, dropped L_HAND pose, derived pelvis target from feet, post-physics anchor with vel-zero. **Fixed.** |

When new playtest sessions happen, append rows here.
