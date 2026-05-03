# 03 — Physics & Mechs

This document specifies how mechs are constructed, animated, and torn apart. The physics here is **load-bearing for game-feel**: the way a mech moves and breaks is most of what the player remembers about a match.

> **At M1, the shipped behavior diverges from this spec in a few
> places** — most notably 60 Hz sim instead of 120 Hz, no angle
> constraints in active use, and a kinematic post-physics anchor for
> the standing pose. See [CURRENT_STATE.md](../CURRENT_STATE.md) and
> [TRADE_OFFS.md](../TRADE_OFFS.md) for the actual state and the
> "revisit when" triggers.

## Pillars

1. **One physics model.** Alive players, dead bodies, gibs, projectiles — all run through the same Verlet particle/constraint solver. Death is a state change in animation, not a switch to a different physics engine. (This is what Soldat does, and it's why ragdolls feel right.)
2. **Single-bone-per-polygon rigid skinning.** Mech bodies are flat polygon plates rendered along a bone. No vertex weighting. (Spine, DragonBones, Soldat's gostek all do this.)
3. **No mech-vs-mech collision.** Live mechs pass through each other. Bullets do the colliding. Dead mechs *do* collide with the world.
4. **Dismemberment is constraint deletion.** When a limb's HP hits zero, we delete the joint constraint connecting it to the parent. The limb becomes a free ragdoll piece.

## The skeleton

Every mech, regardless of chassis, uses the same 14-particle skeleton:

```
                  [HEAD]
                    │ neck
                  [CHEST]
       L_shoulder /  │ spine
                /   │
        [L_HAND]─[L_ELBOW]──[L_SHOULDER]──[CHEST]──[R_SHOULDER]──[R_ELBOW]─[R_HAND]
                            │
                          [PELVIS]
                          /     \
                   [L_HIP]      [R_HIP]
                    │              │
                   [L_KNEE]       [R_KNEE]
                    │              │
                   [L_FOOT]       [R_FOOT]
```

Particle list (these become array indices in the engine; see Rule 1 in [01-philosophy.md](01-philosophy.md)):

```c
enum {
    PART_HEAD = 0,
    PART_NECK,
    PART_CHEST,
    PART_PELVIS,
    PART_L_SHOULDER, PART_L_ELBOW, PART_L_HAND,
    PART_R_SHOULDER, PART_R_ELBOW, PART_R_HAND,
    PART_L_HIP, PART_L_KNEE, PART_L_FOOT,
    PART_R_HIP, PART_R_KNEE, PART_R_FOOT,
    PART_COUNT
};
```

That's 16 particles. (Two more than the diagram lists because shoulders and hips are joint particles, not bones.) Each particle is a 2D point with mass.

```c
typedef struct {
    Vec2  pos;       // current position
    Vec2  prev;      // previous position (Verlet)
    float inv_mass;  // 0 = pinned (we don't pin in practice)
    uint8_t flags;   // ACTIVE, GROUNDED, etc.
} Particle;
```

We store these in struct-of-arrays form in the global pool:

```c
typedef struct {
    float    *pos_x, *pos_y;
    float    *prev_x, *prev_y;
    float    *inv_mass;
    uint8_t  *flags;
    int       count;
    int       capacity;
} ParticlePool;
```

For 32 mechs × 16 particles = 512 particles, plus a few hundred for dismembered limbs and dropped weapons, plus debris. **Capacity = 4096** at startup; we never grow.

## Constraints (sticks)

Bones are distance constraints — Jakobsen sticks. Each constraint is two particle indices, a rest length, and a stiffness (for angle limiters).

```c
typedef enum { CSTR_DISTANCE, CSTR_DISTANCE_LIMIT, CSTR_ANGLE } ConstraintKind;

typedef struct {
    uint16_t       a, b;        // particle indices
    uint16_t       c;           // for angle constraints (middle joint)
    ConstraintKind kind;
    float          rest;        // for DISTANCE
    float          min_len, max_len;  // for DISTANCE_LIMIT
    float          min_ang, max_ang;  // for ANGLE
    uint8_t        active;      // false → constraint deleted (dismemberment)
} Constraint;
```

Per mech we have:

- **13 distance constraints** (the structural bones): neck (head↔chest), spine (chest↔pelvis), 2× upper arm, 2× forearm, 2× upper leg, 2× lower leg, 2× shoulder-spans, 1× hip-span. (Some span constraints stabilize the torso.)
- **6 angle constraints** (joint limits): elbows (don't bend backward), knees (don't bend forward), neck (don't break).
- **2–4 limiter sticks** (Jakobsen's trick: distance constraints with `min/max` bounds approximate cone limits cheaply). Used at shoulders and hips where a hard angle constraint would feel mechanical.

That's ~21 constraints per mech, ~672 across 32 mechs. Plus dismembered limb internal constraints (which decrease over time as bodies decay). **Constraint pool capacity = 2048** at startup.

> **M1 note:** the build ships **21 distance constraints** (head/neck/chest
> triangle, spine, clavicles, hips, arms, legs, shoulder span, hip span,
> and L/R shoulder↔pelvis triangulation) and **no angle constraints in
> active use**. The angle solver works but doesn't prevent the failure
> mode it was supposed to prevent (a leg rotating to horizontal still
> has interior angle = π at the knee). See
> [TRADE_OFFS.md](../TRADE_OFFS.md#no-angle-constraints-in-active-use).

## Integration: Verlet, fixed timestep

The simulation runs at **120 Hz** (every 8.33 ms) inside a fixed-step accumulator. Render runs at whatever frame rate the player has, interpolating between the last two simulated states.

> **M1 note:** the build currently runs **one sim tick per render
> frame** (effectively 60 Hz on a vsynced display). No fixed-step
> accumulator, no render-side interpolation alpha. This leaks: jet
> arcs feel different in fullscreen vs small windows. See
> [TRADE_OFFS.md](../TRADE_OFFS.md#60-hz-simulation-not-120-hz).

The Verlet step for one particle:

```c
// Drag (velocity damping) factor; tuned to taste. RKV = "retain kinetic value."
#define RKV 0.99f

static void verlet_step(float *px, float *py,
                        float *qx, float *qy,
                        float ax, float ay, float dt) {
    // Save current pos, then advance.
    float old_x = *px, old_y = *py;
    *px += (*px - *qx) * RKV + ax * dt * dt;
    *py += (*py - *qy) * RKV + ay * dt * dt;
    *qx = old_x;
    *qy = old_y;
}
```

Gravity is `(0, +0.06)` per simulation tick (matches Soldat's order of magnitude — see [reference/soldat-constants.md](reference/soldat-constants.md), `GRAV` and `PARA_SPEED`). We apply it as an acceleration each step.

After integrate, **8 relaxation iterations** of the constraint pass. Per iteration we walk the constraint array and project each one. Distance:

```c
static inline void solve_distance(Particle *a, Particle *b, float rest) {
    float dx = b->pos.x - a->pos.x;
    float dy = b->pos.y - a->pos.y;
    float d2 = dx*dx + dy*dy;
    if (d2 < 1e-6f) return;
    float d = sqrtf(d2);
    float diff = (d - rest) / d;
    float wa = a->inv_mass, wb = b->inv_mass;
    float wsum = wa + wb;
    if (wsum < 1e-6f) return;
    float ka = (wa / wsum) * 0.5f;
    float kb = (wb / wsum) * 0.5f;
    a->pos.x += dx * diff * ka; a->pos.y += dy * diff * ka;
    b->pos.x -= dx * diff * kb; b->pos.y -= dy * diff * kb;
}
```

Eight iterations gives ~95% rigidity (constraints stretch ~5% under load). Increase to 12 if mechs feel rubbery; decrease to 4 for off-screen / far-LOD bodies.

> **M1 note:** the build runs **12 iterations** with **constraints and
> map collisions interleaved inside the same loop**
> (`physics_constrain_and_collide()` in `src/physics.c`). This was
> required to keep a foot held by the floor from sagging the pelvis a
> little each frame: a single constraint pass followed by a single
> collision pass didn't propagate the lift on the same tick. Only the
> last iteration computes contact friction and writes back to `prev`;
> the earlier iterations apply position-only contact resolution.

## Animation: pose-driven

When a mech is alive, an **animator** writes target positions for a subset of particles each tick — usually the head, hands, and feet — and the constraint solver pulls the rest of the body into alignment.

```c
typedef struct {
    Vec2  target_pos[PART_COUNT];   // where this particle wants to be
    float strength[PART_COUNT];     // 0..1; 0 = ignore, 1 = snap exactly
} AnimPose;
```

Each tick, before integration:

```c
for (int i = 0; i < PART_COUNT; ++i) {
    if (pose.strength[i] > 0) {
        // Pull particle toward target.
        // Treat as a one-frame distance constraint to a kinematic anchor.
        particle.pos = lerp(particle.pos, pose.target_pos[i], pose.strength[i]);
    }
}
```

> **M1 note — kinematic translate.** The lerp above must update both
> `pos` *and* `prev` by the same delta. Verlet reads `pos − prev` as
> velocity, so a `pos`-only lerp injects ghost velocity the next tick
> and the body oscillates. The shipped `apply_pose_to_particles()` in
> `src/mech.c` calls `physics_translate_kinematic()` instead of a raw
> lerp.
>
> **M1 note — pose targets are layout-consistent.** Each pose target
> sits at exactly its layout offset from the (possibly anchored)
> pelvis, so the distance between any two pose-driven particles equals
> the rest length of the constraint between them. That removes the
> pose-vs-constraint tug-of-war that otherwise pumps drift velocity
> through the body.
>
> **M1 note — post-physics anchor.** Pose drive alone wasn't enough to
> hold the standing pose: gravity sag accumulated faster than the
> solver could correct it. After the physics step, when a mech is
> grounded *and* in `ANIM_STAND`, `mech_post_physics_anchor()` lifts
> the pelvis + upper body + knees to standing positions kinematically
> and zeroes Y-velocity (`prev_y = pos_y`). This only runs in
> `ANIM_STAND` so run/jump/jet/death respond naturally. See
> [TRADE_OFFS.md](../TRADE_OFFS.md#post-physics-kinematic-anchor-for-standing-pose).

Animations are sequences of poses (think Soldat's `.poa` files — see `Anims.pas` in the original). We store them as compact arrays of `(particle_index, x, y, strength)` per frame. Animation list (matching Soldat's coverage):

`Stand, Run, RunBack, Jump, JetUp, JetSide, Fall, Crouch, CrouchRun, Reload, Throw, Recoil, BigRecoil, ClipOut, ClipIn, Roll, RollBack, Prone, ProneMove, GetUp, Aim, Fire, Melee, Death`.

24 animations. Frame counts vary: `Stand` is 8 frames looped, `Death` is 30 frames non-looped, `Run` is 12 frames looped.

When the mech dies:

```c
mech.alive = false;
for (int i = 0; i < PART_COUNT; ++i) pose.strength[i] = 0;
```

That's it. The same Verlet integrate + the same constraints run, with no external pose drive. The body becomes a ragdoll. Apply an extra impulse from the killshot's direction and magnitude:

```c
particle_at(hit_part).pos += dir * impulse;
```

The ripple through constraints does the rest.

## Dismemberment

Each **limb** (arm, leg, head) tracks an HP counter (separate from total mech HP). When a body part is hit, both counters decrement. Limb HP defaults: arm 80, leg 80, head 50. (Dismembering the head is rare — and brutal.)

When a limb's HP reaches zero:

1. Mark the joint constraint(s) anchoring the limb as `active = false`.
2. Walk down the limb's particles, mark them as part of a new "loose ragdoll" group.
3. Spawn a blood emitter at the joint position, pinned to the **parent** particle.
4. Spawn a "stump cap" sprite at the parent end of the broken bone.

```c
void dismember(Mech *m, int joint_idx) {
    m->joints[joint_idx].active = false;
    // The limb particles are still in the global pool.
    // They're still connected to each other by their internal constraints.
    // They just no longer connect to the parent body.
    spawn_blood_emitter(m->joints[joint_idx].pos, /*parent*/ m->id, joint_idx);
}
```

The constraint solver doesn't change behavior — it just sees fewer constraints. The dismembered limb continues to be Verlet-integrated, falls, bounces off geometry, and is eventually GC'd when off-screen long enough.

## Collision

### Map collision

Maps are **tile grids with optional polygons per tile** (see [07-level-design.md](07-level-design.md)). Per particle, per tick, after constraint relaxation:

1. Compute particle's tile coordinate.
2. Look up the small set of polygons (at most ~4) overlapping the particle's swept AABB.
3. For each polygon: closest-point query. If `dist < radius`, push the particle out along the polygon's normal by `(radius - dist)`.
4. Apply tangential friction: damp velocity along the surface tangent.

```c
typedef struct {
    Vec2  v[3];        // triangle vertices
    Vec2  normal[3];   // edge normals
    uint8_t kind;      // SOLID, ICE, DEADLY, ONE_WAY
    float bounce;      // restitution coefficient, 0..1
} TilePoly;
```

This matches Soldat's `PolyMap.pas` approach but tiled rather than free-form. Tile size = 32 px world space; broadphase is a flat 2D array indexed by `(tx, ty)`.

### Bullet collision

Bullets are **swept circles** vs **bone segments**. Each bone segment is the line between its two particles. For each bullet, for each candidate bone (broadphase grid lookup):

```c
bool bullet_vs_bone(Bullet *b, Particle *p1, Particle *p2, float bone_r,
                    Vec2 *out_hit, int *out_part) {
    // Sweep b->pos to b->pos + b->vel * dt as a circle of b->radius.
    // Test against capsule formed by [p1, p2] of radius bone_r.
    // Return earliest TOI within (0, 1].
}
```

If hit, apply damage (see [04-combat.md](04-combat.md)) and apply the bullet's impulse to the hit particle: `p->pos += dir * push`. The impulse ripples through the body. Recoil ripples through *your own* body the same way (apply to your hand particle).

### Mech-vs-mech (NO)

We deliberately skip body-on-body collision for live mechs. Reasons:

- Avoids the worst of ragdoll-pile stability problems.
- Keeps the gameplay fast; you don't get stuck on teammates.
- Halves collision cost.
- It's how Soldat works, and Soldat works.

Dead mechs (`alive == false`) **do** collide with map polygons (so they don't fall through floors) but **not** with each other or live mechs. Pile-ups don't pile.

If we want a hint of crowd separation visually, we add a soft "personal space" repulsor on the **root particle only** (PELVIS), not the whole body. This is a tiny distance constraint that activates if two mechs' pelvis-particles are within ~24 px — a one-frame nudge that pushes them apart cosmetically.

## Movement (per chassis, parameterized)

Per-tick player-input-driven forces are applied to specific particles before integration:

- **Run** (left/right): apply horizontal force to feet when grounded. Magnitude = `RUNSPEED * chassis.run_mult`. (Reference: Soldat's `RUNSPEED = 0.118` per tick.)
- **Jump**: impulse to pelvis vertically. Magnitude = `JUMPSPEED = 0.66`. Once per ground contact.
- **Jet**: while jet button held and fuel > 0, apply continuous force to chest (or pelvis for Heavy). Magnitude = `JETSPEED = 0.10`. Drain fuel.
- **Crouch**: scale the leg constraint rest lengths by 0.7. The body crouches for free.
- **Prone**: scale legs by 0.4 and torso similarly. Mass anchors at pelvis.
- **Roll**: apply rotational impulse via foot+hand+head force vectors. The mech tumbles.

These become a small data-driven table per chassis:

```c
typedef struct {
    float run_mult;
    float jump_mult;
    float jet_mult;
    float fuel_max;
    float fuel_regen;
    float mass_scale;
    Sprite *sprites[PART_COUNT];
    float health_max;
    // ... passives via flags or function pointer
} Chassis;
```

## Ground detection

A particle is "grounded" if any of its body's foot particles touched a SOLID polygon in the previous tick AND the contact normal pointed mostly upward (`normal.y < -0.5`).

```c
mech.grounded = (left_foot.flags & GROUNDED) || (right_foot.flags & GROUNDED);
```

Used to gate jumping, switch animations (`Stand` vs `Fall`), and decide whether running force applies.

## Sleeping

Dead bodies whose particles have all moved less than 0.1 px per tick for 30 consecutive ticks (~0.25 s) go inactive:

```c
mech.sleep_timer++;
if (max_displacement_last_n_ticks(mech, 30) < 0.1f) {
    if (++mech.sleep_timer > 30) mech.flags |= SLEEPING;
}
```

Sleeping mechs skip Verlet integrate and constraint passes entirely. Any external impulse (a passing bullet, an explosion) wakes them. This typically lets us skip ~30% of dead bodies at any moment, freeing 30% of physics budget.

## Performance budget

Target: **3 ms** total physics on a 2018-era mid-range CPU (Intel i5-8300H, 4 cores). With 32 active mechs in heavy combat:

| Pass | Estimated cost |
|---|---|
| Pose drive (alive only) | 0.1 ms |
| Verlet integrate (~600 particles) | 0.2 ms |
| Constraint relaxation (8 iters × ~700 constraints) | 0.7 ms |
| Map collision (~600 particles × broadphase + narrowphase) | 1.0 ms |
| Bullet sweeps (~200 bullets × ~30 bones each broadphase) | 0.5 ms |
| Particles & decals (4000 active) | 0.3 ms |
| Bookkeeping (sleep, activations) | 0.2 ms |
| **Total** | **~3.0 ms** |

If we exceed budget: first measure (perf, NOT speculation), then in priority order — increase sleep aggressiveness, reduce constraint iterations off-screen, reduce particle pool, then SIMD the integrate loop.

We do NOT add a thread for physics in v1. 3 ms on one core is fine.

## What we are NOT doing

- **Box2D / Chipmunk2D.** We don't need rigid-body stacking. We need cheap ragdolls. Verlet sticks win for our use case. (See the research doc derivative — Box2D's Sequential Impulses are gold for crate-stacking puzzles, overkill and wrong-shaped for 32 ragdoll-able mechs.)
- **PBD/XPBD.** Architect particles + constraints to allow upgrading to XPBD later (a one-week refactor) but ship Verlet sticks v1.
- **Determinism.** Snapshot interpolation networking (see [05-networking.md](05-networking.md)) means clients render server state — they don't need to reproduce server physics bit-exactly. Float math is fine. No fixed-point.
- **Continuous collision detection (CCD) on mechs.** Verlet at 120 Hz with reasonable particle radius rarely tunnels. We do approximate CCD on **bullets** (swept circles) because bullets are the fast things. Mechs aren't.
- **3D physics.** It's a 2D game. Period. The "polygon mech" aesthetic is 2D plates rendered along bones; depth is fake (parallax / draw order).

## References

- Jakobsen, T. (2001). *Advanced Character Physics*. The Hitman ragdoll paper. Verlet + relaxation. Read first.
- Müller, M., Heidelberger, B., Hennix, M., Ratcliff, J. (2007). *Position Based Dynamics*.
- Macklin, M., Müller, M., Chentanez, N. (2016). *XPBD: Position-Based Simulation of Compliant Constrained Dynamics*.
- Catto, E. — Box2D GDC talks (2005, 2006, 2009, 2011, 2014). Useful background on rigid-body solvers we're explicitly *not* using.
- Soldat source: `Parts.pas` (Verlet system), `Anims.pas` (poses), `PolyMap.pas` (collision), `Constants.pas` (numbers).
- Glenn Fiedler — *Game Physics* article series, especially *Fix Your Timestep!*.
