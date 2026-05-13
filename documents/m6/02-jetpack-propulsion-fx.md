# M6 P02 — Jetpack Propulsion FX

**Goal**: replace the current audio-only jetpack feedback (`SFX_JET_PULSE`
every 4 ticks) with a cohesive **visual + audible** propulsion effect
that gives each of the four visible jetpack types (`JET_STANDARD`,
`JET_BURST`, `JET_GLIDE_WING`, `JET_JUMP_JET`) a distinct silhouette,
color, and ground-takeoff plume. Hook the existing `SFX_JET_BOOST` cue
that's defined but never fired. Add a screen-space heat-shimmer pass
over active plume regions. Keep the impulse application unchanged —
this is a polish layer that consumes replicated state, never writes
it.

Read this whole document end-to-end before opening a single file.
Sections 0–3 are spec, 4–8 are subsystem detail with code shapes,
9–12 are integration mechanics, 13 onward is implementation flow.

---

## 0. Scope and non-goals

**In scope**
- A new `src/mech_jet_fx.{c,h}` module that owns the per-jetpack
  spawn schedules, per-jetpack palette table, and the per-tick spawn
  call. Runs locally on every client + the host; consumes the
  authoritative `Mech` state through the same fields the snapshot
  already replicates (`fuel`, `jetpack_id`, `chassis_id`, `anim_id`,
  `grounded`, `boost_timer`, `pelvis`, `facing_left`, button bits).
- Two new `FxKind` values — `FX_JET_EXHAUST` (the per-tick stream
  particles, drawn additive) and `FX_GROUND_DUST` (the impingement
  cloud, drawn alpha). Live in the existing `FxPool` — no new pool.
- A single textured-quad plume layer drawn per active nozzle, between
  the decal pass and the mech pass so the body silhouettes against
  the plume. Texture is a hand-authored radial gradient at
  `assets/sprites/jet_plume.png` with a line-fallback when missing.
- A scorch-decal entry point `decal_paint_scorch(Vec2 pos, float
  radius)` mirroring `decal_paint_blood`, painted on ground
  impingement and used to leave transient takeoff marks.
- Audio: hook `SFX_JET_BOOST` to the boost-trigger seam in
  `mech_step_drive`, add a new `SFX_JET_IGNITION` cue that fires
  once per grounded → airborne transition with material-keyed
  variants (concrete / ice). `SFX_JET_PULSE` cadence stays.
- A heat-shimmer pass added to the existing halftone post-process
  shader as a second uniform-driven sampling step — no new render
  texture. Up to 16 active "hot zones" passed in as a uniform array;
  the shader perturbs the source-texture UVs near each zone using a
  cheap sin/cos noise.
- Hot-reload registration for the plume sprite, dust sprite, and the
  updated halftone shader (it's already registered — the new
  uniforms just need to be re-set in the existing callback).
- Tunings table `g_jet_fx[JET_COUNT]` is `const`, hand-tuned in C,
  noted as a tunable in `CURRENT_STATE.md` alongside `JET_THRUST_PXS2`
  and the rest of the feel knobs.

**Out of scope**
- Snapshot-wire changes. The visual is purely a function of state
  that's already replicated. We do NOT add jet-particle wire bits.
  (Verified §10.)
- Replacing the FX pool with a per-mech ring buffer. Pool capacity
  bump from 3000 → **4500** is the only sizing change. If the
  worst-case budget in §13 holds, a single pool is correct.
- A full fluid/smoke simulation. Particles are ballistic with
  linear drag — no Navier-Stokes, no metaballs.
- Shock-diamond / Mach-disk geometry in the plume. The aesthetic is
  "futuristic, clean exhaust" per the vision doc, not photoreal
  hypersonic flow visualization. A subtle brightness band may live
  in the plume texture's gradient, but it doesn't move with thrust
  and doesn't need to model overexpansion. (Reference exists in §17
  for the curious.)
- Adding a metal-surface tile flag. Surface differentiation is
  binary today: `TILE_F_ICE` for steam, otherwise dust. A future
  `TILE_F_METAL` is a separate roadmap item.
- Per-particle networking. See §10.
- Editor-side visualization of nozzle offsets. The chassis nozzle
  table lives in C only; the editor doesn't need to render it.

**Why this fits M6 polish, not a new milestone**

