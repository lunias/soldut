#include "projectile.h"

#include "audio.h"
#include "level.h"
#include "log.h"
#include "mech.h"
#include "particle.h"
#include "snapshot.h"
#include "weapons.h"

#include <math.h>
#include <string.h>

void projectile_pool_init(ProjectilePool *p) {
    memset(p, 0, sizeof(*p));
}

void projectile_pool_clear(ProjectilePool *p) {
    p->count = 0;
    memset(p->alive, 0, sizeof(p->alive));
}

static int proj_alloc(ProjectilePool *p) {
    /* Reuse a dead slot first. */
    for (int i = 0; i < p->count; ++i) {
        if (!p->alive[i]) return i;
    }
    if (p->count < PROJECTILE_CAPACITY) return p->count++;
    /* Pool full — overwrite slot 0 (oldest). Rare in practice. */
    LOG_W("projectile pool full, overwriting slot 0");
    return 0;
}

int projectile_spawn(World *w, ProjectileSpawn s) {
    ProjectilePool *p = &w->projectiles;
    int i = proj_alloc(p);
    if (i < 0) return -1;
    p->kind          [i] = (uint8_t)s.kind;
    p->weapon_id     [i] = (int8_t)s.weapon_id;
    p->owner_mech    [i] = (int16_t)s.owner_mech_id;
    p->owner_team    [i] = (int8_t)s.owner_team;
    p->pos_x         [i] = s.origin.x;
    p->pos_y         [i] = s.origin.y;
    /* P03: seed render_prev to spawn pos so the first rendered frame
     * doesn't lerp from (0,0) → spawn. */
    p->render_prev_x [i] = s.origin.x;
    p->render_prev_y [i] = s.origin.y;
    p->vel_x         [i] = s.velocity.x;
    p->vel_y         [i] = s.velocity.y;
    p->life          [i] = s.life;
    p->damage        [i] = s.damage;
    p->aoe_radius    [i] = s.aoe_radius;
    p->aoe_damage    [i] = s.aoe_damage;
    p->aoe_impulse   [i] = s.aoe_impulse;
    p->gravity_scale [i] = s.gravity_scale;
    p->drag          [i] = s.drag;
    p->bouncy        [i] = (uint8_t)(s.bouncy ? 1 : 0);
    p->exploded      [i] = 0;
    p->alive         [i] = 1;
    return i;
}

/* Bone segments to test projectiles against. Same set as the hitscan
 * weapon; kept duplicated here so projectile.c doesn't need to depend
 * on weapons.c just to share a 15-entry table. */
static const struct { int parent, child, part_for_damage; } g_bones[] = {
    { PART_NECK,        PART_HEAD,        PART_HEAD       },
    { PART_CHEST,       PART_NECK,        PART_NECK       },
    { PART_PELVIS,      PART_CHEST,       PART_CHEST      },
    { PART_CHEST,       PART_L_SHOULDER,  PART_L_SHOULDER },
    { PART_CHEST,       PART_R_SHOULDER,  PART_R_SHOULDER },
    { PART_L_SHOULDER,  PART_L_ELBOW,     PART_L_ELBOW    },
    { PART_L_ELBOW,     PART_L_HAND,      PART_L_HAND     },
    { PART_R_SHOULDER,  PART_R_ELBOW,     PART_R_ELBOW    },
    { PART_R_ELBOW,     PART_R_HAND,      PART_R_HAND     },
    { PART_PELVIS,      PART_L_HIP,       PART_L_HIP      },
    { PART_PELVIS,      PART_R_HIP,       PART_R_HIP      },
    { PART_L_HIP,       PART_L_KNEE,      PART_L_KNEE     },
    { PART_L_KNEE,      PART_L_FOOT,      PART_L_FOOT     },
    { PART_R_HIP,       PART_R_KNEE,      PART_R_KNEE     },
    { PART_R_KNEE,      PART_R_FOOT,      PART_R_FOOT     },
};
#define NUM_BONES ((int)(sizeof(g_bones) / sizeof(g_bones[0])))

/* Closest-distance test from segment [a→b] (one tick of projectile
 * motion) to a bone segment [p1→p2], with the projectile and bone both
 * treated as zero-radius lines. We accept a hit if the minimum distance
 * is within (proj_radius + bone_radius). Returns the parametric t along
 * [a,b] of the closest approach, or -1 if no hit. */
static float swept_seg_vs_bone(Vec2 a, Vec2 b, Vec2 p1, Vec2 p2, float r) {
    /* Cheap conservative: sample 8 points along the projectile motion,
     * for each, find the closest point on the bone, and check distance.
     * This mirrors weapons.c::ray_seg_hit's discipline of "good enough
     * for M1, refactor when it bites". */
    const int N = 8;
    float best_t = -1.0f;
    Vec2 ab = { b.x - a.x, b.y - a.y };
    Vec2 pq = { p2.x - p1.x, p2.y - p1.y };
    float pq_len2 = pq.x * pq.x + pq.y * pq.y;
    if (pq_len2 < 1e-6f) pq_len2 = 1e-6f;
    for (int i = 0; i <= N; ++i) {
        float t = (float)i / (float)N;
        Vec2 sp = { a.x + ab.x * t, a.y + ab.y * t };
        Vec2 d  = { sp.x - p1.x, sp.y - p1.y };
        float u = (d.x * pq.x + d.y * pq.y) / pq_len2;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
        Vec2 cp = { p1.x + pq.x * u, p1.y + pq.y * u };
        float dx = sp.x - cp.x, dy = sp.y - cp.y;
        if (dx * dx + dy * dy <= r * r) {
            if (best_t < 0.0f || t < best_t) best_t = t;
        }
    }
    return best_t;
}

