# M5 — Collision polygons & slopes

The runtime side of "tile grid + free polygons" — how the physics step deals with the new polygon list, how slopes work, and where one-way platforms live in the collision pipeline.

The format that delivers polygons to the runtime is in [01-lvl-format.md](01-lvl-format.md); the editor that produces them is in [02-level-editor.md](02-level-editor.md). This document is the change to `src/physics.c` + `src/level.c` to consume them.

## Why polygons (and why not at M1)

The M1 build runs on **tile-rect-only collision** — `physics_constrain_and_collide` walks each particle's neighbourhood of tiles and pushes the particle out of any SOLID rect. Slopes don't exist; ramps don't exist; one-way platforms don't exist. Maps are visually boxy. This is fine for M1's "test the physics" goal, but maps that read as architecture (catwalks with diagonal supports, slopes between layers, hazard pits) need richer geometry.

Soldat's `PolyMap.pas` shipped with pure polygon soup — no tiles. We chose the hybrid (tile bulk + polygons for the awkward 1%) explicitly per [07-level-design.md](../07-level-design.md). M5 lights up the polygon side.

## What changes

```c
// src/world.h — Level grows
typedef struct {
    int       width, height;       // tiles
    int       tile_size;           // px per tile (32)
    LvlTile  *tiles;               // packed; was uint8_t* at M1, now Tile {id, flags}
    Vec2      gravity;
    Vec2      ambient_light;       // unchanged

    // New at M5:
    LvlPoly  *polys;               // packed AoS of triangles
    int       poly_count;

    LvlSpawn  *spawns;             // moved off `maps.c`'s static lane tables
    int        spawn_count;

    LvlAmbi   *ambis;
    int        ambi_count;

    // Polygon broadphase: for each tile, the indices of polygons whose
    // bounding box overlaps it. Built once at level load. Sized for the
    // 5000-poly cap × ~3 tiles per poly = 15000 entries. Lives in level
    // arena.
    int      *poly_grid;           // flat array, indexed via poly_grid_off
    int      *poly_grid_off;       // length = w*h+1; offsets into poly_grid
} Level;
```

The `Level` struct grows but stays POD. All allocations come from the **level arena** at load time, never from the C heap at runtime.

## Tile flags broaden

`world.h` enums are kept narrow at M1 (`TILE_EMPTY`, `TILE_SOLID`). M5 widens to match the format's flag bits:

```c
typedef enum {
    TILE_F_EMPTY      = 0,
    TILE_F_SOLID      = 1u << 0,
    TILE_F_ICE        = 1u << 1,
    TILE_F_DEADLY     = 1u << 2,
    TILE_F_ONE_WAY    = 1u << 3,
    TILE_F_BACKGROUND = 1u << 4,
} TileFlags;
```

`TILE_F_BACKGROUND` is a no-collision, render-only tile — used for foreground silhouettes that obscure sightlines without blocking. (Before this, foreground silhouettes had to be polygons with `kind = BACKGROUND`; tiles are cheaper for big regions.)

**`TILE_KIND_COUNT` is removed**; existing call sites in `level.c::level_tile_at` switch from `TileKind` to `TileFlags` (a bitmask). Behavior compatibility: the M5 loader presents an old `.lvl v0` file's plain `SOLID/EMPTY` as `TILE_F_SOLID`/`TILE_F_EMPTY` so any cached level data continues to work.

## Polygon broadphase

The 5000-polygon cap (per [07-level-design.md](../07-level-design.md) and Soldat tradition) is enough that an N² particle-vs-polygon test would be 600 particles × 5000 polys = 3 million tests per tick. Way over budget.

We borrow Soldat's **sectors** approach but keyed off the existing tile grid: for each tile, store the indices of polygons whose AABB overlaps that tile. At collision time, we look up the polygons for the particle's tile and test only those.

```c
// Build at level load (level_io.c::level_load):
//
// 1. Compute each polygon's AABB (min/max of its 3 vertices).
// 2. For each polygon, walk every tile its AABB overlaps; append the
//    polygon index to that tile's list.
// 3. Flatten to (poly_grid, poly_grid_off) for cache-friendly access.
```