M5 P14–P19 stood up the audio layer; M6 P01 made bones deterministic
across the wire. The jet feedback gap is the loudest remaining
mismatch between "mechs come apart in beautiful, painful ways" and
what a fresh player actually sees in the first thirty seconds of a
match (the vision doc's success criterion). Add propulsion fx,
audio + visual coherence is uniform across the systems we already
ship, and the polish bar matches the rest of M6.

---

## 1. The four visual layers, top-down

A single active jetpack produces, per tick, up to four layers stacked
in this draw order from back to front:

1. **Heat shimmer** (post-process UV displacement) — operates on the
   already-rendered backbuffer scene. Modifies the halftone shader's
   sampling, not a new RT. Drawn implicitly when the post pass runs.
2. **Plume sprite quad** — one billboarded quad per active nozzle,
   stretched between nozzle and a falloff point along the thrust
   vector. Additive blend. Color from `JetFxDef.plume_color`. Drawn
   in `draw_world_pass` **between** `decal_draw_layer` and the
   mech-loop, so the mech body silhouettes against it.
3. **Exhaust particles** (`FX_JET_EXHAUST`) — discrete additive
   particles spawned at the nozzle with velocity opposite the thrust
   vector + small cone jitter. Drawn in the existing FX pass after
   mechs and projectiles, so individual sparks pop in front of
   everything. Short life (0.25–0.5 s).
4. **Ground impingement cloud** (`FX_GROUND_DUST`) — only when
   `grounded == true` and jetting. Larger, slower, alpha-blended
   (not additive — these are *dust*, not heat). Spawn rate triples
   the per-tick rate; they spawn at the ground-strike point under
   each nozzle and disperse horizontally. Drawn with the rest of the
   FX particles but with `kind == FX_GROUND_DUST` triggering an
   alpha-blend branch in `fx_draw`.

The scorch decal is a fifth, persistent-ish layer painted onto the
decal RT during impingement; it lives 2 s before alpha-fading and
shares the decal pass with blood splats (drawn before mechs).

Visual reference:

```
       ╱╲  ← mech body (silhouettes against plume)
       ╲╱
      |##|  ← plume sprite quad (additive, oriented along -thrust)
      |##|     scales by thrust + boost; gates on grounded for the
     |####|    fan-out "takeoff" shape
     ######   ← particles (additive bright)
    ████████  ← ground dust cloud (alpha, brown/grey)
═══════════════ ground surface
    ░░ ░░ ░    ← scorch decal (2s fade, painted to decal RT)
```

---

## 2. The five jetpack types, by feel

| Id | Plume color (RGBA8) | Particle palette | Sustain rate | Boost behavior | Nozzles | Notes |
|---|---|---|---|---|---|---|
| `JET_NONE` | — | — | — | — | 0 | no visual; mech still has `BTN_JET` no-op |
| `JET_STANDARD` | rim `(220,240,255,200)` → core `(80,180,255,255)` | white-blue | 2 particles / tick | n/a (no boost) | 1 (chest-back) | balanced cone; the "default" feel |
| `JET_BURST` | rim `(255,220,120,220)` → core `(255,120,40,255)` | yellow → orange-red | 1 particle / tick sustain, **8 particles / tick** during boost | boost = `0.40 s` × `2.0`× thrust, 12-particle ignition flash on trigger, **fires `SFX_JET_BOOST` once** | 2 (mid-back, side-by-side) | the dramatic one; boost trigger is a single discrete event |
| `JET_GLIDE_WING` | rim `(200,240,255,160)` → core `(160,220,255,220)` | pale cyan | 1 particle / 4 ticks (intermittent) + 1 per `glide_thrust` tick when `fuel == 0` | — | 2 (shoulder-blade wing tips) | wispy, low-thrust feel; glide-while-empty produces tiny puffs |
| `JET_JUMP_JET` | rim `(220,255,200,255)` → core `(120,255,180,255)` | bright green-cyan | **0 sustain** (no continuous thrust) | every grounded → airborne transition fires a **16-particle ignition flash** + ground-dust burst + `SFX_JET_IGNITION` | 1 (pelvis-low) | discrete impulse, no plume between jumps |

Color stops are RGBA8 packed as `0xRRGGBBAA` in the table. The plume
sprite samples from the rim color along its inner edge and the core
color along its centerline (the texture itself encodes the gradient;
the shader multiplies by a uniform tint that's `lerp(rim, core,
0.5)` to avoid dual-uniform plumbing).

Reasoning for the palette choices:

- **Standard / blue**: clean ion-thruster look. Matches the
  "futuristic, not cyberpunk" vision pillar — saturated but cool,
  not neon overload.
- **Burst / orange**: chemical-rocket reading. Loud, transient,
  visually distinct from sustain. The 8× particle spike on boost
  is the visual analogue of `SFX_JET_BOOST`'s acoustic spike.
- **Glide / pale cyan**: low-pressure / "draft" reading. Glide is
  the only jetpack that produces thrust at zero fuel; the wisps
  are a visual hint that the wings are doing the work.
- **Jump-jet / green-cyan**: chemical / "kick" reading. Distinct
  hue avoids confusion with the burst's orange. Green also reads
  as "system armed" — appropriate for a discrete-event jet.

The four palettes are chassis-independent — they tag the **jetpack**,
not the wearer. A Heavy with `JET_BURST` and a Scout with
`JET_BURST` both glow orange.

---

## 3. The chassis nozzle table

The visual nozzle location depends on chassis silhouette. Per
03-physics-and-mechs.md the impulse is applied to the chest (or
pelvis for Heavy); the current implementation distributes the
impulse across all particles, so the impulse's exact site doesn't
constrain us. Pick visual offsets based on what reads cleanly:

```c
/* mech_jet_fx.c — pelvis-relative nozzle offsets in source coords.
 * For facing_left the x component is negated. Y is screen-space (down
 * is positive). Chassis with multi-nozzle jetpacks (BURST 2x, GLIDE
 * 2x) get two entries; single-nozzle ones get one entry and the
 * second is { .active = false }. */
typedef struct {
    Vec2 offset;        /* from PELVIS, source coords (+X = behind mech facing right) */
    Vec2 thrust_dir;    /* unit vector of exhaust direction, world (+Y = down) */
    bool active;
} ChassisNozzle;

typedef struct {
    ChassisNozzle slot[2];
} ChassisNozzleSet;

static const ChassisNozzleSet g_chassis_nozzles[CHASSIS_COUNT] = {
    /* Trooper: single chest-back, exhaust straight down. */
    [CHASSIS_TROOPER]  = {{ {{-4.0f, -10.0f}, {0.0f, 1.0f}, true},
                            {{ 0.0f,   0.0f}, {0.0f, 0.0f}, false} }},
    /* Scout: lighter, shoulder-mounted; angled slightly outward. */
    [CHASSIS_SCOUT]    = {{ {{-3.0f, -14.0f}, {0.10f, 0.99f}, true},
                            {{ 0.0f,   0.0f}, {0.0f, 0.0f}, false} }},
    /* Heavy: pelvis-low, big single nozzle. (Matches the 03-physics
     * doc's "Heavy gets pelvis-mounted thrust" wording even though
     * the impulse code is uniform.) */
    [CHASSIS_HEAVY]    = {{ {{-2.0f,  -4.0f}, {0.0f, 1.0f}, true},
                            {{ 0.0f,   0.0f}, {0.0f, 0.0f}, false} }},
    /* Sniper: hip-mounted, thin profile. */
    [CHASSIS_SNIPER]   = {{ {{-3.0f,  -8.0f}, {0.0f, 1.0f}, true},
                            {{ 0.0f,   0.0f}, {0.0f, 0.0f}, false} }},
    /* Engineer: back-mounted, like a tool case. */
    [CHASSIS_ENGINEER] = {{ {{-4.0f, -12.0f}, {0.0f, 1.0f}, true},
                            {{ 0.0f,   0.0f}, {0.0f, 0.0f}, false} }},
};
```

For `JET_BURST` and `JET_GLIDE_WING` (which want two nozzles), the
spawn function reads `g_jet_fx[jet_id].nozzle_count` and mirrors the
single-entry offset horizontally to produce a left/right pair. The
chassis table only needs to define ONE offset per chassis; the
jetpack's nozzle count drives the mirror.

This avoids a `CHASSIS_COUNT × JET_COUNT` matrix.

The `thrust_dir` field is **world-space**, not source-relative: at
`facing_left == true` the x component is NOT negated. Exhaust always
points down (with a small chassis-specific outward angle). The
impulse direction is opposite the chassis facing in the running case,
but the *visual* exhaust always heads toward the ground because gravity
plus the inertia of hot gas dominates. (Real jetpacks are similar:
the exhaust trails behind the wearer regardless of which way they're
facing.)

---

## 4. New module: `src/mech_jet_fx.{c,h}`

Header (`src/mech_jet_fx.h`):

```c
#pragma once

#include "math.h"
#include "world.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-jetpack visual tunings. The table g_jet_fx[JET_COUNT] is
 * authoritative; mech_jet_fx_step reads from it once per active
 * jetpack per tick. Tunables live in mech_jet_fx.c. */
typedef struct {
    uint32_t plume_color_rim;       /* RGBA8 */
    uint32_t plume_color_core;      /* RGBA8 */
    uint32_t particle_color_hot;    /* RGBA8 — at spawn */
    uint32_t particle_color_cool;   /* RGBA8 — at life end (lerp by life/life_max) */
    uint8_t  nozzle_count;          /* 1 or 2 */
    uint8_t  sustain_particles_per_tick;
    uint8_t  boost_particles_per_tick;
    uint8_t  ignition_particles;    /* fired once on grounded → airborne */
    uint8_t  sustain_tick_divisor;  /* spawn only when (tick % divisor == 0); 1 = every tick */
    float    plume_length_px;       /* base plume sprite length (multiplied by thrust scalar) */
    float    plume_width_px;        /* base plume sprite width  */
    float    particle_speed_pxs;    /* base exhaust velocity */
    float    particle_life_min;
    float    particle_life_max;
    float    particle_size_min;
    float    particle_size_max;
    bool     has_continuous_plume;  /* false for JET_JUMP_JET */
} JetFxDef;

extern const JetFxDef g_jet_fx[JET_COUNT];

/* Per-tick driver. Runs in simulate_step after pose_compute (so the
 * pelvis position is the final one for the tick), once per alive mech.
 * Spawns particles + paints scorch decals; does NOT draw — drawing
 * lives in render.c. Pure-input: every spawn is a function of the
 * passed mech state, the WorldRNG, and the tick counter. No globals
 * inside this function except g_jet_fx + g_chassis_nozzles. */
void mech_jet_fx_step(World *w, int mech_id, float dt);

/* Render the per-mech plume sprites + heat-shimmer hot-zone array.
 * Called from render.c::draw_world_pass between the decal pass and
 * the mech-loop. Writes up to JET_HOT_ZONE_MAX entries into the
 * shimmer uniform staging buffer; the actual halftone shader read
 * happens in renderer_draw_frame's BeginShaderMode block. */
void mech_jet_fx_draw_plumes(const World *w, float interp_alpha);

/* Returns true if any nozzle is currently active across all mechs.
 * Called by render.c to decide whether to set the heat-shimmer
 * uniform array (skip the uniform-set when zero — most ticks). */
bool mech_jet_fx_any_active(const World *w);

/* Per-tick hot-zone export for the shimmer shader. Returns count
 * written (≤ JET_HOT_ZONE_MAX). Each zone is (screen_xy, radius,
 * intensity). Pure: reads world state, doesn't allocate. */
#define JET_HOT_ZONE_MAX 16
typedef struct { float x, y, radius, intensity; } JetHotZone;
int mech_jet_fx_collect_hot_zones(const World *w, const Camera2D *cam,
                                  JetHotZone *out, int max);

#ifdef __cplusplus
}
#endif
```

The module is a peer to `mech.c` and `particle.c` in the dep graph:
it consumes `World` + `Mech` + `FxPool` + the level for surface
queries, calls `fx_spawn_*` and `decal_paint_scorch`. It is consumed
only by `render.c` (`mech_jet_fx_draw_plumes`,
`mech_jet_fx_collect_hot_zones`) and `simulate.c`
(`mech_jet_fx_step`).

Tunings table (excerpted; full table in §13 file plan):

```c
const JetFxDef g_jet_fx[JET_COUNT] = {
    [JET_NONE] = {0},
    [JET_STANDARD] = {
        .plume_color_rim       = 0xDCF0FFC8,  /* 220,240,255,200 */
        .plume_color_core      = 0x50B4FFFF,  /* 80,180,255,255 */
        .particle_color_hot    = 0xC8E0FFFF,  /* 200,224,255,255 */
        .particle_color_cool   = 0x3C5AB400,  /* 60,90,180,0 — alpha 0 at life end */
        .nozzle_count          = 1,
        .sustain_particles_per_tick = 2,
        .boost_particles_per_tick   = 0,
        .ignition_particles    = 6,
        .sustain_tick_divisor  = 1,
        .plume_length_px       = 28.0f,
        .plume_width_px        = 12.0f,
        .particle_speed_pxs    = 240.0f,
        .particle_life_min     = 0.25f,
        .particle_life_max     = 0.45f,
        .particle_size_min     = 2.0f,
        .particle_size_max     = 4.0f,
        .has_continuous_plume  = true,
    },
    [JET_BURST] = {
        .plume_color_rim       = 0xFFDC78DC,  /* 255,220,120,220 */
        .plume_color_core      = 0xFF7828FF,  /* 255,120,40,255 */
        .particle_color_hot    = 0xFFC850FF,  /* 255,200,80,255 */
        .particle_color_cool   = 0xC8281400,  /* 200,40,20,0 */
        .nozzle_count          = 2,
        .sustain_particles_per_tick = 1,
        .boost_particles_per_tick   = 8,
        .ignition_particles    = 12,
        .sustain_tick_divisor  = 1,
        .plume_length_px       = 34.0f,
        .plume_width_px        = 14.0f,
        .particle_speed_pxs    = 320.0f,
        .particle_life_min     = 0.30f,
        .particle_life_max     = 0.55f,
        .particle_size_min     = 2.5f,
        .particle_size_max     = 5.0f,
        .has_continuous_plume  = true,
    },
    [JET_GLIDE_WING] = {
        .plume_color_rim       = 0xC8F0FFA0,  /* 200,240,255,160 */
        .plume_color_core      = 0xA0DCFFDC,  /* 160,220,255,220 */
        .particle_color_hot    = 0xB4F0FFFF,
        .particle_color_cool   = 0x5078C800,
        .nozzle_count          = 2,
        .sustain_particles_per_tick = 1,
        .boost_particles_per_tick   = 0,
        .ignition_particles    = 4,
        .sustain_tick_divisor  = 4,    /* every 4th tick */
        .plume_length_px       = 18.0f,
        .plume_width_px        = 9.0f,
        .particle_speed_pxs    = 160.0f,
        .particle_life_min     = 0.50f,
        .particle_life_max     = 0.90f,
        .particle_size_min     = 1.5f,
        .particle_size_max     = 3.0f,
        .has_continuous_plume  = true,
    },
    [JET_JUMP_JET] = {
        .plume_color_rim       = 0xDCFFCBFF,
        .plume_color_core      = 0x78FFB4FF,
        .particle_color_hot    = 0x8CFFC8FF,
        .particle_color_cool   = 0x3CB47800,
        .nozzle_count          = 1,
        .sustain_particles_per_tick = 0,
        .boost_particles_per_tick   = 0,
        .ignition_particles    = 16,
        .sustain_tick_divisor  = 1,
        .plume_length_px       = 0.0f,   /* no continuous plume */
        .plume_width_px        = 0.0f,
        .particle_speed_pxs    = 360.0f,
        .particle_life_min     = 0.35f,
        .particle_life_max     = 0.60f,
        .particle_size_min     = 3.0f,
        .particle_size_max     = 5.0f,
        .has_continuous_plume  = false,
    },
};
```

These numbers are first-pass. Expect one render iteration round
after Phase 4 to tune. Tag them in `CURRENT_STATE.md` so the
revision history matches the weapons/physics tables.

---

## 5. The per-tick step (`mech_jet_fx_step`)

Runs once per alive mech in `simulate.c::simulate_step`, AFTER
`pose_compute` writes the final bone positions for the tick (so the
pelvis we read is the final pelvis).

```c
void mech_jet_fx_step(World *w, int mech_id, float dt) {
    Mech *m = &w->mechs[mech_id];
    if (!m->alive) return;
    if (m->jetpack_id == JET_NONE) return;

    const JetFxDef *def = &g_jet_fx[m->jetpack_id];
    const ChassisNozzleSet *nset = &g_chassis_nozzles[m->chassis_id];

    /* Read the same state the impulse path reads. Important: we do
     * NOT recompute "is the impulse active this tick" — we read the
     * pre-existing one-tick flag the impulse path sets. See §6 for
     * the flag plumbing. */
    bool is_jetting   = (m->jet_state_bits & MECH_JET_ACTIVE) != 0;
    bool just_ignited = (m->jet_state_bits & MECH_JET_IGNITION_TICK) != 0;
    bool is_boosting  = (m->jet_state_bits & MECH_JET_BOOSTING) != 0;
    bool grounded     = m->grounded;

    if (!is_jetting && !just_ignited) return;

    /* Pelvis is final-of-tick from pose_compute. */
    Vec2 pelv = mech_pelvis_pos(w, mech_id);
    float face_dir = m->facing_left ? -1.0f : 1.0f;

    /* For each active nozzle, compute world-space nozzle position,
     * spawn particles, and (when grounded) emit a ground impingement
     * burst. */
    int n_nozzles = (def->nozzle_count == 2) ? 2 : 1;
    for (int i = 0; i < n_nozzles; ++i) {
        const ChassisNozzle *nz = &nset->slot[0];   /* base offset */
        if (!nz->active) continue;
        /* For 2-nozzle jetpacks, mirror in body-local x. */
        float mirror = (n_nozzles == 2 && i == 1) ? -1.0f : 1.0f;
        Vec2 noz = (Vec2){
            pelv.x + face_dir * (nz->offset.x * mirror),
            pelv.y +            nz->offset.y,
        };
        Vec2 thrust = nz->thrust_dir;  /* world-space, already pointing down */

        /* Ignition burst — fire once on the grounded → airborne
         * transition (just_ignited == true on the FIRST tick of jet
         * thrust, regardless of grounded). */
        if (just_ignited) {
            for (int k = 0; k < def->ignition_particles; ++k) {
                spawn_exhaust_particle(w, def, noz, thrust, /*spread_rad*/0.8f,
                                       /*speed_mul*/1.4f);
            }
        }

        /* Sustain — per-tick rate, gated by tick divisor and current
         * boost state. */
        int sustain_n = is_boosting ? def->boost_particles_per_tick
                                    : def->sustain_particles_per_tick;
        if (sustain_n > 0 && (w->tick % def->sustain_tick_divisor) == 0) {
            for (int k = 0; k < sustain_n; ++k) {
                spawn_exhaust_particle(w, def, noz, thrust, /*spread*/0.30f,
                                       /*speed_mul*/1.0f);
            }
        }

        /* Ground impingement — raycast straight down from nozzle a
         * short distance; if we hit a SOLID polygon, emit a fan of
         * dust + paint scorch decal. */
        if (is_jetting && grounded) {
            Vec2 ground_hit; uint8_t surf_kind;
            if (jet_fx_ground_query(w, noz, /*max_dist*/JET_IMPINGE_MAX_DIST,
                                    &ground_hit, &surf_kind)) {
                emit_ground_dust(w, def, ground_hit, surf_kind, is_boosting);
                if (just_ignited || is_boosting) {
                    decal_paint_scorch(ground_hit, /*radius*/8.0f);
                }
            }
        }
    }
}
```

Helper sketches:

```c
static void spawn_exhaust_particle(World *w, const JetFxDef *def,
                                   Vec2 origin, Vec2 thrust_dir,
                                   float spread_rad, float speed_mul) {
    pcg32_t *rng = &w->fx_rng;   /* see §5 note: add this field on World */
    float angle = atan2f(thrust_dir.y, thrust_dir.x) +
                  (frand(rng) * 2.0f - 1.0f) * spread_rad;
    float speed = def->particle_speed_pxs * speed_mul * (0.85f + frand(rng) * 0.30f);
    Vec2 vel = (Vec2){ cosf(angle) * speed, sinf(angle) * speed };
    float life = def->particle_life_min +
                 frand(rng) * (def->particle_life_max - def->particle_life_min);
    float size = def->particle_size_min +
                 frand(rng) * (def->particle_size_max - def->particle_size_min);
    fx_spawn_jet_exhaust(&w->fx, origin, vel, life, size,
                         def->particle_color_hot, def->particle_color_cool);
}

