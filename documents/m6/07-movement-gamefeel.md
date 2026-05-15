# 07 — Movement gamefeel iteration

---

## ⚠ Orientation — read this first (you are the implementing Claude)

This document is a self-contained plan handed to you to execute in a
fresh context. The user is hands-on for this one — they will playtest
each tuning change, give feedback, and we iterate. Branch:
`m6-movement-tuning` (already created from `main`, ready for you).

### What is already true when you arrive

- **Repository:** `/home/lunias/clones/soldut`. Builds with `make`.
- **Branch:** `m6-movement-tuning`. Already created from `main` post
  P06 merge (commit `bbcafaa`). The profiler from P06 is in.
- **Profiler:** `./soldut --bench 30 --bench-csv …` works for measuring
  any frame-time regressions you introduce. Use it before and after
  every tuning change.
- **Shot tooling:** `make shot SCRIPT=tests/shots/m6_movement_probe.shot`
  runs the existing movement diagnostic — 14 cells in a contact sheet
  showing standing → running → jumping → landing. Re-run it after
  each tuning change for a visual A/B.
- **`CLAUDE.md`** at repo root is load-bearing. Re-read before
  changing anything.

### What you do at the start of your session

1. Read this doc end to end.
2. Re-read `documents/03-physics-and-mechs.md` §"Movement" + §"Pillars".
   That's the original intent; we're refining feel, not rewriting it.
