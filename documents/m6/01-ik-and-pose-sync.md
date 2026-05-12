# M6 — IK & Pose Sync

**Goal**: replace the current `build_pose` + Verlet-constraint-solver pose
driver with a deterministic procedural pose function so every client
renders the same bone positions for every mech, within a small numeric
tolerance. Fix three concrete bugs along the way. Add tests that catch
regressions.

Read this whole document end-to-end before opening a single file. Skip
ahead is allowed for §10 (Test plan) and §12 (Implementation plan
checklist) when you're already deep in the work.

---

## 0. Scope and non-goals

**In scope**
- A new `src/mech_ik.{c,h}` module with analytic 2-bone IK + a pure
  function `pose_compute_deterministic` that maps `(chassis, pelvis,
  aim, facing, anim_id, gait_phase, grounded, dt_unused)` to 16 bone
  positions.
- Snapshot wire-format extension: add `u16 gait_phase_q` to
  `EntitySnapshot` so every client renders the same gait frame.
- Re-wire `simulate.c` + `mech.c` so the pose driver runs on every mech
  on every client, instead of being gated to local-mech-only.
- A small `weapons.c` / `net.c` fix that un-suppresses non-hitscan
  self-fire SFX (the Riot Cannon bug).
- Unit tests for the IK math, headless tests for pose continuity,
  paired-shot tests for client-vs-host pose agreement.
- Documentation: `CURRENT_STATE.md`, `TRADE_OFFS.md`, this file's
  status footnote.

**Out of scope**
- Replacing Verlet entirely. The Verlet body still drives projectile
  collision against bones, dismemberment ragdoll, grapple anchor
  chain, blood spray. We keep it. The change is: bones come from the
  procedural pose function each tick, and the constraint solver
  becomes secondary (a no-op for live mechs; only the dead/dismembered
  case still runs free-flying Verlet).
- Bandwidth optimization beyond the 2-byte `gait_phase` add.
  Snapshot delta encoding is still deferred.
- Per-chassis animation curves beyond what `build_pose` already does
  (Scout lean, Heavy chest strength, etc.). Those carry forward
  unchanged into the new function.
- Reworking `mech_post_physics_anchor` — that one still runs for
  grounded standing/running on flat ground. It's complementary, not
  contradictory, to the new pose driver.

---

## 1. Bugs in scope

### Bug A — Remote mechs are frozen in rest pose (regression from the slope-stretch fix)

After the post-P19 round 4 slope-stretch fix, `apply_pose_to_particles`
early-returns for remote mechs on the client:

```c
// src/mech.c::apply_pose_to_particles (current)
if (!w->authoritative && m->id != w->local_mech_id) return;
```

This stopped the kinematic-mech bone inflation, but it also stopped
the running gait, the aim-arm chain, and any other pose-driven motion
on remote mechs. Remote players now look frozen in chassis rest pose,
with `snapshot_interp_remotes` rigid-translating their bodies along
the pelvis path.

We need an animated remote mech that does NOT inflate. The current
trade-off entry `Remote mechs render in rest pose on the client
(wan-fixes-3)` literally describes this loss; the goal of this work
is to retire that trade-off.

### Bug B — Non-hitscan self-fire SFX is suppressed (Riot Cannon, etc.)

`src/weapons.c::weapons_predict_local_fire` only plays the fire SFX
for `WFIRE_HITSCAN` weapons:

```c
if (wpn->fire == WFIRE_HITSCAN) {
    // ...predict-side tracer + sparks...
    audio_play_at(audio_sfx_for_weapon(me->weapon_id), origin);
}
```

But `src/net.c::client_handle_fire_event` suppresses its own SFX
play on `predict_drew = is_self && from_active_slot`, regardless of
fire kind:

```c
bool predict_drew = is_self && from_active_slot;
if (!predict_drew) {
    // ...spark + audio_play_at(audio_sfx_for_weapon(weapon), origin);
}
```