static bool jet_fx_ground_query(World *w, Vec2 from, float max_dist,
                                Vec2 *out_hit, uint8_t *out_surf) {
    /* Cheap downward step query: walk a few tile cells from `from`
     * downward, check tile flags + slope polygons for the first
     * SOLID hit. Reuse `level_tile_at` + the poly broadphase. The
     * grounded-test path already does similar work; factor a tiny
     * helper if it's clean to share. */
    Level *L = &w->level;
    for (float dy = 4.0f; dy <= max_dist; dy += 4.0f) {
        Vec2 p = (Vec2){ from.x, from.y + dy };
        LvlTile *t = level_tile_at(L, (int)(p.x / TILE_PX), (int)(p.y / TILE_PX));
        if (t && (t->flags & TILE_F_SOLID)) {
            /* Snap y to the top of the tile we hit. */
            float ty = floorf(p.y / TILE_PX) * TILE_PX;
            *out_hit = (Vec2){ from.x, ty };
            *out_surf = t->flags;
            return true;
        }
        /* Also check polygon broadphase for slope hits. */
        if (poly_broadphase_query_point_solid(L, p, /*radius*/2.0f, out_hit, out_surf)) {
            return true;
        }
    }
    return false;
}

static void emit_ground_dust(World *w, const JetFxDef *def, Vec2 hit,
                             uint8_t surf_kind, bool is_boosting) {
    pcg32_t *rng = &w->fx_rng;   /* see §5 note: add this field on World */
    int n = is_boosting ? 8 : 3;
    /* Steam on ice; dust otherwise. Color picked here so the FX
     * particle records the right color at spawn-time. */
    uint32_t color = (surf_kind & TILE_F_ICE) ? 0xE0F0FFC0   /* pale steam */
                                              : 0xA08060C0;  /* warm dust */
    for (int k = 0; k < n; ++k) {
        /* Fan outward horizontally with a small upward component. */
        float angle = -PI_F * 0.5f + (frand(rng) * 2.0f - 1.0f) * (PI_F * 0.45f);
        float speed = 90.0f + frand(rng) * 80.0f;
        Vec2 vel = (Vec2){ cosf(angle) * speed, sinf(angle) * speed };
        fx_spawn_ground_dust(&w->fx, hit, vel,
                             /*life*/0.6f + frand(rng) * 0.4f,
                             /*size*/3.0f + frand(rng) * 3.0f,
                             color);
    }
}
```

Notes:

- **WorldRNG**: `world.rng` is the canonical PCG stream today
  (verified — no separate `fx_rng` field exists on `World`).
  Spawning randomness through `world.rng` perturbs the
  authoritative sim's RNG draws by exactly the number of fx
  spawns, which then differs between host and client (the client
  doesn't run the same sim path). Two acceptable options:
  (a) add `pcg32_t fx_rng;` to `World`, seeded once at
  `world_init`, used only by visual FX paths — keeps `world.rng`
  pristine for simulation. (b) accept the divergence: client FX
  randomness is already client-local (blood spray, sparks), so
  routing jet FX through the same `world.rng` is consistent with
  the existing pattern. **Recommendation**: (a) — add `fx_rng`.
  One-line schema change, isolates visual nondeterminism cleanly.
  Either way: don't touch the simulate-path RNG draws.
- **No fx allocation**: the spawn helpers go through the existing
  pool. Pool fills are silent overwrites at the oldest live slot
  (existing FxPool semantics).
- **Frame-rate independence**: per-tick particle counts are
  absolute, and the sim is fixed 60 Hz. Render-side interpolation
  reads `render_prev_pos` like any other FX particle.

---

## 6. Mech-side state flags (one new uint8 field on `Mech`)

The visual step needs three boolean signals it can't easily recover
from postconditions: "are we jetting THIS tick?", "is this the
first tick after grounded→airborne?", and "are we in a boost?".

Add a single byte to `Mech`:

```c
/* src/world.h additions to Mech struct */
uint8_t  jet_state_bits;       /* MECH_JET_* flags below; updated each tick */
uint8_t  jet_prev_grounded;    /* 1 = was grounded last tick; for ignition edge */
```

```c
enum {
    MECH_JET_ACTIVE        = 1u << 0,  /* this tick consumed fuel + applied thrust */
    MECH_JET_IGNITION_TICK = 1u << 1,  /* first tick after grounded→airborne while ACTIVE */
    MECH_JET_BOOSTING      = 1u << 2,  /* boost_timer > 0 this tick */
};
```

Wire them in `mech.c::mech_step_drive` next to the existing thrust
application (the same lines that drain fuel + apply impulse):

```c
/* mech.c — near the existing apply_jet_force call */
m->jet_state_bits = 0;
bool was_grounded = m->jet_prev_grounded;
bool jet_active = (in.buttons & BTN_JET) && (m->fuel > 0.0f);
if (jet_active) {
    m->jet_state_bits |= MECH_JET_ACTIVE;
    if (was_grounded && !m->grounded)
        m->jet_state_bits |= MECH_JET_IGNITION_TICK;
    if (m->boost_timer > 0.0f)
        m->jet_state_bits |= MECH_JET_BOOSTING;
    /* ... existing apply_jet_force / fuel drain ... */
}
m->jet_prev_grounded = m->grounded ? 1 : 0;
```

These bits are READ-ONLY for `mech_jet_fx_step`. The visual step
never writes them. That keeps the impulse path the source of truth
for "is the jet on" and avoids two redundant button-reading paths.

For remote mechs on the client, the snapshot ships `BTN_JET` via
`SNAP_STATE_JET` (verify: this is already part of `state_bits` in
P19+ wire format). `snapshot_apply` mirrors it onto the local
`Mech`, and the FX step reads the same flag the local one does. The
ignition edge `was_grounded && !grounded` is computed locally on
each side using the snapshot-replicated `grounded` flag — same edge
trigger on host + client.

For the snapshot-driven remote case, also mirror `boost_timer` from
the snapshot (it's part of state bits via `SNAP_STATE_BOOSTING`; if
not, add it as a single bit). Without `SNAP_STATE_BOOSTING`, remote
clients won't see the 8× particle spike on boost. **Add this bit**
if it doesn't already exist — see §10.

---

## 7. New FxKind values + `fx_draw` branching

`src/world.h`:

```c
typedef enum {
    FX_BLOOD = 0,
    FX_SPARK,
    FX_TRACER,
    FX_SMOKE,
    FX_STUMP,
    FX_JET_EXHAUST,       /* NEW: additive, color lerps hot→cool */
    FX_GROUND_DUST,       /* NEW: alpha, slower drag, brief lift */
    FX_KIND_COUNT
} FxKind;
```

The `FxParticle` struct gains nothing new — the existing `color`
RGBA8 + `kind` byte are sufficient. The color-lerp from
`particle_color_hot` → `particle_color_cool` over the particle's
life is computed at draw time by `fx_draw`:

```c
/* fx_draw — add cases for the two new kinds */
case FX_JET_EXHAUST: {
    float t = 1.0f - (p->life / p->life_max);   /* 0 at spawn, 1 at end */
    Color c = color_lerp_rgba8(p->color_hot, p->color_cool, t);
    /* Stored as packed `color` → cool, with hot in size's high bits? No;
     * we need both. Use the existing `color` field as `hot` and add a
     * second packed u32 to FxParticle for `cool`. See §13 file plan. */
    BeginBlendMode(BLEND_ADDITIVE);
    DrawCircleV(rp, p->size, c);
    EndBlendMode();
    break;
}
case FX_GROUND_DUST: {
    float t = 1.0f - (p->life / p->life_max);
    Color c = color_lerp_rgba8(p->color_hot, p->color_cool, t);
    DrawCircleV(rp, p->size, c);    /* alpha blend, no Begin */
    break;
}
```

The "store BOTH hot and cool colors per particle" requirement is
why we need a second `uint32_t color_cool` field on `FxParticle`.
That adds **4 bytes per particle**. At capacity 4500, that's 18 KB
permanent — fine.

Spawn helpers in `particle.c`:

```c
int fx_spawn_jet_exhaust(FxPool *p, Vec2 pos, Vec2 vel,
                         float life, float size,
                         uint32_t color_hot, uint32_t color_cool);