static void detonate(World *w, int i) {
    ProjectilePool *p = &w->projectiles;
    if (p->exploded[i]) return;
    p->exploded[i] = 1;
    /* M6 ship-prep — keep the grenade ALIVE for one more render frame
     * after detonation so the player sees the grenade sprite AT the
     * explosion center, with the spark/smoke FX fanning out from the
     * same point. Pre-fix the projectile was marked dead the same tick
     * the explosion spawned, leaving a few render frames where the
     * sprite was already gone but the particles were still expanding —
     * read as "grenade disappeared, then a delayed explosion appeared
     * somewhere else" (because the sparks center is the only spatial
     * cue, and it lerps with alpha against an already-dead source).
     *
     * Snap render_prev to pos so the next frame's lerp(prev, pos,
     * alpha) collapses to pos regardless of alpha — grenade renders
     * AT the explosion center, no offset. The kill happens at the top
     * of projectile_step on the following tick (exploded && alive →
     * alive = 0). */
    p->render_prev_x[i] = p->pos_x[i];
    p->render_prev_y[i] = p->pos_y[i];

    /* M6 ship-prep — camera linger record. Set HERE (not only inside
     * explosion_spawn) so the client-side path also gets it. On a
     * client, bouncy AOE projectiles skip the predicted-explosion
     * branch below (per the wan-fixes-10 comment: bouncy + fuse paths
     * diverge from the server by hundreds of pixels). That left a
     * ~RTT/2 window where the grenade was dead, the server's
     * NET_MSG_EXPLOSION hadn't arrived yet, and the camera had no
     * focus point — so it snapped back to the mech, then SNAPPED again
     * to the explosion when the wire event arrived, then snapped back
     * to the mech when linger expired. Setting last_explosion_pos at
     * detonate gives the camera a continuous focus point from the
     * moment the grenade fizzles. */
    if (p->aoe_radius[i] > 0.0f) {
        w->last_explosion_pos = (Vec2){ p->pos_x[i], p->pos_y[i] };
        w->last_explosion_tick = w->tick;
        w->last_explosion_owner_mech = (int)p->owner_mech[i];
    }
    /* wan-fixes-10 (server) / wan-fixes-21 (client) — explosion visual
     * placement.
     *
     * SERVER: spawn the authoritative explosion (damage + impulse +
     * sparks/smoke/sfx). NET_MSG_EXPLOSION is also broadcast from
     * `explosion_spawn` so every client receives the canonical pos
     * (`src/net.c::net_server_broadcast_explosion`).
     *
     * CLIENT: predict the visual locally if the server's
     * NET_MSG_EXPLOSION hasn't arrived yet — wan-fixes-10 used to
     * skip the client visual entirely, which delayed all AOE
     * feedback by 1× RTT (~86 ms WAN). For slow projectiles like
     * Mass Driver this caused "looks like a miss" → "actually a
     * hit" surprise. `explosion_spawn`'s authoritative gate keeps
     * the damage loop a no-op on the client so the prediction is
     * visual-only. The opposite-source lookup against the
     * explosion record ring de-dupes against the server's later
     * EXPLOSION arrival. If the server's event ARRIVED FIRST
     * (rare but possible) we skip the prediction and let the
     * server's visual stand. */
    if (p->aoe_radius[i] > 0.0f) {
        if (w->authoritative) {
            explosion_spawn(w,
                (Vec2){ p->pos_x[i], p->pos_y[i] },
                p->aoe_radius[i],
                p->aoe_damage[i],
                p->aoe_impulse[i],
                (int)p->owner_mech[i], (int)p->owner_team[i],
                (int)p->weapon_id[i]);
        } else {
            /* M6 ship-prep — predict for ALL client-side AOE
             * projectiles, including bouncy frag grenades. Pre-fix
             * (wan-fixes-10) skipped bouncy because their sim diverges
             * across the fuse and "prediction would land in the wrong
             * spot." The cost of skipping turned out to be worse: the
             * client's grenade dies silently, then ~RTT/2 later the
             * server's NET_MSG_EXPLOSION arrives and the FX appears
             * somewhere else than where the visible grenade was —
             * read as "the grenade exploded in the wrong place," and
             * the camera linger jumped from LOCAL_POS to SERVER_POS
             * mid-pan.
             *
             * With prediction on: client visual is continuous (FX
             * spawns AT the grenade), and the wire-arrival path
             * (net.c::client_handle_explosion) finds the PREDICTED
             * record and dedupes — no double FX, no last_explosion_pos
             * overwrite. Dedupe window bumped to 120 ticks (2 s) so
             * bouncy fuses (1.5 s) + bounce-divergence comfortably
             * fit inside it. Damage still server-authoritative (the
             * authoritative branch above runs damage). */
            ExplosionRecord *server_already = explosion_record_find_consume(
                w, (int)p->owner_mech[i], (int)p->weapon_id[i],
                EXPL_SRC_SERVER, /*max_age_ticks*/600);
            if (server_already) {
                /* Server's EXPLOSION already showed the visual.
                 * Consume the record and die silently. */
                server_already->valid = false;
            } else {
                Vec2 pred = { p->pos_x[i], p->pos_y[i] };
                explosion_spawn(w, pred,
                    p->aoe_radius[i],
                    p->aoe_damage[i],
                    p->aoe_impulse[i],
                    (int)p->owner_mech[i], (int)p->owner_team[i],
                    (int)p->weapon_id[i]);
                explosion_record_push(w, pred,
                    (int)p->owner_mech[i], (int)p->weapon_id[i],
                    EXPL_SRC_PREDICTED);
            }
        }
    }
    /* alive stays 1 for one more render frame — see comment at top of
     * detonate. The kill happens at the top of projectile_step next
     * tick on the `exploded && alive` check. */
}