3. Read `documents/00-vision.md` pillar 1 ("movement is the
   protagonist") and pillar 7. Game-feel is the regression bar.
4. Reproduce the §3 "current movement model" diagnosis by reading the
   four file:line sites listed there. **Do not start tuning before you
   can explain in your own words why holding RIGHT during a slope
   descent kills the slope's momentum.**
5. Run `make shot SCRIPT=tests/shots/m6_movement_probe.shot` to
   establish the pre-change baseline visual.
6. Then start §6 — Phase 1.

### What the user asked for, verbatim

> Multiple people reported that moving side to side is not what they
> expect, on the ground or in the air. The game should allow players
> to do things like gain momentum sliding down a slope and then run
> out from the bottom of the slope quicker, jump higher, more
> parabolic motion, right now jump and jetpack do not allow you to
> move very far right / left. On the ground, take some inspiration
> from sonic the hedgehog. We want the movement to be fun, but still
> competitive Soldat style at its core.

Stake those goals into the wall:

1. **Slope momentum carries.** Slide down a slope → run off the
   bottom faster than RUN_SPEED would let you. (Currently: the moment
   you press a direction at the bottom of the slope, your velocity is
   clamped to ±RUN_SPEED — momentum lost.)
2. **Jumps go further horizontally.** Air control + jump apex height
   currently produce a short, low arc. Want a more parabolic, more
   committed arc — Soldat-feel, where a running jump throws you
   meaningfully across the screen.
3. **Jet pack moves you sideways.** Currently pure vertical thrust;
   the only horizontal motion in air comes from AIR_CONTROL (33% of
   ground speed). Jet should pull you in the direction you're holding,
   not just up.
4. **Sonic-y ground feel.** Acceleration toward top speed, deceleration
   when input released, slope acceleration adds to the speed cap.
   Top speed should be **exceedable** by external sources (slopes,
   dashes, recoil) and persist until friction grinds it down.
5. **Soldat-y aerial combat at the core.** Air control responsive
   enough to dodge a shot mid-jet, but momentum-preserving enough
   that you can't just hover-strafe. Competitive — every player has
   the same physics, no random feel.

---

## 1 — Why this matters (philosophy alignment)

- `documents/00-vision.md` pillar 1: *"Movement is the protagonist.
  The mech is a body, not a hitbox."* If movement feels bad,
  everything else feels bad.
- `documents/01-philosophy.md` rule 2: *"Pure functions where
  possible."* The movement code in `mech.c::apply_run_velocity` /
  `apply_jump` / `apply_jet_force` is already a clean function-of-
  inputs surface. Tuning means changing numbers and the
  velocity-additive-vs-velocity-setting model, not adding state.
- `documents/01-philosophy.md` rule 7: *"We commit to numbers."*
  Every change in §6 lands with a measured before/after delta in
  the commit body. The §6 "playtest checklist" is the contract.

### The Sonic / Soldat tension (the actual design problem)

Sonic and Soldat sit at opposite ends of a momentum spectrum:

- **Sonic** is **terrain-flow**: slopes accelerate the player past
  their top speed, ground friction is low, jumps preserve the
  horizontal velocity you had when you left the ground. Air control is
  weak (Sonic 2's air control is famously tiny). Top speed isn't a
  cap — it's a *baseline* you exceed via slope/spin-dash.
- **Soldat** is **aerial-combat**: jet pack rules everything, air
  control is responsive (you can dodge mid-jet), jumps are tall,
  recoil-jumps stack with jet for skilled movement. Slopes have
  some momentum effect but the player's verbs are mostly air-side.

The user wants **Soldat aerial verbs on top of Sonic ground flow.**
That's a coherent design — the bottom of the player's stack (when
their feet are on the ground) feels like Sonic; the top of the stack
(jet, jump, air combat) feels like Soldat. The two transition
through *momentum preservation across the ground↔air boundary* —
which is exactly what is broken now.

---

## 2 — Current state: tunables and code map

### Tunable table (READ THIS — these are the levers)

| Name | Value | File:line | Unit | Effect |
|---|---:|---|---|---|
| `GRAVITY` | `1080.0f` | `src/level.c:67`, `src/level_io.c:509` | px/s² | World gravity (per-level override possible) |
| `PHYSICS_VELOCITY_DAMP` | `0.99f` | `src/physics.h:22` | per-tick mult | 1% velocity drag every tick |
| `PHYSICS_CONSTRAINT_ITERATIONS` | `12` | `src/physics.h:23` | iterations | per-tick relaxation passes |
| `RUN_SPEED_PXS` | `280.0f` | `src/mech.c:150` | px/s | Base ground run speed |
| `JUMP_IMPULSE_PXS` | `320.0f` | `src/mech.c:151` | px/s | Vertical impulse (instant) on jump |
| `JET_THRUST_PXS2` | `2200.0f` | `src/mech.c:152` | px/s² | Continuous upward force while BTN_JET held |
| `JET_DRAIN_PER_SEC` | `0.60f` | `src/mech.c:153` | fuel/s | Fuel consumption rate |
| `AIR_CONTROL` | `0.35f` | `src/mech.c:154` | ratio | Air-input is 35% of ground run speed |
| `SCOUT_DASH_PXS` | `720.0f` | `src/mech.c:157` | px/s | Scout's BTN_DASH burst (ground-only) |
| floor friction (flat) | `~0.92` | `src/physics.c:365` | per-tick mult | `0.99 - 0.07·|ny|` (max friction at ny=-1) |
| slope friction (45°) | `~0.94` | same | | |
| slope friction (60°) | `~0.955` | same | | |
| ICE override | `0.998` | `src/physics.c:367` | per-tick mult | TILE_F_ICE bypasses slope formula |
| Jet ceiling taper | 64 → 24 px | `src/mech.c:641-642` | px | Thrust scales to 0 within 24 px of ceiling |
| `MAX_FRAME_DT` | `0.25` | `src/main.c:58` | seconds | Caps frame dt for the accumulator |
| Sim Hz | `60` | `src/main.c:56` | Hz | Fixed timestep |

### Per-chassis multipliers (`src/mech.c` near line 38-95)

| Chassis | `run_mult` | `jump_mult` | `jet_mult` | `fuel_max` | `fuel_regen` | `mass_scale` |
|---|---:|---:|---:|---:|---:|---:|
| Trooper | 1.00 | 1.00 | 1.00 | 1.00 | 0.20 | 1.00 |
| Scout | 1.20 | 1.10 | 1.30 | 1.20 | 0.25 | 0.80 |
| Heavy | 0.85 | 0.85 | 0.80 | 0.85 | 0.15 | 1.40 |
| Sniper | 0.95 | 1.00 | 1.00 | 1.10 | 0.20 | 0.95 |
| Engineer | 1.00 | 1.00 | 1.10 | 1.00 | 0.25 | 1.00 |

### Per-armor multipliers (`src/mech.c:110-113`)

| Armor | `run_mult` | `jet_mult` |
|---|---:|---:|
| None | 1.00 | 1.00 |
| Light | 1.00 | 1.00 |
| Heavy | 0.90 | 0.90 |
| Reactive | 1.00 | 1.00 |

### Per-jetpack multipliers (`src/mech.c:123-130`)

| Jetpack | `thrust_mult` | `fuel_mult` | Notes |
|---|---:|---:|---|
| Baseline | 1.00 | 1.00 | |
| Standard | 1.10 | 1.20 | Default |
| Burst | 1.00 | 1.00 | + `boost_thrust_mult` on BTN_DASH |
| Glide | 0.85 | 0.70 | + `glide_thrust` lift at empty fuel |
| JumpJet | 0.00 | 1.00 | BTN_JET = re-jump, no continuous thrust |

### Computed game-feel (current tunables — Trooper / Light / Standard)

| Quantity | Formula | Value |
|---|---|---:|
| Apex height (standing jump) | v²/(2g) = 320²/(2·1080) | **47 px** |
| Time to apex | v/g = 320/1080 | **0.30 s** ≈ 18 ticks |
| Total airtime | 2·(v/g) | **0.60 s** ≈ 36 ticks |
| Air-input velocity | RUN_SPEED · AIR_CONTROL = 280·0.35 | **98 px/s** |
| Horizontal travel in air (running jump) | 98 · 0.60 | **59 px** |
| Horizontal travel at ground RUN speed in same time | 280 · 0.60 | **168 px** |
| **Air travel as % of ground travel** | 59/168 | **35%** |

For comparison, a running jump in Soldat lands roughly *where the
player would have been* if they kept running on the ground — i.e.
~100% horizontal-velocity preservation across the jump. Our current
35% is what produces the "jump kills your horizontal motion" feel.

### The smoking gun (read this code before tuning anything)

`src/mech.c::apply_run_velocity` lines 550-628. This function is the
core of the bug.

**Air-control branch** (lines 553-561):

```c
if (!grounded) {
    float vx_per_tick = vx_pxs * dt * AIR_CONTROL;
    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        physics_set_velocity_x(p, idx, vx_per_tick);
    }
    return;
}
```

Note `physics_set_velocity_x` — this **sets** the per-tick velocity
to `vx_pxs · dt · 0.35`, **regardless of what the velocity already
was.** So:

- Jump up while running RIGHT at 280 px/s. The moment your feet
  leave the ground, the next tick clamps your X velocity to
  98 px/s (35% of 280). **Two thirds of your horizontal momentum
  vanishes the instant you become airborne.**
- Jet right while moving at 400 px/s (you got there via Scout dash
  or recoil). Same thing — velocity clamped to 98 px/s.

**Ground-input branch** (lines 596-627): projects the run input
onto the slope tangent and **sets** velocity tangent-aligned at
`vx_pxs · dt`:

```c
float vt_per_tick_x = tx * speed * dt;
float vt_per_tick_y = ty * speed * dt;
for (int part = 0; part < PART_COUNT; ++part) {
    physics_set_velocity_x(p, idx, vt_per_tick_x);
    physics_set_velocity_y(p, idx, vt_per_tick_y);
}
```

So when you've slid down a slope at 450 px/s and press RIGHT at the
bottom, your X velocity is **set** to 280 px/s — slope momentum
deleted.

**No-input + flat ground branch** (lines 588-594):

```c
if (vx_pxs == 0.0f) {
    if (!flat) return;
    for (int part = 0; part < PART_COUNT; ++part) {
        physics_set_velocity_x(p, m->particle_base + part, 0.0f);
    }
    return;
}
```

Releasing inputs on flat ground **snaps** velocity to 0 — no decel
curve, no slide. Sonic this is not.

### Other relevant code paths

- **`apply_jump`** (`src/mech.c:630-639`) — sets vy directly to
  `-jump_pxs · dt`. **Doesn't preserve any existing upward velocity**
  (you can't double-source a jump by stacking the impulse). Doesn't
  touch horizontal velocity — so horizontal momentum at the moment of
  jump *is* preserved by the jump itself, but then immediately wiped
  by `apply_run_velocity`'s air-control branch on the next tick.
- **`apply_jet_force`** (`src/mech.c:644-711`) — adds `fy = -thrust ·
  scale · dt²` to every particle's `pos_y`. **Pure vertical** unless
  the ceiling-tangent branch fires (only when `PARTICLE_FLAG_CEILING`
  is set on the particle). No horizontal input is consumed for jet
  direction.
- **Gravity** (`src/physics.c:15-53`) — applied per-tick to every
  particle with `inv_mass > 0`. Default 1080 px/s² = `+0.30 px/tick`
  vy. This is **5× heavier than Soldat's reference** GRAV ≈ 0.06
  px/frame (which at 60 Hz works out to ~216 px/s²).
- **Slope-tangent friction** (`src/physics.c:349-375`) — final
  iteration of the relaxation loop applies tangential velocity
  damping at every contact. The friction formula `0.99 - 0.07·|ny|`
  gives less friction on steeper slopes (good — that's how slopes
  produce passive slide). But the per-tick velocity damp is *every*
  tick contact happens, so even on a steep slope you grind down to
  ~0 if you sit still long enough.
- **`physics_set_velocity_x` / `_y`** (`src/physics.h:90-95`) — these
  are inline helpers that overwrite `prev` to produce a given
  per-tick velocity. They are the "set, don't add" gateway. The
  whole movement model is built around these.

---

## 3 — Diagnosis: why current movement feels wrong

Five concrete claims, each with a code reference:

1. **Side-to-side is "snappy" because velocity is SET, not added.**
   `physics_set_velocity_x` (`src/physics.h:90`) overwrites `prev`
   to produce a target velocity. The input loop calls this every
   tick the player holds left/right (`mech.c:858-866`). Consequence:
   no momentum carryover across input changes; no Sonic-like accel
   curve; the player's max attainable horizontal speed is *exactly*
   `RUN_SPEED_PXS · run_mult · ar->run_mult`, even on a slope.

2. **Slopes can produce passive slide but pressing a direction
   wipes it.** Gravity adds 0.30 px/tick downward each tick.
   Slope-tangent friction (0.92-0.99) leaves some of that as
   tangent velocity. Sit on a 60° slope with no input — you slide.
   Press LEFT or RIGHT — `apply_run_velocity` SETs your velocity
   to ±RUN_SPEED-along-tangent. Slope's momentum gone.

3. **Jumps are short and low.** Apex 47 px, airtime 0.60 s. For
   a 2D side-scroller running at the M5/M6 zoom (camera follows
   pelvis, the mech is ~80 px tall on screen at default zoom),
   that means the player goes up by less than the mech's own
   height. Soldat-feel wants apex ≈ mech-height to 1.5×
   mech-height — call it ~80-120 px.

4. **Jet only goes up.** `apply_jet_force` adds straight-vertical
   `fy` to every particle (`mech.c:708`). No reading of
   `latched_input` for L/R sign except the ceiling-tangent
   branch (`mech.c:684-686`), which only fires when the head is
   already up against an angled ceiling. So in open air, BTN_JET
   gives you straight-up thrust; the only horizontal motion you
   can produce mid-jet is via AIR_CONTROL = 98 px/s. Useless for
   covering distance.

5. **Air control number itself is too low.** Even if we kept the
   set-velocity model, AIR_CONTROL = 0.35 means the in-air max
   horizontal velocity is 98 px/s — that's slower than a Soldat
   walk. Soldat's air-control is roughly equivalent to ~0.6-0.7
   of ground speed.

### What ISN'T broken

- Gravity at 1080 px/s² **feels heavy but right** for the mech
  aesthetic; mechs are heavy machines. **Do not lower gravity** as
  the first move — that lengthens airtime, which compounds with
  the slow-air-control issue rather than fixing it.
- Slope-aware friction formula is good. The slope-tangent
  projection in `apply_run_velocity` is good. The bug is in HOW
  the projection result is applied (SET not ADD).
- The Verlet integrator is fine. The constraint solver is fine.
  Movement tuning is on the input → velocity surface, not on
  physics internals.

---

## 4 — Reference points

### Soldat constants (approximated; cross-check against
`reference/soldat-constants.md` if present, otherwise treat these
as starting points)

| Soldat | Reference value (60 Hz) | Soldut current | Note |
|---|---:|---:|---|
| Gravity | ~216 px/s² | 1080 px/s² | Soldut is 5× heavier — keep that |
| Run speed | ~425 px/s | 280 px/s | Soldut slower; could bump |
| Jump impulse | ~620 px/s | 320 px/s | **Soldat ~2× higher** |
| Jet thrust | ~3500 px/s² | 2200 px/s² | Soldat ~60% stronger |
| Air control | ADD-with-cap, ~0.6× | SET 0.35× | **Different model entirely** |

(These reference numbers are *guides*, not exact targets. The point
is to show that the Soldat model adds force, has a higher cap, and
preserves momentum across the ground-air boundary.)

### Sonic physics primer (the canonical reference is the *Sonic
Physics Guide*)

Sonic 1/2/3 ground physics, in our units:

- **`acc`** (acceleration): velocity is `min(top_speed, vx + acc)`
  per tick when input held in direction of motion. Sonic has
  `acc ≈ 168 px/s²` (slow ramp-up — feels weighty).
- **`dec`** (deceleration when input opposed): `vx - dec` per tick.
  Sonic has `dec ≈ 1800 px/s²` (sharp braking — feels responsive).
- **`frc`** (friction when no input on ground): `vx · (1 - frc·dt)`.
  Sonic `frc ≈ acc` so passive deceleration matches active accel.
- **`top`** (top speed cap, INPUT only): `vx ≤ top` while accel
  toward top. **External sources (slopes, springs, dashes) can push
  vx ABOVE top** and friction grinds it back down.
- **Slope momentum:** gravity-along-tangent adds to `vx` every tick
  (gain going down, lose going up). Net result: top of a hill +
  hold direction = launched off the bottom faster than `top`.
- **Air physics:** weaker accel (~0.5× ground), no friction (no
  ground contact), but momentum preserved 100% across jump.

The translation to mechs (this is the design target):

| Param | Proposed start value | Rationale |
|---|---:|---|
| ground_accel | `RUN_SPEED / 0.10s = 2800 px/s²` | 6 frames to top speed |
| ground_decel (opposed) | `RUN_SPEED / 0.06s = 4666 px/s²` | 4 frames to reverse |
| ground_friction | `RUN_SPEED / 0.20s = 1400 px/s²` | 12 frames to stop on flat |
| air_accel | `0.6 · ground_accel = 1680 px/s²` | 60% — Soldat-y |
| top_input_speed (cap on input-accel) | `280 px/s` | unchanged for input feel |
| slope_accel_gain | gravity·sin(slope) | already implicit via Verlet |
| jump_impulse | `480 px/s` (50% up from 320) | apex 107 px ≈ mech-height |
| jet horiz component | `0.5 · JET_THRUST_PXS2` toward input | Soldat-style |

---

## 5 — The design proposal (what we're going to change)

### A. Switch from "set velocity" to "add force toward target"

The single most important change. Replace the SET-velocity model
with an ADD-toward-target-speed model:

```c
// Pseudocode for ground move
float vx_now = pos_x[i] - prev_x[i];   // current per-tick vx
float vx_target = run_speed * input_dir * dt;
float vx_delta  = vx_target - vx_now;
// Cap the per-tick acceleration so the player ramps to top speed
// over `ground_accel_time` seconds, not in one tick:
float max_delta = ground_accel * dt * dt;
if (vx_delta >  max_delta) vx_delta =  max_delta;
if (vx_delta < -max_delta) vx_delta = -max_delta;
prev_x[i] -= vx_delta;   // i.e., new_vx = vx_now + vx_delta
```

Critical property: if `vx_now > vx_target` (you're moving FASTER
than RUN_SPEED because of a slope or dash), `vx_delta` is **negative
but capped** — so the slope momentum bleeds off naturally over
multiple ticks, not instantly. *External sources can push you past
the cap; input cannot.*

### B. Higher jump

`JUMP_IMPULSE_PXS 320 → 480`. Apex 47 → 107 px. Airtime 0.60 →
0.89 s. Combined with §A's momentum preservation, a running jump
now covers ~0.89 × 280 = **250 px** horizontally instead of the
current 59 px.

### C. Jet pack horizontal thrust

Read `latched_input.buttons & BTN_LEFT/RIGHT` inside
`apply_jet_force`. Apply a horizontal acceleration component
toward the held direction at ~50% of vertical thrust. So holding
JET + RIGHT thrusts up + right; pure JET = up only (preserving
current behavior for "I just want to climb").

### D. Air control number

`AIR_CONTROL 0.35 → 0.65`. Combined with §A (additive model with
preservation), this gives Soldat-like responsiveness — you can
nudge your trajectory mid-jet, but you can't reverse 280 px/s of
horizontal momentum in a single direction-flick.

### E. Slope friction tuning

Verify slope friction is **enough to bleed off slope momentum
over time** but **not enough to wipe it in one tick**. Current
formula `0.99 - 0.07·|ny|` may need recalibration after §A lands.

### F. Decel curve on input release

In §A's model, when `input_dir == 0`, set
`vx_target = 0` and let the same accel-capped lerp bring you down.
The "snap-to-0" branch goes away — you skid to a stop over ~10
ticks on flat ground. Slope: gravity-along-tangent dominates, you
continue to slide.

### What this proposal explicitly does NOT change

- Gravity (keep 1080 px/s² — mechs are heavy; the heaviness IS
  the feel).
- Velocity damp (keep 0.99; that's already light drag).
- Constraint iterations (keep 12).
- Per-chassis run/jump/jet multipliers (relative balance is fine).
- The Verlet integrator (don't touch).
- Jet ceiling taper (keep — prevents over-ceiling abuse).
- Jet fuel drain rate (keep, balance later).
- Slope-tangent projection direction (keep, just stop setting
  velocity at it — accumulate to it instead).

---

## 6 — Phased implementation plan

Each phase is a single commit. Each commit gets a measured
before/after delta in its body — pelvis trail from the shot probe,
plus a one-line "feel" report after the user playtests. Per rule 7
("we commit to numbers") and the M6 P06 fix-application discipline.

**The user will playtest each phase before the next ships.** Don't
batch.

### Phase 1 — Add-toward-target ground movement (§5A, ground only)

Goal: pressing RIGHT at the bottom of a slope no longer wipes
slope momentum.

Touch: `src/mech.c::apply_run_velocity`, ground branch only.

1. Add `ground_accel`, `ground_decel`, `ground_friction` constants
   alongside `RUN_SPEED_PXS` (with the values from §4).
2. Rewrite the ground branch to read current vx via
   `(pos_x[i] - prev_x[i])`, compute target vx (slope-projected as
   today), compute `delta = target - vx_now`, cap delta by accel,
   apply delta via `prev_x[i] -= delta`.
3. No-input branch: `target = (0, 0)`, same accel-capped lerp. On a
   slope, skip the input-pull (gravity-along-tangent will
   accelerate you naturally). On flat, apply friction-decel.
4. Test plan:
   - `make shot SCRIPT=tests/shots/m6_movement_probe.shot` —
     visual A/B before/after.
   - `make shot SCRIPT=tests/shots/m5_slope.shot` — passive slide
     should still happen; pressing RIGHT at the bottom should now
     produce a faster run-out than before.
   - `make test-physics` + `make test-mech-ik` + `make
     test-pose-compute` must still pass.
   - Paired `tests/shots/net/run.sh 2p_basic` — bot/remote mechs
     should still walk normally.
5. **Playtest gate.** User plays a round, reports feel. Don't
   move to Phase 2 until user signs off.

### Phase 2 — Add-toward-target air movement (§5A, §5D for air)

Goal: a running jump preserves horizontal momentum; mid-air
input nudges trajectory rather than snapping to it.

Touch: `src/mech.c::apply_run_velocity`, air branch.

1. Replace the SET-velocity air branch with the same
   accel-capped lerp model, using `air_accel` and a different
   (lower) friction.
2. Bump `AIR_CONTROL` 0.35 → 0.65 (or replace AIR_CONTROL entirely
   with a separate `air_accel` constant).
3. Test plan: same as Phase 1. Pay particular attention to the
   jump panels in `m6_movement_probe.shot` — the airborne distance
   should clearly grow.
4. **Playtest gate.** Watch for: jet+air-control combo feels right;
   no mid-air "ice skating"; can still dodge a shot mid-jet.

### Phase 3 — Higher jump (§5B)

Goal: a jump goes up roughly mech-height (~80-120 px).

Touch: `src/mech.c:151` — `JUMP_IMPULSE_PXS 320 → 480`. Possibly
also retune per-chassis `jump_mult` to keep the relative balance.

Test plan: probe shot. **Watch for**: head no longer hits ceiling
on jump in Reactor's pillar archway (96 px tall — at 107 px apex
you'll headbutt the ceiling on a standing jump there). May need
to retune to 440 if the headroom is tight on shipped maps.

**Playtest gate.** Reactor + Catwalk + Citadel — confirm the jump
height feels right and ceilings are not problematic.

### Phase 4 — Jet pack horizontal (§5C)

Goal: holding JET + RIGHT moves you up + right; pure JET stays up.

Touch: `src/mech.c::apply_jet_force` — read
`m->latched_input.buttons & BTN_LEFT/RIGHT`, add horizontal `fx`
proportional to thrust.

Test plan:
- Probe shot (jet+right panels at t=500..540).
- Re-run `tests/shots/net/run_frag_grenade.sh` — grenade arcs
  may interact with jet behavior; verify no regressions.
- Bake test `tools/bake/run_bake.c` — bots already use BTN_JET
  for pathing; their behavior may shift if jet now has lateral
  component. Re-run bot tier verdicts.

**Playtest gate.** User confirms jet-flight feels right (can cover
distance horizontally while jetting, but doesn't fly-spam too far).

### Phase 5 — Slope momentum verification (§5E)

Goal: slope momentum is *additive* on top of run cap, and friction
bleeds it gracefully.

Touch: probably no code changes — Phase 1 should already give us
this for free. Phase 5 is a measurement-only pass that confirms
slope-down then run-out produces speeds > RUN_SPEED that decay
toward RUN_SPEED over a measurable window.

If passive slide grinds out too fast or carries too long, retune
`floor friction (flat)` and the slope coefficient in
`src/physics.c:365`.

### Phase 6 — Recoil & dash momentum compatibility check

Goal: confirm Scout's BTN_DASH (`SCOUT_DASH_PXS = 720`) and weapon
recoil now produce *cumulative* speed boosts instead of clamped-by-
cap speeds.

The Phase 1/2 changes make this work for free IF
`SCOUT_DASH_PXS > RUN_SPEED_PXS`, which it is (720 > 280). With
the additive model, a Scout dash adds 720 px/s, then ground
friction bleeds it down toward 280 over ~12 frames. Should feel
*great*.

Test plan: visual via probe shot; bake regression for Scout bots.

### Phase 7 — Network sync validation

Goal: confirm none of the movement changes break client-server
position prediction or remote-mech interpolation.

This is the trickiest non-feel risk. The new movement code reads
`pos - prev` to compute current velocity, which is the same data
the snapshot system encodes via `pos` + `prev_*` per particle.
But the SERVER's tick rate is 60 Hz and the CLIENT's prediction
runs at 60 Hz from the same code, so prediction-reconciliation
should remain aligned.

Test plan:
- `tests/net/run.sh` — 3-process basic connectivity
- `tests/net/run_3p.sh` — 3 players over the wire
- `tests/shots/net/run.sh 2p_basic` — paired byte-identical (paired
  shots may DIFFER post-tuning because movement is different; the
  comparison goal is that host and client see the SAME diff, not
  that the diff is zero relative to pre-tuning).
- `make test-frag-grenade` — paired-dedi explosion sync.

If any paired-process tests show *host-vs-client divergence* (not
post-tuning frame-byte diff, but the SHAPE of the divergence
between host and client logs/PNGs), stop and fix before any other
phase moves.

### Phase 8 — Update CURRENT_STATE.md + commit body for the merged work

Goal: leave the next session a clear record of what was tuned and
why. Per `documents/01-philosophy.md` rule 10 + the M6 P06 commit
pattern.

- Update CURRENT_STATE.md "Tunables (current values)" table with
  the new constants.
- Update `documents/03-physics-and-mechs.md` §"Movement" if the
  shipping numbers diverge meaningfully from the spec.
- Open the PR with a SUMMARY of each phase's playtest verdict
  attached.

---

## 7 — How to test (the harness)

### The bench harness (already shipped in M6 P06)

```bash
./soldut --bench 20 --bench-csv build/perf/movement-after.csv \
         --window 1920x1080
```

The CSV will tell you if movement changes induced a frame-time
regression (they shouldn't — the changes are velocity-arithmetic
inside the existing tick).

### The movement probe shot (added in this session)

```bash
make shot SCRIPT=tests/shots/m6_movement_probe.shot
```

Produces `build/shots/m6_movement_probe/probe_sheet.png` — 14 cells
showing standing → running → jumping → landing → post-land →
stopped. Re-run after each phase and visually compare.

**The probe captures the running-jump scenario only.** Add more
probe shots for slope + jet as needed in the corresponding phase.
Suggested:
- `tests/shots/m6_movement_slope.shot` — slope descent → flat run-out
- `tests/shots/m6_movement_jet.shot` — jet + horizontal hold

(Templates: copy `m6_movement_probe.shot` and edit the inputs +
spawn_at + tick layout.)

### The user playtest (the only gate that matters)

After each phase, the user plays a real round (single-player + 1
bot is enough). Report:

- "Side-to-side feels [right / floaty / snappy]"
- "Jumps feel [right / short / too tall]"
- "Jet feels [right / sluggish / OP]"
- "Slope momentum carries [yes / no / too much]"

If feel is right, ship the phase. If not, retune (don't add new
mechanisms; just adjust the numbers from §5).

---

## 8 — Risks and watch-fors

1. **Network desync.** Server and client both run the same
   movement code, so as long as the inputs match (which they do —
   `NET_MSG_INPUT` is unchanged), positions converge via the same
   physics. Phase 7 has the regression sweep. If a problem shows
   up, the most likely culprit is the velocity read
   `(pos_x[i] - prev_x[i])` differing between server (running
   physics) and client (running predict over latched_input) — but
   that's the SAME read both sides do today for snapshot encoding,
   so this should be safe.

2. **Collision tunneling at higher speeds.** §5B (higher jump) and
   §5A (slope momentum) both raise the maximum per-tick particle
   velocity. The Verlet integrator already does a swept-collision
   ray check (`physics.c:74-104`) so tunneling should be caught —
   but if you see body parts ending up inside walls after a fast
   slope descent, the swept check may need a tighter epsilon.

3. **Pose drift under high velocity.** The procedural pose
   (M6 P01) writes bone positions kinematically each tick. At
   higher velocities the per-tick translation is larger, which
   means a momentary mismatch between the pose-computed bone
   position and the pre-physics particle position is larger. This
   could surface as visible "pop" on landing. Verify with the
   probe sheet — look at the t005-t030 frames during the jump for
   any limb-flicker.

4. **Bot AI regression.** Bots use BTN_LEFT/RIGHT/JUMP/JET via the
   same `mech_step_drive` path; if movement tuning makes bots
   walk through walls or fall off ledges they used to handle, the
   bake test (`tools/bake/run_bake.c`) will catch it. Run
   `make bake-all` after Phase 4.

5. **Tile-collision contact normals.** `apply_run_velocity` reads
   `contact_nx_q / contact_ny_q` to detect slopes. With higher
   velocities, the contact normal from the previous tick may be
   one tick more stale — i.e., on a steep transition (60° to 5°
   slope abrupt boundary), the runner may briefly behave as if
   still on the 60° tangent for one tick. This is already happens
   today; just be aware that the *symptom* changes (more visible
   forward-momentum past the transition).

6. **CTF carrier balance.** §5C (horizontal jet) may make flag
   carriers harder to catch — the 50% jet thrust penalty
   (`ctf_is_carrier`, `mech.c:649-651`) still applies, but if
   that 50%-of-thrust-up has a 50%-of-thrust-sideways component
   now, the carrier moves laterally faster than before. May need
   to reduce horizontal-jet contribution for carriers, or boost
   the carrier penalty.

7. **Heavy chassis at low jump_mult.** Heavy's `jump_mult = 0.85`
   means after Phase 3 (`JUMP_IMPULSE 320→480`), Heavy jumps at
   480·0.85 = 408 px/s = apex 77 px. Trooper at 480·1.00 = apex
   107 px. Scout at 480·1.10 = apex 130 px. Verify these feel
   right — Scout shouldn't be touching the 96-px archway every
   jump.

---

## 9 — Where this work lives

- **Branch:** `m6-perf-profiling` was just merged. Switch to (and
  stay on) **`m6-movement-tuning`**, which was created from main
  post-merge.
- **One PR per phase.** Don't batch. The user is in the loop
  between phases.
- **Commit messages:** per the M6 P06 pattern. Format:

  ```
  M6 P07 Phase N — short title
  
  <2-4 sentence summary of what changed and why>
  
  Measured (probe shot, pelvis-x delta over ticks 110-150):
    before: <number>
    after:  <number>
  
  Playtest verdict (Y/N + 1 sentence): "<feel>"
  
  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  ```

- **No new shipped doc** beyond updates to CURRENT_STATE.md and
  03-physics-and-mechs.md. This plan doc itself stays — but at
  the end of the iteration, mark each phase with a one-line
  "LANDED in commit <sha>" so future-Claude can see what shipped.

---

## 10 — Anti-patterns to reject

Per `documents/01-philosophy.md` and what would tank this work:

- **No new state on Mech.** Don't add `vx_smoothed` or
  `velocity_carryover` fields. The Verlet `prev` already encodes
  velocity; that's the surface.
- **No new threading.** Movement is hot per-tick but well within
  budget (sim p99 ≈ 0.12 ms in profile data). No.
- **No SIMD.** Per `10-performance-budget.md` SIMD is the LAST
  resort. Per-particle work in the run loop is 16 × N-mechs ≈
  256 particles at 32 mechs. Trivial.
- **No new config knobs in soldut.cfg.** The tunables stay
  `#define`s in code. Per philosophy rule 9.
- **No "movement profiles" abstraction.** Don't add `MovementProfile
  *m = mech_movement_profile(chassis)`. The per-chassis
  multipliers (`run_mult`, `jump_mult`, `jet_mult`) ARE the
  parameterization.
- **No backwards-compat shim.** When you change `apply_run_velocity`
  to the additive model, change it. Don't leave the SET path
  behind a flag.
- **No "while I was in there" refactor of apply_jet_force or
  apply_jump.** Those touch different verbs; tune them in their
  own phases.
- **No feature flag.** `#define MOVEMENT_V2 1` — no. The new
  movement either ships or doesn't.

---

## 11 — Quick reference: pelvis particle index

Useful when reading the probe-shot log or writing new probe scripts.

```c
int pelv = m->particle_base + PART_PELVIS;
float vx = w->particles.pos_x[pelv] - w->particles.prev_x[pelv];  // px/tick
float vx_per_sec = vx * 60.0f;                                    // px/s
```

`PART_PELVIS` is the body's anchor — track it for the
"what's the player's velocity" question.

To get a per-tick pelvis trail in shot mode, add a SHOT_LOG line
in `mech_step_drive` or `simulate_step`:

```c
int pelv = m->particle_base + PART_PELVIS;
SHOT_LOG("move t=%llu mech=%d pos=(%.1f,%.1f) v=(%.1f,%.1f) grounded=%d",
         (unsigned long long)w->tick, m->id,
         (double)w->particles.pos_x[pelv], (double)w->particles.pos_y[pelv],
         (double)(w->particles.pos_x[pelv] - w->particles.prev_x[pelv]) * 60.0,
         (double)(w->particles.pos_y[pelv] - w->particles.prev_y[pelv]) * 60.0,
         (int)m->grounded);
```

`SHOT_LOG` is a no-op outside shot mode, free to leave in.

---

## 12 — Findings from the playtest pass that produced this doc
(2026-05-14)

What the contact sheet of `m6_movement_probe.shot` shows under the
*current* (pre-tune) movement model:

- **Standing → running → jumping → landing.** The mech moves
  horizontally during the airborne window, but the per-tick rate
  visibly slows the moment the feet leave the ground (the screen
  scrolls less per frame in jump_t005-t030 than in run_t020-t040).
  Confirms the AIR_CONTROL = 0.35 clamp is firing.
- **Landing → continued run.** After landing at t≈148, the
  player keeps running because BTN_RIGHT was held throughout.
  But there's NO visible "settle" frame — the mech doesn't keep
  its airborne velocity briefly; it snaps back to ground RUN_SPEED.
  Confirms the SET-velocity ground path is the canon.
- **Stop on release.** At t=220 RIGHT is released; at t=240
  (`stopped_t020`) the mech is **stopped, not coasting**.
  Confirms the no-input-on-flat snap-to-0 branch
  (`apply_run_velocity` line 588-594).

No surprises in the diagnosis. The data confirms the analytical
read of the code.

---

## 13 — Implementation log (this branch, m6-movement-tuning)

Each entry: commit sha, what shipped, playtest verdict, what's next.
Append a new entry after every pass so the next session can resume
from the bottom of this log without rediscovering context.

### Phase 1 — `63ad669` (2026-05-14) ✅ shipped

- **Code:** `src/mech.c::apply_run_velocity` ground branch — replaced
  SET-velocity with a per-component lerp-toward-target. Constants
  `GROUND_ACCEL_PXS2=2800`, `GROUND_DECEL_PXS2=4666`,
  `GROUND_FRICTION_PXS2=1400`. Air branch untouched (Phase 2).
- **Probe trail:** release-skid 4.6→0 in 1 tick → 3.7→0.01 over
  ~10 ticks (visible coast).
- **Playtest:** "side-to-side fine, release skid fine. Slope:
  collapses going up, stuck at slope→flat boundary, no momentum
  carry, transition doesn't transfer velocity."
- **Next:** Phase 1.1 — fix slope behavior.

### Phase 1.1 — `fbda156` (2026-05-14) ✅ shipped

- **Code:** Two fixes layered on Phase 1, same function:
  1. **Above-cap guard** — when `vt_now >= vt_target`, return early
     (input is a CAP, not a target). v1 was lerp-toward-target which
     bled slope momentum back to RUN_SPEED.
  2. **Tangent-aligned delta** — per-component cap on (dx, dy)
     produced a normal-axis push that lifted the foot off the slope
     mid-climb. New code projects the per-tick delta onto the slope
     tangent only.
- **Aurora trail:** climb the west hill (640→1024) in ~120 ticks;
  pelvis vx steady at ~3.8 climbing.
- **Playtest:** "Better. Sliding down feels alright. Walking up the
  ramp the body is not upright — looks like the mech is collapsing /
  diving forward. Want it to look like walking up the ramp."
- **Next:** Phase 1.2 — slope-aware pose so the body reads upright
  while feet plant on the slope.

### Phase 1.2 — slope-aware foot placement, body stays upright ✅ shipped

- **Code:**
  - `src/mech_ik.h` — added `ground_normal` to `PoseInputs` (averaged
    foot contact normal; `(0, 0)` = airborne / unknown → flat
    fallback).
  - `src/mech_ik.c::pose_compute` ANIM_RUN — replaced fixed
    `foot_y_ground = lhip.y + chain` with slope-aware
    `foot.y = hip.y + chain + fx * slope_dydx`. Stride stays
    horizontal so the body reads as gravity-upright; one leg extends
    and one compresses to track the slope. Flat-ground
    (`ground_normal = (0, -1)`) collapses to the original formula
    (zero `slope_dydx`).
  - `src/simulate.c` — pass averaged foot contact normal into
    `PoseInputs`.
  - `src/shotmode.c` — new `no_parallax` directive (single token, no
    value): zeros `g_map_kit.parallax_*.id` after world build so
    movement-tuning shots don't have parallax obscuring the mech
    silhouette. `draw_parallax_layer` already early-outs on
    `tex.id == 0`.
  - `tests/shots/m6_aurora_slope.shot` — new shot driving the climb
    + descent + walk-back on aurora's west hill.
- **Aurora trail (post-fix):** climb to peak in ~120 ticks
  (730 → 956 → 1080 px x); descent picks up ~5.4 px/tick from
  gravity-along-tangent + the above-cap guard preserving it.
- **Tests:** `test-mech-ik` ALL PASS, `test-pose-compute` ALL PASS,
  `tests/shots/net/run.sh 2p_basic` 12/12 PASS, flat-ground probe
  trail unchanged (verified bit-for-bit vs. Phase 1.1).
- **Caveats:**
  - Remote mechs on a pure client run kinematically (`inv_mass = 0`),
    so the local `contact_*_q` slots never update and their
    `ground_normal` defaults to flat. Cosmetic difference vs. the
    host's view on slopes — bones still in correct positions because
    the snapshot drives pelvis directly, just leg pose is flat-ground
    on the spectator view. Track as a follow-up if the user notices.
  - Sliding down the slope still cycles the RUN gait (legs walking)
    rather than a dedicated SLIDE pose. Not addressed here; deferred
    if the user reports it.
- **Playtest:** pending.
- **Next:** if pose feels right, **Phase 2** — air-control
  add-toward-target (per §6 Phase 2). If pose still wrong, retune
  here.

### Phase 1.2 — regression sweep + spawn fix ✅ shipped

After Phase 1.2 the user asked for a regression sweep on bots /
network play and a spawn-point validation (concern: are mechs
spawning inside terrain?).

**Network sync — pre-existing lag, NOT a regression.** Ran
`tests/shots/net/run.sh 2p_motion` on Phase 1.2 and on
`63ad669^` (parent of Phase 1). Both versions show ~500 px lag in
the client's view of the host's pelvis at t=460 (host at x=1198,
client sees host at x=691). Same shape, same magnitude. The
position desync exists on `main` and is unrelated to the movement
work in this branch. Worth filing as its own bug — likely a
snapshot rate / interp delay issue — but out of scope for M6 P07.

**Bot bake on aurora — pre-existing FAIL.** `build/bake_runner aurora
--bots 4 --duration_s 30` returns 0 fires / 0 kills / 1 pickup grab on
both Phase 1.2 and `63ad669^`. Bots are not engaging on aurora
regardless of my changes. Aurora's nav graph (`bot_nav`: 111 nodes,
3382 dead cells per heatmap) is too sparse for the crude wandering
AI to find enemies. Tracked separately; fixing it is a bot-AI / nav
job, not a movement job.

**Spawn validation — found and fixed 2 embedded spawns on aurora.**
New test `tests/spawn_geometry_test.c` (`make test-spawn-geometry`)
loads every shipped `.lvl` and samples 4 body points per spawn
(pelvis / head / L+R shoulders) against `level_point_solid`. Foot
is intentionally not sampled — a properly-placed spawn lands the
foot on the floor surface, which is solid by construction.

  - `aurora.lvl` spawn[6] (red, lane 6) at world (128, 1720) AND
    spawn[14] (blue, lane 14) at (4992, 1720) were INSIDE the peak
    rise. The cooker's `peak_spawn = peak_top - 40` used `peak_top`
    (top of the peak BODY at row H-35 = y 1760) but the rise sits
    above the body (rows H-38..H-35 = y 1664..1760). 40 px above
    `peak_top` lands inside the rise. Fixed by
    `peak_spawn = t2w(H - 38) - 40` (40 px above the actual apex).
  - All other 7 maps + `grapple_test.lvl` + `ctf_test.lvl` PASS.

**Tests after fix:**
  - `test-spawn-geometry` — all 10 maps, 64 body samples, all clear
  - `test-spawn` / `test-snapshot` / `test-pickups` / `test-ctf` /
    `test-mech-ik` / `test-pose-compute` — all PASS
  - `tests/net/run.sh` (3-process basic) — 13/13 PASS
  - `tests/net/run_3p.sh` (3 players) — 10/10 PASS
  - `tests/shots/net/run.sh 2p_basic` — 12/12 PASS
  - bake aurora post-fix — same pre-existing 0-kills FAIL

**What still hurts (pre-existing, separate issues):**
  - Snapshot position lag on the client view of remote moving mechs
    (~500 px at full RUN_SPEED). Not a Phase 1.x regression.
  - Bots on aurora don't engage (nav-blocked). Not a Phase 1.x
    regression.
  - Bots reportedly get stuck at slope→tile transitions in the user's
    playtest. Not reproduced in the shot harness yet — needs a
    visual bot test (host with `--bots`, shot client observer with
    `network connect`). Deferred to next pass if it shows up.

### Phase 2 — air-control add-toward-target ✅ shipped

- **Code:** `src/mech.c::apply_run_velocity` air branch — replaced
  SET-velocity (`vx_per_tick = vx_pxs · dt · AIR_CONTROL`) with the
  same accel-capped lerp model as the ground branch. New constant
  `AIR_ACCEL_PXS2 = 1680` (~10 frames to RUN_SPEED in air, 0.6×
  ground accel). `AIR_CONTROL = 0.35` deleted per philosophy rule 8
  ("no abstraction we cannot delete in an afternoon") + the plan
  doc's anti-pattern list ("no backwards-compat shim"). Same
  CAP-not-target invariant as the ground branch: when `v_in_dir >=
  vx_cap` (running jump, slope-launch, recoil pushing past
  RUN_SPEED in the input direction) the air branch returns without
  doing anything; only `PHYSICS_VELOCITY_DAMP` (0.99/tick) bleeds
  excess. Single accel rate in air (no separate decel) — flicking
  the opposite direction mid-jet reverses over ~20 frames, which
  is the Soldat-y inertia we want.

- **Probe trail (`tests/shots/m6_movement_probe.shot`):**
  Pre-Phase-2: vx clamped to 1.56 px/tick (94 px/s = `280·0.35·dt`)
  immediately after foot-lift; **horizontal travel in airborne
  window ≈ 52 px**. Post-Phase-2: vx steady at 4.62 px/tick
  (277 px/s — at the cap, drag-trimmed); **horizontal travel in
  airborne window ≈ 190 px (3.6× improvement)**.

- **Probe-shot fix:** `tap jump` (single-tick BTN_JUMP press)
  changed to `press jump @ 108 / release jump @ 115`. The running
  gait briefly lifts feet off the ground every 5-6 ticks; the
  jump path (`mech.c:1022`) only fires when
  `(in.buttons & BTN_JUMP) && grounded` are both true on the same
  tick. A 1-tick press can miss the grounded=1 window when Phase 2
  preserves enough momentum across gait foot-lifts to shift the
  bouncy grounded pattern relative to baseline. The 6-tick hold is
  effectively single-shot (once airborne, the check fails until
  next landing) so no double-jump risk.

- **Tests:** `test-mech-ik` ALL PASS, `test-pose-compute` ALL PASS,
  `test-snapshot` all passed, `tests/shots/net/run.sh 2p_basic` 12/12
  PASS, `tests/net/run.sh` 13/13 PASS, slope shot
  `m6_aurora_slope.shot` slope-launched slide still reaches vx=
  -11.45 px/tick (245 % of RUN_SPEED cap — no regression in
  slope-momentum preservation).
- **Pre-existing FAIL (not Phase 2):** `test-spawn-geometry` reports
  `slipstream.lvl spawn[6]/[7]` head samples at (640, 484) /
  (2560, 484) inside SOLID polygon. Verified by `git stash` —
  identical failure on `3626e36` (parent of Phase 2). The Phase 1.2
  spawn fix from `742c0ad` and the `3626e36` mech-clipping fix
  covered FEET clipping but missed HEADs in this corner. Tracked
  for a future spawn-fix pass.

- **Playtest:** pending.
- **Next:** if jump feel is right, **Phase 3** — higher jump
  (`JUMP_IMPULSE_PXS 320 → 480`, apex 47 → 107 px). If jump+jet
  combo feels wrong (mid-air ice skating, over-responsive
  trajectory reversal), retune `AIR_ACCEL_PXS2` here.

### Phase 3 — taller jump ✅ shipped

- **Code:** `src/mech.c:151` — `JUMP_IMPULSE_PXS 320 → 480`. Per-chassis
  `jump_mult` untouched (relative balance was already tuned). Analytical
  apex 47 → 107 px; analytical airtime 0.60 → 0.89 s. Per-chassis at
  the new impulse (drag-free): Heavy 0.85 → 77 px, Trooper/Sniper/
  Engineer 1.00 → 107 px, Scout 1.10 → 130 px.

- **Probe trail (`tests/shots/m6_movement_probe.shot`):** vy/tick at
  the impulse moment = -8.0 (= 480 / 60 ✓). Measured apex (Trooper,
  drag-trimmed): pelv y 917.6 → 830.1 = **87.5 px** (vs. ~40 px on
  Phase 2 — **2.2× taller**). vx held at 4.62 px/tick across the
  full airborne arc (Phase 2 invariant intact). Airborne horizontal
  travel on the test-play map's downsloped ground: 287 px to ground-
  contact (~62 ticks airtime due to the sloped descent extending the
  arc); flat-ground equivalent ≈ 250 px. Pre-Phase-3 (Phase 2): 190 px.

- **Probe-shot timing tweak:** widened the airborne capture window
  (apex now lands later, full arc lasts longer). Renamed snapshot
  labels `jump_t005..t035` → `jump_t007..t047`, moved `landed_t040`
  (now misleading — mech is still airborne at t=150) to
  `landed_t062` @ t=170, extended `postland`/`stopped`/`end` markers
  accordingly. Contact-sheet column count 5 → 4 to keep the layout
  square.

- **Tests:** `test-mech-ik` ALL PASS, `test-pose-compute` ALL PASS,
  `test-snapshot` all passed, `tests/shots/net/run.sh 2p_basic`
  12/12 PASS.

- **Caveats:**
  - Apex artifact: the probe holds BTN_JUMP for 7 ticks across the
    gait's bouncy grounded pattern. While the button is held AND
    `m->grounded` is true on any tick, `apply_jump` fires (it SETs
    vy each tick; no edge gate). The probe sees jump impulse fire at
    t=108, t=109, t=110 — three stacked impulses inflate the
    measured apex above what a single-impulse jump would give. This
    is Soldat-style spam-jump (intentional) and identical to Phase 2's
    behavior under the same script, so the 2.2× before/after ratio is
    apples-to-apples.
  - Reactor's 96-px pillar archway sits below the analytical 107-px
    Trooper apex. **Watch in playtest** — Scout at 130 px will
    definitely headbutt. If problematic, retune to 440 (apex 90 px
    Trooper / 109 px Scout) or scale the per-chassis jump_mult down
    for the chassis that headbutt most.

- **Playtest:** pending. **Map-test triad:** Reactor (pillar
  archway), Catwalk (low overhangs), Citadel (castle ceilings) per
  the plan's §6 Phase 3 gate.
- **Next:** if jump height feels right and ceilings are OK,
  **Phase 4** — jet pack horizontal (§5C). If ceilings are too
  tight, retune `JUMP_IMPULSE_PXS` here.

### Phase 4 — jet horizontal thrust ✅ shipped

- **Code:** `src/mech.c::apply_jet_force` now reads
  `m->latched_input.buttons & BTN_LEFT/RIGHT` for both the existing
  ceiling-tangent direction selection AND a new horizontal thrust
  component. `fx = -fy · 0.5 · run_sign` adds lateral push at 50 % of
  vertical thrust magnitude when L/R is held. Pure JET (run_sign=0
  → fx=0) stays vertical — the "just climb" verb is preserved.
  Applied to every particle including those flagged CEILING — a
  player can scoot horizontally along an overhang at full sideways
  speed even when the ceiling-taper has eaten the vertical thrust.

- **Phase 2 interaction (intended):** Phase 2's air-control caps the
  run-input contribution at RUN_SPEED. Phase 4's `fx` pushes vx PAST
  the cap; Phase 2's above-cap-in-input-direction return then keeps
  the run input from undoing the jet push. Only PHYSICS_VELOCITY_DAMP
  (0.99/tick) bleeds the excess. So jet+RIGHT settles into a steady
  horizontal speed well above RUN_SPEED — exactly the "jet covers
  distance" verb the user asked for.

- **Jet probe (`tests/shots/m6_movement_jet.shot`, new):** two
  scenarios captured in sequence.
  - **(a) Pure JET (t=40-65):** vx stays at ~0 px/tick throughout
    the jet hold (0.2 px drift at landing settle — terrain). **Pure
    JET stays vertical-only.**
  - **(b) JET + RIGHT (t=220-265, 45 ticks of jet hold):** pelv
    moves from (803.8, 1111.3) → (1237.3, 754.1) = **433 px right,
    357 px up**. Peak velocity (13.96, -12.90) px/tick =
    (838 px/s right, 774 px/s up) — roughly 45° diagonal at ~3×
    RUN_SPEED lateral. Pre-Phase-4 (capped at RUN_SPEED by Phase 2):
    would have been 45·4.67 = 210 px right. **Phase 4 ~2× lateral
    range during jet.**

- **Tests:** `test-mech-ik` ALL PASS, `test-pose-compute` ALL PASS,
  `test-snapshot` all passed, `tests/shots/net/run.sh 2p_basic`
  12/12 PASS, `tests/shots/net/run_frag_grenade.sh` 9/9 PASS,
  running-jump probe (`m6_movement_probe.shot`) bit-identical to
  Phase 3 baseline (pelv=(614.2, 917.6) at t=108 jump, apex
  (721.2, 829.9) at t=131) — Phase 4 only touches the jet path.

- **Caveats:**
  - **Lateral speed at peak ≈ 838 px/s (~3× RUN_SPEED).** This may
    be in the "fly-spam too far" zone the plan warns about; if
    playtest reports jet feels OP for traversal, reduce the 0.5
    multiplier to 0.3-0.4 (yields 2-2.5× RUN_SPEED lateral). Tune
    here, not by recapping Phase 2.
  - **CTF carrier balance** (§8 risk 6): carrier already halves
    `thrust_pxs2`, so both fy AND fx halve for carriers — they get
    half-up, half-sideways. Symmetric penalty. Probably fine; flag
    for CTF playtest.
  - **Bot AI** (§8 risk 4): bots use BTN_JET via the same
    `mech_step_drive` path. They may now drift sideways while
    jetting (whichever direction they're facing). Bake-test
    informational only — re-run if behavior looks off.

- **Playtest:** pending. **Test grid:** open arena (verify
  lateral speed feels right), Reactor pillars (vertical climbs
  still work), CTF flag chase (carrier penalty effective).
- **Next:** if jet feel is right, **Phase 5** — slope-momentum
  verification + retune (likely no code change; just measure).
  If jet is too fast, retune the 0.5 multiplier here.

### Phase 5 — slope-momentum measurement ✅ shipped (no code change)

Per the plan §6 Phase 5 — "probably no code changes; this is a
measurement pass confirming slope-down then run-out produces speeds
> RUN_SPEED that decay toward RUN_SPEED over a measurable window."

- **Probe (`tests/shots/m6_movement_slope.shot`, new):** spawn at
  the top of aurora's west hill (30° slope, peak at (1024, 2516)),
  hold LEFT to slide+run downhill, keep LEFT held through the
  slope→flat transition. Measure vx during the slope descent, at
  the transition, and on the flat run-out.

- **Measurement — active slide (LEFT held):**
  - On the slope, vx oscillates between -5 and -6 px/tick =
    1.1-1.3× RUN_SPEED. Phase 1.1's above-cap guard lets
    gravity-along-tangent push past the input cap; input still
    pulls toward the cap when it dips below.
  - At slope→flat transition (around t=90-101): vx briefly stays
    at -4.7 to -4.9 px/tick (1.0-1.05× RUN_SPEED), then settles
    to the RUN_SPEED cap of -4.667 px/tick.
  - Carry window on flat: ~5 ticks at ~5 % over cap. The flat-
    contact friction (0.92/tick tangential, `src/physics.c:365`)
    is what's bleeding the slope excess; input-cap-not-target
    keeps run input from snapping the velocity down faster.

- **Measurement — impact slide (no input, existing
  `m6_aurora_slope.shot`):** mech free-falls onto the slope at
  spawn, peaks at vx = -11.45 px/tick (2.45× RUN_SPEED). At the
  slope→flat transition (t=10→t=11): vx drops -5.71 → -2.83
  px/tick in one tick. This larger-than-friction drop is the
  constraint-solver impact loss as the foot transitions between
  contact-normal regimes; it predates this branch and the user
  already approved its feel in the Phase 1.1 playtest verdict.

- **Verdict:** Phase 1.1's above-cap invariant holds — slope
  momentum > RUN_SPEED persists on flat for a measurable window
  (~5 ticks at 1.05×). The carry is subtle on 30° slopes because
  flat friction (0.92/tick) is strong relative to the modest slope-
  gravity contribution. **No code change in Phase 5.** If the
  user reports slope advantage feels too subtle, the tuning lever
  is `floor friction (flat)` in `src/physics.c:365` — bumping from
  0.92 → ~0.96 gives ~4× longer carry window (15-20 ticks of
  visible excess), at the cost of slower release-decel on flat.

- **Tests:** mech-ik / pose / snapshot / 2p_basic — unchanged from
  Phase 4 (no code touched).

- **Playtest:** N/A — measurement-only ship.
- **Next:** **Phase 6** — recoil + dash momentum compatibility
  check. Phase 1/2 changes should give Scout's BTN_DASH a
  cumulative effect for free (720 px/s > RUN_SPEED, above-cap
  guard preserves the dash spike, friction grinds it down). Visual
  via the probe shot.

### Phase 6 — Scout dash fix + recoil compatibility check ✅ shipped

The plan §6 Phase 6 predicted Scout dash would "work for free"
with Phase 1's add-toward-target model. **That prediction was
wrong.** Phase 1 routed Scout dash through `apply_run_velocity`,
which caps the per-tick delta at `GROUND_ACCEL_PXS2·dt² =
0.778 px/tick`. A 720 px/s dash needs 12 px/tick of impulse — the
accel-cap clamps it to 0.778 and the dash effectively does nothing.

- **Bug confirmed (probe `tests/shots/m6_movement_dash.shot`,
  new):** Scout running RIGHT at vx=5.71 px/tick (cap),
  tap DASH at t=100. Pre-fix: vx jumps to 6.37 px/tick (+0.66 —
  a tiny nudge, not a dash). Post-fix: vx jumps to **12.01
  px/tick** (= 720/60 ✓), then decays through 11.85 → 11.49 →
  11.08 → 10.52 over the next 5 ticks, still at 9.22 at t=130
  (30 ticks post-dash), bleeds back to RUN_SPEED cap by t=148.
  Persistent excess lasts ~45 ticks — the cumulative-boost feel
  the plan describes.

- **Fix:** Scout dash branch in `mech.c` now does a direct
  velocity SET (via `physics_set_velocity_x` per particle), same
  idiom as `apply_jump` — both are edge-triggered one-shot
  impulses, neither should route through the held-input accel-cap.
  Phase 1's above-cap-return invariant then preserves the dash
  spike on subsequent ticks; contact friction grinds it down.

- **Recoil:** no fix needed. `weapons.c` applies recoil as a
  position offset on R_HAND (`pos_x -= dir.x · recoil_impulse`),
  not through `apply_run_velocity`. The constraint solver
  propagates the offset into pelvis velocity via bone constraints;
  Phase 1's above-cap-return then preserves any pelvis-vx excess
  above RUN_SPEED until friction bleeds it down. Works for free
  because recoil sits outside the run-velocity surface.

- **Tests:** `test-mech-ik` ALL PASS, `test-pose-compute` ALL
  PASS, `test-snapshot` all passed, `tests/shots/net/run.sh
  2p_basic` 12/12 PASS.

- **Caveat:** the dash spike is now substantial (vx=12 px/tick =
  720 px/s ≈ 2.6× RUN_SPEED) and persists for 30+ ticks at >2×.
  If playtest reports Scout dash feels OP, reduce
  `SCOUT_DASH_PXS` in `mech.c:165` from 720 → ~500 (vx=8.3
  px/tick, ~1.8× cap, decay-to-cap in ~12 ticks) instead of
  re-routing through the accel-cap.

- **Playtest:** pending. Scout-specific feel check + verify dash
  still feels distinct from a regular run.
- **Next:** **Phase 7** — network sync validation. The new
  movement code reads `pos - prev` for current velocity and this
  is the same data the snapshot system encodes. Server and client
  both run 60 Hz, both run the same code path, so prediction-
  reconciliation should stay aligned. Test plan: `tests/net/run.sh`,
  `tests/shots/net/run.sh 2p_basic`, paired-process net tests.

### Phase 7 — network sync validation ✅ shipped (no code change)

Per plan §6 Phase 7: confirm none of the Phase 1-6 movement changes
break client-server position prediction or remote-mech interpolation.

**Verdict: zero regressions from Phase 1-6 against parent commit
`3626e36`.** All Phase 1-6 motion/sync paths preserve host↔client
agreement under the same paired-process tests that gate the
movement-tuning work.

- **3-process net tests (real ENet over loopback):**
  - `tests/net/run.sh`        — 13/13 PASS
  - `tests/net/run_3p.sh`     — 10/10 PASS

- **Paired-shot tests directly exercising Phase 1-6 surfaces:**
  - `2p_basic`                — 12/12 PASS (run.sh wrapper)
  - `run_frag_grenade`        — 9/9 PASS (explosion sync, projectile arcs)
  - `run_grapple_lag_comp`    — 5/5 PASS (server-side lag comp)
  - `run_snapshot_rate`       — 5/5 PASS (interp clock + rate)
  - `run_rmb_hitscan`         — 5/5 PASS (secondary fire / hitscan)
  - `run_secondary_fire`      — 7/7 PASS
  - `run_input_redundancy`    — 5/5 PASS (input bundling, ENet throttle)
  - `run_riot_cannon_sfx`     — 3/3 PASS
  - `run_meet_named`          — 18/18 PASS (custom-map host→client)
  - `run_meet_custom`         — 15/15 PASS

- **Triage on the broader paired-shot suite (25 scripts):** 15
  GREEN, 8 PARTIAL (stale, pre-existing — identical pass/fail
  counts on `3626e36` confirmed by `git stash` round-trip), 2
  infrastructure (`run_dedi` needs a scenario arg, `run_round_sync_
  banner` has a non-standard output format). Stale tests:
  `run_anim_stability`, `run_ctf`, `run_ctf_combat`,
  `run_ctf_drop_on_kill`, `run_kill_feed`, `run_lag_comp`,
  `run_round_loop`, `run_round_sync`. Root causes (per a kill_feed
  case study):
  1. **Map size expansion** (M6 P05 `4db9e7b`) — crossfire is now
     140×60 tiles (4480 px wide). Tests that assumed pulse-rifle
     range across the spawn separation now miss.
  2. **Spawn separation tightening** (this branch `ad75daf` /
     `3626e36`) — spawn allocator gives the 2-player pair more
     space; aim coordinates hardcoded against older spawn layouts
     no longer target the opponent.
  3. **Lobby flow drift** (wan-fixes-11) — round-timing assertions
     shifted by ±5 ticks.

- **Tuning lever for the stale tests:** add a
  `--dedi-spawn-override` CLI flag to the dedicated server (~30
  lines in `src/net.c` / `src/dedi.c`) so paired-dedi tests can
  script "spawn ClientA at (X1, Y1), ClientB at (X2, Y2)" the same
  way host-client tests use `peer_spawn`. Out of scope for this
  branch; tracked for a future shot-test-hygiene pass.

- **No code change in Phase 7.** Measurement-only ship.

- **Playtest:** N/A.
- **Next:** **Phase 8** — update CURRENT_STATE.md tunables table +
  the spec doc `documents/03-physics-and-mechs.md` §Movement to
  reflect the Phase 1-6 shipping numbers; PR open.

### Resume here when next session starts

1. Read this section (§13) for the running log.
2. `git log --oneline main..HEAD` to confirm the commits above are
   on the branch.
3. Re-run all four probe shots to reproduce baselines:
   - `tests/shots/m6_movement_probe.shot`
   - `tests/shots/m6_movement_jet.shot`
   - `tests/shots/m6_movement_slope.shot`
   - `tests/shots/m6_movement_dash.shot`
   - `tests/shots/m6_aurora_slope.shot`
4. Phases 1-7 all landed; Phase 8 (docs update + open PR) is the
   wrap-up.

### Phase 1.2 followup — sync investigation (NOT a Phase 1.x regression)

User reported that client mechs look jittery/shaking and that the
jet pack "doesn't work the same" between host and client. Built two
new probes and bisected against `63ad669^` (parent of Phase 1):

- `tests/shots/m6_idle_baseline.shot` — single-player, no input,
  default tutorial. Mech is rock-solid still through 240 ticks. So
  Phase 1.x does NOT inject phantom velocity in single-player.
- `tests/shots/net/2p_sync_aurora.{host,client}.shot` — paired
  burst test on aurora's flat plain. Host walks RIGHT + jets;
  client (placed via host's `peer_spawn 1 ...` 200 px to the right)
  observes via `burst client_t from 200 to 460 every 6`. Both mechs
  fit in each window's camera so frames can be compared 1:1.

What the burst frames show on Phase 1.2:
  - **Position desync.** At world tick 320: host's view of itself
    at (2906, 2712) v=0 (settled); client's view of host at
    (2510, 2714) v=4.17 (still walking right). ~400 px lag.
    Eventually closes but never matches the host's actual position.
  - **Jet-plume "desync"** is just the position lag manifesting
    visually — the plume DOES draw at the host's actual jet moment,
    but the client renders the host at an OLD position where the
    jet hadn't started yet.

**Bisect verdict (`tests/shots/net/run.sh 2p_motion`):**
  - Pre-Phase-1 (`63ad669^`): client lag = 498 px at t=460
  - Phase 1.2:                client lag = 507 px at t=460
  - Identical magnitude (within snapshot noise). The lag is
    **PRE-EXISTING** and unrelated to the movement-tuning work in
    this branch.

**Likely root cause** (not investigated further this pass): the
client's interp clock arming + ring lerp in
`snapshot_interp_remotes` either uses a stale clock or the
snapshots arrive too slowly under shotmode's per-tick pump. The
intended `INTERP_DELAY_MS = 100ms` (~6 ticks) explains ~30 px of
lag at RUN_SPEED, not 500 px. Worth its own debugging round
outside the movement branch.

**Phase 1.x is clean.** `apply_run_velocity` reads `vx_now =
pos - prev` from pelvis, which is reconciliation-safe because
`snapshot_apply` calls `physics_set_velocity_x(vx)` after the
translate (sets prev = pos - vx).

---

### Phase 1.2 followup #2 — interp clock + out-of-order snapshot fix

User reported the position lag was unacceptable for WAN play. Found
the root cause and fixed:

**Two real bugs in the network layer (both PRE-EXISTING, not Phase
1.x regressions, but exposed by the user's WAN play and by my
diagnostic shot tests):**

1. `client_render_time_ms` arms on the FIRST snapshot the client
   receives. If that arrives during LOBBY (where world.tick / sim
   does not advance, hence render_time also stays frozen), the
   server keeps broadcasting at 60 Hz and pushes
   `client_latest_server_time_ms` forward while render_time is
   stuck at the early value. When the client transitions to MATCH,
   render_time begins advancing at the same rate as server_time,
   but the entire LOBBY/countdown gap is now baked in as permanent
   extra lag — visible as "the client renders the host's mech 1-2
   SECONDS behind where the host actually is."

2. `client_handle_snapshot` processes EVERY decoded snapshot,
   including out-of-order ones. UDP doesn't guarantee order, so a
   stale snapshot can push back-in-time data into the per-mech
   ring AND re-snap the local mech backward (followed by a full
   input replay against the stale base).

**Fix (src/net.c + src/net.h):**

- `client_handle_snapshot` now drops snapshots whose
  `server_time_ms < client_latest_server_time_ms` (UDP ordering
  protection).
- New helper `net_client_advance_render_clock(ns, dt_ms)` replaces
  the bare `client_render_time_ms += dt_ms` previously inlined at
  every call site (main.c + shotmode.c). Strict tracking: each
  tick, set render_time to MAX(smooth_advance, latest - INTERP).
  The smooth advance happens in steady state (lag stays at
  INTERP_DELAY_MS = 100 ms); when a stall resolves or the LOBBY
  freeze unblocks, render_time snaps forward to the target.
  Visual cost: remote mechs may teleport forward by a few px on
  the catch-up, but they never render 1+ second of stale state.

**Death-anim mismatch** (user reported "looks correct on client,
but not on server"): not addressed by this fix. The cause is
separate — for dead mechs `pose_compute` is skipped (alive guard),
the Verlet ragdoll runs independently on host vs client, snapshot
only carries the pelvis. Limb positions diverge. Fix would need
either (a) all 16 particles on the wire for dead mechs or (b)
freeze the local-side ragdoll once the death event is received
and let snapshot interp drive everything from the pelvis. Tracked
as a follow-up; out of scope for this pass.

**Tests:**
  - test-snapshot / test-mech-ik / test-pose-compute — all PASS
  - tests/shots/net/run.sh 2p_basic — 12/12 PASS

---

**End of plan.**