int fx_spawn_ground_dust(FxPool *p, Vec2 pos, Vec2 vel,
                         float life, float size, uint32_t color);
```

The `fx_update` integrator extension for these two kinds:

```c
case FX_JET_EXHAUST: {
    /* Linear drag — exhaust slows fast as it dissipates. */
    p->vel.x *= 0.92f;
    p->vel.y *= 0.92f;
    /* Mild upward buoyancy for the hot exhaust (vs gravity). */
    p->vel.y -= JET_BUOY_PXS2 * dt;
    p->pos.x += p->vel.x * dt;
    p->pos.y += p->vel.y * dt;
    p->life  -= dt;
    if (p->life <= 0.0f) p->alive = 0;
    break;
}
case FX_GROUND_DUST: {
    /* Heavy drag, mild gravity. */
    p->vel.x *= 0.88f;
    p->vel.y *= 0.88f;
    p->vel.y += GROUND_DUST_GRAVITY_PXS2 * dt * 0.3f;
    p->pos.x += p->vel.x * dt;
    p->pos.y += p->vel.y * dt;
    p->life  -= dt;
    /* Optional: wall-collide vs ground tiles so dust doesn't sink
     * through the floor — reuse the blood-splat ground stop. */
    if (p->life <= 0.0f) p->alive = 0;
    break;
}
```

`JET_BUOY_PXS2 = 80.0f` and `GROUND_DUST_GRAVITY_PXS2 = 120.0f` are
first-pass tunables in `particle.c`.

---

## 8. The plume sprite quad

A single textured quad per active nozzle, drawn additive. Not a
particle — a deterministic shape that scales with thrust.

```c
/* render.c::draw_jet_plumes (called between decals and mechs) */
static void draw_jet_plumes(const World *w, Texture2D plume_tex) {
    for (int i = 0; i < w->mech_count; ++i) {
        const Mech *m = &w->mechs[i];
        if (!m->alive) continue;
        if (m->jetpack_id == JET_NONE) continue;
        if (!(m->jet_state_bits & MECH_JET_ACTIVE)) continue;
        const JetFxDef *def = &g_jet_fx[m->jetpack_id];
        if (!def->has_continuous_plume) continue;

        const ChassisNozzleSet *nset = &g_chassis_nozzles[m->chassis_id];
        Vec2 pelv = mech_pelvis_pos(w, i);
        float face_dir = m->facing_left ? -1.0f : 1.0f;

        /* Plume length grows with boost; shrinks slightly when nearly
         * out of fuel as a visual fuel cue. */
        bool boosting = (m->jet_state_bits & MECH_JET_BOOSTING) != 0;
        float fuel_frac = (m->fuel_max > 0.01f) ? (m->fuel / m->fuel_max) : 0.0f;
        float len_scale = (boosting ? 1.6f : 1.0f) * (0.6f + 0.4f * fuel_frac);
        float plume_len = def->plume_length_px * len_scale;
        float plume_w   = def->plume_width_px;

        int n_nozzles = (def->nozzle_count == 2) ? 2 : 1;
        for (int k = 0; k < n_nozzles; ++k) {
            const ChassisNozzle *nz = &nset->slot[0];
            float mirror = (n_nozzles == 2 && k == 1) ? -1.0f : 1.0f;
            Vec2 noz = (Vec2){
                pelv.x + face_dir * (nz->offset.x * mirror),
                pelv.y +            nz->offset.y,
            };
            /* Quad runs from `noz` for `plume_len` along thrust_dir.
             * Color: lerp(rim, core, 0.5) tint multiplied with the
             * texture's encoded gradient. */
            Color tint = color_blend_rgba8(def->plume_color_rim,
                                           def->plume_color_core, 0.5f);
            BeginBlendMode(BLEND_ADDITIVE);
            /* Use raylib's DrawTexturePro with a 1-tex source rect and
             * a rotated destination quad. Rotation = atan2(thrust). */
            float angle_deg = RAD2DEG * atan2f(nz->thrust_dir.y, nz->thrust_dir.x) - 90.0f;
            Rectangle src = (Rectangle){ 0, 0, plume_tex.width, plume_tex.height };
            Rectangle dst = (Rectangle){ noz.x, noz.y, plume_w, plume_len };
            Vector2 origin = (Vector2){ plume_w * 0.5f, 0.0f };
            DrawTexturePro(plume_tex, src, dst, origin, angle_deg, tint);
            EndBlendMode();
        }
    }
}
```

**Plume texture spec** (for the art pass):

- Path: `assets/sprites/jet_plume.png`
- Size: 32×96 (width × length)
- Format: RGBA8 alpha-on-black
- Visual: a vertical radial-gradient flame — bright white at the
  top-center (nozzle end), fading to transparent at the bottom-tip.
  Sharper at the sides (narrow rim), softer at the bottom. A single
  authored gradient that the per-jetpack tint colors.
- Authored alpha encodes the shape; RGB stays white. Tint
  multiplies in at draw time.
- Missing-asset fallback: draw a bright line from nozzle to
  `noz + thrust_dir * plume_len` with `DrawLineEx`. Already-present
  pattern for atlasless rendering.

**Render order placement**: in `draw_world_pass`, after
`decal_draw_layer` (so plume covers the scorch decals it just
painted last frame), before the mech loop (so the mech body
silhouettes against the plume). Adjacent line numbers per the
inventory: between `~1241` and `~1243`. Call:

```c
draw_jet_plumes(w, g_jet_plume_atlas);
```

---

## 9. Heat shimmer — uniform-driven shader pass

The existing halftone post-process renders the world to
`g_post_target`, then samples it with `g_halftone_post` shader on
the backbuffer. We extend the **same shader** to accept a hot-zone
array uniform and perturb UVs near each zone before the halftone
math runs. No second RT, no second pass.

`assets/shaders/halftone_post.fs.glsl` additions:

```glsl
#version 330

