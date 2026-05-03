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

void simulate(World *w, ClientInput in, float dt);