For a typical map (200 polys, 100×60 tiles), each tile holds ≤4 polygon indices. The broadphase is O(1) per particle.

## Per-iteration collision: tile-rect first, then polygon

The existing physics relaxation loop is `physics_constrain_and_collide` in `src/physics.c`. M5 adds a polygon pass *after* the tile-rect pass on each iteration:

```c
// src/physics.c — pseudo-code for the new shape
void physics_constrain_and_collide(World *w) {
    for (int iter = 0; iter < PHYSICS_CONSTRAINT_ITERATIONS; ++iter) {
        // Constraints (existing)
        for each constraint: solve_distance/...
        // Tile-rect collision (existing)
        for each particle: collide_map_one_pass(...)
        // Polygon collision (new)
        for each particle: collide_polys_one_pass(w, p)
    }
    // Final friction pass on the last iter (existing)
    ...
}
```

Each polygon pass is:

1. Particle's tile coord `(tx, ty)`.
2. Read poly indices from `poly_grid[ poly_grid_off[ty*w+tx] .. poly_grid_off[ty*w+tx+1] )`.
3. For each candidate polygon: closest-point on triangle to particle position.
4. If `dist < radius (=4 px)`: push particle out along the polygon's edge normal by `(radius - dist)`.

Friction is applied tangentially on the last iteration only, mirroring the tile path's discipline.

## Closest-point on triangle