void projectile_step(World *w, float dt) {
    ProjectilePool *p = &w->projectiles;
    if (p->count == 0) return;

    Vec2 g = w->level.gravity;
    int last_alive = -1;

    for (int i = 0; i < p->count; ++i) {
        if (!p->alive[i]) continue;

        /* M6 ship-prep — second-stage kill for projectiles that
         * detonated on the previous tick. detonate() now keeps
         * `alive = 1` for one extra render frame so the sprite reads
         * AT the explosion center; consume that ghost frame here. */
        if (p->exploded[i]) {
            p->alive[i] = 0;
            continue;
        }

        /* Fuse / lifetime. Reaching zero detonates AOE projectiles
         * (frag grenades, mass driver duds), expires non-AOE ones.
         *
         * P06: a PROJ_GRAPPLE_HEAD that runs out its lifetime without
         * landing resets the firer's grapple state to IDLE so the next
         * fire press can re-fire. */
        p->life[i] -= dt;
        if (p->life[i] <= 0.0f) {
            if (p->kind[i] == PROJ_GRAPPLE_HEAD && w->authoritative) {
                int fmid = (int)p->owner_mech[i];
                if (fmid >= 0 && fmid < w->mech_count) {
                    Mech *firer = &w->mechs[fmid];
                    if (firer->grapple.state == GRAPPLE_FLYING) {
                        firer->grapple.state = GRAPPLE_IDLE;
                        firer->grapple.constraint_idx = -1;
                        SHOT_LOG("t=%llu mech=%d grapple_miss",
                                 (unsigned long long)w->tick, fmid);
                    }
                }
                p->alive[i] = 0;
                continue;
            }
            if (p->aoe_radius[i] > 0.0f) detonate(w, i);
            else                          p->alive[i] = 0;
            continue;
        }

        /* Drag (per-second exponential). */
        if (p->drag[i] > 0.0f) {
            float k = expf(-p->drag[i] * dt);
            p->vel_x[i] *= k;
            p->vel_y[i] *= k;
        }

        /* Gravity. */
        p->vel_x[i] += g.x * p->gravity_scale[i] * dt;
        p->vel_y[i] += g.y * p->gravity_scale[i] * dt;

        Vec2 a = { p->pos_x[i], p->pos_y[i] };
        Vec2 b = { a.x + p->vel_x[i] * dt, a.y + p->vel_y[i] * dt };

        /* Tile collision via DDA ray. */
        float t_wall;
        bool hit_wall = level_ray_hits(&w->level, a, b, &t_wall);

        /* Bone collision against every other mech. The lower of the two
         * hit times wins; AOE projectiles detonate at the contact, non-
         * AOE ones apply damage to that body part. */
        float t_hit = hit_wall ? t_wall : 1.0f;
        int   hit_mech = -1;
        int   hit_part = -1;

        /* Phase 4 — Grapple head lag-comp. For PROJ_GRAPPLE_HEAD only
         * (per the matching TRADE_OFFS entry "Grapple anchor uses
         * server-current position"), pre-load each target mech's bone
         * positions from snapshot_lag_lookup at the firer's input view
         * tick. Other projectiles intentionally remain current-time —
         * slower projectile travel-time is part of the gameplay and
         * Source's standard stance is to lag-comp hitscan only.
         *
         * Without this, a client firing the hook at a moving target
         * regularly missed at WAN ping (~50–150 ms RTT) because the
         * server tested the head's swept segment against where the
         * target IS, not where the firer's screen showed it. With it,
         * the swept test runs against the rewound bones; if it hits,
         * we still attach the constraint to the bone's *current*
         * particle (rope visually snaps over ~1 frame as the
         * constraint solver pulls the firer toward the new anchor —
         * acceptable given the alternative is missing). */
        bool lag_comp_used = false;
        float lc_bx[PART_COUNT], lc_by[PART_COUNT];
        uint64_t lc_view_tick = 0;
        if (p->kind[i] == PROJ_GRAPPLE_HEAD) {
            int fmid = (int)p->owner_mech[i];
            if (fmid >= 0 && fmid < w->mech_count) {
                lc_view_tick = w->mechs[fmid].input_view_tick;
            }
        }

        for (int mi = 0; mi < w->mech_count; ++mi) {
            const Mech *m = &w->mechs[mi];
            if (!m->alive) continue;
            if (mi == p->owner_mech[i]) continue;   /* don't hit yourself */

            /* For PROJ_GRAPPLE_HEAD with a remote firer, fetch this
             * target's rewound bone snapshot once per mech. If the
             * lookup fails (no history covering view_tick), fall back
             * to current positions via the per-bone read below. */
            const float *bx_lookup = NULL;
            const float *by_lookup = NULL;
            if (lc_view_tick > 0) {
                if (snapshot_lag_lookup(w, mi, lc_view_tick, lc_bx, lc_by)) {
                    bx_lookup = lc_bx;
                    by_lookup = lc_by;
                    lag_comp_used = true;
                }
            }

            /* Bone-collision radius. Bullets stay tight (8 px) but
             * frag grenades use a wider radius — the sprite itself
             * is ~7 px wide and the player expects a grenade that
             * VISUALLY touches a mech to detonate, even when rolling
             * along the floor next to a foot or grazing a hand. 18 px
             * gives a generous bubble that captures sprite-edge
             * contact AND the typical mech-bone motion (walk / pose
             * anim can move a bone 5-10 px/tick — bumping the radius
             * is cheaper than a true moving-vs-moving CCD). */
            float bone_r = (p->kind[i] == PROJ_FRAG_GRENADE) ? 18.0f : 8.0f;
            for (int bi = 0; bi < NUM_BONES; ++bi) {
                int pa = m->particle_base + g_bones[bi].parent;
                int pb = m->particle_base + g_bones[bi].child;
                Vec2 va, vb;
                if (bx_lookup) {
                    va = (Vec2){ bx_lookup[g_bones[bi].parent],
                                 by_lookup[g_bones[bi].parent] };
                    vb = (Vec2){ bx_lookup[g_bones[bi].child],
                                 by_lookup[g_bones[bi].child]  };
                } else {
                    va = (Vec2){ w->particles.pos_x[pa], w->particles.pos_y[pa] };
                    vb = (Vec2){ w->particles.pos_x[pb], w->particles.pos_y[pb] };
                }
                /* Test against the END-of-tick bone position. */
                float th = swept_seg_vs_bone(a, b, va, vb, bone_r);

                /* Also test against the START-of-tick bone position
                 * (render_prev). When the mech is walking / animating
                 * the bones can sweep ~10 px per tick. The static
                 * end-of-tick test alone misses contacts where the
                 * bone SWEPT THROUGH the projectile during the tick
                 * — taking the closer-t hit between the two static
                 * tests is a cheap proxy for moving-vs-moving CCD. */
                if (!bx_lookup) {
                    Vec2 va_p = (Vec2){ w->particles.render_prev_x[pa],
                                        w->particles.render_prev_y[pa] };
                    Vec2 vb_p = (Vec2){ w->particles.render_prev_x[pb],
                                        w->particles.render_prev_y[pb] };
                    float th_prev = swept_seg_vs_bone(a, b, va_p, vb_p, bone_r);
                    if (th_prev >= 0.0f && (th < 0.0f || th_prev < th)) {
                        th = th_prev;
                    }
                }

                if (th < 0.0f) continue;
                if (th < t_hit) {
                    t_hit = th;
                    hit_mech = mi;
                    hit_part = g_bones[bi].part_for_damage;
                }
            }
        }
        (void)lag_comp_used; /* surfaced via SHOT_LOG at attach time below */

        bool any_hit = (hit_mech >= 0) || hit_wall;

        /* P06 — Grapple head: on hit, stick. No damage, no AOE. The
         * firer's grapple state transitions FLYING → ATTACHED and a
         * constraint is allocated by mech_grapple_attach. Tile hits
         * use CSTR_FIXED_ANCHOR; bone hits use CSTR_DISTANCE between
         * firer pelvis and target bone particle. Server-only — clients
         * mirror the state via SNAP_STATE_GRAPPLING + grapple suffix. */
        if (p->kind[i] == PROJ_GRAPPLE_HEAD) {
            if (any_hit) {
                /* Land at the clamped point. */
                p->pos_x[i] = a.x + (b.x - a.x) * t_hit;
                p->pos_y[i] = a.y + (b.y - a.y) * t_hit;
                if (w->authoritative) {
                    int fmid = (int)p->owner_mech[i];
                    if (fmid >= 0 && fmid < w->mech_count) {
                        Mech *firer = &w->mechs[fmid];
                        Vec2 anchor = (Vec2){ p->pos_x[i], p->pos_y[i] };
                        /* Pull anchor 4 px back along the flight
                         * direction so floating-point error doesn't
                         * leave it embedded in a SOLID tile. */
                        if (hit_mech < 0 && hit_wall) {
                            float dx = b.x - a.x, dy = b.y - a.y;
                            float vlen = sqrtf(dx*dx + dy*dy);
                            if (vlen > 1e-3f) {
                                anchor.x -= dx / vlen * 4.0f;
                                anchor.y -= dy / vlen * 4.0f;
                            }
                        }
                        firer->grapple.state       = GRAPPLE_ATTACHED;
                        firer->grapple.anchor_pos  = anchor;
                        firer->grapple.anchor_mech = (int8_t)((hit_mech >= 0) ? hit_mech : -1);
                        firer->grapple.anchor_part = (uint8_t)((hit_mech >= 0) ? hit_part : 0);
                        int pelv = firer->particle_base + PART_PELVIS;
                        float pdx = w->particles.pos_x[pelv] - anchor.x;
                        float pdy = w->particles.pos_y[pelv] - anchor.y;
                        float L = sqrtf(pdx*pdx + pdy*pdy);
                        /* Clamp the initial rope length to a swingable
                         * radius. If the firer hit something far away,
                         * the rope is shorter than the hit distance —
                         * meaning the body is "outside" the rope at
                         * attach time, so the constraint pulls it in
                         * to GRAPPLE_MAX_REST_LEN over the next few
                         * iterations and they swing on a tight
                         * pendulum. Without this cap a 600-px rope
                         * fired across the screen left players
                         * dangling on a line they couldn't actually
                         * swing on. The W-retract path can shorten
                         * further down to the 60 px floor. */
                        const float GRAPPLE_MAX_REST_LEN = 300.0f;
                        const float GRAPPLE_INIT_MIN_LEN =  80.0f;
                        if (L > GRAPPLE_MAX_REST_LEN) L = GRAPPLE_MAX_REST_LEN;
                        if (L < GRAPPLE_INIT_MIN_LEN) L = GRAPPLE_INIT_MIN_LEN;
                        firer->grapple.rest_length = L;
                        mech_grapple_attach(w, fmid);
                        SHOT_LOG("t=%llu mech=%d grapple_attach anchor=(%.1f,%.1f) tgt_mech=%d part=%d L=%.1f lag_comp=%llu",
                                 (unsigned long long)w->tick, fmid,
                                 anchor.x, anchor.y,
                                 (int)firer->grapple.anchor_mech,
                                 (int)firer->grapple.anchor_part, L,
                                 (unsigned long long)(lag_comp_used ? lc_view_tick : 0u));
                    }
                }
                /* Tiny visual sparks at landing point on both sides. */
                Vec2 hp = { p->pos_x[i], p->pos_y[i] };
                for (int k = 0; k < 3; ++k) {
                    fx_spawn_spark(&w->fx, hp,
                        (Vec2){ -p->vel_x[i] * 0.05f,
                                -p->vel_y[i] * 0.05f }, w->rng);
                }
                /* Audio runs on both host + client (alive=0 path is
                 * unconditional per the P09 sync fix), so the firer
                 * hears the anchor land regardless of role. */
                audio_play_at(SFX_GRAPPLE_HIT, hp);
                p->alive[i] = 0;
                continue;
            }
            /* No hit yet — advance and keep flying. */
            p->pos_x[i] = b.x;
            p->pos_y[i] = b.y;
            if (i > last_alive) last_alive = i;
            continue;
        }

        if (any_hit) {
            /* Land at the clamped point. */
            p->pos_x[i] = a.x + (b.x - a.x) * t_hit;
            p->pos_y[i] = a.y + (b.y - a.y) * t_hit;

            SHOT_LOG("t=%llu proj=%d kind=%d hit owner=%d authoritative=%d at=(%.2f,%.2f) "
                     "wall=%d mech=%d part=%d bouncy=%d vel=(%.1f,%.1f)",
                     (unsigned long long)w->tick, i, (int)p->kind[i],
                     (int)p->owner_mech[i], (int)w->authoritative,
                     p->pos_x[i], p->pos_y[i],
                     (int)hit_wall, hit_mech, hit_part,
                     (int)p->bouncy[i], p->vel_x[i], p->vel_y[i]);

            /* Bouncy projectiles (frag grenades) ricochet off tiles
             * instead of detonating; they keep their fuse and continue
             * to integrate. Direct mech hits (hit_mech >= 0) bypass
             * this branch — those fall through to the detonate path
             * below, so a grenade that bounces and THEN strikes a mech
             * on a later tick also detonates immediately. */
            if (hit_wall && hit_mech < 0 && p->bouncy[i]) {
                /* Reflect velocity. Estimate normal by the dominant
                 * axis at the moment of contact (cheap approximation
                 * for tile-aligned maps). Damping factors are tuned
                 * for "skipping rock" feel — 0.70 perpendicular (30 %
                 * absorbed per bounce so a max-charge throw skips 3-4
                 * times across a flat floor), 0.90 parallel (10 %
                 * rolling friction so it keeps drifting on the
                 * follow-through). */
                if (fabsf(p->vel_x[i]) > fabsf(p->vel_y[i])) {
                    p->vel_x[i] *= -0.70f;
                    p->vel_y[i] *=  0.90f;
                } else {
                    p->vel_x[i] *=  0.90f;
                    p->vel_y[i] *= -0.70f;
                }
                /* Pull pos a hair away from the wall so the next tick
                 * doesn't immediately re-collide. */
                if (p->vel_x[i] != 0.0f) p->pos_x[i] += sign_f(p->vel_x[i]) * 1.5f;
                if (p->vel_y[i] != 0.0f) p->pos_y[i] += sign_f(p->vel_y[i]) * 1.5f;
                /* Tiny spark on bounce. */
                fx_spawn_spark(&w->fx,
                    (Vec2){ p->pos_x[i], p->pos_y[i] },
                    (Vec2){ p->vel_x[i] * -1.0f, p->vel_y[i] * -1.0f },
                    w->rng);

                /* Settled-detonate — if the bounce left the grenade
                 * crawling (|v| < FRAG_SETTLED_VMAG_PXS), fire NOW
                 * instead of letting it sit and wait for the fuse.
                 * Avoids the "grenade disappears, then explodes after
                 * a pause" feel: as soon as energy is gone, BOOM. */
                float vmag2 = p->vel_x[i] * p->vel_x[i] +
                              p->vel_y[i] * p->vel_y[i];
                if (vmag2 < FRAG_SETTLED_VMAG_PXS * FRAG_SETTLED_VMAG_PXS) {
                    if (p->aoe_radius[i] > 0.0f) detonate(w, i);
                    else                          p->alive[i] = 0;
                    continue;
                }

                /* Don't detonate / die; keep fuse running. */
                if (i > last_alive) last_alive = i;
                continue;
            }

            if (hit_mech >= 0) {
                /* Direct hit on a mech. */
                Vec2 dir;
                float vlen = sqrtf(p->vel_x[i]*p->vel_x[i] +
                                   p->vel_y[i]*p->vel_y[i]);
                if (vlen > 1e-3f) {
                    dir = (Vec2){ p->vel_x[i] / vlen, p->vel_y[i] / vlen };
                } else {
                    dir = (Vec2){ 1.0f, 0.0f };
                }
                if (w->authoritative) {
                    /* Track for kill-feed weapon attribution. */
                    w->mechs[hit_mech].last_killshot_weapon = (int)p->weapon_id[i];
                    mech_apply_damage(w, hit_mech, hit_part,
                                      p->damage[i], dir,
                                      (int)p->owner_mech[i]);
                }
                /* Visual feedback even on the client (spark + tracer
                 * end). */
                Vec2 hp = { p->pos_x[i], p->pos_y[i] };
                for (int k = 0; k < 4; ++k) {
                    fx_spawn_spark(&w->fx, hp, dir, w->rng);
                }
            } else if (hit_wall) {
                /* Wall hit — sparks and a small puff. */
                Vec2 hp = { p->pos_x[i], p->pos_y[i] };
                for (int k = 0; k < 5; ++k) {
                    fx_spawn_spark(&w->fx, hp,
                        (Vec2){ -p->vel_x[i] * 0.1f,
                                -p->vel_y[i] * 0.1f }, w->rng);
                }
            }

            /* Detonate if AOE; otherwise consume the projectile. */
            if (p->aoe_radius[i] > 0.0f) detonate(w, i);
            else                          p->alive[i] = 0;
            continue;
        }

        /* No collision this tick — advance. */
        p->pos_x[i] = b.x;
        p->pos_y[i] = b.y;
        if (i > last_alive) last_alive = i;
    }

    /* Trim count down to highest alive slot. */
    p->count = last_alive + 1;
}

