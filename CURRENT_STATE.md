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
| **M1**    | Playable end-to-end. Stability good.    |
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
