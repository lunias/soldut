#include "simulate.h"

#include "log.h"
#include "mech.h"
#include "particle.h"
#include "physics.h"
#include "projectile.h"
#include "snapshot.h"
#include "weapons.h"

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

    /* Per-tick pelvis dump for EVERY alive mech, not just the local
     * one. This catches remote-mech jitter — if mech 0's pelvis_x
     * oscillates rather than monotonically increases when the host is
     * walking right, the snapshot apply path is fighting prediction.
     * Cheap (a couple lines per mech per tick) and only fires in shot
     * mode. */
    const ParticlePool *pp = &w->particles;
    for (int i = 0; i < w->mech_count; ++i) {
        const Mech *mi = &w->mechs[i];
        if (!mi->alive) continue;
        int pelv = mi->particle_base + PART_PELVIS;
        float px = pp->pos_x[pelv], py = pp->pos_y[pelv];
        float vx = px - pp->prev_x[pelv];
        float vy = py - pp->prev_y[pelv];
        bool is_local = (i == w->local_mech_id);
        SHOT_LOG("t=%llu mech=%d%s pelv=(%.1f,%.1f) v=(%.2f,%.2f)",
                 (unsigned long long)w->tick, i, is_local ? "*" : "",
                 px, py, vx, vy);
    }

    int lid = w->local_mech_id;
    if (lid < 0) return;
    const Mech *m = &w->mechs[lid];
    int pelv = m->particle_base + PART_PELVIS;
    SHOT_LOG("t=%llu local anim=%s grnd=%d fuel=%.2f hp=%.0f/%.0f ammo=%d/%d",
             (unsigned long long)w->tick, anim_name(m->anim_id),
             (int)m->grounded, m->fuel,
             m->health, m->health_max, m->ammo, m->ammo_max);
    (void)pelv;
}

void simulate(World *w, ClientInput in, float dt) {
    /* Latch the externally-supplied input onto the local mech.
     * Server / replay paths call simulate_step() directly with
     * per-mech inputs already populated; single-player and the host's
     * own player path go through this convenience wrapper. */
    if (w->local_mech_id >= 0) {
        w->mechs[w->local_mech_id].latched_input = in;
    }
    simulate_step(w, dt);
}

void simulate_step(World *w, float dt) {
    /* P03: snapshot pos → render_prev for every live thing the renderer
     * reads. Done unconditionally at the top of the tick so the
     * renderer's alpha lerp has a consistent "where it was last tick"
     * anchor regardless of hit_pause / FX-only paths. ~30 µs at
     * worst-case (4096 particles + 512 projectiles + 3000 FX). */
    {
        ParticlePool *pp = &w->particles;
        for (int i = 0; i < pp->count; ++i) {
            pp->render_prev_x[i] = pp->pos_x[i];
            pp->render_prev_y[i] = pp->pos_y[i];
        }
        ProjectilePool *prp = &w->projectiles;
        for (int i = 0; i < prp->count; ++i) {
            prp->render_prev_x[i] = prp->pos_x[i];
            prp->render_prev_y[i] = prp->pos_y[i];
        }
        FxPool *fxp = &w->fx;
        for (int i = 0; i < fxp->count; ++i) {
            fxp->items[i].render_prev_pos = fxp->items[i].pos;
        }
    }

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

    /* For each mech: drive (pose, input forces). Each mech consumes
     * its own latched_input (filled by the platform layer for the
     * local mech, by net.c for remote mechs on the server, or by
     * reconcile during client replay). */
    for (int i = 0; i < w->mech_count; ++i) {
        ClientInput drive_in = w->mechs[i].latched_input;
        if (drive_in.dt <= 0.0f) drive_in.dt = dt;
        mech_step_drive(w, i, drive_in, dt);
    }

    /* Try-fire. Authoritative server fires for *every* mech driven by
     * a remote peer (their latched input may have BTN_FIRE set). The
     * local-only path also fires for the local mech. Pure clients
     * never fire authoritatively — the server overrules — so we gate
     * on world.authoritative. The local client still spawns a tracer
     * locally for instant feedback inside mech_step_drive's pose
     * pass, but no damage is applied. */
    if (w->authoritative) {
        for (int i = 0; i < w->mech_count; ++i) {
            mech_try_fire(w, i, w->mechs[i].latched_input);
        }
        /* Now that both passes have seen this tick's edges, latch the
         * input into prev_buttons so next tick's edge-detect is fresh. */
        for (int i = 0; i < w->mech_count; ++i) {
            mech_latch_prev_buttons(w, i);
        }
    } else if (w->local_mech_id >= 0) {
        /* Client-only: visual tracer for predicted shots. The server
         * will overrule with the real hit on the next snapshot. */
        Mech *m = &w->mechs[w->local_mech_id];
        if ((m->latched_input.buttons & BTN_FIRE) &&
            m->fire_cooldown <= 0.0f && m->reload_timer <= 0.0f &&
            m->ammo > 0) {
            weapons_predict_local_fire(w, w->local_mech_id);
            m->ammo--;
        }
        /* Pure-client path: also latch prev_buttons so the predicted
         * mech sees edges correctly during replay. */
        for (int i = 0; i < w->mech_count; ++i) {
            mech_latch_prev_buttons(w, i);
        }
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

    /* P02: environmental damage tick — DEADLY tiles, DEADLY polygons,
     * ACID ambient zones. Server-only (clients receive HP via
     * snapshots). */
    if (w->authoritative) {
        for (int i = 0; i < w->mech_count; ++i) {
            mech_apply_environmental_damage(w, i, dt);
        }
    }

    /* Projectiles — integrate, collide, detonate. Runs after the mech
     * physics pass so projectiles see the just-settled bone positions
     * (matters for hit attribution on a shot that lands on the same
     * tick the target finished a jump). */
    projectile_step(w, dt);

    /* FX particles (blood, sparks, tracers). */
    fx_update(w, dt);

    /* Age the kill feed; HUD reads .age to fade entries out. */
    for (int i = 0; i < KILLFEED_CAPACITY; ++i) {
        w->killfeed[i].age += dt;
    }

    /* Decays. */
    w->shake_intensity *= 0.92f;
    if (w->shake_intensity < 0.001f) w->shake_intensity = 0.0f;
    w->last_event_time += dt;

    /* Server-only: record bone history for lag compensation. We do
     * this every authoritative tick so the rolling 12-tick window is
     * always fresh. Clients don't need lag history (they don't run
     * authoritative hit detection). */
    if (w->authoritative) {
        for (int i = 0; i < w->mech_count; ++i) {
            snapshot_record_lag_hist(w, i);
        }
    }

    if (g_shot_mode) shot_dump_tick(w);

    w->tick++;
}