Standard primitive (Möller-Trumbore-adjacent or Eberly's "Geometric Tools" book version). Inline-able in `physics.c`:

```c
static Vec2 closest_point_on_tri(Vec2 p, Vec2 a, Vec2 b, Vec2 c) {
    // Compute barycentric coords; if inside, return p.
    // If outside, project to nearest edge / vertex.
    // ~30 LOC, no allocation.
}
```

Returns the closest point and (out-param) the edge index hit (0/1/2) so the push-out direction is the edge's pre-baked normal from the `LvlPoly` record. Edges from corner cases (vertex-closest) use the average of the two adjacent edge normals.

## Slopes, hills, valleys, angled ceilings — the Soldat feel

This is the load-bearing visual-feel design of M5. **Soldut should feel like Soldat**: you sprint into a downslope and accelerate, you charge uphill and visibly slow, you slide down a steep ramp if you stop pressing forward, and the same geometry vocabulary is available on **floors, platforms, and ceilings** so a map can have an angled overhang under which jet feels different from a flat one.

The current M4 physics path doesn't do any of this. We change three things in `src/physics.c` and `src/mech.c`.

### Where M4 falls short

`src/mech.c::apply_run_velocity` (line ~530) hard-sets the X velocity of every particle:

```c
static void apply_run_velocity(World *w, const Mech *m, float vx_pxs, float dt, bool grounded) {
    float vx_per_tick = vx_pxs * dt;
    if (!grounded) vx_per_tick *= AIR_CONTROL;
    for (int part = 0; part < PART_COUNT; ++part) {
        physics_set_velocity_x(p, m->particle_base + part, vx_per_tick);
    }
}
```

That's "glide horizontally at run speed regardless of terrain." On a 45° slope the body slides along sideways, oblivious to the slope; gravity is the only thing pulling it down, fighting the X-set every tick. Net feel: rubbery, neither slope-aware nor Soldat-like.

`src/physics.c::contact_with_velocity` (line ~228) applies tangential friction at a flat 0.92 (8% loss per contact):

```c
float vtx = (vx - vn * nx) * 0.92f;
float vty = (vy - vn * ny) * 0.92f;
```

Same coefficient on flat ground and on a 50° slope. No sliding, no extra grip on shallow ramps.

`src/mech.c::apply_jet_force` (line ~554) applies thrust as `(0, -dy)` regardless of overhead contact:

```c
float dy = -thrust_pxs2 * scale * dt * dt;
for (...) p->pos_y[idx] += dy;
```

The `JET_CEILING_TAPER_*` constants taper thrust as the head approaches `y=0` (the world top edge), but they don't taper against an *angled overhang* sitting in the middle of the map. Players jetting into an underside flat against a slanted ceiling get an unsatisfying hard stop instead of a slide.

### What we add

Three coupled changes:

1. **Per-particle contact normal** — store the most recent contact normal each particle receives during a tick. Available to `apply_run_velocity` / `apply_jet_force` next tick.
2. **Slope-tangent run velocity** — when a foot is on a sloped surface, the run target velocity vector is *along the slope tangent*, not purely horizontal. Gravity then naturally adds to downhill running and subtracts from uphill running.
3. **Slope-aware friction** — friction coefficient scales with the slope angle. Steeper = lower grip (more slide), shallower = full grip.

Plus the symmetric versions for ceilings:

4. **`PARTICLE_FLAG_CEILING`** — set when contact normal points downward (`ny > 0.5`).
5. **Jet thrust slides along angled ceilings** — the same tangent-projection trick that makes downhill running feel right makes "jet pushing against an angled overhang" slide along the underside instead of hard-stopping.

### Per-particle contact normal

```c
// src/world.h — added to ParticlePool
int8_t  *contact_nx_q;      // last contact normal X, Q1.7 (-128..127 maps to -1..+1)
int8_t  *contact_ny_q;      // last contact normal Y
uint8_t *contact_kind;      // last contact's flag bits (TILE_F_*)
```

Three bytes per particle. For 4096 particle capacity = 12 KB total. Allocated alongside the existing pool data.

`contact_with_velocity` and `contact_position_only` write these fields on every contact. They're zeroed on integrate (no contact this tick = neutral). The most recent contact wins on multi-contact ticks; we accept that simplification.

`PARTICLE_FLAG_GROUNDED` is set when `ny < -0.5` (existing); `PARTICLE_FLAG_CEILING` is set when `ny > 0.5` (new). Slope-without-floor cases (vertical wall, e.g. `|ny| < 0.5`) leave both flags clear.

### Slope-tangent run velocity

```c
// src/mech.c — replaces apply_run_velocity
static void apply_run_velocity(World *w, const Mech *m, float vx_pxs, float dt, bool grounded) {
    ParticlePool *p = &w->particles;
    if (!grounded) {
        // Air: keep the existing AIR_CONTROL × horizontal behavior.
        float vx_per_tick = vx_pxs * dt * AIR_CONTROL;
        for (int part = 0; part < PART_COUNT; ++part) {
            physics_set_velocity_x(p, m->particle_base + part, vx_per_tick);
        }
        return;
    }

    // Average the foot contact normals to get the slope tangent.
    int lf = m->particle_base + PART_L_FOOT;
    int rf = m->particle_base + PART_R_FOOT;
    float nx = (p->contact_nx_q[lf] + p->contact_nx_q[rf]) / 254.0f;
    float ny = (p->contact_ny_q[lf] + p->contact_ny_q[rf]) / 254.0f;
    float nlen = sqrtf(nx*nx + ny*ny);
    if (nlen < 0.5f) {
        // No fresh contact data — fall back to flat-ground behavior.
        nx = 0.0f; ny = -1.0f;
    } else {
        nx /= nlen; ny /= nlen;
    }

    // Slope tangent points in the direction of motion. For a normal
    // that's (0, -1) (flat floor), tangent is (1, 0) — purely
    // horizontal. For a normal tilted (sin θ, -cos θ) (slope going up
    // to the right), the tangent (cos θ, sin θ) — moving "right and
    // down" along the slope.
    float tx = -ny * (vx_pxs > 0.0f ? 1.0f : -1.0f);
    float ty =  nx * (vx_pxs > 0.0f ? 1.0f : -1.0f);

    // Project run speed onto the tangent. Magnitude matches |vx_pxs|;
    // direction follows the slope.
    float speed = fabsf(vx_pxs);
    float vt_per_tick_x = tx * speed * dt;
    float vt_per_tick_y = ty * speed * dt;

    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        physics_set_velocity_x(p, idx, vt_per_tick_x);
        // Y is set on feet/legs only — upper body keeps the gravity
        // component so the chain doesn't compress against the constraint
        // solver. The constraint relaxation reconciles.
        if (part == PART_L_FOOT || part == PART_R_FOOT ||
            part == PART_L_KNEE || part == PART_R_KNEE) {
            physics_set_velocity_y(p, idx, vt_per_tick_y);
        }
    }
}
```

Why not set the Y component on every particle: doing so propagates the slope-tangent motion uniformly through the body, but the upper body (chest, head, arms) doesn't *want* to be on the slope — it wants gravity. Setting Y only on the lower chain (knees, feet) lets the body's own constraint relaxation adapt the upper body naturally, which produces the visible "leaning forward into the slope" pose for free.

This also gives us downhill acceleration *automatically*: on a downslope, the tangent's Y component is positive (down); add gravity each tick; the body accelerates. We cap downhill speed implicitly by friction (see next section) and explicitly by clamping `physics_set_velocity_y` per-particle to a runtime maximum if needed (M5 ships without the explicit cap; we add it if playtest reveals "you can roll down a long slope at infinity speed").

### Slope-aware friction

```c
// src/physics.c — replaces contact_with_velocity
static inline void contact_with_velocity(ParticlePool *p, int i,
                                         float nx, float ny, float amount) {
    p->pos_x[i] += nx * amount;
    p->pos_y[i] += ny * amount;
    float vx = p->pos_x[i] - p->prev_x[i];
    float vy = p->pos_y[i] - p->prev_y[i];
    float vn = vx * nx + vy * ny;

    // Slope-angle-aware friction. ny tells us how vertical the
    // contact normal is. Floor (ny ≈ -1): full friction (0.92). 45°
    // slope (ny ≈ -0.7): less friction (0.96 — slide a bit). Steep
    // slope (ny ≈ -0.4, 65°): very low friction (0.99 — slide
    // freely).
    float ny_abs = (ny < 0.0f) ? -ny : ny;
    float friction = 0.99f - 0.07f * ny_abs;     // [0.92 .. 0.99]
    if (friction > 0.998f) friction = 0.998f;     // clamp; never freeze

    // ICE polygon kind override.
    if (p->contact_kind[i] & TILE_F_ICE) friction = 0.998f;

    float vtx = (vx - vn * nx) * friction;
    float vty = (vy - vn * ny) * friction;
    p->prev_x[i] = p->pos_x[i] - vtx;
    p->prev_y[i] = p->pos_y[i] - vty;

    // Persist the contact normal for next tick's mech_step_drive.
    p->contact_nx_q[i] = (int8_t)(nx * 127.0f);
    p->contact_ny_q[i] = (int8_t)(ny * 127.0f);
}
```

The friction range `[0.92, 0.99]` is starting; tune in playtest. The point is: **shallower = stickier, steeper = slidier**. Combined with slope-tangent run velocity, the result is the Soldat feel:

- **Walking up a 30° slope**: run impulse along the upward tangent, friction 0.94 (some grip), gravity opposing. Net speed ~70% of flat run.
- **Walking up a 60° slope**: tangent has a large vertical component, gravity heavily opposing, friction 0.985 (slidy). Net speed ~30% of flat run; player visibly struggles.
- **Walking down a 30° slope**: tangent points downward, gravity adds, friction 0.94. Net speed ~120% of flat run.
- **Walking down a 60° slope**: same direction, much higher acceleration, friction 0.985 lets the body slide. Net speed unbounded by friction; the player can outrun their own pose drive and start to fall forward.
- **Standing still on a 60° slope without pressing run**: friction 0.985 isn't enough to overcome gravity-along-tangent — the body slides downhill naturally. **This is the slide-down-to-attack move from Soldat.**
- **Standing still on a 30° slope**: friction 0.94 + gravity-along-tangent (= sin(30°) * gravity) ≈ holds station. Body sits there.

The threshold "below which you slide passively" works out to the slope angle where `sin(angle) * gravity > (1 - friction) * vmax`. We pick friction values empirically so the threshold is around 50° — visually, "looks too steep to stand on" and indeed it is.

### Angled ceilings: jet thrust slides along overhangs

```c
// src/mech.c — replaces apply_jet_force
static void apply_jet_force(World *w, const Mech *m, float thrust_pxs2, float dt) {
    ParticlePool *p = &w->particles;
    int b = m->particle_base;
    float head_y = p->pos_y[b + PART_HEAD];

    // World-edge ceiling taper (existing).
    float scale = 1.0f;
    if (head_y < JET_CEILING_TAPER_BEGIN) {
        if (head_y <= JET_CEILING_TAPER_END) scale = 0.0f;
        else scale = (head_y - JET_CEILING_TAPER_END) /
                     (JET_CEILING_TAPER_BEGIN - JET_CEILING_TAPER_END);
    }
    if (scale <= 0.0f) return;

    // Per-particle: if a particle has a ceiling contact, project the
    // thrust direction onto the ceiling tangent.
    float fy = -thrust_pxs2 * scale * dt * dt;     // base impulse: straight up
    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        if (p->flags[idx] & PARTICLE_FLAG_CEILING) {
            // Ceiling contact: project the (0, fy) impulse onto the
            // tangent perpendicular to the contact normal. For a flat
            // ceiling (n = (0, +1)), tangent is (1, 0); the full
            // upward thrust is killed and we add nothing. For a 45°
            // angled ceiling (n = (-0.7, +0.7)), tangent is
            // (0.7, 0.7) or (-0.7, -0.7); the upward thrust gets
            // redirected sideways, sliding the head along the
            // underside.
            float nx = p->contact_nx_q[idx] / 127.0f;
            float ny = p->contact_ny_q[idx] / 127.0f;
            float tx = -ny;
            float ty =  nx;
            float dot = 0.0f * tx + fy * ty;        // (0, fy) ⋅ tangent
            // Pick the tangent direction the player's run input wants
            // (left/right) — same sign as (tx).
            float sign = (m->latched_input.buttons & BTN_LEFT)  ? -1.0f
                       : (m->latched_input.buttons & BTN_RIGHT) ? +1.0f
                                                                 :  0.0f;
            if (sign != 0.0f) {
                p->pos_x[idx] += tx * fabsf(dot) * sign;
                p->pos_y[idx] += ty * fabsf(dot) * sign;
            }
            // No vertical component: the ceiling ate the upward push.
        } else {
            // No ceiling contact: full upward thrust as before.
            p->pos_y[idx] += fy;
        }
    }
}
```

The tangent projection means jet against an angled ceiling becomes "slide along the underside in the direction the player is leaning." Visually: jet up, hit the angled overhang, get pushed sideways down the slope of it, can choose left or right via the run input. **Combined with downward-angled platforms, this is the Soldat-style aerial maneuver vocabulary.**

For perfectly flat ceilings (the `JET_CEILING_TAPER_*` y=0 case), the existing taper still applies and the tangent projection does nothing visible.

### Concrete editor presets for slope-friendly geometry

The editor's polygon palette adds 8 preset slopes plus 4 ceiling presets:

```
floor presets:
  ramp_up_30°    — single triangle, 1.5 tiles wide × 1 tile tall
  ramp_up_45°    — 1×1
  ramp_up_60°    — 1 wide × 2 tall (steep)
  ramp_down_30°  / ramp_down_45° / ramp_down_60° — mirrors

valley presets:
  bowl_30°       — two opposing 30° ramps meeting at a 1-tile flat bottom
  bowl_45°

ceiling presets (mirror the floors):
  overhang_30° / overhang_45° / overhang_60°
```

Each preset is a **single SOLID polygon** that the editor drops into place when the user clicks. For "ramp_up_45° with a 4-tile run," the user clicks four times, one per tile-step; the editor stacks 4 instances. Stitching is not automatic — if the user wants a custom slope, they draw it freehand with the polygon tool.

The presets are parameterized by **height** (1, 2, 3 tiles) and **angle** (30°, 45°, 60°). 18 named buttons total in the slope palette tab.

### Slope discipline in maps

[07-maps.md](07-maps.md) §"Slopes, hills, valleys, angled ceilings" specifies which features each of the 8 ship maps uses. Bake-test acceptance includes "every slope category appears on at least 2 maps."

### Slope-aware post-physics anchor

The M1 fix `mech_post_physics_anchor()` zeros `prev_y = pos_y` on the upper body when grounded + standing/running — it was load-bearing at M1 because gravity sag accumulated faster than the constraint solver corrected it.

**The slope physics breaks this**. On a 60° slope with no run input, the body should slide passively downhill. But the post-physics anchor zeros Y-velocity every tick, killing the slide. The slope feel doesn't work.

Fix: gate the anchor on **flat contact only**:

```c
// src/mech.c::mech_post_physics_anchor — added at the top
ParticlePool *p = &w->particles;
int lf = m->particle_base + PART_L_FOOT;
int rf = m->particle_base + PART_R_FOOT;
float ny_l = p->contact_ny_q[lf] / 127.0f;
float ny_r = p->contact_ny_q[rf] / 127.0f;
float ny_avg = (ny_l + ny_r) * 0.5f;
if (ny_avg > -0.92f) return;       // sloped — let slope physics handle pose
/* ... existing anchor body unchanged ... */
```

The threshold `-0.92` corresponds to ~22° from vertical — flat enough that the standing pose anchor is still appropriate, sloped enough that slope physics takes over. Tune in playtest if needed.

Verifying the fix:
- Mech standing on a 30° slope with no input: holds station (gravity-along-tangent ≤ kinetic-friction-overcome threshold).
- Mech standing on a 60° slope with no input: slides downhill passively (the slide-down-fast feel).
- Mech standing on flat ground: anchor fires; pose stays rigidly upright.
- Mech running on a 30° slope: tangent-projected velocity carries it; anchor doesn't fire (so gravity still adds downhill / subtracts uphill).

This change is small but load-bearing for the slope feel. ~6 LOC added to `mech_post_physics_anchor`. No other changes needed.

### Performance impact

Per-particle contact normals add 12 KB total RAM and one extra `int8_t × 2` write per contact. Negligible — the contact path already does an arithmetic-heavy push-out.

The slope-tangent math in `apply_run_velocity` is one normalization + one cross-product per mech per tick, ~32 mechs × negligible cost.

Total budget impact: <0.05 ms per simulate tick. Inside slack.

## ICE tiles and polygons

`TILE_F_ICE` and `kind = ICE` polygons reduce the friction coefficient applied on the last iteration:

```c
// src/physics.c — pseudo-code
float friction = 0.85f;                  // default
if (contact_flags & TILE_F_ICE) friction = 0.05f;  // slick
// tangential vel *= friction
```

This is one branch in the contact-resolution code path; keeps the hot loop tight.

## DEADLY tiles + ACID ambient zones

`TILE_F_DEADLY` and `kind = DEADLY` polygons apply 5 HP/s damage on contact (per the Soldat tradition). Implemented in `src/mech.c` as a per-particle check after the physics step: any of the mech's particles overlapping a deadly contact triggers damage on the mech.

```c
// src/mech.c — new helper called after physics
static void apply_environmental_damage(World *w, int mech_id, float dt) {
    Mech *m = &w->mechs[mech_id];
    if (!m->alive) return;
    bool in_deadly = false;
    for (int part = 0; part < PART_COUNT; ++part) {
        int idx = m->particle_base + part;
        TileFlags flags = level_flags_at(&w->level, /*pos*/...);
        if (flags & TILE_F_DEADLY) { in_deadly = true; break; }
        // (polygon check via broadphase)
    }
    if (in_deadly) {
        mech_apply_damage(w, mech_id, PART_PELVIS, 5.0f * dt,
                         (Vec2){0,-1}, /*shooter*/-1);
    }
}
```

The same check covers AMBI zones with `kind = ACID`. Both share the 5 HP/s rate.

## ONE_WAY platforms

A one-way tile or polygon blocks particles only when they approach from one side (the "top," typically). Implementation: the contact resolver checks the particle's `prev` position. If `prev` was on the "blocked" side and `pos` is on the "passable" side, push back. If `prev` was on the "passable" side, pass through.

For tiles, "blocked side" is always *up* — particles falling down hit the top. For polygons, the editor records the "up" direction (see [02-level-editor.md](02-level-editor.md) §"Polygon kinds") and the runtime uses the polygon's `normal[0]` as the blocked-direction reference.

A particle entering a one-way from below passes through without interaction. A particle landing on top sticks (tangential friction applied). The crouch button (BTN_CROUCH) drops through a one-way platform: while crouched, the contact check inverts to "particles can pass through downward."

## WIND ambient zones

`AMBI` records with `kind = WIND` apply a per-tick force to mech particles inside the rectangle. The force is `(strength * dir.x, strength * dir.y)` per tick, applied during the gravity pass:

```c
// src/physics.c — apply_ambient_forces, called from physics_apply_gravity
void physics_apply_ambient_forces(World *w, float dt) {
    for (int z = 0; z < w->level.ambi_count; ++z) {
        const LvlAmbi *a = &w->level.ambis[z];
        if (a->kind != AMBI_WIND) continue;
        float fx = (a->dir_x_q / 32767.0f) * (a->strength_q / 32767.0f);
        float fy = (a->dir_y_q / 32767.0f) * (a->strength_q / 32767.0f);
        for (int p = 0; p < w->particles.count; ++p) {
            float px = w->particles.pos_x[p], py = w->particles.pos_y[p];
            if (px < a->rect_x || px > a->rect_x + a->rect_w) continue;
            if (py < a->rect_y || py > a->rect_y + a->rect_h) continue;
            // Apply as velocity nudge via prev (Verlet).
            w->particles.prev_x[p] -= fx * dt;
            w->particles.prev_y[p] -= fy * dt;
        }
    }
}
```

`ZERO_G` ambient zones zero out the gravity contribution for particles inside them, in the same per-tick pass. `FOG` is render-only.

## Bullet vs polygon

Hitscan rays already use `level_ray_hits` (DDA) for tile-grid intersection. M5 extends this with a polygon sweep: after the DDA returns its earliest tile hit, we test the ray against all polygons whose AABB the ray crosses (using the same broadphase grid the particle path uses).

```c
// src/level.c — extended
bool level_ray_hits(const Level *l, Vec2 a, Vec2 b, float *out_t) {
    float t_tile = 1.0f;
    bool tile_hit = level_ray_hits_tile(l, a, b, &t_tile);
    float t_poly = 1.0f;
    bool poly_hit = level_ray_hits_poly(l, a, b, &t_poly);
    float t = (tile_hit && poly_hit) ? fminf(t_tile, t_poly)
            : tile_hit                 ? t_tile
            : poly_hit                 ? t_poly
                                       : 1.0f;
    if (out_t) *out_t = t;
    return tile_hit || poly_hit;
}
```

Polygon-vs-ray: standard segment-vs-triangle test (Möller-Trumbore in 2D). Each ray tests at most ~16 polygons (coarse upper bound); negligible cost compared to the 600 bone-segment tests every hitscan already runs.

`BACKGROUND`-kind polygons are skipped in this path — the bullet passes through. ICE and DEADLY are still SOLID for the purpose of ray-stop.

## Projectile vs polygon

Already uses `level_ray_hits` in `projectile.c::projectile_step`. Inheriting the polygon path is automatic — the call site doesn't change.

Bouncy frag grenades that hit a polygon use the polygon's pre-baked edge normal for the reflection (instead of the cheap "bigger-velocity-axis" approximation that tile bounces use). Keeps grenade physics legible on slopes.

## Performance budget

Per [10-performance-budget.md](../10-performance-budget.md), map collision is budgeted at 1.0 ms. Adding the polygon path:

| Pass | Estimated cost |
|---|---|
| Tile-rect collision (~600 particles × 4 nearby tiles) | 0.7 ms |
| Polygon collision (~600 particles × 4 nearby polys × closest-point) | 0.3 ms |
| **Total map collision** | **1.0 ms** |

Bullet/projectile sweeps were 0.5 ms; adding polygon checks brings them to 0.6 ms. Total tick stays inside the 4.0 ms budget with ~3 ms physics on the reference hardware.

If we exceed budget after first measure: per the budget doc's escalation order — sleep more aggressively, reduce constraint iters off-screen, then SIMD the integrate loops. Polygon collision shouldn't be the first thing we tune.

## Static-poly precondition

The runtime treats polygons as **immutable** once the level is loaded. No per-frame polygon updates, no destructible geometry, no runtime polygon spawning. This is consistent with [07-level-design.md](../07-level-design.md) §"Destructible geometry — NOT at v1."

The one exception: dropped weapons / corpses are not polygons; they're managed by the existing particle/mech systems, which already collide against the tile grid and (now) polygons.

## Spawn point migration

Currently `src/maps.c::map_spawn_point` uses static `g_red_lanes` / `g_blue_lanes` / `g_ffa_lanes` arrays. M5 converts this to read from `world.level.spawns`:

```c
// src/maps.c — new shape
Vec2 map_spawn_point(MapId id, const Level *level, int slot_index,
                     int team, MatchModeId mode)
{
    (void)id;
    // Build the candidate set.
    int candidates[MAX_LOBBY_SLOTS];
    int n = 0;
    for (int i = 0; i < level->spawn_count; ++i) {
        const LvlSpawn *s = &level->spawns[i];
        if (mode == MATCH_MODE_FFA) {
            // FFA: any spawn except spectator-only (team==0).
            if (s->team == 0 || s->team == MATCH_TEAM_FFA) candidates[n++] = i;
        } else {
            if (s->team == team) candidates[n++] = i;
        }
        if (n >= MAX_LOBBY_SLOTS) break;
    }
    if (n == 0) {
        // Shouldn't happen — editor validates ≥1 spawn per team.
        return (Vec2){ level->tile_size * level->width / 2, /* etc */ };
    }
    // Sort by lane_hint, pick (slot_index % n).
    // (sort omitted — editor produces lane_hint in order)
    int chosen = slot_index % n;
    int idx = candidates[chosen];
    return (Vec2){ (float)level->spawns[idx].pos_x,
                   (float)level->spawns[idx].pos_y };
}
```

This means: an editor-authored map with 16 spawns produces 16 spawn lanes; an old code-built map with no spawn data falls back via the same path that handles `level->spawn_count == 0` (use a center-of-map default).

## API additions to `src/level.h`

```c
// Read a tile's flag bitmask. Out-of-bounds reads as TILE_F_SOLID (the
// existing behavior).
TileFlags level_flags_at(const Level *l, int tx, int ty);

// Get the count of polygons whose bbox overlaps tile (tx, ty), and a
// pointer to the first index in poly_grid. Caller iterates count slots.
int  level_polys_at_tile(const Level *l, int tx, int ty,
                         const int **out_indices);

// Closest polygon (or none) to point p, within radius. Used by mech
// damage code for ACID/DEADLY checks. Returns -1 if none.
int  level_polygon_at_point(const Level *l, Vec2 p, float radius);
```

## Done when

- `Level` struct has `polys`, `spawns`, `ambis`, `poly_grid`, `poly_grid_off` fields.
- `level_io.c` populates them from the `.lvl` file.
- `physics_constrain_and_collide` runs the tile-rect + polygon passes per iteration.
- A map with a 45° slope: a mech runs up the slope cleanly, walks across it, doesn't get stuck on the corner.
- A map with a one-way platform: a mech jumps up onto it from below, walks across, drops through with crouch.
- A map with a deadly tile region: a mech standing on it loses 5 HP/s; a mech jumping over it doesn't.
- A wind-ambient region: a mech walking through it experiences the per-tick force.
- Hitscan and projectiles hit polygons; bullet sparks land at the polygon contact point.
- The headless physics test passes both old (rect-only) and new (rect + polygon) regression scenarios.

## Trade-offs to log

- **No polygon-vs-polygon collision.** Free polygons are static; we never test polygon against polygon. If a designer overlaps two SOLID polygons, the runtime treats the overlap region as solid (collision pushes particles out of either).
- **45° slopes are the only authored slope.** Editor presets cover 45°. A user *can* draw arbitrary-angle slopes with the polygon tool, but the editor warns at edit time if a polygon has a slope between 30° and 60° (the visually-confusing range) without a 45° preset; mostly to catch typos.
- **Bullet-pass-through `BACKGROUND` polygons skip the broadphase grid.** They're built into a separate index that the bullet/projectile path skips entirely. Saves a flag check in the hot loop.