For a self-fire of any non-hitscan weapon (Riot Cannon /
Auto-Cannon / Plasma SMG / Microgun / Plasma Cannon / Mass Driver /
Frag Grenades / Micro-Rockets / Burst SMG / Sidearm — every weapon
that isn't Pulse Rifle, Rail Cannon, Combat Knife, or hitscan-melee),
the predict path never plays the SFX, and the network event path
suppresses it. **Result: silent fire on both windows.** The user
specifically called out Riot Cannon.

### Bug C — Procedural pose without a deterministic driver

The deeper issue: `build_pose` writes pose targets, `apply_pose`
nudges particles toward those targets by a strength factor (0.5-0.9
per tick), and the constraint solver pulls the chain into rest-length
shape over 12 iterations. The end-of-tick bone positions are a
function of *the previous tick's bone positions* plus this tick's
inputs. That's history-dependent. Two clients running the same
input stream from a fresh state can drift slowly even with identical
inputs (floating-point order of operations, snapshot-interp
rigid-translates, etc.).

For sync we want: bone positions = pure function of `(pelvis, aim,
facing, anim_id, gait_phase, grounded, chassis, dismember_mask)`. No
history. No iteration. Then host and every client compute the same
bones from the same inputs.

---

## 2. Background research

### Soldat (the reference)

Soldat (and its OpenSoldat fork) is the closest comparable game. Its
approach, summarized from
[opensoldat/opensoldat](https://github.com/opensoldat/opensoldat)
source files:

- **Per-tick snapshot**: pelvis position + velocity + input keys
  (16-bit bitmask) + mouse aim x/y + stance (stand/crouch/prone) +
  weapon + ammo. **Not** the 16-particle skeleton. See
  `shared/network/NetworkServerSprite.pas`.
- **Skeleton on death only**: `TMsg_SpriteDeath` ships
  `Skeleton.Pos[1..16]` + `OldPos[1..16]` + constraint-active bits
  exactly once when the soldier dies. The ragdoll then Verlet-evolves
  locally.
- **Two animation channels per soldier**: `LegsAnimation` and
  `BodyAnimation`, each indexing into a pre-baked frame list with
  positions for up to 20 skeleton points. The frames are loaded from
  `*.poa` files at startup. Each tick `DoAnimation` advances the
  current frame at a per-anim Speed setting. See `shared/Anims.pas`
  and `shared/mechanics/Sprites.pas::BodyApplyAnimation /
  LegsApplyAnimation`.
- **Pose application**: the anim's `Frame[CurrFrame].Pos[i]` gives
  the world-space target for each skeleton point relative to the
  soldier's pelvis. `MoveSkeleton(x, y, FromZero)` rigid-translates
  the whole body, and the Verlet system's `SatisfyConstraints` runs
  every tick to keep bone lengths at rest. The anim frames define
  what the body LOOKS like; Verlet keeps it physically coherent.
- **Determinism**: every client runs the SAME `BodyAnimation` and
  `LegsAnimation` with the SAME `CurrFrame` because: (a) the input
  keys ride the wire, so clients derive the same anim_id; (b) the
  frame index advances deterministically by `Speed * tick`; (c)
  pelvis is in the snapshot. There is no per-bone snapshot. Every
  client renders the same gostek.

Citations:
- [OpenSoldat repo](https://github.com/opensoldat/opensoldat)
- [Soldat gostek reference](https://urraka.github.io/soldat-gostek/)
- [Soldat Mod.ini docs (gostek section)](https://wiki.soldat.pl/index.php/Mod.ini)

### 2-bone analytic IK

Standard closed-form solution. Given:
- Root position `s` (e.g., shoulder, hip)
- Target endpoint `t` (e.g., hand reaching toward aim point, foot
  planted at ground level)
- Upper-bone length `L1`, lower-bone length `L2`
- Bend direction sign `s ∈ {+1, -1}` (elbow-up vs elbow-down)

Compute joint position `j` (elbow / knee):

```
d  = t - s
dl = |d|

// Out-of-reach: target farther than L1 + L2 → fully extended
if (dl >= L1 + L2 - eps) {
    n = d / dl
    j = s + n * L1
    end = s + n * (L1 + L2)
    return j, end  // 'end' is now SHORT of t but pointed at it
}

// Over-folded: target closer than |L1 - L2| → bent at min angle
if (dl <= abs(L1 - L2) + eps) {
    // pick the closer rest position; degenerate, render straight
    n = d / max(dl, eps)
    j = s + n * L1
    return j, s + n * (L1 + L2)   // end clamped to a stable point
}

// Normal case: law of cosines.
// cos(theta_at_root_for_j) = (L1² + dl² - L2²) / (2 * L1 * dl)
a = (L1*L1 - L2*L2 + dl*dl) / (2.0f * dl)
h2 = L1*L1 - a*a
h  = sqrtf(max(0.0f, h2))

// Forward along (s → t) by 'a'; perpendicular by ±h.
fx = d.x / dl, fy = d.y / dl              // unit along
px = -fy,      py =  fx                   // unit perpendicular (left-handed turn)
j.x = s.x + a*fx + bend_sign * h * px
j.y = s.y + a*fy + bend_sign * h * py
```

`bend_sign` picks elbow-up vs elbow-down. For a side-scroller with the
aim-arm:
- Right arm aiming forward → elbow bends DOWN behind the line (sign
  picked so the elbow sits on the side away from the chest's "up").
- Knees bend forward in the running direction.

The trick is the sign convention. We will encode it per-joint in a
small table.

References:
- [Ryan Juckett — Analytic Two-Bone IK in 2D](https://www.ryanjuckett.com/analytic-two-bone-ik-in-2d/)
- [Alan Zucconi — Inverse Kinematics in 2D, Part 2](https://www.alanzucconi.com/2018/05/02/ik-2d-2/)
- [The Orange Duck — Simple Two Joint IK](https://theorangeduck.com/page/simple-two-joint)

### Why procedural beats network-streamed bones

Three options were considered:
1. **Ship full bones in snapshot**. 16 particles × 4 bytes (2× Q12
   each axis) = 64 bytes/mech/snapshot. At 32 mechs × 60 Hz = 122
   KB/s. Triples the snapshot bandwidth and offers no
   smoothing — every snapshot is a hard set, between which the body
   has nothing to do.
2. **Run physics on remote mechs**. Tried at M1; reverted at
   wan-fixes-3 because the constraint solver couldn't converge
   against stale inputs and produced visible jitter.
3. **Procedural pose from synced inputs**. ~2 bytes/mech of new state
   (gait_phase). Same code runs on host and clients. Mechs animate
   identically without any per-bone wire data. Soldat does this.

We pick option 3.

---

## 3. Architectural decision

> **Bone positions of a live mech are a pure function of the
> synced state. Period.**

State that defines the pose:
- `pelvis` (Vec2)
- `aim` (unit vector, derived from `aim_world - pelvis`)
- `facing_left` (bool)
- `anim_id` (STAND / RUN / JET / FALL / FIRE)
- `gait_phase` (float ∈ [0, 1), advances in ANIM_RUN)
- `grounded` (bool)
- `chassis_id` (constants table)
- `dismember_mask` (bits, governs which limbs render)

`active_slot` is currently consumed by Engineer's "skip right-arm
aim" quirk. That's also derivable from snapshot state (the snapshot
ships `weapon_id` and the active slot follows from it). Wire it
through.

`recoil_kick` and visual one-shot effects (muzzle flash, hit flash,
fire-tick offsets) are NOT part of the pose function. They render on
top.

### What changes vs today

Today (rough flow per tick on the host):
```
mech_step_drive(mid):
  m->grounded = any_foot_grounded(...)        // or snapshot for remote
  if input_drives_anim: m->anim_id = ...
  apply_run_velocity / apply_jet_force / apply_jump
  build_pose(chassis, w, m, dt)               // writes pose_target[16]
  apply_pose_to_particles(w, m)               // nudges particles toward targets
physics_apply_gravity
physics_integrate
physics_constrain_and_collide (12 iters)
mech_post_physics_anchor (local only)
```

After (per tick on every side):
```
mech_step_drive(mid):
  (input → anim_id / m->grounded same as today on the authoritative side)
  (on the client: read m->anim_id + m->grounded from the snapshot apply)
  apply_run_velocity etc. for the LOCAL MECH only (prediction)
physics_apply_gravity / integrate / collide  (local mech only on client)
post_physics_anchor (local mech only on client)
pose_compute_deterministic(chassis, m, &out_bones)   // every mech, every tick
pose_write_to_particles(w, m, out_bones)             // every mech, every tick
mech_post_physics_anchor stays where it is
```

The procedural pose runs LATE in the tick, AFTER physics, AFTER
post_physics_anchor. Its output is the final bone position for the
tick — it overwrites whatever physics produced for the LIVE
parts. Physics still runs because the LOCAL mech needs prediction
for movement input → pelvis displacement; we just trust the pose
function for shape.

For a DEAD mech (alive=false), we DON'T call the pose function.
Instead, the existing Verlet ragdoll evolves freely. Same for
dismembered limbs — particles flagged dismembered fall through
gravity unimpeded.

### Snapshot-driven inputs on the client

For remote mechs on the client:
- `pelvis_x/y`: from `snapshot_interp_remotes` (bracket-lerped between
  two snapshots; existing code).
- `aim`: derive from `aim_q` in the snapshot (already there).
- `facing_left`: from `SNAP_STATE_FACING_LEFT` (already there).
- `anim_id`: from `SNAP_STATE_RUNNING / JET / FIRE` (already there).
- `gait_phase`: from the new `gait_phase_q` field (added in §4).
- `grounded`: from `SNAP_STATE_GROUNDED` (already there).
- `chassis_id`: from snapshot (already there).
- `dismember_mask`: from `limb_bits` (already there).

For the LOCAL mech on the client, `gait_phase` is computed locally
from the predicted `anim_time` (matching the host's computation
because they share `anim_time += dt` semantics; reconcile keeps them
close).

For the AUTHORITATIVE side (host or offline-solo), everything comes
from the local simulation. `gait_phase` is `anim_time *
cycle_freq - floor(anim_time * cycle_freq)`.

---

## 4. Wire format changes

Add one field to `EntitySnapshot`:

```c
// src/snapshot.h (current)
typedef struct {
    uint16_t mech_id;
    int16_t  pos_x_q, pos_y_q;
    int16_t  vel_x_q, vel_y_q;
    uint16_t aim_q, torso_q;
    uint8_t  health, armor, weapon_id, ammo;
    uint16_t state_bits;
    uint8_t  team;
    uint16_t limb_bits;
    uint8_t  chassis_id, armor_id, jetpack_id, primary_id, secondary_id, ammo_secondary;
    // ... optional grapple suffix when SNAP_STATE_GRAPPLING
} EntitySnapshot;
```

Add:

```c
    uint16_t gait_phase_q;   // Q0.16: cycle position ∈ [0, 1). 0 = stride start.
```

`ENTITY_SNAPSHOT_WIRE_BYTES` goes from 29 → 31. Encode/decode the field
between `ammo_secondary` and the optional grapple suffix.

Quantization helpers (add to `snapshot.h`):

```c
static inline uint16_t quant_phase(float phase) {
    if (phase < 0.0f) phase -= floorf(phase);   // normalize into [0,1)
    if (phase >= 1.0f) phase -= floorf(phase);
    int v = (int)(phase * 65536.0f);
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    return (uint16_t)v;
}
static inline float dequant_phase(uint16_t q) {
    return (float)q * (1.0f / 65536.0f);
}
```

Bump the protocol id `S0LI → S0LJ` in `src/version.h`. Add a comment
to the version-history block explaining the addition.

**No other wire changes.** Sticking strictly to "one new field" keeps
the migration minimal.

---

## 5. New module: `src/mech_ik.{c,h}`

Header (`src/mech_ik.h`):

```c
#pragma once

#include "math.h"
#include "world.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-tick inputs to the deterministic pose function. Every field must
 * be reproducible on every client given the snapshot stream and the
 * authoritative side's per-tick simulation. */
typedef struct {
    Vec2     pelvis;          /* world pelvis */
    Vec2     aim_dir;          /* unit aim direction (right-handed +X right, +Y down) */
    bool     facing_left;      /* aim.x < 0 */
    int      anim_id;          /* ANIM_STAND / ANIM_RUN / ANIM_JET / ANIM_FALL / ANIM_FIRE */
    float    gait_phase;       /* ∈ [0, 1), advances in ANIM_RUN */
    bool     grounded;
    int      chassis_id;
    int      active_slot;      /* 0=primary, 1=secondary — drives Engineer quirk */
    uint16_t dismember_mask;   /* bitmap of dismembered limbs */
} PoseInputs;

/* Output: world-space position for every PART_*. 16 positions. */
typedef Vec2 PoseBones[PART_COUNT];

/* The canonical procedural pose function. Pure: same inputs → same
 * output bit-for-bit (modulo IEEE754 reorderings in optimized builds —
 * we test for tolerance, not bit-exactness). */
void pose_compute(const PoseInputs *in, PoseBones out);

/* 2-bone analytic IK. Given root `s`, target `t`, bone lengths
 * `L1`/`L2`, and a bend sign (+1 or -1), produce the middle joint
 * position `j`. The end-effector is at `t` when reachable; clamped to
 * `s + (t-s) * (L1+L2)/dist` when out of reach. Returns whether the
 * target was reachable. */
bool mech_ik_2bone(Vec2 s, Vec2 t, float L1, float L2,
                   float bend_sign, Vec2 *out_joint);

#ifdef __cplusplus
}
#endif
```

Implementation (`src/mech_ik.c`) — see §6 for the math and §7 for the
per-anim pose rules. Module sits below `mech.c` in the include graph:
`mech_ik` depends on `world` + `math` + `mech` (for the Chassis
struct). `mech.c` calls `pose_compute(...)` once per tick per mech.

---

## 6. 2-bone IK math, exactly

```c
bool mech_ik_2bone(Vec2 s, Vec2 t, float L1, float L2,
                   float bend_sign, Vec2 *out_joint)
{
    float dx = t.x - s.x, dy = t.y - s.y;
    float d2 = dx*dx + dy*dy;
    float d  = sqrtf(d2);
    const float EPS = 1e-4f;

    /* Out of reach (target too far): joint is on the s→t line at L1 from s. */
    if (d >= L1 + L2 - EPS) {
        float inv = (d > EPS) ? 1.0f / d : 0.0f;
        out_joint->x = s.x + dx * inv * L1;
        out_joint->y = s.y + dy * inv * L1;
        return false;
    }
    /* Collapsed (target closer than |L1 - L2|): joint goes on s→t direction at L1.
     * Renders as a degenerate but stable straight chain. */
    float min_d = fabsf(L1 - L2);
    if (d <= min_d + EPS) {
        float inv = (d > EPS) ? 1.0f / d : 0.0f;
        out_joint->x = s.x + dx * inv * L1;
        out_joint->y = s.y + dy * inv * L1;
        return false;
    }

    /* Normal case: law of cosines. 'a' is the signed offset along
     * (s→t) of the foot of the perpendicular from j; 'h' is the
     * perpendicular distance. */
    float a  = (L1*L1 - L2*L2 + d2) / (2.0f * d);
    float h2 = L1*L1 - a*a;
    float h  = (h2 > 0.0f) ? sqrtf(h2) : 0.0f;

    float fx = dx / d, fy = dy / d;
    /* Right-perpendicular in screen coords (y-down): rotate forward
     * by +90° → (-fy, fx). bend_sign flips to the other side. */
    float px = -fy, py = fx;

    out_joint->x = s.x + a*fx + bend_sign * h * px;
    out_joint->y = s.y + a*fy + bend_sign * h * py;
    return true;
}
```

Unit tests in §10. Sign conventions for our skeleton:

| Bone chain | Root (`s`) | End (`t`) | L1 / L2 | `bend_sign` |
|---|---|---|---|---|
| Right arm (aim) | R_SHOULDER | R_HAND target (aim line) | `bone_arm` / `bone_forearm` | `facing_left ? +1 : -1` (elbow points down/back) |
| Left arm (foregrip, two-handed) | L_SHOULDER | foregrip world | `bone_arm` / `bone_forearm` | `facing_left ? -1 : +1` |
| Left leg (gait) | L_HIP | L_FOOT target (gait curve) | `bone_thigh` / `bone_shin` | `+1` (knee always points forward = in screen coords, the perpendicular side away from the back) |
| Right leg | R_HIP | R_FOOT target | `bone_thigh` / `bone_shin` | `+1` |

Pick the signs once with a render test, then lock them in. They're
chassis-independent because every chassis has the same handedness.

---

## 7. Per-anim pose rules

`pose_compute` runs through these rules in order. Output is `PoseBones
out[16]`. All targets are world-space.

### 7.1 Anchor: PELVIS

```c
out[PART_PELVIS] = in->pelvis;
```

Trivially `in->pelvis`. The slope-stretch fix from P19 follow-up #4
established that pelvis is the authoritative anchor — everything
hangs off it.

### 7.2 Hips and shoulders

Fixed offsets from pelvis, mirrored by `facing_left` only for the
left/right distinction (the offsets themselves don't flip):

```c
out[PART_L_HIP] = (Vec2){ pelvis.x - 7, pelvis.y };
out[PART_R_HIP] = (Vec2){ pelvis.x + 7, pelvis.y };
out[PART_L_SHOULDER] = (Vec2){ pelvis.x - 10, pelvis.y - ch->torso_h + 4 };
out[PART_R_SHOULDER] = (Vec2){ pelvis.x + 10, pelvis.y - ch->torso_h + 4 };
```

(Numbers from the current `build_pose`.)

### 7.3 CHEST + NECK + HEAD

Stacked along the vertical axis with chassis quirks:

```c
out[PART_CHEST]= (Vec2){ pelvis.x, pelvis.y - ch->torso_h };
out[PART_NECK] = (Vec2){ pelvis.x, pelvis.y - ch->torso_h - ch->neck_h * 0.5f };
out[PART_HEAD] = (Vec2){ pelvis.x, pelvis.y - ch->torso_h - ch->neck_h - 8.0f };
// Chassis quirks (carry forward from build_pose):
switch (ch_id) {
    case CHASSIS_SCOUT:  out[PART_CHEST].x += face_dir * 2.0f; break;
    case CHASSIS_SNIPER: out[PART_HEAD].x  += face_dir * 2.0f;
                          out[PART_HEAD].y  += 3.0f; break;
    case CHASSIS_HEAVY:  break; // chest_strength quirk was for old pose driver — drop
    default: break;
}
```

### 7.4 Right arm (aim-driven)

The aim arm extends from R_SHOULDER toward a hand target at
`R_SHOULDER + aim_dir * arm_reach`. Engineer with `active_slot == 1`
drops the aim drive (carry forward; replace with rest-pose dangle).

```c
float arm_reach = ch->bone_arm + ch->bone_forearm;
bool aim_arm = !(ch_id == CHASSIS_ENGINEER && in->active_slot == 1) && !in->dismember_R_ARM;
if (aim_arm) {
    Vec2 hand = { out[PART_R_SHOULDER].x + in->aim_dir.x * arm_reach,
                  out[PART_R_SHOULDER].y + in->aim_dir.y * arm_reach };
    Vec2 elbow;
    mech_ik_2bone(out[PART_R_SHOULDER], hand,
                  ch->bone_arm, ch->bone_forearm,
                  in->facing_left ? +1.0f : -1.0f, &elbow);
    out[PART_R_ELBOW] = elbow;
    out[PART_R_HAND]  = hand;
} else {
    // Rest-pose dangle for Engineer-with-tool or dismembered arms.
    out[PART_R_ELBOW] = (Vec2){ out[PART_R_SHOULDER].x + face_dir * 2,
                                out[PART_R_SHOULDER].y + ch->bone_arm };
    out[PART_R_HAND]  = (Vec2){ out[PART_R_ELBOW].x + face_dir * 2,
                                out[PART_R_ELBOW].y + ch->bone_forearm };
}
```

### 7.5 Left arm

Default: dangle (the M1 "no IK" default).

```c
out[PART_L_ELBOW] = (Vec2){ out[PART_L_SHOULDER].x - face_dir * 2,
                             out[PART_L_SHOULDER].y + ch->bone_arm };
out[PART_L_HAND]  = (Vec2){ out[PART_L_ELBOW].x - face_dir * 2,
                             out[PART_L_ELBOW].y + ch->bone_forearm };
```

**Optional two-handed foregrip** (resolves an old trade-off): if the
weapon has a foregrip pivot and the chassis isn't Engineer-secondary,
solve the left arm with IK toward the foregrip world position. Use
the `weapon_sprites.h` helper `weapon_foregrip_world(R_HAND, aim,
sprite_def, &out)` and IK from L_SHOULDER.

```c
Vec2 fg;
if (weapon_foregrip_world(out[PART_R_HAND], in->aim_dir, wsp, &fg)) {
    Vec2 elbow;
    mech_ik_2bone(out[PART_L_SHOULDER], fg,
                  ch->bone_arm, ch->bone_forearm,
                  in->facing_left ? -1.0f : +1.0f, &elbow);
    // Out-of-reach clamp lives inside mech_ik_2bone; we don't fight it.
    // Crucially: the foregrip is OFTEN out of reach (rifle is far from
    // L_SHOULDER). When unreachable, the arm extends fully toward fg;
    // L_HAND lands SHORT of fg. That's correct — the held-weapon
    // render in render.c already paints the rifle from R_HAND, so a
    // partial reach just shows the off-hand reaching toward the grip.
    out[PART_L_ELBOW] = elbow;
    // hand position: at (s + (fg - s) * (L1+L2) / dist) when out of
    // reach, or at fg when reachable. mech_ik_2bone returns false when
    // unreachable; in that case we set hand to s + dir * arm_reach.
    ...
}
```

This eliminates the "L_HAND has no pose target" trade-off because the
procedural pose explicitly chooses dangle vs foregrip; there's no
constraint solver to fight, so the chassis doesn't drift in the aim
direction the way it did at P11.

### 7.6 Legs — STAND / FALL / FIRE

Straight down, slight bias:

```c
out[PART_L_KNEE] = (Vec2){ out[PART_L_HIP].x - 1, out[PART_L_HIP].y + ch->bone_thigh };
out[PART_L_FOOT] = (Vec2){ out[PART_L_HIP].x - 1, out[PART_L_HIP].y + ch->bone_thigh + ch->bone_shin };
// R mirror.
```

Use 2-bone IK with the FOOT target slightly offset from the HIP and a
fixed bend_sign for the knee. Or just compute analytically as above —
the legs are nearly straight, so IK is overkill here. Pick whichever
is more readable; tests determine correctness.

### 7.7 Legs — RUN gait

Gait phase ∈ [0, 1). Each foot does a cycle:

```c
const float STRIDE  = 28.0f;
const float LIFT_H  = 9.0f;
float foot_y_ground = out[PART_L_HIP].y + ch->bone_thigh + ch->bone_shin;
float dir   = in->facing_left ? -1.0f : 1.0f;
float front =  STRIDE * 0.5f * dir;
float back  = -STRIDE * 0.5f * dir;
float p_l = in->gait_phase;
float p_r = p_l + 0.5f; if (p_r >= 1.0f) p_r -= 1.0f;

Vec2 lfoot, rfoot;
// stance phase (p < 0.5): foot slides back along ground
if (p_l < 0.5f) {
    float u = p_l * 2.0f;
    lfoot = (Vec2){ out[PART_L_HIP].x + front + (back - front) * u, foot_y_ground };
} else {
    // swing phase: foot lifts in an arc, comes around to front
    float u = (p_l - 0.5f) * 2.0f;
    lfoot = (Vec2){ out[PART_L_HIP].x + back + (front - back) * u,
                    foot_y_ground - LIFT_H * sinf(u * PI) };
}
// rfoot same with p_r
```

Then IK the knees from HIP toward each FOOT target. **bend_sign = +1**
for both knees (knee points "forward" relative to the screen-perp
direction). Verify with a render test; if wrong, flip and lock.

The numbers (`STRIDE`, `LIFT_H`) are the same as `build_pose` so the
gait shape matches the existing animation.

### 7.8 Legs — JET

Both feet swept back behind the body:

```c
float dir = in->facing_left ? 1.0f : -1.0f;   // sweeps opposite to facing
out[PART_L_FOOT] = (Vec2){ out[PART_L_HIP].x + dir * 12,
                            out[PART_L_HIP].y + ch->bone_thigh + ch->bone_shin - 4 };
// IK the L_KNEE from L_HIP toward L_FOOT.
```

### 7.9 Dismemberment

Any limb with its bit set in `dismember_mask`:
- The limb's particles are NOT in the live skeleton anymore — they
  are free-flying Verlet bodies from `mech_dismember`.
- For these particles, **skip pose computation** and leave their
  positions whatever Verlet last produced.
- Render reads `mech->dismember_mask` and either omits the limb
  (sprite path) or hides it (capsule path) — already handled.

In `pose_write_to_particles`, gate per-particle:

```c
for (int i = 0; i < PART_COUNT; ++i) {
    if (limb_dismembered_for_part(i, mask)) continue;
    p->pos_x[base + i] = out[i].x;
    p->pos_y[base + i] = out[i].y;
    p->prev_x[base + i] = out[i].x;   // zero injected velocity (kinematic)
    p->prev_y[base + i] = out[i].y;
}
```

`prev_x/y = pos_x/y` is critical — same pattern as
`physics_translate_kinematic`. Without it, the Verlet integrate would
read the bone displacement as injected velocity for the LOCAL mech
(physics still runs on it).

### 7.10 Grapple

If `m->grapple.state == GRAPPLE_ATTACHED`, the R_HAND target is the
anchor point instead of the aim line. Existing render code already
draws the rope from R_HAND; we just need the pose function to put
R_HAND at the anchor when attached.

The grapple state ships through the snapshot (`SNAP_STATE_GRAPPLING`
+ the 8-byte suffix). Add `grapple_state` and `grapple_anchor` to
`PoseInputs` and the function checks it before computing the aim arm.

---

## 8. SFX bug fix (Bug B) — small, separate change

`src/net.c::client_handle_fire_event` currently:

```c
bool predict_drew = is_self && from_active_slot;
if (!predict_drew) {
    /* sparks */
    for (int k = 0; k < 3; ++k) fx_spawn_spark(...);
    /* SFX */
    audio_play_at(fsfx, origin);
}
```

The `predict_drew` flag conflates two questions: "did predict draw
the sparks?" and "did predict play the SFX?". `weapons_predict_local_fire`
plays SFX only for `WFIRE_HITSCAN` but spawns sparks for all weapon
kinds. Split them:

```c
bool predict_drew_sparks = is_self && from_active_slot;
bool predict_drew_sfx    = is_self && from_active_slot && wpn->fire == WFIRE_HITSCAN;
if (!predict_drew_sparks) {
    for (int k = 0; k < 3; ++k) fx_spawn_spark(...);
}
if (!predict_drew_sfx) {
    SfxId fsfx = audio_sfx_for_weapon((int)weapon);
    if (wpn->fire == WFIRE_GRAPPLE) fsfx = SFX_GRAPPLE_FIRE;
    audio_play_at(fsfx, origin);
}
```

Or, easier and more maintainable: extend the predict path to play
SFX for every fire kind, and keep the existing single `predict_drew`
gate. Either fix works; the split-gate approach localizes the change
to net.c and avoids touching weapons.c.

Add a unit-ish test: `tests/shots/net/run_riot_cannon_sfx.sh` —
paired-process scenario, host fires Riot Cannon, host's log expects
`SFX play SFX_WPN_RIOT_CANNON` line.

Note: the host fires by holding LMB which goes through both the
predict path (visual) AND the server simulate path (audio at
`weapons_spawn_projectiles` line 622). For Bug B to actually fire on
the SERVER thread, that audio_play_at call also needs to work — and
per the wan-fixes-16 trade-off, the server thread shares the same
audio device (raylib audio is process-global). Verify in test.

---

## 9. Implementation phases

Each phase ends with the build green and the existing tests passing.

### Phase 1 — SFX bug fix

Smallest, separable from the IK work. Land first.

- `src/net.c::client_handle_fire_event`: split `predict_drew` into
  `_sparks` and `_sfx` per §8.
- New paired-shot test
  `tests/shots/net/run_riot_cannon_sfx.sh` that asserts the SFX play
  log line on the host (firing window).
- Update CURRENT_STATE.md.

Commit. Done. Move on.

### Phase 2 — Stand up `mech_ik.{c,h}` skeleton

- Create `src/mech_ik.h` with the API in §5.
- Create `src/mech_ik.c` with `mech_ik_2bone` only.
- Unit test `tests/mech_ik_test.c` with the cases in §10.1.
- Wire to Makefile via the standard HEADLESS_OBJ pattern.
- Build green, test green. Don't touch mech.c yet.

### Phase 3 — Procedural pose function

- Implement `pose_compute(...)` in `mech_ik.c`. Pass the chassis
  pointer + PoseInputs in; return PoseBones out. Pure function.
- Match `build_pose` output on flat-ground STAND first; iterate to
  match RUN gait; then JET, FALL, FIRE.
- Add `tests/pose_compute_test.c` exercising every anim_id with
  fixed inputs and asserting bone positions within 0.5 px tolerance
  (the IEEE754 reorder budget).

### Phase 4 — Wire-format extension

- Add `gait_phase_q` to `EntitySnapshot` in `src/snapshot.h`.
- Encode in `snapshot_record` from the authoritative mech's
  `anim_time * cycle_freq - floor(...)`.
- Decode in `snapshot_decode` (place between `ammo_secondary` and
  the grapple suffix).
- Bump `SOLDUT_PROTOCOL_ID` to `S0LJ` in `src/version.h`.
- Add a `gait_phase` field to `Mech` (a `float` — already there as
  `gait_phase_l/_r` for the footstep wrap, but a single canonical
  `gait_phase` is what the snapshot ships). Reuse `gait_phase_l` if
  you can — it already covers the semantic.
- Update `tests/snapshot_test.c` to round-trip the new field.

### Phase 5 — Switch every mech to procedural pose

- Replace `build_pose` + `apply_pose_to_particles` in
  `mech_step_drive` with `pose_compute` + `pose_write_to_particles`.
- Remove the `!w->authoritative && m->id != w->local_mech_id`
  early-return from `apply_pose_to_particles` (the procedural pose
  is safe to run on remote mechs because it's pure-input, no
  feedback loop).
- Re-run paired-shot tests to verify host's and client's bone
  positions match within tolerance.

### Phase 6 — Cleanups

- The constraint solver still runs for the local mech; that's fine
  because the procedural pose is the LAST writer per tick. Run an
  explicit `pose_write_to_particles` AFTER `mech_post_physics_anchor`.
- Delete `build_pose`'s now-dead pose_target / pose_strength code
  (or shrink it to a stub). Keep `pose_target`/`pose_strength` fields
  on Mech for the dismemberment path that still relies on free-flying
  Verlet.
- Update TRADE_OFFS.md: delete "Remote mechs render in rest pose on
  the client" and "Left hand has no pose target". Add any new
  entries pre-disclosed below.

### Phase 7 — Documentation

- Update `documents/03-physics-and-mechs.md` to note that the
  live-mech pose is procedural; Verlet is for prediction (local mech)
  + ragdoll + projectile collision against bone capsules only.
- Append a "Status" footnote at the end of THIS document with the
  phase landing dates and the commits.

---

## 10. Test plan

### 10.1 Unit tests for `mech_ik_2bone`

`tests/mech_ik_test.c`, asserts and returns non-zero on failure.

Cases (all in world coords, y-down):

| Case | s | t | L1 | L2 | bend | expect joint | expect reachable |
|---|---|---|---|---|---|---|---|
| Reach right | (0,0) | (50, 0) | 30 | 30 | +1 | (25, +√(900−625)) ≈ (25, +16.58) | true |
| Reach right, opposite bend | (0,0) | (50, 0) | 30 | 30 | -1 | (25, -16.58) | true |
| Aim straight up | (0,0) | (0, -50) | 30 | 30 | +1 | (-16.58, -25) | true |
| Out of reach | (0,0) | (100, 0) | 30 | 30 | +1 | (30, 0) (full extension along axis) | false |
| Collinear collapsed | (0,0) | (5, 0) | 30 | 30 | +1 | (30, 0) (degenerate) | false |
| Asymmetric | (0,0) | (40, 30) | 25 | 35 | +1 | (~24, ~8) (computed) | true |
| Zero distance | (0,0) | (0, 0) | 30 | 30 | +1 | (30, 0) or any stable point | false |

Tolerance: 0.01 px. Floating-point noise should be well under that.

### 10.2 Pose continuity test

`tests/pose_continuity_test.c`. Drive `pose_compute` with the same
inputs over 600 ticks; assert that NO bone position drifts (each tick's
output equals the previous tick's output bit-for-bit when inputs are
constant). Catches non-determinism (uninitialized memory, hash-of-
pointer style nondeterminism).

Drive with a slowly-advancing `gait_phase` and assert that bone
positions advance smoothly (no >50 px frame-to-frame jumps).

### 10.3 Pose-sync paired-shot test

New `tests/shots/net/2p_pose_sync.{host,client}.shot`. Both spawn on
Foundry, host walks left+right, jets up, fires. Client observes. At
the end:

```bash
# tests/shots/net/run_pose_sync.sh
diff <(grep "^bone-dump host" build/shots/net/2p_pose_sync.host/...log) \
     <(grep "^bone-dump host" build/shots/net/2p_pose_sync.client/...log)
# Tolerance: per-bone position diff ≤ 1 px.
```

`bone-dump` is a SHOT_LOG line we add to `simulate.c::shot_dump_tick`
that prints the WORLD's view of `mech=0`'s 16 bone positions every
tick. The host's view of itself is the authoritative source; the
client's view must match within 1 px.

### 10.4 Slope-traverse test

`tests/shots/net/2p_slope_aurora` (already exists) becomes a
regression for both the slope-stretch fix AND the procedural pose.
Assert `chest_dy = -ch->torso_h ± 0.5` across the whole climb on
both host and client.

### 10.5 Riot-cannon-SFX test

`tests/shots/net/run_riot_cannon_sfx.sh`. Two windows; one fires Riot
Cannon; assert log contains an SFX line in the firer's log. Other
non-hitscan weapons (Auto-Cannon, Plasma SMG, Mass Driver) get a
parametric variant.

### 10.6 Existing tests must stay green

- `tests/headless_sim.c` — no asserts but visually checks pose
  stability.
- `tests/net/run.sh` (13/13) — must still pass.
- `tests/net/run_3p.sh` (10/10).
- `tests/shots/m5_*.shot` — keep their existing shot fixtures
  passing; pose deltas under tolerance.
- `tests/shots/m5_drift_isolate.shot` — the "Trooper stands still
  for 200 ticks; pelvis must not drift" regression. Procedural
  pose should make this trivially true.

---

## 11. Bandwidth + budget impact

| Change | Per-tick cost |
|---|---|
| `gait_phase_q` u16 per entity | +2 bytes/mech |
| At 32 mechs × 60 Hz | +3840 bytes/s = ~30 kbps |

Well inside the 80 kbps budget. The protocol_id bump means existing
clients can't connect to new servers (and vice versa), which is the
intended behavior — the format changed.

CPU cost of `pose_compute`: ~32 sqrts + ~16 atan2s per mech per tick.
At 32 mechs that's ~1500 trig ops per tick = trivial vs the
constraint solver (12 iters × 8 distance constraints × 32 mechs =
3000+ sqrts and they're tighter loops). Net win.

---

## 12. Risks and trade-offs

### Risks
- **Bend-sign tuning**: getting elbow-up vs elbow-down right per
  joint takes a render test. Allocate budget for one round of
  visual iteration after Phase 5 lands.
- **Edge-case IK targets**: a target ON the root (zero distance)
  needs a stable fallback. `mech_ik_2bone` returns `s + (L1, 0)` in
  that case — visually fine. Test in §10.1.
- **Reconciliation jitter on the local mech**: when the local mech
  is reconciled, pelvis_y can jump by a few px. The procedural pose
  follows the pelvis, so bones jump with it. The existing
  `Reconcile.visual_offset` smoother should handle this; verify.
- **Snapshot interp lag**: remote mechs are rendered 100 ms in the
  past. Their pelvis and gait_phase are interpolated between two
  snapshots. The pose is computed from the INTERPOLATED inputs, so
  it animates smoothly even if individual snapshots are 16 ms apart.
- **Floating-point cross-platform determinism**: we don't require
  bit-exact, only ≤ 1 px tolerance. Same compiler, same target
  architecture, same compile flags → same output to 0.01 px. Cross-
  platform play (Linux ↔ Windows ↔ macOS) might see 0.1 px drift —
  well under the 1 px tolerance.

### New trade-offs we expect to log (pre-disclosed)

Add these to `TRADE_OFFS.md` when the work lands:

- **"Live-mech bones are procedural; Verlet is for prediction +
  ragdoll only"** — design doc says Verlet drives everything; the
  reality is that the procedural pose overwrites bone positions
  every tick for live mechs. Justified above; this entry exists
  so future readers know the constraint solver is essentially
  dead code for live mechs.
- **"Footstep SFX fires from the gait_phase wrap in
  `pose_compute`"** — the existing footstep wrap (see
  `build_pose`'s ANIM_RUN case) needs to migrate into the new
  pose function. SFX is a side effect of pose computation, which
  is unusual; isolate it into a callback or a per-tick flag so
  the pose function stays pure (testable in isolation).

### Trade-offs this work RETIRES (deletion targets)

- `Remote mechs render in rest pose on the client (wan-fixes-3)` —
  deleted when Phase 5 lands.
- `Left hand has no pose target (incl. two-handed weapons P11)` —
  deleted when the foregrip-IK path of §7.5 lands. Confirm by
  walking + aiming a Pulse Rifle: L_HAND should land on the
  rifle's foregrip pixel.
- The post-P19 "apply_pose_to_particles skip for remote mechs"
  in `CURRENT_STATE.md` becomes a paragraph in this doc's status
  footnote.

---

## 13. Files to touch (cheat sheet)

| File | Change |
|---|---|
| `src/mech_ik.{c,h}` | NEW: 2-bone IK + `pose_compute` |
| `src/mech.h` | `Mech` struct: ensure `gait_phase` (single canonical field) |
| `src/mech.c` | replace `build_pose`+`apply_pose_to_particles` callsite with `pose_compute`+`pose_write_to_particles` |
| `src/simulate.c` | call pose AFTER `mech_post_physics_anchor`; remove remote-mech inv_mass dance once procedural pose is the only writer (keep for dismembered limbs only) |
| `src/snapshot.h` | add `gait_phase_q`; bump `ENTITY_SNAPSHOT_WIRE_BYTES` 29 → 31 |
| `src/snapshot.c` | encode/decode `gait_phase_q`; populate it on the auth side from `anim_time * cycle_freq` |
| `src/version.h` | bump `SOLDUT_PROTOCOL_ID` to `S0LJ`; doc the new field |
| `src/net.c` | Phase 1 SFX gate fix |
| `src/weapons.c` | optionally play SFX in `weapons_predict_local_fire` for non-hitscan (alternative to net.c gate fix) |
| `tests/mech_ik_test.c` | NEW: unit tests for `mech_ik_2bone` |
| `tests/pose_compute_test.c` | NEW: pose-function output vs fixed expected bones |
| `tests/snapshot_test.c` | extend: round-trip `gait_phase_q` |
| `tests/shots/net/2p_pose_sync.{host,client}.shot` | NEW |
| `tests/shots/net/run_pose_sync.sh` | NEW: diff host vs client bone dumps |
| `tests/shots/net/run_riot_cannon_sfx.sh` | NEW |
| `Makefile` | wire the new tests + new module |
| `CLAUDE.md` | bump status line |
| `CURRENT_STATE.md` | append status note |
| `TRADE_OFFS.md` | delete the retiring entries (§12) + add the new ones |
| `documents/03-physics-and-mechs.md` | note that live mechs use procedural pose; physics is for prediction + ragdoll |
| `documents/m6/01-ik-and-pose-sync.md` | THIS FILE — append a status footnote when each phase lands |

---

## 14. References

External (research):
- [opensoldat/opensoldat on GitHub](https://github.com/opensoldat/opensoldat) — see `shared/Anims.pas`, `shared/Parts.pas`, `shared/mechanics/Sprites.pas`, `shared/network/NetworkServerSprite.pas`, `client/GostekGraphics.pas`.
- [Soldat gostek visualizer (urraka)](https://urraka.github.io/soldat-gostek/) — interactive view of the skeleton hooks.
- [Soldat Mod.ini wiki](https://wiki.soldat.pl/index.php/Mod.ini) — `[GOSTEK]` section describes per-sprite p1/p2/cx/cy anchors.
- [Ryan Juckett — Analytic Two-Bone IK in 2D](https://www.ryanjuckett.com/analytic-two-bone-ik-in-2d/) — the math used in §6.
- [Alan Zucconi — Inverse Kinematics in 2D, Part 2](https://www.alanzucconi.com/2018/05/02/ik-2d-2/) — bend direction + out-of-reach handling.
- [The Orange Duck — Simple Two Joint IK](https://theorangeduck.com/page/simple-two-joint) — alternative reference.

Internal (this codebase):
- `src/mech.c::build_pose` (~lines 510-746) — current pose driver, to be replaced.
- `src/mech.c::apply_pose_to_particles` (~lines 478-508) — current pose nudger, to be replaced.
- `src/physics.c::physics_constrain_and_collide` (~line 673) — constraint solver, stays for ragdoll/projectile collision.
- `src/snapshot.c::snapshot_record / _decode` — wire format, extend for `gait_phase_q`.
- `src/net.c::client_handle_fire_event` (~lines 1787-1849) — predict_drew gate, fix in Phase 1.
- `CURRENT_STATE.md` "post-P19 follow-up #4 / #5 / #6 / #7" — paper trail of the slope-stretch + remote-mech regressions that motivated this rework.
- `TRADE_OFFS.md` "Remote mechs render in rest pose on the client (wan-fixes-3)" and "Left hand has no pose target" — the two entries this work retires.

---

## 15. Definition of done

The work is done when:

1. `tests/mech_ik_test.c` passes 7/7 cases at ≤ 0.01 px tolerance.
2. `tests/pose_compute_test.c` passes (no drift, smooth advance).
3. `tests/snapshot_test.c` round-trips `gait_phase_q`.
4. `tests/shots/net/run_pose_sync.sh` passes: every bone of every
   mech on the client agrees with the host within 1 px.
5. `tests/shots/net/run_riot_cannon_sfx.sh` passes: SFX is heard in
   the firer's window.
6. `tests/net/run.sh` (13 assertions) and `tests/net/run_3p.sh`
   (10 assertions) still pass.
7. `tests/shots/net/run.sh 2p_slope_aurora` (the slope regression):
   no bone drift on either side, no visible inflation.
8. A 60-second paired-window real-play run (host + Direct Connect
   client) on Aurora and on Concourse: both players see the other's
   mech walk, jet, fire, dismember, all in sync.
9. The two retired trade-off entries are removed from
   `TRADE_OFFS.md` and the new ones added per §12.
10. `CURRENT_STATE.md` and `CLAUDE.md` reflect the M6 milestone.

---

## 16. Status footnote (append when work lands)

- Phase 0 — design document drafted 2026-05-12.
- **Phase 1 (2026-05-12) — SFX bug fix shipped.** Split
  `client_handle_fire_event`'s `predict_drew` gate into `_sparks`
  (active-slot self regardless of fire kind) and `_sfx` (additionally
  requires `WFIRE_HITSCAN`). Non-hitscan self-fire SFX now lands on
  the firer's window. New test `tests/shots/net/run_riot_cannon_sfx.sh`
  (3 assertions). Make target `test-riot-cannon-sfx`.
- **Phase 2 (2026-05-12) — `mech_ik.{c,h}` skeleton shipped.** New
  module with `mech_ik_2bone` analytic IK + `pose_compute` stub +
  `pose_write_to_particles`. Unit test `tests/mech_ik_test.c` covers
  the 7 cases from §10.1 (all PASS at 0.01 px tolerance). Sign
  convention noted: doc table §6 had `bend_sign` inverted relative to
  the math comment; the prose underneath was correct. Code follows
  the prose (`facing_left ? -1 : +1` for aim arm).
- **Phase 3 (2026-05-12) — Procedural pose function shipped.**
  `pose_compute` implements §7.1–7.10 fully: pelvis anchor, chassis
  quirks (Scout chest lean / Sniper head bias / Engineer secondary
  arm dangle), R_ARM aim IK, optional L_ARM foregrip IK, RUN gait
  via gait_phase, JET swept legs, STAND/FALL/FIRE straight-down legs,
  grapple ATTACHED hand-at-anchor override. Unit test
  `tests/pose_compute_test.c` covers determinism (600-iter bit-identical
  output), per-anim shape sanity, gait continuity, foregrip
  reachable / out-of-reach, grapple, dummy quirk, and all 5 chassis.
  Gait continuity threshold loosened from 1 px to 5 px to accept the
  sqrt-of-Δd boundary jump at IK reach limit; bounded ~3 px in
  practice, well within frame visual budget.
- **Phase 4 (2026-05-12) — Wire format extension shipped.**
  `EntitySnapshot.gait_phase_q` u16 between `ammo_secondary` and the
  optional grapple suffix. `ENTITY_SNAPSHOT_WIRE_BYTES` 29 → 31.
  Quantization helpers `quant_phase` / `dequant_phase` (Q0.16,
  1/65536 resolution). Protocol id `SOLDUT_PROTOCOL_ID` bumped
  `S0LI` (0x53304C49) → `S0LJ` (0x53304C4A). Snapshot test extended
  with 5 round-trip assertions across [0.0, 0.999].
- **Phase 5 (2026-05-12) — Switch every mech to procedural pose
  shipped.** `mech.c::build_pose` stripped to `mech_update_gait`
  (anim_time + gait_phase_l/r + footstep SFX wrap). `mech_step_drive`
  gates it on `w->authoritative || mid == w->local_mech_id` —
  remote mechs on the client use the snapshot's gait_phase_l.
  `apply_pose_to_particles` deleted; the `inv_mass=0` kinematic gate
  for remote mechs on the client stays (wan-fixes-3 trade-off retired
  for a different reason now). `simulate.c::simulate_step` runs a new
  pass AFTER `mech_post_physics_anchor`: builds `PoseInputs` per
  alive mech, calls `pose_compute` + `pose_write_to_particles`.
  Foregrip world is computed from `weapon_sprite_def(weapon_id)` +
  the R_HAND aim ray. Footstep SFX for remote mechs on the client
  fires from `snapshot_apply`'s gait_phase mirror. All baseline
  paired-shot tests pass (`tests/net/run.sh` 13/13,
  `tests/net/run_3p.sh` 10/10, all `tests/shots/net/run_*.sh` green
  including anim_stability, slope_aurora, rmb_hitscan, secondary_fire,
  kill_feed, grapple_lag_comp, riot_cannon_sfx, frag_grenade).
- **Phase 6 (2026-05-12) — Cleanups + TRADE_OFFS update.** Added
  `is_dummy` to `PoseInputs` so dummies dangle the right arm instead
  of aiming at default state. Mech.h `pose_target`/`pose_strength`
  fields retained (memory-only — no readers, but cheap and reserved
  for future dismember-pose work per design doc §12). `mech_kill`'s
  `clear_pose` call removed (the alive guard in simulate's pose pass
  is now what stops pose drive on death). TRADE_OFFS.md retired the
  two entries from §12 (remote mechs in rest pose, no L_HAND IK) and
  added the two new ones (live-mech procedural; footstep SFX two
  paths).
- **Phase 7 (2026-05-12) — Documentation.** CURRENT_STATE.md +
  CLAUDE.md status line updated. This footnote appended.
  `documents/03-physics-and-mechs.md` now notes that live-mech bones
  are procedural; the Verlet constraint solver is for ragdoll +
  projectile collision against bone capsules only.
