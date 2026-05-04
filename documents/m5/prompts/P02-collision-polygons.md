# P02 — Polygon collision + slope physics + slope-aware anchor

## What this prompt does

Lights up the polygon side of the hybrid tile/polygon collision system, then implements the Soldat-style slope physics (tangent-projected run velocity, slope-aware friction, angled-ceiling jet redirection) and the slope-aware post-physics anchor that prevents the M1 standing-pose anchor from killing slope sliding.

Depends on P01 (`.lvl` format) for the `LvlPoly` struct and the `Level.polys` field.

## Required reading (in this order)

1. `CLAUDE.md` — project conventions
2. `documents/01-philosophy.md` — performance + allocation rules
3. `documents/03-physics-and-mechs.md` — physics canon (note the M1 trade-offs)
4. `CURRENT_STATE.md` — see "Recently fixed" for M1 close-out blockers + the kinematic-translate gotcha
5. `TRADE_OFFS.md` — see "60 Hz simulation, not 120 Hz", "Post-physics kinematic anchor for standing pose", "Only a tile grid; no per-tile polygons"
6. **`documents/m5/03-collision-polygons.md`** — the spec for this prompt (the largest sub-doc)
7. `documents/m5/13-controls-and-residuals.md` §"Slope-aware post-physics anchor" — the small extension that keeps slope-feel working
8. `src/world.h` — `ParticlePool`, `Level`, the part enum
9. `src/physics.c` — `physics_constrain_and_collide`, `collide_map_one_pass`, `contact_with_velocity`
10. `src/mech.c` — `apply_run_velocity`, `apply_jet_force`, `mech_post_physics_anchor`, `any_foot_grounded`
11. `src/level.c` — `level_ray_hits` (DDA tile traversal you'll extend with polygon checks)

## Background

After P01, the `Level` struct has `polys`, `spawns`, etc. fields but the polygon broadphase (`poly_grid`) is empty and nothing in physics consumes polygons. The M4 build does tile-rect-only collision via the existing 3×3-neighborhood scan in `collide_map_one_pass`.

Soldat-style slope feel — running uphill is slow, running downhill is fast, steep slopes slide passively — emerges from three coupled physics changes:

1. Per-particle contact normal stored each tick.
2. Run velocity projected onto the slope tangent (not purely horizontal).
3. Friction coefficient that depends on slope angle.

Plus a fourth, smaller change: `mech_post_physics_anchor` must skip when the contact is sloped, otherwise it zeros Y-velocity and kills the passive slide.

## Concrete tasks

### Task 1 — Per-particle contact normal storage

In `src/world.h`'s `ParticlePool`, add three new SoA fields:

```c
int8_t  *contact_nx_q;      // last contact normal X, Q1.7 (-128..127 maps to -1..+1)
int8_t  *contact_ny_q;      // last contact normal Y
uint8_t *contact_kind;      // last contact's flag bits (TILE_F_*)
```

Allocate alongside the existing pool fields at startup. Zero each tick at the start of `physics_apply_gravity` (the contact-normal field is "what was the most recent contact normal during this tick"; it's fresh per tick, not accumulated).

Add `PARTICLE_FLAG_CEILING = 1u << 3` to the flags enum.

### Task 2 — Polygon broadphase grid

In `src/level_io.c::level_load` (which P01 wrote), after parsing the POLY section, build the broadphase grid per `documents/m5/03-collision-polygons.md` §"Polygon broadphase":

For each polygon, compute its AABB. For each tile its AABB overlaps, append the polygon index to that tile's list. Flatten to `(poly_grid, poly_grid_off)` for cache-friendly iteration. Store in `level_arena`.

### Task 3 — Polygon collision pass

Extend `physics_constrain_and_collide` per `documents/m5/03-collision-polygons.md` §"Per-iteration collision: tile-rect first, then polygon":

```c
for (int iter = 0; iter < ITERS; ++iter) {
    solve_constraints_one_pass(w);
    collide_map_one_pass(w, /*finalize*/ iter == ITERS - 1);
    collide_polys_one_pass(w);    // NEW
}
```

`collide_polys_one_pass` walks each particle, looks up candidate polygons via the broadphase, runs closest-point-on-triangle vs particle radius, pushes out along the polygon's edge normal by `(radius - dist)`. Use the polygon's pre-baked edge normals (Q1.15 → float).

Closest-point-on-triangle is ~30 LOC inline. See `documents/m5/03-collision-polygons.md` §"Closest-point on triangle".

Update `level_ray_hits` to also test against polygons (DDA gives an upper bound on `t`; polygon ray-vs-triangle test refines it). `BACKGROUND`-kind polygons are skipped in this path.

### Task 4 — Slope-tangent run velocity

Replace `src/mech.c::apply_run_velocity` per `documents/m5/03-collision-polygons.md` §"Slope-tangent run velocity":

```c
static void apply_run_velocity(World *w, const Mech *m, float vx_pxs, float dt, bool grounded) {
    // Air branch unchanged.
    if (!grounded) { /* existing AIR_CONTROL branch */ return; }

    // Read the average foot contact normal.
    float nx = (p->contact_nx_q[lf] + p->contact_nx_q[rf]) / 254.0f;
    float ny = (p->contact_ny_q[lf] + p->contact_ny_q[rf]) / 254.0f;
    float nlen = sqrtf(nx*nx + ny*ny);
    if (nlen < 0.5f) { nx = 0; ny = -1; }  // no fresh contact, treat as flat
    else { nx /= nlen; ny /= nlen; }

    // Tangent perpendicular to normal, in run direction.
    float tx = -ny * (vx_pxs > 0 ? 1.0f : -1.0f);
    float ty =  nx * (vx_pxs > 0 ? 1.0f : -1.0f);

    float speed = fabsf(vx_pxs);
    float vt_per_tick_x = tx * speed * dt;
    float vt_per_tick_y = ty * speed * dt;

    // Set X on every particle; set Y only on lower chain.
    for (int part = 0; part < PART_COUNT; ++part) {
        physics_set_velocity_x(p, m->particle_base + part, vt_per_tick_x);
        if (part == PART_L_FOOT || part == PART_R_FOOT ||
            part == PART_L_KNEE || part == PART_R_KNEE) {
            physics_set_velocity_y(p, m->particle_base + part, vt_per_tick_y);
        }
    }
}
```

The Y-on-lower-chain-only is intentional — see the doc for the rationale.

### Task 5 — Slope-aware friction

Replace `contact_with_velocity` in `src/physics.c` per `documents/m5/03-collision-polygons.md` §"Slope-aware friction":

```c
float ny_abs = (ny < 0.0f) ? -ny : ny;
float friction = 0.99f - 0.07f * ny_abs;     // [0.92 .. 0.99]
if (friction > 0.998f) friction = 0.998f;
if (p->contact_kind[i] & TILE_F_ICE) friction = 0.998f;
// ... existing tangential damping ...
// Persist the contact normal:
p->contact_nx_q[i] = (int8_t)(nx * 127.0f);
p->contact_ny_q[i] = (int8_t)(ny * 127.0f);
```

Tune values in playtest if needed. Starting numbers per the doc.

### Task 6 — Angled ceiling jet redirection

Replace `apply_jet_force` per `documents/m5/03-collision-polygons.md` §"Angled ceilings: jet thrust slides along overhangs":

For each particle: if `PARTICLE_FLAG_CEILING` is set, project the (0, fy) thrust onto the ceiling tangent; redirect sideways in the run direction. Otherwise apply the full upward thrust.

### Task 7 — Slope-aware post-physics anchor

In `src/mech.c::mech_post_physics_anchor`, gate the anchor on flat contact (`documents/m5/03-collision-polygons.md` §"Slope-aware post-physics anchor"):

```c
ParticlePool *p = &w->particles;
int lf = m->particle_base + PART_L_FOOT;
int rf = m->particle_base + PART_R_FOOT;
float ny_l = p->contact_ny_q[lf] / 127.0f;
float ny_r = p->contact_ny_q[rf] / 127.0f;
float ny_avg = (ny_l + ny_r) * 0.5f;
if (ny_avg > -0.92f) return;       // sloped — let slope physics handle pose
/* ... existing anchor body ... */
```

### Task 8 — TileKind expansion + ambient force application

Add the expanded TileFlags enum: `TILE_F_EMPTY=0, TILE_F_SOLID=1, TILE_F_ICE=2, TILE_F_DEADLY=4, TILE_F_ONE_WAY=8, TILE_F_BACKGROUND=16`. Update `level_tile_at` and the rest of the level helpers to handle bitmask flags.

Implement `physics_apply_ambient_forces` (called from `physics_apply_gravity`) per `documents/m5/03-collision-polygons.md` §"WIND ambient zones". WIND zones nudge `prev_x/prev_y`. ZERO_G zones zero the gravity contribution for particles inside the rect. FOG zones are render-only (no physics).

DEADLY tiles + ACID polygons + ACID ambient zones apply 5 HP/s damage via `mech_apply_damage`; per-mech check after the physics step.

## Done when

- `make` builds clean.
- A mech standing on a 60° slope (you'll need to put a polygon in code or wait for the editor — for testing, hardcode a slope polygon in `build_foundry` temporarily) without input slides downhill.
- A mech standing on a 5° slope holds station.
- A mech running uphill on a 30° slope visibly moves slower than on flat ground; downhill is faster.
- Jet pushes against a 45° angled ceiling (test with a temporary polygon overhang); head deflects sideways instead of hard-stopping.
- An ICE tile / polygon: the mech slides farther after the run input releases.
- A DEADLY tile: the mech in contact loses 5 HP/sec.
- A WIND zone: the mech is pushed sideways.
- The headless sim still produces sensible output: `make test-physics`.
- The networking smoke tests still pass: `tests/net/run.sh`.
- Performance: `simulate()` per tick stays under 4 ms target. Add a one-time log of the polygon-collision cost so we can see the budget.

## Out of scope

- Authoring the actual slope geometry in maps — that's P17/P18 (map content).
- Rendering free polygons (`draw_polys`) — that's P13 (rendering kit).
- Per-particle contact-normal smoothing across ticks — accept the "most recent wins" simplification.
- One-way platform render-time direction indicator — handled by the polygon's normal at edit time (P04).
- Slope-physics tuning beyond the starting numbers — playtest after maps land.

## How to verify

```bash
make
make test-physics
./tests/net/run.sh
```

Visual smoke test (write a small `tests/shots/m5_slope.shot` file that places the player on a hardcoded slope polygon):

```
seed 1 0
window 1280 720
spawn_at 1600 800
# place a 45° slope under the player by hardcoding a poly in build_foundry temporarily
at 30 press right
at 200 release right
at 300 shot slope_run
at 600 end
```

Run with `./soldut --shot tests/shots/m5_slope.shot` and inspect the PNG + log. The log should show pelvis Y descending while running right (downhill).

## Close-out

1. Update `CURRENT_STATE.md` with the polygon-collision + slope-physics deliverables.
2. Update `TRADE_OFFS.md`:
   - **Delete** the entry "Only a tile grid; no per-tile polygons" once polygons work end-to-end.
   - **Update** the entry "Post-physics kinematic anchor for standing pose" to note the slope-gating fix; keep the entry (the full PBD/XPBD replacement is still pending).
3. Don't commit unless explicitly asked.

## Common pitfalls

- **The Verlet kinematic translate trap** (CURRENT_STATE.md "Recently fixed"): when you write to `pos`, you must also write to `prev` by the same delta, or Verlet reads the displacement as injected velocity. The `physics_set_velocity_*` helpers handle this; if you go around them, you'll regress the M1 fix.
- **Contact normals from corner cases**: when a particle hits a triangle vertex (closest point is the vertex), the normal direction is ambiguous. Use the average of the two adjacent edge normals for that case.
- **Performance**: the polygon broadphase is the load-bearing optimization. If you skip it and run N² particle-vs-poly tests, you'll exceed budget at >100 polygons.
- **The post-physics anchor threshold** (`ny_avg > -0.92f`): this is roughly 22° from vertical. Don't make it tighter (e.g., -0.95) without testing on a 30° slope — small slopes need the anchor to NOT fire so passive standing biases downhill.
- **Slope friction is hard to tune**: the spec gives `[0.92, 0.99]`. If a test mech slides forever or never slides, adjust before shipping; don't change the API shape.
