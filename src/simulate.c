#include "simulate.h"

#include "log.h"
#include "mech.h"
#include "particle.h"
#include "physics.h"

static const char *anim_name(int a) {
    switch (a) {
        case ANIM_STAND: return "STAND";
        case ANIM_RUN:   return "RUN";
        case ANIM_JET:   return "JET";
        case ANIM_FALL:  return "FALL";
        case ANIM_FIRE:  return "FIRE";
        case ANIM_DEATH: return "DEATH";
        default:         return "?";
    }
}

/* End-of-tick diagnostic dump for shot mode. Emits transition events
 * (anim/grounded/alive) for every mech, then a compact one-line
 * summary for the local mech. Kept in simulate.c so the rest of the
 * codebase doesn't need to maintain a "previous state" copy of the
 * mech struct just for logging. State persists across ticks via
 * file-scope arrays — only one shot run happens per process. */
static void shot_dump_tick(const World *w) {
    static int  s_prev_anim    [MAX_MECHS];
    static bool s_prev_grounded[MAX_MECHS];
    static bool s_prev_alive   [MAX_MECHS];
    static bool s_initialized = false;
    if (!s_initialized) {
        for (int i = 0; i < MAX_MECHS; ++i) {
            s_prev_anim[i]     = -1;     /* force a "→" line on first observation */
            s_prev_grounded[i] = false;
            s_prev_alive[i]    = true;   /* suppress spurious alive=1 at startup */
        }
        s_initialized = true;
    }

    for (int i = 0; i < w->mech_count; ++i) {
        const Mech *m = &w->mechs[i];
        if (m->anim_id != s_prev_anim[i]) {
            if (s_prev_anim[i] >= 0) {
                SHOT_LOG("t=%llu mech=%d anim %s->%s",
                         (unsigned long long)w->tick, i,
                         anim_name(s_prev_anim[i]), anim_name(m->anim_id));
            }
            s_prev_anim[i] = m->anim_id;
        }
        if (m->grounded != s_prev_grounded[i]) {
            SHOT_LOG("t=%llu mech=%d grounded=%d",
                     (unsigned long long)w->tick, i, (int)m->grounded);
            s_prev_grounded[i] = m->grounded;
        }
        if (m->alive != s_prev_alive[i]) {
            SHOT_LOG("t=%llu mech=%d alive=%d",
                     (unsigned long long)w->tick, i, (int)m->alive);
            s_prev_alive[i] = m->alive;
        }
    }

    int lid = w->local_mech_id;
    if (lid < 0) return;
    const Mech *m = &w->mechs[lid];
    const ParticlePool *pp = &w->particles;
    int pelv = m->particle_base + PART_PELVIS;
    float px = pp->pos_x[pelv], py = pp->pos_y[pelv];
    float vx = px - pp->prev_x[pelv];
    float vy = py - pp->prev_y[pelv];
    SHOT_LOG("t=%llu anim=%s grnd=%d pelv=(%.1f,%.1f) v=(%.2f,%.2f) "
             "fuel=%.2f hp=%.0f/%.0f ammo=%d/%d",
             (unsigned long long)w->tick, anim_name(m->anim_id),
             (int)m->grounded, px, py, vx, vy, m->fuel,
             m->health, m->health_max, m->ammo, m->ammo_max);
}

void simulate(World *w, ClientInput in, float dt) {
    /* Hit-pause: the world clock freezes for a few ticks after a
     * notable kill. We still run FX (so blood keeps falling), but
     * physics and pose drive don't advance. */
    if (w->hit_pause_ticks > 0) {
        w->hit_pause_ticks--;
        fx_update(w, dt);
        w->shake_intensity *= 0.95f;
        w->last_event_time += dt;
        if (g_shot_mode) {
            SHOT_LOG("t=%llu hit_pause remaining=%d",
                     (unsigned long long)w->tick, w->hit_pause_ticks);
        }
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

    if (g_shot_mode) shot_dump_tick(w);

    w->tick++;
}