/* ---- Rendering ---------------------------------------------------- */

static Color proj_color(uint8_t kind) {
    switch ((ProjectileKind)kind) {
        case PROJ_PLASMA_BOLT:      return (Color){ 100, 220, 255, 230 };
        case PROJ_PLASMA_ORB:       return (Color){  90, 255, 200, 240 };
        case PROJ_PELLET:           return (Color){ 255, 220, 140, 220 };
        case PROJ_RIFLE_SLUG:       return (Color){ 255, 240, 200, 230 };
        case PROJ_MICROGUN_BULLET:  return (Color){ 255, 220, 180, 220 };
        case PROJ_ROCKET:           return (Color){ 255, 180,  80, 240 };
        case PROJ_MICRO_ROCKET:     return (Color){ 255, 200, 100, 240 };
        case PROJ_FRAG_GRENADE:     return (Color){ 200, 200, 100, 230 };
        case PROJ_THROWN_KNIFE:     return (Color){ 220, 230, 240, 230 };
        case PROJ_GRAPPLE_HEAD:     return (Color){ 240, 220, 100, 240 };
        default:                    return (Color){ 255, 255, 255, 220 };
    }
}

static float proj_size(uint8_t kind) {
    switch ((ProjectileKind)kind) {
        case PROJ_ROCKET:           return 5.5f;
        case PROJ_MICRO_ROCKET:     return 3.5f;
        case PROJ_PLASMA_ORB:       return 5.0f;
        case PROJ_FRAG_GRENADE:     return 4.0f;
        case PROJ_THROWN_KNIFE:     return 3.0f;
        case PROJ_PELLET:           return 1.6f;
        case PROJ_GRAPPLE_HEAD:     return 3.0f;
        default:                    return 2.2f;
    }
}