in  vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;
uniform vec2  resolution;
uniform float halftone_density;

/* NEW: jet hot zones. xy = screen px (origin top-left), z = radius
 * in px, w = intensity 0..1. Unused slots have w = 0.0. */
#define JET_HOT_ZONE_MAX 16
uniform vec4 jet_hot_zones[JET_HOT_ZONE_MAX];
uniform float jet_time;   /* monotonically advancing seconds */

vec2 shimmer_offset(vec2 frag_px) {
    vec2 offs = vec2(0.0);
    for (int i = 0; i < JET_HOT_ZONE_MAX; ++i) {
        vec4 z = jet_hot_zones[i];
        if (z.w <= 0.001) continue;
        vec2 d = frag_px - z.xy;
        float r2 = dot(d, d);
        float falloff = exp(-r2 / (z.z * z.z));
        if (falloff < 0.01) continue;
        /* Cheap pseudo-noise: 2D sin/cos at frag-derived freqs, time-
         * advanced. Two octaves give a more natural shimmer than one. */
        float n1 = sin(frag_px.x * 0.08 + jet_time * 3.0)
                 + cos(frag_px.y * 0.10 + jet_time * 2.7);
        float n2 = sin(frag_px.x * 0.21 - jet_time * 4.1)
                 + cos(frag_px.y * 0.19 + jet_time * 3.5);
        offs += vec2(n1, n2) * falloff * z.w * 0.6;
    }
    return offs;
}

void main() {
    vec2 frag_px = fragTexCoord * resolution;
    vec2 shimmer = shimmer_offset(frag_px);
    vec2 uv = (frag_px + shimmer) / resolution;
    /* Existing halftone math operates on `uv` instead of fragTexCoord. */
    vec4 src = texture(texture0, uv);
    /* ... existing halftone dither math here, unchanged ... */
    finalColor = src;   /* placeholder; keep the existing math */
}
```

The CPU side passes the hot zones each frame:

```c
/* render.c::renderer_draw_frame, inside the BeginShaderMode(g_halftone_post) block */
JetHotZone zones[JET_HOT_ZONE_MAX] = {0};
int nz = mech_jet_fx_collect_hot_zones(w, &r->camera, zones, JET_HOT_ZONE_MAX);
/* Pad unused slots with w=0 so the GLSL skip works. */
SetShaderValueV(g_halftone_post, g_uniloc_jet_hot_zones,
                zones, SHADER_UNIFORM_VEC4, JET_HOT_ZONE_MAX);
float jet_time = (float)GetTime();
SetShaderValue(g_halftone_post, g_uniloc_jet_time, &jet_time, SHADER_UNIFORM_FLOAT);
```

Uniform locations cached on shader load:

```c
g_uniloc_jet_hot_zones = GetShaderLocation(g_halftone_post, "jet_hot_zones");
g_uniloc_jet_time      = GetShaderLocation(g_halftone_post, "jet_time");
```

Add these to the existing `load_halftone_shader` / `reload_halftone_shader`
function so they get re-cached on hot-reload.

`mech_jet_fx_collect_hot_zones` walks alive mechs, finds those with
`MECH_JET_ACTIVE`, projects the nozzle world position into screen
space via `GetWorldToScreen2D`, and writes (sx, sy, radius_px,
intensity). Radius scales with thrust: ~40 px sustain, ~80 px boost,
~120 px ignition flash. Intensity is 0.6 sustain / 1.0 boost.
**Skip the SetShaderValueV when `mech_jet_fx_any_active(w) == false`**
to avoid uniform-set cost on idle ticks (most ticks).

**Why uniform array, not a mask RT**: cheaper memory traffic
(no second RT bind / clear / blit), simpler hot-reload (no RT
lifecycle), better at small zone counts (we top out at 16 active
jets — never more than `MAX_LOBBY_SLOTS = 16` mechs jetting at
once). The mask-RT approach would shine if hot zones numbered in
the hundreds, which they don't.

**Quality fallback**: shimmer is purely cosmetic. Skip the
uniform set + the in-shader loop on integrated GPUs by a config
key `shimmer_enabled=0` in `soldut.cfg`. Default on; off for shot
mode (skip the visual jitter so screenshot diffs stay stable).

---

## 10. Snapshot / network — what's already replicated + what's missing

Per the codebase inventory:
- `BTN_JET` is replicated via `state_bits` (`SNAP_STATE_JET`).
- `grounded` is replicated via `SNAP_STATE_GROUNDED`.
- `fuel` is in the snapshot.
- `boost_timer` is **not currently replicated**.
- `facing_left`, `chassis_id`, `jetpack_id`, `anim_id` already
  replicated.

We need ONE new wire bit: `SNAP_STATE_BOOSTING`, set on the host when
`m->boost_timer > 0.0f` at snapshot record time. Decoded on the
client into `m->jet_state_bits |= MECH_JET_BOOSTING`. Without this,
the BURST plume on remote mechs misses the 8× spike during boost.

**No new bytes** — `state_bits` is a `u16`. Used slots at protocol
`S0LJ` are 0..13 (ALIVE, JET, CROUCH, PRONE, FIRE, RELOAD, GROUNDED,
FACING_LEFT, BERSERK, INVIS, GODMODE, IS_DUMMY, GRAPPLING, RUNNING).
Slots 14 and 15 are free. **Assign `SNAP_STATE_BOOSTING = 1u << 14`.**
Document the bit assignment in `documents/05-networking.md` if it has
a state-bit table (check first).

**No protocol-id bump.** The bit is gated behind an unused slot;
older clients ignore it (the field is the same width). For a
clean migration story we can bump `S0LJ` → `S0LK` anyway and note
that boost-spike visuals on remote BURST jets need the new bit.
**Recommended**: bump the protocol id to be explicit; the cost is
nil and the audit trail is cleaner.

**Per-particle networking**: still no. Each client spawns particles
locally from its own copy of the (replicated) jet state. The fx_rng
is per-client — particles will not match bit-exact across clients,
which is fine: they're spawn-distribution-identical, not
particle-identical. (Same model as blood spray on `NET_MSG_HIT_EVENT`.)

**Verified `MECH_JET_IGNITION_TICK` survives the wire**: the
ignition edge is derived from per-tick `(was_grounded, grounded,
jetting)`. All three are replicated; the edge is computed locally
on each side and fires on the matching tick. No special wire
treatment needed.

---

## 11. Decal scorch

Mirror `decal_paint_blood` in `src/decal.{c,h}`:

```c
/* src/decal.h */
void decal_paint_scorch(Vec2 pos, float radius);
```

```c
/* src/decal.c */
typedef struct { Vec2 pos; float radius; } PendingScorch;
static PendingScorch g_pending_scorch[MAX_PENDING];
static int g_pending_scorch_count = 0;

void decal_paint_scorch(Vec2 pos, float radius) {
    if (g_pending_scorch_count >= MAX_PENDING) return;
    g_pending_scorch[g_pending_scorch_count++] = (PendingScorch){ pos, radius };
}

/* In decal_flush_pending, after the existing blood-flush loop: */
for (int i = 0; i < g_pending_scorch_count; ++i) {
    PendingScorch *s = &g_pending_scorch[i];
    /* Same chunk-routing as blood; different color. */
    Color outer = (Color){  20,  16,  14, 110 };
    Color inner = (Color){  40,  32,  28, 200 };
    decal_paint_circles_to_chunks(s->pos, s->radius, outer, inner);
}
g_pending_scorch_count = 0;
```

Factor the chunk-routing into a private helper so blood and scorch
share it without duplication — but ONLY if the existing
`decal_flush_pending` already does the dirty-chunk batching as a
distinct step. If it's inlined, leave it inlined and accept the
duplication. (Rule 5: function calls earn their non-static-ness.)

**Fade**: scorch decals are currently permanent for the round. The
spec calls for a 2-second fade, but the decal RT doesn't track
per-splat ages — it's just painted pixels. Adding age tracking
would require a parallel ring buffer of splat records, which is
not in scope.

**Trade-off entry to log**: "Scorch decals are permanent for the
round (no per-decal fade)". Revisit when the decal RT gets a
per-splat record buffer.

**Alternative**: paint scorch as an FX particle (FX_SCORCH) with
~2 s life that gets composited into the decal RT just before it
expires. That trades visual fidelity (it's a particle that
suddenly turns into a permanent decal) for the fade window. Skip
this for v1; the binary "scorch appears, never goes" reads fine
in playtest of similar games.

---

## 12. Audio coupling

Three audio surfaces:

### 12.1 `SFX_JET_PULSE` (existing, unchanged)

Already fires every 4 ticks while jetting from
`mech.c::apply_jet_force`. No change.

### 12.2 `SFX_JET_BOOST` (existing, NOT currently fired)

The inventory confirms: defined in the manifest but
`audio_play_at(SFX_JET_BOOST, ...)` is never called. **Add the call**
in `mech.c::mech_step_drive`, on the same tick the boost is
triggered (the line that sets `m->boost_timer = jp->boost_duration`):

```c
if (jp->boost_thrust_mult > 0.0f && (pressed & BTN_DASH)
    && m->boost_timer <= 0.0f && m->fuel > jp->boost_fuel_cost * m->fuel_max) {
    m->boost_timer = jp->boost_duration;
    m->fuel -= jp->boost_fuel_cost * m->fuel_max;
    audio_play_at(SFX_JET_BOOST, mech_chest_pos(w, m->id));   /* NEW */
}
```

For remote mechs on the client: the boost-trigger event isn't
replicated explicitly — `boost_timer` decays locally on each side.
To keep the SFX timing right on remote mechs, fire `SFX_JET_BOOST`
in `snapshot_apply` on the leading edge of `SNAP_STATE_BOOSTING`
(see §10). Pattern lifted from how `NET_MSG_HIT_EVENT` triggers
audio-local sparks.

### 12.3 `SFX_JET_IGNITION` (NEW)

Fires once per `MECH_JET_IGNITION_TICK`. Two material variants:

- `SFX_JET_IGNITION_CONCRETE` — solid surface
- `SFX_JET_IGNITION_ICE` — ice surface

Per the manifest pattern, two new entries in `g_sfx_manifest` with
3-alias pools each. Both 0.85 SFX-bus volume.

```c
/* audio.h */
SFX_JET_IGNITION_CONCRETE,
SFX_JET_IGNITION_ICE,

