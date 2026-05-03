#pragma once

#include "input.h"
#include "world.h"

/*
 * The pure simulation step. simulate(World*, ClientInput, dt) advances
 * the world by `dt` seconds:
 *
 *   1. Pose drive + per-input forces (mech_step_drive)  — for each mech
 *   2. Try-fire weapons                                  — for the local mech only
 *   3. Apply gravity
 *   4. Verlet integrate
 *   5. Constraint relaxation
 *   6. Map collision
 *   7. FX particle update
 *   8. Bookkeeping (shake decay, hit-pause, last-event timer)
 *
 * No globals, no wall-clock reads. The world's pcg32 rng is the only
 * non-deterministic source and it's seeded by the host.
 */

/* Convenience: latch `in` onto the local mech, then call simulate_step.
 * The single-player and host-player paths use this. */
void simulate(World *w, ClientInput in, float dt);

/* Step the world without latching a fresh input. Each mech consumes
 * its own World->mechs[i].latched_input. The server populates those
 * from received NET_MSG_INPUT packets before calling simulate_step;
 * client reconciliation calls simulate_step during replay with the
 * local mech's latched_input rotated through the unacked-inputs ring. */
void simulate_step(World *w, float dt);
