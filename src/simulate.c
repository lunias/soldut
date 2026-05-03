#include "simulate.h"

#include "mech.h"
#include "particle.h"
#include "physics.h"

void simulate(World *w, ClientInput in, float dt) {
    /* Hit-pause: the world clock freezes for a few ticks after a
     * notable kill. We still run FX (so blood keeps falling), but
     * physics and pose drive don't advance. */
    if (w->hit_pause_ticks > 0) {
        w->hit_pause_ticks--;
        fx_update(w, dt);
        w->shake_intensity *= 0.95f;
        w->last_event_time += dt;
        w->tick++;
        return;
    }

    /* For each mech: drive (pose, input forces, fire). Only the local
     * mech consumes input; the others get a zeroed input. */
    for (int i = 0; i < w->mech_count; ++i) {
        ClientInput drive_in = (i == w->local_mech_id) ? in : (ClientInput){0};
        if (drive_in.dt <= 0.0f) drive_in.dt = dt;
        mech_step_drive(w, i, drive_in, dt);
    }

    /* Try-fire happens after drive so the latest hand position is used. */
    if (w->local_mech_id >= 0) {
        mech_try_fire(w, w->local_mech_id, in);
    }

    /* Physics passes. Constraints and tile collisions are interleaved
     * inside one relaxation loop so a foot held by the floor lifts the
     * body via the chain on the very same tick — instead of letting the
     * pelvis sag a little each frame. */
    physics_apply_gravity(w, dt);
    physics_integrate(w, dt);
    physics_constrain_and_collide(w);

    /* Post-physics: lift any grounded mech up to standing height
     * kinematically, so gravity sag doesn't accumulate from frame to
     * frame. Skip-foot translation preserves the chain. */
    for (int i = 0; i < w->mech_count; ++i) {
        mech_post_physics_anchor(w, i);
    }

    /* FX particles (blood, sparks, tracers). */
    fx_update(w, dt);

    /* Decays. */
    w->shake_intensity *= 0.92f;
    if (w->shake_intensity < 0.001f) w->shake_intensity = 0.0f;
    w->last_event_time += dt;

    w->tick++;
}