/* audio.c — manifest */
{ SFX_JET_IGNITION_CONCRETE, "assets/sfx/jet_ignition_concrete.wav", 3, 0.85f, AUDIO_BUS_SFX },
{ SFX_JET_IGNITION_ICE,      "assets/sfx/jet_ignition_ice.wav",      3, 0.85f, AUDIO_BUS_SFX },
```

The audio asset pipeline (tools/audio_inventory + audio_normalize)
needs two new source samples sourced + normalized. Per the M5 P19
flow, that lives in the audio source map and runs through `make
audio-normalize`. **Asset task** — see §13.

Fire site in `mech_jet_fx_step`, paired with the ground impingement
emission:

```c
if (just_ignited) {
    /* ... spawn ignition particles ... */
    if (grounded && jet_fx_ground_query(w, noz, ...)) {
        SfxId cue = (surf_kind & TILE_F_ICE) ? SFX_JET_IGNITION_ICE
                                             : SFX_JET_IGNITION_CONCRETE;
        audio_play_at(cue, noz);
    }
}
```

For non-grounded ignitions (jetting from mid-air on a re-press),
no ignition SFX fires — `SFX_JET_PULSE` covers it. The ground cue
is specifically the "rocket-takeoff thump" the user asked for.

---

## 13. Performance budget

| Pass | Worst case | Estimated cost |
|---|---|---|
| `mech_jet_fx_step` × 16 jetting mechs | 16 mechs × 2 nozzles × (1 ground query + 8 particle spawns + 8 dust spawns + 1 scorch) | 0.20 ms |
| `fx_update` extra particles (peak ~3000 live JET_EXHAUST + 1500 GROUND_DUST) | scalar integrate; SIMD not required | 0.30 ms |
| `mech_jet_fx_draw_plumes` | 16 mechs × 2 quads × DrawTexturePro | 0.10 ms (16 draw calls; raylib's overhead dominates) |
| `mech_jet_fx_collect_hot_zones` + uniform set | 32 mechs walk + 16 vec4 uniforms | 0.02 ms |
| Halftone shader (with shimmer loop) | up to 16 zones × per-fragment loop at 1920×1080 ≈ 2M pixel-iters | 0.50 ms (GPU; not on CPU budget) |
| Decal scorch flushes | rare, batched | 0.05 ms |
| **Total CPU added** | | **~0.67 ms** |

Total of the existing physics budget is 3.0 ms (per
`documents/03-physics-and-mechs.md`), with current measured spend
~2.0 ms. Adding 0.67 ms keeps us at 2.67 ms total CPU on physics
and FX combined — under budget. Halftone shader GPU cost is
~0.5 ms additional; current halftone is ~0.3 ms, so the shimmer
roughly doubles GPU post-cost. Still trivial on integrated graphics
at 1920×1080.

**Pool sizing**:
- Current FX cap: 3000.
- Worst case live JET_EXHAUST: 16 mechs × 2 nozzles × (8 particles/tick
  during boost) × 60 ticks/s × 0.5 s average life = **7680**.
  ← exceeds 3000.

Two options:
1. **Bump pool to 8000.** 8000 × `sizeof(FxParticle)` (≈ 48 B with
   the new `color_cool` field) = ~384 KB permanent. Inside budget.
2. **Cap per-mech particle count.** Track `Mech.fx_particle_count`
   and skip spawns above a cap (e.g., 256). Cheaper memory; harder
   to tune visually.

**Recommendation**: Option 1. Bump capacity to **8000**. The 384 KB
is below the noise floor of the 256 MB resident memory budget per
the vision doc.

Update `src/particle.h` cap comment + the `fx_pool_init` call site
to use the new value.

---

## 14. Hot-reload registration

Mirror the existing pattern in `main.c`:

```c
/* main.c — add to the hotreload_register block after weapons.png */
hotreload_register("assets/sprites/jet_plume.png",   reload_jet_atlases);
hotreload_register("assets/sprites/jet_dust.png",    reload_jet_atlases);
```

Implementation in render.c:

```c
static Texture2D g_jet_plume_atlas;
static Texture2D g_jet_dust_atlas;

