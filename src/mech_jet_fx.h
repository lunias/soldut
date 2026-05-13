#pragma once

#include "math.h"
#include "mech.h"
#include "world.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * M6 P02 — Jetpack propulsion FX.
 *
 * Spec: documents/m6/02-jetpack-propulsion-fx.md.
 *
 * Driven from `simulate_step` once per alive mech AFTER pose_compute /
 * post-physics-anchor: the pelvis position the FX step reads is the
 * final-of-tick pelvis. The state it consumes (MECH_JET_* in
 * `Mech.jet_state_bits` + boost_timer + fuel + chassis_id + jetpack_id
 * + facing_left + grounded) is owner-side written by mech_step_drive
 * and remote-side written by snapshot_apply, so this module is a pure
 * read-only consumer of the simulation state. It writes only to the
 * FxPool (via fx_spawn_*) and the decal RT (via decal_paint_scorch).
 */

/* Per-jetpack visual tunings. Authoritative table is g_jet_fx in
 * mech_jet_fx.c — values are first-pass, intended to iterate after
 * the visual is on screen. See §4 / §13 of the spec. */
typedef struct {
    uint32_t plume_color_rim;        /* RGBA8 (0xRRGGBBAA) */
    uint32_t plume_color_core;       /* RGBA8 — multiplied as tint on the plume sprite */
    uint32_t particle_color_hot;     /* RGBA8 — at spawn */
    uint32_t particle_color_cool;    /* RGBA8 — at life end (linear-interp by life fraction) */
    uint8_t  nozzle_count;           /* 1 or 2 (2 mirrors the single chassis-offset around body-local x) */
    uint8_t  sustain_particles_per_tick;
    uint8_t  boost_particles_per_tick;
    uint8_t  ignition_particles;     /* fired once on grounded → airborne while ACTIVE */
    uint8_t  sustain_tick_divisor;   /* spawn only when (tick % divisor) == 0; 1 = every tick */
    uint8_t  pad[3];
    float    plume_length_px;        /* base plume sprite length (multiplied by thrust scalar) */
    float    plume_width_px;         /* base plume sprite width */
    float    particle_speed_pxs;     /* base exhaust velocity */
    float    particle_life_min;
    float    particle_life_max;
    float    particle_size_min;
    float    particle_size_max;
    bool     has_continuous_plume;   /* false for JET_JUMP_JET */
} JetFxDef;

extern const JetFxDef g_jet_fx[JET_COUNT];

/* Per-chassis nozzle offset from PELVIS in body-local source coords.
 * `offset.x > 0` is "behind the mech when facing right"; mech_jet_fx
 * negates the x-component when facing left. `thrust_dir` is the
 * world-space exhaust direction (always pointing down with a small
 * chassis-specific outward angle). For 2-nozzle jetpacks the slot[0]
 * offset is mirrored around body-local x to produce a left/right pair. */
typedef struct {
    Vec2 offset;
    Vec2 thrust_dir;
    bool active;
    uint8_t pad[3];
} ChassisNozzle;

typedef struct {
    ChassisNozzle slot[2];
} ChassisNozzleSet;

extern const ChassisNozzleSet g_chassis_nozzles[CHASSIS_COUNT];

/* Per-tick driver. Runs in simulate_step after pose_compute (so the
 * pelvis position is the final one for the tick), once per mech. Pure
 * consumer of jet_state_bits + boost_timer + chassis/jet tables; spawns
 * particles + paints scorch decals. */
void mech_jet_fx_step(World *w, int mech_id, float dt);

/* Render the per-mech plume sprites. Called from render.c's
 * draw_world_pass between the decal layer and the mech loop, so the
 * mech body silhouettes against the plume. */
void mech_jet_fx_draw_plumes(const World *w, float interp_alpha);

/* Returns true if any nozzle on any alive mech is currently active.
 * Used by the renderer to gate the heat-shimmer uniform push. */
bool mech_jet_fx_any_active(const World *w);

/* Per-tick hot-zone export for the heat-shimmer shader. Returns count
 * written (≤ JET_HOT_ZONE_MAX). Each entry is (screen_xy, radius,
 * intensity). Pure read of world + camera state. */
#define JET_HOT_ZONE_MAX 16
typedef struct {
    float x, y, radius, intensity;
} JetHotZone;

int mech_jet_fx_collect_hot_zones(const World *w, const Camera2D *cam,
                                  JetHotZone *out, int max);

/* Hot-reload entry point for the plume / dust atlases. Called by the
 * watcher in main.c when assets/sprites/jet_plume.png or
 * assets/sprites/jet_dust.png changes on disk. DEV_BUILD-only path. */
void mech_jet_fx_reload_atlases(const char *path);