int projectile_find_grapple_head(const ProjectilePool *p, int owner_mech_id) {
    for (int i = 0; i < p->count; ++i) {
        if (!p->alive[i]) continue;
        if (p->kind[i] != PROJ_GRAPPLE_HEAD) continue;
        if (p->owner_mech[i] != owner_mech_id) continue;
        return i;
    }
    return -1;
}

/* M6 ship-prep — frag grenade gets its own multi-element sprite:
 * dark olive body with a darker outline, a metal lever cap on top
 * (oriented opposite to the velocity vector so it reads like it's
 * spinning with the throw), and a pulsing red fuse spark for
 * "the fuse is lit, this thing is about to blow." Drawn instead of
 * the generic colored circle so a frag projectile reads as a
 * GRENADE at a glance — the previous 4-px yellow ball was easy to
 * lose against the level art. */
static void draw_frag_grenade_sprite(Vec2 pos, float vx, float vy, double time_s) {
    /* Body — dark outline + olive fill + a small highlight to imply
     * volume (light source upper-left). */
    DrawCircleV((Vector2){ pos.x, pos.y }, 7.0f, (Color){ 18, 26, 18, 230 });
    DrawCircleV((Vector2){ pos.x, pos.y }, 6.0f, (Color){ 78, 108, 60, 240 });
    DrawCircleV((Vector2){ pos.x - 1.6f, pos.y - 1.6f }, 2.2f,
                (Color){ 150, 175, 110, 220 });

    /* Lever cap — small dark rectangle on the side OPPOSITE the
     * velocity. As the grenade tumbles through its arc the cap
     * orbits the body, reading as rotation without the cost of a
     * sprite atlas + per-grenade angular-velocity state. */
    float vlen = sqrtf(vx * vx + vy * vy);
    float cx, cy;
    if (vlen > 1e-3f) {
        cx = -vx / vlen;   /* opposite to velocity */
        cy = -vy / vlen;
    } else {
        cx = 0.0f;
        cy = -1.0f;        /* still grenade: cap on top */
    }
    Vec2 cap_center = { pos.x + cx * 5.5f, pos.y + cy * 5.5f };
    DrawCircleV((Vector2){ cap_center.x, cap_center.y }, 2.2f,
                (Color){ 40, 45, 35, 240 });
    DrawCircleV((Vector2){ cap_center.x, cap_center.y }, 1.4f,
                (Color){ 110, 110, 100, 220 });

    /* Pulsing fuse spark — bright red dot at the cap location,
     * oscillates at ~6 Hz so the eye can track the grenade against
     * busy backgrounds. Spawn a tiny halo behind it for emphasis. */
    float pulse = 0.55f + 0.45f * sinf((float)time_s * 12.0f);
    uint8_t spark_r = (uint8_t)(220 + 35 * pulse);
    uint8_t spark_g = (uint8_t)(60 + 80 * pulse);
    Color spark = (Color){ spark_r, spark_g, 30, 240 };
    Vec2 spark_pos = { pos.x + cx * 7.5f, pos.y + cy * 7.5f };
    DrawCircleV((Vector2){ spark_pos.x, spark_pos.y },
                1.5f + 1.2f * pulse,
                (Color){ 255, 200, 40, (uint8_t)(120 * pulse) });
    DrawCircleV((Vector2){ spark_pos.x, spark_pos.y }, 1.2f, spark);
}