void reload_jet_atlases(const char *path) {
    (void)path;
    if (g_jet_plume_atlas.id != 0) UnloadTexture(g_jet_plume_atlas);
    if (g_jet_dust_atlas.id  != 0) UnloadTexture(g_jet_dust_atlas);
    g_jet_plume_atlas = LoadTexture("assets/sprites/jet_plume.png");
    g_jet_dust_atlas  = LoadTexture("assets/sprites/jet_dust.png");
    /* Atlas-id 0 = missing; draw path takes the line-fallback. */
    LOG_I("jet_atlas: plume=%s dust=%s",
          g_jet_plume_atlas.id ? "ok" : "missing",
          g_jet_dust_atlas.id  ? "ok" : "missing");
}
```

The halftone shader is already hot-reload-registered. The reload
callback (`reload_halftone_shader`) needs to **re-cache the two new
uniform locations** after loading:

```c
g_uniloc_jet_hot_zones = GetShaderLocation(g_halftone_post, "jet_hot_zones");
g_uniloc_jet_time      = GetShaderLocation(g_halftone_post, "jet_time");
```

Without this, hot-reloading the shader keeps the stale uniform
locations and shimmer breaks silently. Mirror the `g_uniloc_*` cache
pattern from the existing `resolution` / `halftone_density` uniforms.

---

## 15. Implementation phases

Each phase ends with the build green, the existing tests passing,
and the artifact useful on its own. The earlier phases ship visible
improvements even if later phases never land — so we can stop at
any point if a phase reveals a problem.

### Phase 1 — Wire up `SFX_JET_BOOST` + add `SFX_JET_IGNITION`

Smallest, separable from the visual work. Audio-only.

- Add `SFX_JET_BOOST` `audio_play_at` call at the boost-trigger
  seam in `mech_step_drive` (one line).
- Add `SFX_JET_IGNITION_CONCRETE` + `SFX_JET_IGNITION_ICE` to the
  enum + manifest.
- Asset task: source two ignition WAVs through
  `tools/audio_inventory/source_map.sh` and run
  `make audio-normalize`. Until assets land, the SFX entries log
  "missing — playback no-op" (existing pattern).
- Add `SNAP_STATE_BOOSTING` bit (slot 12 of `state_bits`). Encode
  in `snapshot_record` from `boost_timer > 0`. Decode in
  `snapshot_apply` and mirror leading-edge to `audio_play_at`.
- Bump `SOLDUT_PROTOCOL_ID` `S0LJ` → `S0LK`. Update version-history
  comment.
- Test: `tests/shots/net/run_jet_boost_sfx.sh` — host fires Burst
  + DASH, host log asserts SFX line.

Commit. Move on.

### Phase 2 — `mech_jet_fx.{c,h}` skeleton + tunings table

- Create `src/mech_jet_fx.h` with the API in §4.
- Create `src/mech_jet_fx.c` with `g_jet_fx[JET_COUNT]`,
  `g_chassis_nozzles[CHASSIS_COUNT]`, and stubbed
  `mech_jet_fx_step` / `_draw_plumes` / `_collect_hot_zones` /
  `_any_active` (all returning empty / no-op for now).
- Add to Makefile via the standard module pattern (compile to
  `build/mech_jet_fx.o`, link into headless test object set).
- Add `jet_state_bits` + `jet_prev_grounded` to `Mech` struct.
  Update `mech_step_drive` to set the flags (§6).
- Build green, no behavior change yet.

### Phase 3 — FxKinds + particle emission

- Add `FX_JET_EXHAUST` + `FX_GROUND_DUST` to `FxKind` in `world.h`.
- Add `color_cool` field to `FxParticle` (4 bytes). Update
  `fx_pool_init` accordingly.
- Implement `fx_spawn_jet_exhaust` + `fx_spawn_ground_dust` in
  `particle.c`.
- Extend `fx_update` integrator for the two new kinds.
- Extend `fx_draw` to render the two new kinds (additive for
  exhaust, alpha for dust). Color lerp from `color_hot` to
  `color_cool` over `1 - life/life_max`.
- Bump `FxPool` capacity to 8000 in `main.c`'s `fx_pool_init` call.
- Implement `mech_jet_fx_step` (§5): nozzle resolution, ignition
  burst, sustain stream, ground impingement query + dust emission.
- Wire `mech_jet_fx_step(w, i, dt)` into `simulate_step` after the
  M6 P01 `pose_compute` pass.
- Smoke-test: `tests/shots/m6_jet_standard.shot` — Trooper jets
  straight up from Foundry, takes shots at t=10/20/40/60 ticks,
  contact-sheet.

### Phase 4 — Plume sprite quad

- Add `g_jet_plume_atlas` + `g_jet_dust_atlas` to `render.c`.
  Lazy-load on first `draw_jet_plumes` call (mirror weapons atlas).
- Implement `draw_jet_plumes` (§8). Call it in `draw_world_pass`
  after `decal_draw_layer`, before the mech loop.
- Asset stub: ship a placeholder `jet_plume.png` (32×96 radial
  gradient white-on-transparent) and `jet_dust.png` (16×16 soft
  disc).
- Register hot-reload for both textures (§14).
- Shot test: `tests/shots/m6_jet_plume_per_chassis.shot` — each of
  5 chassis × 4 jetpack types jets in a corridor; contact-sheet
  shows all 20 combinations.

### Phase 5 — Heat shimmer shader

- Extend `assets/shaders/halftone_post.fs.glsl` per §9.
- Add `g_uniloc_jet_hot_zones` + `g_uniloc_jet_time` to `render.c`.
  Cache in `load_halftone_shader` + `reload_halftone_shader`.
- Implement `mech_jet_fx_collect_hot_zones` + the per-frame uniform
  push in `renderer_draw_frame`.
- Gate uniform-push on `mech_jet_fx_any_active(w)` to skip cost on
  idle ticks.
- Add `shimmer_enabled` to `Config` (default 1). Off in shot mode
  for deterministic screenshots.
- Shot test: `tests/shots/m6_jet_shimmer.shot` — high-thrust burst
  next to a vertical tile line; the line should appear to waver
  in the contact sheet around the plume region.

### Phase 6 — Decal scorch

- Add `decal_paint_scorch` to `src/decal.{c,h}` (§11).
- Call from `mech_jet_fx_step` on `just_ignited` and boost-while-
  grounded ticks.
- Trade-off entry: "Scorch decals are permanent for the round (no
  per-decal fade)" — add to TRADE_OFFS.md.

### Phase 7 — Surface differentiation + ignition SFX

- Wire `SFX_JET_IGNITION_CONCRETE` / `SFX_JET_IGNITION_ICE` per §12.3
  to the ignition seam in `mech_jet_fx_step`.
- Ice surfaces also swap ground-dust color to pale steam (§5
  `emit_ground_dust`).
- Map-specific tuning: Catwalk + Aurora have ICE tiles; verify
  visually on those maps.

### Phase 8 — Audio asset sourcing

- Source 2 ignition WAVs from Kenney / opengameart through the
  existing audio inventory script.
- Normalize to 22050 Hz mono PCM16 via `make audio-normalize`.
- Run `make test-audio-smoke` to verify the new entries don't
  log missing.
- Update `assets/credits.txt`.

### Phase 9 — Documentation + polish

- Update CURRENT_STATE.md: add the M6 P02 tunables alongside
  existing JET_THRUST_PXS2 / GRAV etc.
- Update CLAUDE.md status line: "M6 P02 — jetpack propulsion FX".
- Append Phase status footnote to this document (§19).
- Two TRADE_OFFS.md additions, both pre-disclosed in §16.
- Update `documents/06-rendering-audio.md` if it has an FX section
  describing the existing FxKinds.

---

## 16. Risks and trade-offs

### Risks

- **Per-jetpack visual tuning needs an iteration round.** First-pass
  numbers in §4 are best guesses. Allocate a render-test session
  after Phase 5 lands.
- **Shimmer-shader perf on integrated GPUs.** A 16-zone fragment
  loop is small but not free at 1920×1080. The `shimmer_enabled`
  config knob is the kill switch.
- **Pool spikes during 16-player boost storm.** Worst-case
  estimate in §13 lands inside 8000-particle cap, but real play
  may surprise. Monitor in playtest; bump cap to 12000 if needed.
  Per-mech caps are the next-level mitigation.
- **`SNAP_STATE_BOOSTING` slot conflict.** Need to confirm slot 12
  is unused at protocol `S0LJ`. Audit `state_bits` usage in
  `snapshot.h` before assigning.
- **Decal scorch fills the chunk RT on long matches.** Mitigated by
  the existing chunk-rotation policy (oldest pixels overwritten in
  a high-traffic chunk). Verify in a 30-minute match-length test.

### Trade-offs we expect to log (pre-disclosed)

Add these to `TRADE_OFFS.md` when the work lands:

- **"Scorch decals are permanent for the round (no per-decal fade)."**
  Reason: decal RT has no per-splat age tracking. Revisit when the
  decal layer grows per-splat records.
- **"Jet exhaust is client-local (no per-particle wire data)."**
  Same model as blood spray: each client spawns its own particle
  stream from the same replicated jet state. Visually
  distribution-identical, not particle-identical. Revisit only if
  visual desync (specific particle landing positions) becomes a
  reportable bug.
- **"Heat shimmer uniform loop has a 16-zone hard cap."** With 16
  player slots and 1-nozzle effective per mech (we collect at the
  pelvis, not per-nozzle, to fit the cap), 16 zones is the natural
  worst case. Revisit if MAX_LOBBY_SLOTS grows above 16.

### Trade-offs this work RETIRES (deletion candidates)

- **"`SFX_JET_BOOST` is defined but never fired."** Implicit
  trade-off in audio.c; not a TRADE_OFFS.md entry today, but the
  Phase 1 hookup eliminates the dead code path. Note in the Phase 1
  commit message.
- *(none of the TRADE_OFFS.md entries directly cover jet visuals
  today — this work is net-additive.)*

---

## 17. References

External:
- [Shock diamond — Wikipedia](https://en.wikipedia.org/wiki/Shock_diamond)
  — overexpanded-nozzle pattern. Cited for "we are not modeling this"
  in §0. The brightness banding in the plume texture is artistic, not
  physical.
- [Aerospaceweb — Shock Diamonds and Mach Disks](https://aerospaceweb.org/question/propulsion/q0224.shtml)
  — readable explainer for designers who want to art-direct the
  plume texture more authentically.
- [Apogee Rockets Newsletter #441 — Phenomena of Rocket Exhaust Plumes](https://www.apogeerockets.com/education/downloads/Newsletter441.pdf)
  — wider survey of plume visuals, including takeoff dust kick.
- [Kyle Halladay — Screen Space Distortion (sci-fi shield)](https://kylehalladay.com/blog/tutorial/2016/01/15/Screen-Space-Distortion.html)
  — the textbook RT-grab + UV-offset technique. Our halftone shader
  is the equivalent of the "second pass" he describes.
- [Linden Reid — Heat Distortion Shader Tutorial](https://lindenreidblog.com/2018/03/05/heat-distortion-shader-tutorial/)
  — practical noise-driven UV offset; the sin/cos noise in §9 is
  the cheapest version of her flowmap technique.
- [Wolfire Games — Screen-space distortion](http://blog.wolfire.com/2006/03/screen-space-distortion/)
  — Overgrowth's approach, useful for the "many small distortion
  zones" case.
- [Godot Shaders — Yoshi's Island Shimmer](https://godotshaders.com/shader/yoshis-island-shimmer-heat-haze-distortion/)
  — gentle reference for the pixel-art-friendly version of heat
  haze that fits this art style.
- [CREASTA — Rocket Boost / Thruster FX sprite sheet](https://creasta.itch.io/rocket-boost-vfx-sprite-sheet)
  — confirms the standard 2D approach (sprite-strip animation +
  particle pool). We are doing the particle-pool half, not the
  sprite-strip half.

Internal (this codebase):
- `src/mech.c::apply_jet_force` (~lines 644–711) — current impulse
  path. Don't touch (except to set the new flag bits).
- `src/mech.c::mech_step_drive` (~lines 743–928) — boost trigger
  seam. Add the `SFX_JET_BOOST` call here (§12.2).
- `src/particle.c::fx_update` + `fx_draw` (~lines 150–282) — FX
  pool integrator + renderer. Extend with the two new kinds.
- `src/decal.c::decal_paint_blood` (~line 139) — pattern for
  `decal_paint_scorch`.
- `src/render.c::draw_world_pass` (~lines 1230–1275) — render order;
  add `draw_jet_plumes` between decals and mechs.
- `src/render.c::renderer_draw_frame` (~lines 1282–1358) — post-
  process pipeline; uniform push for hot zones goes here.
- `assets/shaders/halftone_post.fs.glsl` — fragment shader; extend
  with shimmer (§9).
- `src/snapshot.h` + `snapshot.c` — `state_bits` slot 12 for
  `SNAP_STATE_BOOSTING`.
- `src/world.h::FxParticle` (~line 605) — extend with `color_cool`
  (4 bytes).
- `src/world.h::TileFlagBits` — `TILE_F_ICE` is the differentiator
  for ice vs concrete in `emit_ground_dust`.
- `documents/m6/01-ik-and-pose-sync.md` — reference for the M6 doc
  format + Status Footnote convention used in §19.
- `documents/03-physics-and-mechs.md` — note this work doesn't
  change the impulse path; the doc's existing JETSPEED / chassis
  mult description stays accurate.

---

## 18. Definition of done

The work is done when:

1. `SFX_JET_BOOST` fires on host + remote when any Burst-equipped
   mech triggers boost (Phase 1, asserted in
   `tests/shots/net/run_jet_boost_sfx.sh`).
2. `SFX_JET_IGNITION_*` fires once per grounded → airborne edge with
   the surface-correct variant (`_CONCRETE` on Foundry, `_ICE` on
   Catwalk's ice patches). Asserted in
   `tests/shots/m6_jet_ignition_ice.shot`.
3. Every alive jetting mech has a visible plume sprite in the
   render path, color-tinted by `jetpack_id`. Verified by visual
   inspection of `tests/shots/m6_jet_plume_per_chassis.shot`
   contact sheet (20 cells, one per chassis × jetpack).
4. Ground impingement produces a dust cloud + scorch decal on
   grounded jet, with steam (cyan) on ice and dust (warm grey) on
   concrete. Verified by `tests/shots/m6_jet_ground_plume.shot`.
5. Heat shimmer visibly warps a vertical tile line near an active
   plume, verified by `tests/shots/m6_jet_shimmer.shot`.
6. All existing CI tests still pass: `test-physics`,
   `test-level-io`, `test-spawn`, `test-pickups`, `test-ctf`,
   `test-snapshot`, `test-mech-ik`, `test-pose-compute`.
7. `test-audio-smoke` reports two new entries loaded
   (`SFX_JET_IGNITION_CONCRETE`, `SFX_JET_IGNITION_ICE`) and zero
   missing files post-asset-sourcing.
8. A 60-second paired-window real-play run (host + client) on
   Foundry with both players using `JET_BURST`: both windows
   render the same plume color, ground dust, and boost flash on
   the other player's mech. No visible desync of jet state.
9. CPU added cost measured ≤ 1.0 ms via in-game `--shot` profile
   harness on 16-mech worst case. Halftone shader GPU cost
   measured ≤ 1.5 ms.
10. CURRENT_STATE.md tunables table includes the M6 P02 entries.
    CLAUDE.md status line bumped. TRADE_OFFS.md adds the entries
    from §16.
11. Status footnote in §19 of this document filled in with phase
    landing dates + commits.

---

## 19. Status footnote (append when work lands)

- Phase 0 — design document drafted 2026-05-12.
- Phase 1 — shipped 2026-05-12. Wired `SFX_JET_BOOST` at boost
  trigger in `mech_step_drive`; added `SFX_JET_IGNITION_CONCRETE` +
  `SFX_JET_IGNITION_ICE` to audio enum + manifest;
  `SNAP_STATE_BOOSTING = 1u << 14` plus leading-edge SFX trigger
  for remote mechs in `snapshot_apply`; protocol id bumped
  `S0LJ` → `S0LK`; `tests/snapshot_test.c` updated to assert the
  new protocol id. (Folded the `Mech.jet_state_bits` +
  `jet_prev_grounded` plumbing from Phase 2 into this phase since
  the leading-edge SFX needed the previous-tick boost state.)
- Phase 2 — shipped 2026-05-12. `src/mech_jet_fx.{c,h}` skeleton
  with `g_jet_fx[JET_COUNT]` tuning table + `g_chassis_nozzles
  [CHASSIS_COUNT]` nozzle table + stubbed driver functions
  (`mech_jet_fx_step` / `_draw_plumes` / `_any_active` /
  `_collect_hot_zones` / `_reload_atlases`). Picked up automatically
  by `Makefile`'s `wildcard src/*.c` rule — no manifest edit needed.
  Routed FX RNG through the existing `world.rng` (accepted the
  client-local nondeterminism per spec §5 option b) rather than
  adding a separate `fx_rng` field — keeps churn small; the
  trade-off is logged in TRADE_OFFS.md.
- Phase 3 — shipped 2026-05-12. `FX_JET_EXHAUST` + `FX_GROUND_DUST`
  added to `FxKind`; new `color_cool` u32 on `FxParticle` (+4 B,
  total ~52 B); `fx_spawn_jet_exhaust` + `fx_spawn_ground_dust` in
  `particle.c` with additive / alpha branches in `fx_draw` + drag /
  buoyancy / gravity in `fx_update`; `MAX_BLOOD` bumped 3000 → 8000;
  `mech_jet_fx_step` body filled in (nozzle resolution, ignition
  burst, sustain stream, ground-impingement query, dust emission)
  and wired into `simulate.c::simulate_step` right after
  `pose_write_to_particles`.
- Phase 4 — shipped 2026-05-12. `mech_jet_fx_draw_plumes` lazy-loads
  `assets/sprites/jet_plume.png` + `_dust.png` (missing-asset
  fallback: additive `DrawLineEx` from nozzle along thrust_dir);
  wired into `render.c::draw_world_pass` between `decal_draw_layer`
  and the mech loop so bodies silhouette against the plume; hot-
  reload paths registered in `main.c` alongside the existing chassis
  / weapons / decoration / HUD entries.

  Visual iteration (post-paired-shot review): `plume_length_px`
  bumped from 28/34/18 (Standard/Burst/Glide) → 56/64/36 so the
  plume sprite quad extends visibly past the body silhouette
  instead of being half-occluded by the legs. Per-jetpack values
  recorded in CURRENT_STATE.md tunables table.
- Phase 5 — shipped 2026-05-12. `halftone_post.fs.glsl` extended
  with `uniform vec4 jet_hot_zones[16]` + `uniform float jet_time`
  and a `shimmer_offset` helper (exp-falloff weighted two-octave
  sin/cos UV displacement). `render.c` caches the two new uniform
  locations on shader load (re-cached on hot-reload via the
  existing `renderer_post_shutdown` path), and pushes the array
  per-frame inside the `BeginShaderMode` block — gated on
  `mech_jet_fx_any_active(w)` so idle ticks (most ticks) skip the
  uniform set. Shimmer disabled in shot mode to keep
  screenshot diffs deterministic; the zone-zeroing path resets the
  uniform once on first-idle-tick so the shader skip is reliable.
  `mech_jet_fx_collect_hot_zones` collects per-mech-at-pelvis (one
  zone per active mech, not per nozzle) to fit the 16-slot cap.
- Phase 6 — shipped 2026-05-12. `decal_paint_scorch(Vec2, float)`
  added to `decal.{c,h}`; the existing blood pending queue now
  carries a `SplatKind` discriminator so blood + scorch share the
  chunk-routing + flush path (one new branch in `stamp_splat`).
  Scorch fired from `mech_jet_fx_step` on the just-ignited tick
  (where the ground query still reaches the floor we just left)
  and on boost-while-grounded ticks. Scorch is permanent for the
  round (TRADE_OFFS.md entry added).
- Phase 7 — shipped 2026-05-12. `SFX_JET_IGNITION_CONCRETE` /
  `SFX_JET_IGNITION_ICE` wired from `mech_jet_fx_step` at the
  ignition site (paired with the scorch + dust + ignition particle
  burst). Surface key is `surf_flags & TILE_F_ICE` from the ground-
  impingement query; `emit_ground_dust` already swaps color to pale
  cyan steam on ice tiles. Non-grounded re-press ignitions stay
  silent — the ground query misses the floor and `SFX_JET_PULSE`
  covers them, matching the spec's "rocket-takeoff thump is the
  specifically-grounded cue" guidance.
- Phase 8 — shipped 2026-05-12. `assets/sfx/jet_ignition_concrete.wav`
  sourced from `kenney_sci-fi-sounds/lowFrequency_explosion_000.ogg`
  (0.55 s, deep punchy thump) and `assets/sfx/jet_ignition_ice.wav`
  from `kenney_sci-fi-sounds/thrusterFire_003.ogg` (0.50 s, whoosh +
  hiss, distinct from the concrete low-end). Both 22050 Hz mono
  PCM16 via the existing `tools/audio_inventory/source_map.sh` →
  `tools/audio_normalize/normalize.sh` pipeline. `assets/credits.txt`
  attributes both. `make test-audio-smoke` reports 49/49 loaded
  (was 47 pre-P02). `make audio-inventory` reports 0 missing;
  `make audio-credits` reports all attributed.

  Sprite atlases — `assets/sprites/jet_plume.png` (32×96 RGBA8
  teardrop) and `assets/sprites/jet_dust.png` (16×16 soft disc)
  generated procedurally via `tools/build_jet_atlases.sh` (Python
  + PIL; RGB stays white, alpha shape per spec §8 — narrow rim,
  soft bottom). Soldut-original geometric content; tint comes
  from per-jetpack `plume_color_rim` + `_core` at draw time.
  Re-run the script to re-bake (deterministic given the same
  source).
- Phase 9 — shipped 2026-05-12. CURRENT_STATE.md tunables table
  gained the M6 P02 entries (MAX_BLOOD bump, JET_BUOY_PXS2,
  GROUND_DUST_GRAVITY_PXS2, JET_IMPINGE_MAX_DIST, per-jetpack
  spawn rates, hot-zone radii, JET_HOT_ZONE_MAX, plume_length).
  CLAUDE.md status line bumped to "M6 P02 — jetpack propulsion
  FX". TRADE_OFFS.md gained three new entries:
  "Scorch decals are permanent for the round (no per-decal fade)";
  "Jet exhaust is client-local (no per-particle wire data)";
  "Heat shimmer uniform loop has a 16-zone hard cap (one per active mech, at pelvis)".

  Paired-window sync verification — `tests/shots/net/2p_jet_fx.{host,
  client}.shot` (Burst vs Standard on Reactor) + `2p_jet_fx_glide.
  {host,client}.shot` (Glide vs JumpJet on Reactor) capture matching
  PNGs from each side at ignition / sustain / boost / post-boost /
  falling / grounded ticks. Both runs pass run.sh's 12 plumbing
  assertions; visual review confirms all 4 jetpack types render
  with correct color (Burst orange, Standard cyan-white, Glide
  pale-cyan wisps, JumpJet green-cyan ignition kick) and that host
  and client see the same plume on the same tick.

  JumpJet ignition-flag fix — the spec-default IGNITION_TICK
  condition (`was_grounded && !grounded`) never fired for JET_JUMP_JET
  because the apply_jump impulse only sets velocity (mech is still
  on the floor at end of the press tick) while BTN_JET is released
  on the next tick (`tap` = press@N + release@N+1). Result: no
  ignition burst on jump. Fixed in `mech_step_drive` by setting
  IGNITION_TICK directly on the press tick when `jet_jump_active`;
  continuous-thrust jets still use the grounded→airborne edge.
  Verified via the Glide+JumpJet paired shot.
