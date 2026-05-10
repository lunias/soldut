#include "projectile.h"

#include "audio.h"
#include "level.h"
#include "log.h"
#include "mech.h"
#include "particle.h"

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
    if (p->aoe_radius[i] > 0.0f) {
        explosion_spawn(w,
            (Vec2){ p->pos_x[i], p->pos_y[i] },
            p->aoe_radius[i],
            p->aoe_damage[i],
            p->aoe_impulse[i],
            (int)p->owner_mech[i], (int)p->owner_team[i],
            (int)p->weapon_id[i]);
    }
    p->alive[i] = 0;
}

void projectile_step(World *w, float dt) {
    ProjectilePool *p = &w->projectiles;
    if (p->count == 0) return;

    Vec2 g = w->level.gravity;
    int last_alive = -1;

    for (int i = 0; i < p->count; ++i) {
        if (!p->alive[i]) continue;

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

        for (int mi = 0; mi < w->mech_count; ++mi) {
            const Mech *m = &w->mechs[mi];
            if (!m->alive) continue;
            if (mi == p->owner_mech[i]) continue;   /* don't hit yourself */
            for (int bi = 0; bi < NUM_BONES; ++bi) {
                int pa = m->particle_base + g_bones[bi].parent;
                int pb = m->particle_base + g_bones[bi].child;
                Vec2 va = { w->particles.pos_x[pa], w->particles.pos_y[pa] };
                Vec2 vb = { w->particles.pos_x[pb], w->particles.pos_y[pb] };
                float th = swept_seg_vs_bone(a, b, va, vb, /*r*/ 8.0f);
                if (th < 0.0f) continue;
                if (th < t_hit) {
                    t_hit = th;
                    hit_mech = mi;
                    hit_part = g_bones[bi].part_for_damage;
                }
            }
        }

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
                        SHOT_LOG("t=%llu mech=%d grapple_attach anchor=(%.1f,%.1f) tgt_mech=%d part=%d L=%.1f",
                                 (unsigned long long)w->tick, fmid,
                                 anchor.x, anchor.y,
                                 (int)firer->grapple.anchor_mech,
                                 (int)firer->grapple.anchor_part, L);
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

            /* Bouncy projectiles (frag grenades) ricochet off tiles
             * instead of detonating; they keep their fuse and continue
             * to integrate. We pick a normal by which axis crossed
             * first (cheap approximation; works for tile-aligned maps). */
            if (hit_wall && hit_mech < 0 && p->bouncy[i]) {
                /* Reflect velocity. Estimate normal: prev pos was outside,
                 * new pos is at tile boundary; whichever axis dominates
                 * the velocity is the one we bounce. */
                if (fabsf(p->vel_x[i]) > fabsf(p->vel_y[i])) {
                    p->vel_x[i] *= -0.45f;
                    p->vel_y[i] *=  0.65f;
                } else {
                    p->vel_x[i] *=  0.65f;
                    p->vel_y[i] *= -0.45f;
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

void projectile_draw(const ProjectilePool *p, float alpha) {
    for (int i = 0; i < p->count; ++i) {
        if (!p->alive[i]) continue;
        Color c = proj_color(p->kind[i]);
        float sz = proj_size(p->kind[i]);
        /* P03: lerp between start-of-tick pos and latest physics pos. */
        Vec2 pos = {
            p->render_prev_x[i] + (p->pos_x[i] - p->render_prev_x[i]) * alpha,
            p->render_prev_y[i] + (p->pos_y[i] - p->render_prev_y[i]) * alpha,
        };
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
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.4f);

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