void projectile_draw(const ProjectilePool *p, float alpha) {
    double now = GetTime();
    for (int i = 0; i < p->count; ++i) {
        if (!p->alive[i]) continue;
        /* P03: lerp between start-of-tick pos and latest physics pos. */
        Vec2 pos = {
            p->render_prev_x[i] + (p->pos_x[i] - p->render_prev_x[i]) * alpha,
            p->render_prev_y[i] + (p->pos_y[i] - p->render_prev_y[i]) * alpha,
        };

        /* Frag grenades get a bespoke sprite — bigger + grenade-shaped
         * so the player can actually see them mid-throw. */
        if (p->kind[i] == PROJ_FRAG_GRENADE) {
            draw_frag_grenade_sprite(pos, p->vel_x[i], p->vel_y[i], now);
            continue;
        }

        Color c = proj_color(p->kind[i]);
        float sz = proj_size(p->kind[i]);
        DrawCircleV(pos, sz, c);
        /* Trailing line for fast projectiles so they read at speed. */
        float vx = p->vel_x[i], vy = p->vel_y[i];
        float vlen = sqrtf(vx * vx + vy * vy);
        if (vlen > 200.0f) {
            float trail = 14.0f;
            Vec2 tail = { pos.x - vx / vlen * trail,
                          pos.y - vy / vlen * trail };
            DrawLineEx(pos, tail, sz * 0.6f, c);
        }
    }
}

/* ---- Explosions ---------------------------------------------------- */

