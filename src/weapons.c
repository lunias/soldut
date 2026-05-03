#include "weapons.h"

#include "level.h"
#include "log.h"
#include "mech.h"
#include "particle.h"

#include <math.h>

/* ---- Weapon table -------------------------------------------------- */

static const Weapon g_weapons[WEAPON_COUNT_M1] = {
    [WEAPON_PULSE_RIFLE] = {
        .name           = "Pulse Rifle",
        .hitscan        = true,
        .damage         = 18.0f,
        .fire_rate_sec  = 0.110f,
        .reload_sec     = 1.50f,
        .mag_size       = 30,
        .range_px       = 2400.0f,
        .recoil_impulse = 1.2f,
        .muzzle_offset  = 22.0f,
    },
};

const Weapon *weapon_def(int id) {
    if ((unsigned)id >= WEAPON_COUNT_M1) return NULL;
    return &g_weapons[id];
}

/* ---- Bone segment list (per-mech) ---------------------------------- */

/* Bones we can hit. Each entry is a (parent_part, child_part) — the
 * "bone" is the segment between those two particles. We test the ray
 * against each as a capsule of `bone_radius`. */
typedef struct {
    int parent;
    int child;
    int part_for_damage;   /* which body part to attribute the hit to */
} BoneSeg;

static const BoneSeg g_bones[] = {
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

/* ---- Ray vs segment test ------------------------------------------- */

/* Return the smallest t in (0, t_max] where ray (origin + t*dir) gets
 * within `r` of segment [a,b], or -1 if no hit. Uses a capsule
 * approximation: project ray onto segment, take the closest point, and
 * if distance <= r and projection-along-ray is positive within t_max,
 * report it.
 *
 * This is conservative — we treat the segment as a thin capsule.
 * Adequate for M1; if it bites in playtest we'll switch to closed-form
 * ray-vs-capsule. */
static float ray_seg_hit(Vec2 ro, Vec2 rd, float t_max,
                         Vec2 a, Vec2 b, float r) {
    /* Sample 16 points along the segment; for each, find the t along
     * the ray closest to it, take the min. */
    const int N = 16;
    float best_t = -1.0f;
    for (int i = 0; i <= N; ++i) {
        float u = (float)i / (float)N;
        Vec2 p = { a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u };
        Vec2 v = { p.x - ro.x, p.y - ro.y };
        float t = v.x * rd.x + v.y * rd.y;
        if (t < 0.0f || t > t_max) continue;
        Vec2 q = { ro.x + rd.x * t, ro.y + rd.y * t };
        float dx = q.x - p.x, dy = q.y - p.y;
        if (dx * dx + dy * dy <= r * r) {
            if (best_t < 0.0f || t < best_t) best_t = t;
        }
    }
    return best_t;
}

/* ---- Fire ---------------------------------------------------------- */

void weapons_fire_hitscan(World *w, int mid) {
    Mech *me = &w->mechs[mid];
    const Weapon *wpn = weapon_def(me->weapon_id);
    if (!wpn || !wpn->hitscan) return;

    /* Origin = right hand, plus a small muzzle offset along aim. */
    Vec2 hand = mech_hand_pos(w, mid);
    Vec2 dir  = mech_aim_dir(w, mid);
    Vec2 origin = { hand.x + dir.x * wpn->muzzle_offset,
                    hand.y + dir.y * wpn->muzzle_offset };
    Vec2 tracer_end = { origin.x + dir.x * wpn->range_px,
                        origin.y + dir.y * wpn->range_px };
    float t_max = wpn->range_px;

    /* Test against the level first — that's the hard ceiling on range. */
    float wall_t;
    if (level_ray_hits(&w->level,
            origin,
            (Vec2){ origin.x + dir.x * wpn->range_px,
                    origin.y + dir.y * wpn->range_px },
            &wall_t)) {
        t_max = wall_t * wpn->range_px;
    }

    /* Walk every other mech's bones. */
    int   hit_mech = -1;
    int   hit_part = -1;
    float hit_t    = -1.0f;

    for (int i = 0; i < w->mech_count; ++i) {
        if (i == mid) continue;
        const Mech *t = &w->mechs[i];
        for (int bi = 0; bi < NUM_BONES; ++bi) {
            const BoneSeg *b = &g_bones[bi];
            int pa = t->particle_base + b->parent;
            int pb = t->particle_base + b->child;
            Vec2 va = { w->particles.pos_x[pa], w->particles.pos_y[pa] };
            Vec2 vb = { w->particles.pos_x[pb], w->particles.pos_y[pb] };
            float th = ray_seg_hit(origin, dir, t_max, va, vb, /*radius*/ 6.0f);
            if (th < 0.0f) continue;
            if (hit_t < 0.0f || th < hit_t) {
                hit_t    = th;
                hit_mech = i;
                hit_part = b->part_for_damage;
            }
        }
    }

    /* Apply hit. */
    Vec2 final_end;
    if (hit_t >= 0.0f) {
        final_end = (Vec2){ origin.x + dir.x * hit_t, origin.y + dir.y * hit_t };
        SHOT_LOG("t=%llu fire mech=%d origin=(%.1f,%.1f) dir=(%.2f,%.2f) "
                 "hit mech=%d part=%d at=(%.1f,%.1f) dmg=%.1f",
                 (unsigned long long)w->tick, mid, origin.x, origin.y,
                 dir.x, dir.y, hit_mech, hit_part,
                 final_end.x, final_end.y, wpn->damage);
        /* Damage. dir is a unit vector and is forwarded as-is —
         * mech_apply_damage uses it only for blood-spray angle and to
         * pass through to mech_kill, which scales by its own kill
         * impulse. The previous "impulse_px" pre-scaling here got
         * compounded with the kill scaling and produced a 432-px
         * displacement on death (an L_ELBOW killshot left the elbow
         * stuck ~430 px right of the shoulder, drawn as a long red
         * line in the ragdoll). */
        mech_apply_damage(w, hit_mech, hit_part, wpn->damage, dir);
    } else {
        final_end = (Vec2){ origin.x + dir.x * t_max, origin.y + dir.y * t_max };
        SHOT_LOG("t=%llu fire mech=%d origin=(%.1f,%.1f) dir=(%.2f,%.2f) "
                 "miss end=(%.1f,%.1f) wall=%d",
                 (unsigned long long)w->tick, mid, origin.x, origin.y,
                 dir.x, dir.y, final_end.x, final_end.y,
                 (int)(t_max < wpn->range_px));
        /* Sparks if it hit a wall. */
        if (t_max < wpn->range_px) {
            for (int k = 0; k < 6; ++k) {
                fx_spawn_spark(&w->fx, final_end,
                    (Vec2){ -dir.x * 200.0f, -dir.y * 200.0f }, w->rng);
            }
        }
        (void)tracer_end;
    }

    fx_spawn_tracer(&w->fx, origin, final_end);

    /* Recoil — punch the firing hand backward; the constraint pass
     * ripples it through the rest of the body. */
    int hand_idx = me->particle_base + PART_R_HAND;
    w->particles.pos_x[hand_idx] -= dir.x * wpn->recoil_impulse;
    w->particles.pos_y[hand_idx] -= dir.y * wpn->recoil_impulse;
    me->recoil_kick = 1.0f;

    /* Cooldown + shake + brief muzzle flash spark. */
    me->fire_cooldown = wpn->fire_rate_sec;
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.05f);
    for (int k = 0; k < 3; ++k) {
        fx_spawn_spark(&w->fx, origin,
            (Vec2){ dir.x * 350.0f, dir.y * 350.0f }, w->rng);
    }
}
