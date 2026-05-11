#include "simulate.h"

#include "hash.h"
#include "log.h"
#include "mech.h"
#include "particle.h"
#include "physics.h"
#include "pickup.h"
#include "projectile.h"
#include "snapshot.h"
#include "weapons.h"

#include <math.h>

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

    /* wan-fixes-3 — client-side stability for REMOTE mechs.
     *
     * On the client (`!w->authoritative`), remote mechs have their
     * pelvis position written each tick by snapshot_interp_remotes
     * (lerping between bracketing snapshots in `remote_snap_ring`).
     * They are NOT predicted; the snapshot stream is authoritative.
     *
     * Pre-fix: simulate_step still ran the full physics stack on
     * remote mech particles — gravity, Verlet, pose drive, the
     * 12-iter constraint+collide pass. Because remote latched_input
     * is stale (snapshot_apply only sets aim_world / facing / state
     * bits, not buttons), the pose drive produced erratic targets;
     * the constraint solver couldn't fully converge in 12 iters
     * against those + gravity + tile pushes; bone offsets from
     * pelvis drifted tick-to-tick. snapshot_interp_remotes
     * rigid-translates the whole body to put pelvis on the lerp
     * target — but the *shape* (bone offsets) was whatever the
     * physics pass left it, so the per-frame render saw the body
     * wobble around its smooth pelvis path. That was the residual
     * jitter reported after Phase 2's interp-delay restoration.
     *
     * Post-fix: zero inv_mass for every non-local mech particle on
     * the client. Verlet (lines 48, 76) and the constraint solver
     * (solve_distance / solve_distance_limit / solve_fixed_anchor)
     * all early-return on `inv_mass <= 0`. Direct kinematic writes
     * (physics_translate_kinematic / physics_set_velocity_x|y) are
     * unaffected, so snapshot_interp_remotes still moves the body.
     * The body stays in whatever pose mech_create initialized it to
     * (the chassis rest pose); pelvis path is the interp lerp;
     * bones rigid-translate with the pelvis — no shape drift, no
     * jitter.
     *
     * Trade-off: remote mechs render in rest pose only — no walk
     * cycle, no aim-arm tracking, no ragdoll on death. Walk/aim
     * animation should come from a procedural pose pass driven by
     * snapshot state (a follow-on); ragdoll wants per-limb wire
     * data we don't ship yet. Cost-of-change here is one loop;
     * benefit is smooth remote motion at WAN ping. (See
     * TRADE_OFFS.md: "Remote mechs render in rest pose on the
     * client".) */
    if (!w->authoritative) {
        ParticlePool *pp = &w->particles;
        int local_id = w->local_mech_id;
        for (int i = 0; i < w->mech_count; ++i) {
            if (i == local_id) continue;
            const Mech *m = &w->mechs[i];
            for (int part = 0; part < PART_COUNT; ++part) {
                pp->inv_mass[m->particle_base + part] = 0.0f;
            }
        }
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

    /* P12 — Hit-flash decay. Runs unconditionally (auth + non-auth) so
     * the local client's flash decays in lockstep with the server's
     * authoritative side. The kick is set by `mech_apply_damage`
     * (server) or by the snapshot apply path on remote-mech damage
     * (deferred until P14 if hit-flash desyncs become noticeable —
     * for now, the local client's hit-flash on a remote mech only
     * fires once the next snapshot delivers HP delta, which is
     * one snapshot late. Acceptable.). */
    for (int i = 0; i < w->mech_count; ++i) {
        Mech *m = &w->mechs[i];
        if (m->hit_flash_timer > 0.0f) {
            m->hit_flash_timer -= dt;
            if (m->hit_flash_timer < 0.0f) m->hit_flash_timer = 0.0f;
        }
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
     * frame. Skip-foot translation preserves the chain. wan-fixes-3 —
     * on the client, only run the anchor for the local (predicted)
     * mech. Remote mechs are kinematic (inv_mass=0) and positioned
     * by snapshot_interp_remotes; an extra anchor pull here would
     * shift body parts away from their rest-pose offsets before the
     * interp translate runs. */
    for (int i = 0; i < w->mech_count; ++i) {
        if (!w->authoritative && i != w->local_mech_id) continue;
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

    /* P05: pickup state machine — touch detection + cooldown rollover +
     * transient lifetime expiry. Server-only; broadcasts state changes
     * via the pickupfeed queue (drained in main.c). */
    pickup_step(w, dt);

    /* Projectiles — integrate, collide, detonate. Runs after the mech
     * physics pass so projectiles see the just-settled bone positions
     * (matters for hit attribution on a shot that lands on the same
     * tick the target finished a jump). */
    projectile_step(w, dt);

    /* FX particles (blood, sparks, tracers). */
    fx_update(w, dt);

    /* P12 — Smoke from heavily damaged limbs. Tick-gated to every 8th
     * tick (~7.5 Hz cap), with a square-of-deficit RNG roll so light
     * damage is essentially silent and near-death is a continuous
     * plume. The five tracked limbs map back to their joint particles
     * (PART_*_ELBOW / _KNEE / HEAD) so the puff originates at the
     * visibly-damaged region of the limb sprite. Server-only —
     * clients see HP via snapshots and run the same threshold rule
     * locally so smoke renders identically without an extra wire
     * message. */
    if ((w->tick % 8u) == 0u) {
        for (int mi = 0; mi < w->mech_count; ++mi) {
            Mech *m = &w->mechs[mi];
            if (!m->alive) continue;
            const struct { float hp; float max; int part; } limbs[5] = {
                { m->hp_arm_l, 80.0f, PART_L_ELBOW },
                { m->hp_arm_r, 80.0f, PART_R_ELBOW },
                { m->hp_leg_l, 80.0f, PART_L_KNEE  },
                { m->hp_leg_r, 80.0f, PART_R_KNEE  },
                { m->hp_head,  50.0f, PART_HEAD    },
            };
            for (int k = 0; k < 5; ++k) {
                float frac = limbs[k].hp / limbs[k].max;
                if (frac >= 0.30f) continue;
                float intensity = (0.30f - frac) / 0.30f;
                intensity *= intensity;
                if (pcg32_float01(w->rng) > intensity) continue;
                int idx = (int)m->particle_base + limbs[k].part;
                Vec2 src = (Vec2){
                    w->particles.pos_x[idx],
                    w->particles.pos_y[idx],
                };
                Vec2 vel = {
                    (pcg32_float01(w->rng) - 0.5f) * 30.0f,
                    -20.0f - pcg32_float01(w->rng) * 30.0f,
                };
                fx_spawn_smoke(&w->fx, src, vel, w->rng);
            }
        }
    }

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