void explosion_spawn(World *w, Vec2 pos, float radius, float damage,
                     float impulse, int owner_mech_id, int owner_team,
                     int weapon_id)
{
    if (radius <= 0.0f) return;

    /* M6 ship-prep — record for the camera linger. Lets update_camera()
     * keep the focus biased toward the impact for a short window after
     * the projectile dies (otherwise the camera snaps back to the mech
     * the same tick the explosion fires, and the player misses the
     * sparks). Both the authoritative path and the client-side
     * prediction populate this; the client-receive path (net.c
     * client_handle_explosion) also writes here so remote explosions
     * we observe locally also benefit. */
    w->last_explosion_pos = pos;
    w->last_explosion_tick = w->tick;
    w->last_explosion_owner_mech = owner_mech_id;

    /* Visuals — sparks + smoke fan-out + screen shake within ~800 px. */
    for (int k = 0; k < 28; ++k) {
        float ang = (float)k / 28.0f * 6.283185f;
        Vec2 dir = { cosf(ang), sinf(ang) };
        fx_spawn_spark(&w->fx, pos,
            (Vec2){ dir.x * 320.0f, dir.y * 320.0f }, w->rng);
    }
    for (int k = 0; k < 6; ++k) {
        Vec2 d = { (float)((int)k - 3) * 18.0f,
                   -(float)k * 8.0f };
        /* Smoke is cosmetic — we currently render smoke as a blood-like
         * particle (gray/black tint). The blood path's color randomizer
         * gives orange/red here; close enough for M3 first pass. */
        (void)d;
    }
    SHOT_LOG("t=%llu explosion at (%.1f,%.1f) r=%.1f dmg=%.1f imp=%.1f owner=%d weapon=%d",
             (unsigned long long)w->tick, pos.x, pos.y,
             radius, damage, impulse, owner_mech_id, weapon_id);
    /* M6 P10 follow-up — tier explosion shake by AOE damage so heavy
     * weapons feel chunkier than incidental small splashes. Pre-tier
     * value was a flat +0.4 for every detonation; user feedback was
     * that Mass Driver / Frag Grenade reads as "polite" against the
     * post-fix camera. Three buckets matched to the actual weapon
     * roster: ≥100 dmg → Mass Driver-class (heaviest hit), ≥50 dmg →
     * Frag Grenade-class (medium AOE), <50 dmg → Plasma Cannon /
     * Micro-Rockets (small splashes — unchanged). Decay is ×0.92/tick
     * so even 0.60 is back below 0.10 in under 0.5 s — chunky on the
     * impulse, gone before the next salvo. */
    float shake_add;
    if      (damage >= 100.0f) shake_add = 0.60f;
    else if (damage >=  50.0f) shake_add = 0.50f;
    else                       shake_add = 0.40f;
    w->shake_intensity = fminf(1.0f, w->shake_intensity + shake_add);

    /* P14 — explosion SFX. Size buckets per spec: large = Mass Driver
     * + ≥150 dmg AOE; medium = Frag Grenade / Plasma Cannon mid-tier
     * (≥50 dmg); small = Micro-rockets / sub-50 splashes. The duck
     * fires on big detonations (≥100 dmg AOE) so a Mass Driver
     * audibly drops the music + ambient bus for ~300 ms. Both branches
     * run on host + client so explosion audio is universal (the
     * authoritative gate below is for damage/impulse, not visuals). */
    SfxId esfx;
    if      (damage >= 150.0f) esfx = SFX_EXPLOSION_LARGE;
    else if (damage >=  50.0f) esfx = SFX_EXPLOSION_MEDIUM;
    else                       esfx = SFX_EXPLOSION_SMALL;
    audio_play_at(esfx, pos);
    if (damage >= 100.0f) audio_request_duck(0.5f, 0.30f);

    if (!w->authoritative) return;   /* damage is server-authoritative */

    /* wan-fixes-10 — queue the detonation so main.c can broadcast it
     * via NET_MSG_EXPLOSION. Clients use the server's pos as the
     * visual anchor instead of detonating their own visual grenade
     * against rest-pose remote bones (which differ from the server's
     * animated bones — see ExplosionFeedEntry doc in world.h). */
    if (w->explosionfeed_count < EXPLOSIONFEED_CAPACITY) {
        ExplosionFeedEntry *e = &w->explosionfeed[w->explosionfeed_count++];
        e->owner_mech_id = (int16_t)owner_mech_id;
        e->weapon_id     = (uint8_t)weapon_id;
        e->reserved      = 0;
        e->pos_x         = pos.x;
        e->pos_y         = pos.y;
    }

    for (int mi = 0; mi < w->mech_count; ++mi) {
        Mech *m = &w->mechs[mi];
        if (!m->alive) {
            /* Apply impulse to ragdolls so corpses get blown around. */
            for (int part = 0; part < PART_COUNT; ++part) {
                int idx = m->particle_base + part;
                float dx = w->particles.pos_x[idx] - pos.x;
                float dy = w->particles.pos_y[idx] - pos.y;
                float d2 = dx*dx + dy*dy;
                float r2 = radius * radius;
                if (d2 >= r2) continue;
                float d = sqrtf(d2);
                float ndx = (d > 1e-3f) ? dx / d : 0.0f;
                float ndy = (d > 1e-3f) ? dy / d : -1.0f;
                float fall = 1.0f - (d / radius); fall *= fall;
                float k_imp = impulse * fall * 0.4f;   /* corpses get less */
                w->particles.pos_x[idx] += ndx * k_imp;
                w->particles.pos_y[idx] += ndy * k_imp;
            }
            continue;
        }

        /* Bounding-box reject before per-particle work. The pelvis is a
         * good cheap centroid; if the mech extends beyond `radius` from
         * pelvis we still might catch a foot, but a cone-shaped check
         * is overkill here. */
        int pelv = m->particle_base + PART_PELVIS;
        float dxp = w->particles.pos_x[pelv] - pos.x;
        float dyp = w->particles.pos_y[pelv] - pos.y;
        float dist_p = sqrtf(dxp * dxp + dyp * dyp);
        if (dist_p > radius + 80.0f) continue;

        /* Friendly-fire / self-damage rules. The mass driver self-
         * damages the firer (intended), but we still respect the
         * friendly-fire toggle for teammates. */
        bool same_team = (owner_mech_id != mi)
                      && (owner_team >= 0)
                      && (m->team == owner_team);
        if (same_team && !w->friendly_fire) continue;

        /* Find the closest body part to the explosion center; that
         * decides hit_location_mult and gives us a particle to
         * attribute damage to. We then apply impulse + damage falloff
         * to every particle in the mech. */
        int   closest_part = PART_CHEST;
        float closest_d2   = 1e30f;
        for (int part = 0; part < PART_COUNT; ++part) {
            int idx = m->particle_base + part;
            float dx = w->particles.pos_x[idx] - pos.x;
            float dy = w->particles.pos_y[idx] - pos.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < closest_d2) {
                closest_d2 = d2;
                closest_part = part;
            }
        }

        /* Out of range entirely (no part inside radius)? Skip. */
        if (closest_d2 >= radius * radius) continue;

        float closest_d = sqrtf(closest_d2);
        float fall = 1.0f - (closest_d / radius);
        if (fall < 0.0f) fall = 0.0f;
        float dmg_falloff = fall * fall;     /* matches doc: 1-(d/r)^2 */

        /* LOS check: ray from explosion center to pelvis. If a wall
         * blocks, halve the damage (per documents/04-combat.md). */
        Vec2 mp = { w->particles.pos_x[pelv],
                    w->particles.pos_y[pelv] };
        float t_los;
        bool blocked = level_ray_hits(&w->level, pos, mp, &t_los);
        if (blocked && t_los < 1.0f) dmg_falloff *= 0.5f;

        float final_dmg = damage * dmg_falloff;

        /* Reactive armor: if the victim has reactive charges, eat one
         * here and zero damage. */
        if (m->armor_id == ARMOR_REACTIVE && m->armor_charges > 0
            && final_dmg > 0.0f) {
            m->armor_charges--;
            if (m->armor_charges <= 0) {
                /* Visible "armor breaks" event. */
                m->armor_id = ARMOR_NONE;
                m->armor_hp = 0.0f;
                m->armor_hp_max = 0.0f;
            }
            final_dmg = 0.0f;
        }

        /* Heavy chassis passive: -10% explosion damage. */
        const Chassis *ch = mech_chassis((ChassisId)m->chassis_id);
        if (ch && ch->passive == PASSIVE_HEAVY_AOE_RESIST) {
            final_dmg *= 0.9f;
        }

        if (final_dmg > 0.0f) {
            Vec2 dir = { (mp.x - pos.x), (mp.y - pos.y) };
            float dl = sqrtf(dir.x * dir.x + dir.y * dir.y);
            if (dl > 1e-3f) { dir.x /= dl; dir.y /= dl; }
            else            { dir = (Vec2){ 0.0f, -1.0f }; }
            mech_apply_damage(w, mi, closest_part, final_dmg, dir,
                              owner_mech_id);
        }

        /* Always apply impulse — even on FF-skipped damage we'd want
         * the body to react. (We get here only if FF passed; same-team
         * skip happened above. Self-damage from own rocket is
         * intended.) */
        for (int part = 0; part < PART_COUNT; ++part) {
            int idx = m->particle_base + part;
            float dx = w->particles.pos_x[idx] - pos.x;
            float dy = w->particles.pos_y[idx] - pos.y;
            float d2 = dx * dx + dy * dy;
            if (d2 >= radius * radius) continue;
            float d = sqrtf(d2);
            float ndx = (d > 1e-3f) ? dx / d : 0.0f;
            float ndy = (d > 1e-3f) ? dy / d : -1.0f;
            float pf = 1.0f - (d / radius);
            float k_imp = impulse * pf;
            w->particles.pos_x[idx] += ndx * k_imp;
            w->particles.pos_y[idx] += ndy * k_imp;
        }
    }
}

/* ---- wan-fixes-21: explosion record ring -------------------------- *
 *
 * Used to de-duplicate the client's predicted AOE explosion visual
 * (spawned by `detonate` above) against the server's authoritative
 * NET_MSG_EXPLOSION broadcast (handled in `src/net.c::client_handle_
 * explosion`). One ring per World; FIFO eviction; O(EXPLOSION_RECORD_
 * RING = 16) scan per push/find. Free even at frag-grenade fire rates. */

void explosion_record_push(World *w, Vec2 pos, int owner_mech,
                           int weapon_id, int source)
{
    int slot = w->explosion_record_head;
    w->explosion_record_ring[slot] = (ExplosionRecord){
        .pos        = pos,
        .at_tick    = w->tick,
        .owner_mech = (uint16_t)owner_mech,
        .weapon_id  = (uint8_t)weapon_id,
        .source     = (uint8_t)source,
        .valid      = true,
    };
    w->explosion_record_head = (slot + 1) % EXPLOSION_RECORD_RING;
}

ExplosionRecord *explosion_record_find_consume(World *w,
                                                int owner_mech,
                                                int weapon_id,
                                                int source,
                                                uint32_t max_age_ticks)
{
    ExplosionRecord *best = NULL;
    uint64_t best_at = (uint64_t)-1;
    uint64_t cutoff  = (w->tick > (uint64_t)max_age_ticks)
                            ? (w->tick - (uint64_t)max_age_ticks)
                            : 0;
    for (int i = 0; i < EXPLOSION_RECORD_RING; ++i) {
        ExplosionRecord *r = &w->explosion_record_ring[i];
        if (!r->valid) continue;
        if (r->at_tick < cutoff) {
            /* Stale — expire it. Cheap GC done lazily on every scan. */
            r->valid = false;
            continue;
        }
        if (r->owner_mech != (uint16_t)owner_mech) continue;
        if (r->weapon_id  != (uint8_t)weapon_id)   continue;
        if (r->source     != (uint8_t)source)      continue;
        if (r->at_tick < best_at) {
            best    = r;
            best_at = r->at_tick;
        }
    }
    return best;
}
