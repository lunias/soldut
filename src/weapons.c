#include "weapons.h"

#include "audio.h"
#include "level.h"
#include "log.h"
#include "mech.h"
#include "particle.h"
#include "projectile.h"
#include "snapshot.h"
#include "weapon_sprites.h"

#include <math.h>
#include <stdint.h>

/* ---- Weapon table -------------------------------------------------- */

static const Weapon g_weapons[WEAPON_COUNT] = {
    [WEAPON_PULSE_RIFLE] = {
        .name = "Pulse Rifle",
        .klass = WEAPON_CLASS_PRIMARY,
        .fire = WFIRE_HITSCAN,
        .damage = 18.0f, .fire_rate_sec = 0.110f, .reload_sec = 1.50f,
        .mag_size = 30, .range_px = 2400.0f,
        .recoil_impulse = 1.2f, .bink = 0.012f, .self_bink = 0.008f,
        .muzzle_offset = 22.0f,
    },
    [WEAPON_PLASMA_SMG] = {
        .name = "Plasma SMG",
        .klass = WEAPON_CLASS_PRIMARY,
        .fire = WFIRE_PROJECTILE,
        .damage = 10.0f, .fire_rate_sec = 0.060f, .reload_sec = 1.30f,
        .mag_size = 40, .range_px = 1800.0f,
        .recoil_impulse = 0.7f, .bink = 0.008f, .self_bink = 0.014f,
        .muzzle_offset = 22.0f,
        .projectile_kind = PROJ_PLASMA_BOLT,
        .projectile_speed_pxs = 1800.0f, .projectile_life_sec = 1.2f,
        .projectile_drag = 0.05f, .projectile_grav_scale = 0.0f,
    },
    [WEAPON_RIOT_CANNON] = {
        .name = "Riot Cannon",
        .klass = WEAPON_CLASS_PRIMARY,
        .fire = WFIRE_SPREAD,
        .damage = 8.0f, .fire_rate_sec = 0.350f, .reload_sec = 1.70f,
        .mag_size = 6, .range_px = 800.0f,
        .recoil_impulse = 3.5f, .bink = 0.060f, .self_bink = 0.020f,
        .muzzle_offset = 22.0f,
        .spread_pellets = 6, .spread_cone_rad = 0.18f,
        .projectile_kind = PROJ_PELLET,
        .projectile_speed_pxs = 1500.0f, .projectile_life_sec = 0.4f,
        .projectile_drag = 1.20f, .projectile_grav_scale = 0.05f,
    },
    [WEAPON_RAIL_CANNON] = {
        .name = "Rail Cannon",
        .klass = WEAPON_CLASS_PRIMARY,
        .fire = WFIRE_HITSCAN,
        .damage = 95.0f, .fire_rate_sec = 1.20f, .reload_sec = 2.20f,
        .mag_size = 4, .range_px = 4096.0f,
        .recoil_impulse = 6.0f, .bink = 0.10f, .self_bink = 0.04f,
        .muzzle_offset = 26.0f,
        .charge_sec = 0.40f,
    },
    [WEAPON_AUTO_CANNON] = {
        .name = "Auto-Cannon",
        .klass = WEAPON_CLASS_PRIMARY,
        .fire = WFIRE_PROJECTILE,
        .damage = 14.0f, .fire_rate_sec = 0.080f, .reload_sec = 1.80f,
        .mag_size = 60, .range_px = 2200.0f,
        .recoil_impulse = 1.6f, .bink = 0.014f, .self_bink = 0.018f,
        .muzzle_offset = 24.0f,
        .projectile_kind = PROJ_RIFLE_SLUG,
        .projectile_speed_pxs = 1700.0f, .projectile_life_sec = 1.4f,
        .projectile_drag = 0.0f, .projectile_grav_scale = 0.05f,
    },
    [WEAPON_MASS_DRIVER] = {
        .name = "Mass Driver",
        .klass = WEAPON_CLASS_PRIMARY,
        .fire = WFIRE_PROJECTILE,
        /* M6 P05 Phase 6 — balance pass. Iter7 matrix had MD at 76 kills,
         * 9 more than the next-best Rail Cannon (67). The AOE-on-low-fire-rate
         * combo dominated the bot's "group fight at a pickup" pattern.
         * Option C from the plan §4.6: 50/50 split between fire-rate
         * slowdown and AOE shrink. fire_rate_sec 1.10 → 1.25 (-12 %),
         * aoe_radius 160 → 140 (-13 %). */
        .damage = 220.0f, .fire_rate_sec = 1.25f, .reload_sec = 3.00f,
        .mag_size = 1, .range_px = 4096.0f,
        .recoil_impulse = 5.5f, .bink = 0.10f, .self_bink = 0.04f,
        .muzzle_offset = 28.0f,
        .projectile_kind = PROJ_ROCKET,
        .projectile_speed_pxs = 850.0f, .projectile_life_sec = 5.0f,
        .projectile_drag = 0.0f, .projectile_grav_scale = 0.0f,
        .aoe_radius = 140.0f, .aoe_damage = 130.0f, .aoe_impulse = 70.0f,
    },
    [WEAPON_PLASMA_CANNON] = {
        .name = "Plasma Cannon",
        .klass = WEAPON_CLASS_PRIMARY,
        .fire = WFIRE_PROJECTILE,
        .damage = 60.0f, .fire_rate_sec = 0.70f, .reload_sec = 2.20f,
        .mag_size = 8, .range_px = 3000.0f,
        .recoil_impulse = 2.2f, .bink = 0.04f, .self_bink = 0.02f,
        .muzzle_offset = 26.0f,
        .projectile_kind = PROJ_PLASMA_ORB,
        .projectile_speed_pxs = 1200.0f, .projectile_life_sec = 2.5f,
        .projectile_drag = 0.0f, .projectile_grav_scale = 0.0f,
        .aoe_radius = 60.0f, .aoe_damage = 30.0f, .aoe_impulse = 25.0f,
    },
    [WEAPON_MICROGUN] = {
        .name = "Microgun",
        .klass = WEAPON_CLASS_PRIMARY,
        .fire = WFIRE_PROJECTILE,
        .damage = 6.0f, .fire_rate_sec = 0.025f, .reload_sec = 4.50f,
        .mag_size = 200, .range_px = 1800.0f,
        .recoil_impulse = 0.5f, .bink = 0.006f, .self_bink = 0.022f,
        .muzzle_offset = 26.0f, .charge_sec = 0.50f,
        .projectile_kind = PROJ_MICROGUN_BULLET,
        .projectile_speed_pxs = 1900.0f, .projectile_life_sec = 0.95f,
        .projectile_drag = 0.0f, .projectile_grav_scale = 0.05f,
    },

    /* --- Secondaries --- */
    [WEAPON_SIDEARM] = {
        .name = "Sidearm",
        .klass = WEAPON_CLASS_SECONDARY,
        .fire = WFIRE_HITSCAN,
        .damage = 25.0f, .fire_rate_sec = 0.20f, .reload_sec = 0.80f,
        .mag_size = 12, .range_px = 1400.0f,
        .recoil_impulse = 1.4f, .bink = 0.018f, .self_bink = 0.010f,
        .muzzle_offset = 18.0f,
    },
    [WEAPON_BURST_SMG] = {
        .name = "Burst SMG",
        .klass = WEAPON_CLASS_SECONDARY,
        .fire = WFIRE_BURST,
        .damage = 12.0f, .fire_rate_sec = 0.350f, .reload_sec = 1.40f,
        .mag_size = 24, .range_px = 1600.0f,
        .recoil_impulse = 0.9f, .bink = 0.012f, .self_bink = 0.014f,
        .muzzle_offset = 20.0f,
        .burst_rounds = 3, .burst_interval_sec = 0.070f,
        .projectile_kind = PROJ_RIFLE_SLUG,
        .projectile_speed_pxs = 1700.0f, .projectile_life_sec = 1.0f,
    },
    [WEAPON_FRAG_GRENADES] = {
        .name = "Frag Grenades",
        .klass = WEAPON_CLASS_SECONDARY,
        .fire = WFIRE_THROW,
        .damage = 0.0f, .fire_rate_sec = 0.60f, .reload_sec = 0.0f,
        .mag_size = 3, .range_px = 0.0f,
        .recoil_impulse = 0.4f, .bink = 0.0f, .self_bink = 0.0f,
        .muzzle_offset = 16.0f,
        .projectile_kind = PROJ_FRAG_GRENADE,
        .projectile_speed_pxs = 700.0f, .projectile_life_sec = 1.5f,
        .projectile_drag = 0.4f, .projectile_grav_scale = 1.0f,
        .aoe_radius = 140.0f, .aoe_damage = 80.0f, .aoe_impulse = 55.0f,
        .bouncy = true,
    },
    [WEAPON_MICRO_ROCKETS] = {
        .name = "Micro-Rockets",
        .klass = WEAPON_CLASS_SECONDARY,
        .fire = WFIRE_PROJECTILE,
        .damage = 35.0f, .fire_rate_sec = 0.25f, .reload_sec = 2.20f,
        .mag_size = 5, .range_px = 2400.0f,
        .recoil_impulse = 1.0f, .bink = 0.020f, .self_bink = 0.020f,
        .muzzle_offset = 22.0f,
        .projectile_kind = PROJ_MICRO_ROCKET,
        .projectile_speed_pxs = 1100.0f, .projectile_life_sec = 2.5f,
        .projectile_drag = 0.0f, .projectile_grav_scale = 0.0f,
        .aoe_radius = 50.0f, .aoe_damage = 18.0f, .aoe_impulse = 22.0f,
    },
    [WEAPON_COMBAT_KNIFE] = {
        .name = "Combat Knife",
        .klass = WEAPON_CLASS_SECONDARY,
        .fire = WFIRE_MELEE,
        .damage = 90.0f, .fire_rate_sec = 0.20f, .reload_sec = 0.0f,
        .mag_size = 0, .range_px = 60.0f,
        .recoil_impulse = 0.0f, .bink = 0.0f, .self_bink = 0.0f,
        .muzzle_offset = 0.0f,
    },
    [WEAPON_GRAPPLING_HOOK] = {
        .name = "Grappling Hook",
        .klass = WEAPON_CLASS_SECONDARY,
        .fire = WFIRE_GRAPPLE,
        .damage = 0.0f, .fire_rate_sec = 1.20f, .reload_sec = 0.0f,
        .mag_size = 1, .range_px = 600.0f,
        .recoil_impulse = 0.0f, .bink = 0.0f, .self_bink = 0.0f,
        .muzzle_offset = 18.0f,
        /* P06 — head projectile params; mech_try_fire's WFIRE_GRAPPLE
         * path reads these directly rather than going through
         * weapons_spawn_projectiles (which is for damaging projectiles).
         * Lifetime = range / speed = 0.5 s. */
        .projectile_kind        = PROJ_GRAPPLE_HEAD,
        .projectile_speed_pxs   = 1200.0f,
        .projectile_life_sec    = 0.5f,
        .projectile_drag        = 0.0f,
        .projectile_grav_scale  = 0.0f,
    },
};

const Weapon *weapon_def(int id) {
    if ((unsigned)id >= WEAPON_COUNT) return NULL;
    return &g_weapons[id];
}

const char *weapon_short_name(int id) {
    if ((unsigned)id >= WEAPON_COUNT) return "?";
    return g_weapons[id].name;
}

/* ---- Bone segment list (per-mech) ---------------------------------- */

typedef struct {
    int parent;
    int child;
    int part_for_damage;
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

static float ray_seg_hit(Vec2 ro, Vec2 rd, float t_max,
                         Vec2 a, Vec2 b, float r) {
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

/* Apply incoming-fire bink to every non-shooter mech whose body is
 * within `near_px` of the line origin→end. Used by hitscan + projectile
 * fire paths. */
static void apply_bink_along_segment(World *w, int shooter,
                                     Vec2 origin, Vec2 end,
                                     float bink_amount, float near_px)
{
    if (bink_amount <= 0.0f) return;
    float seg_dx = end.x - origin.x;
    float seg_dy = end.y - origin.y;
    float seg2 = seg_dx * seg_dx + seg_dy * seg_dy;
    if (seg2 < 1e-3f) return;

    for (int mi = 0; mi < w->mech_count; ++mi) {
        if (mi == shooter) continue;
        Mech *m = &w->mechs[mi];
        if (!m->alive) continue;
        int p = m->particle_base + PART_CHEST;
        Vec2 c = { w->particles.pos_x[p], w->particles.pos_y[p] };
        /* Closest distance from line origin→end to point c. */
        float t = ((c.x - origin.x) * seg_dx + (c.y - origin.y) * seg_dy) / seg2;
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        Vec2 q = { origin.x + seg_dx * t, origin.y + seg_dy * t };
        float dx = c.x - q.x, dy = c.y - q.y;
        float d2 = dx * dx + dy * dy;
        if (d2 > near_px * near_px) continue;
        float prox = 1.0f - sqrtf(d2) / near_px;
        mech_apply_bink(m, bink_amount, prox, w->rng);
    }
}

/* ---- Hitscan ------------------------------------------------------- */

static Vec2 apply_self_bink(World *w, Mech *me, Vec2 base_dir) {
    /* Rotate the aim direction by the shooter's accumulated bink. The
     * bink decays each tick in mech_step_drive. */
    float ang = me->aim_bink;
    float ca = cosf(ang), sa = sinf(ang);
    Vec2 d = { base_dir.x * ca - base_dir.y * sa,
               base_dir.x * sa + base_dir.y * ca };
    (void)w;
    return d;
}

/* Server-side: queue a fire event so main.c can broadcast it to
 * clients. They use it to spawn matching tracer/projectile visuals.
 * Authoritative-only — clients run their own predict path.
 *
 * Exposed via weapons.h as `weapons_record_fire` so mech.c's grapple
 * fire branch can broadcast its head spawn (P09 sync fix — pre-P09 the
 * grapple head's flight was invisible to remote clients). */
void weapons_record_fire(World *w, int mid, int weapon_id, Vec2 origin, Vec2 dir) {
    if (!w->authoritative) return;
    int slot = w->firefeed_count % FIREFEED_CAPACITY;
    if (slot < 0) slot += FIREFEED_CAPACITY;
    w->firefeed[slot] = (FireFeedEntry){
        .shooter_mech_id = (int16_t)mid,
        .weapon_id       = (uint8_t)weapon_id,
        .reserved        = 0,
        .origin_x        = origin.x,
        .origin_y        = origin.y,
        .dir_x           = dir.x,
        .dir_y           = dir.y,
    };
    w->firefeed_count++;
}

void weapons_fire_hitscan(World *w, int mid) {
    Mech *me = &w->mechs[mid];
    const Weapon *wpn = weapon_def(me->weapon_id);
    if (!wpn || wpn->fire != WFIRE_HITSCAN) return;

    Vec2 hand = mech_hand_pos(w, mid);
    Vec2 dir  = apply_self_bink(w, me, mech_aim_dir(w, mid));
    /* P11 — muzzle from the weapon sprite def so visible muzzle and
     * physics muzzle coincide. The fallback (`wpn->muzzle_offset`)
     * preserves the M3 origin if a future weapon ships without a
     * sprite-def entry. */
    const WeaponSpriteDef *wsp = weapon_sprite_def(me->weapon_id);
    Vec2 origin = weapon_muzzle_world(hand, dir, wsp, wpn->muzzle_offset);
    weapons_record_fire(w, mid, me->weapon_id, origin, dir);
    audio_play_at(audio_sfx_for_weapon(me->weapon_id), origin);
    float t_max = wpn->range_px;

    float wall_t;
    if (level_ray_hits(&w->level,
            origin,
            (Vec2){ origin.x + dir.x * wpn->range_px,
                    origin.y + dir.y * wpn->range_px },
            &wall_t)) {
        t_max = wall_t * wpn->range_px;
    }

    int   hit_mech = -1;
    int   hit_part = -1;
    float hit_t    = -1.0f;

    for (int i = 0; i < w->mech_count; ++i) {
        if (i == mid) continue;
        const Mech *t = &w->mechs[i];
        if (!t->alive) continue;
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

    Vec2 final_end;
    if (hit_t >= 0.0f) {
        final_end = (Vec2){ origin.x + dir.x * hit_t, origin.y + dir.y * hit_t };
        SHOT_LOG("t=%llu fire mech=%d wpn=%d hit mech=%d part=%d at=(%.1f,%.1f) dmg=%.1f",
                 (unsigned long long)w->tick, mid, me->weapon_id,
                 hit_mech, hit_part, final_end.x, final_end.y, wpn->damage);
        if (w->authoritative) {
            w->mechs[hit_mech].last_killshot_weapon = me->weapon_id;
            mech_apply_damage(w, hit_mech, hit_part, wpn->damage, dir, mid);
        }
        audio_play_at(SFX_HIT_FLESH, final_end);
    } else {
        final_end = (Vec2){ origin.x + dir.x * t_max, origin.y + dir.y * t_max };
        SHOT_LOG("t=%llu fire mech=%d wpn=%d miss end=(%.1f,%.1f) wall=%d",
                 (unsigned long long)w->tick, mid, me->weapon_id,
                 final_end.x, final_end.y,
                 (int)(t_max < wpn->range_px));
        if (t_max < wpn->range_px) {
            for (int k = 0; k < 6; ++k) {
                fx_spawn_spark(&w->fx, final_end,
                    (Vec2){ -dir.x * 200.0f, -dir.y * 200.0f }, w->rng);
            }
            audio_play_at(SFX_HIT_CONCRETE, final_end);
        }
    }

    fx_spawn_tracer(&w->fx, origin, final_end);

    /* Bink to non-shooters along the line. */
    apply_bink_along_segment(w, mid, origin, final_end, wpn->bink, 80.0f);

    /* Self-bink on the shooter — adds aim jitter that grows with rate
     * of fire. We just add the per-shot value; the decay is in
     * mech_step_drive. */
    if (wpn->self_bink > 0.0f) {
        float sign = ((pcg32_next(w->rng) & 1u) ? 1.0f : -1.0f);
        me->aim_bink += sign * wpn->self_bink;
    }

    /* Recoil — punch the firing hand backward. */
    int hand_idx = me->particle_base + PART_R_HAND;
    w->particles.pos_x[hand_idx] -= dir.x * wpn->recoil_impulse;
    w->particles.pos_y[hand_idx] -= dir.y * wpn->recoil_impulse;
    me->recoil_kick = 1.0f;

    me->fire_cooldown = wpn->fire_rate_sec;
    /* M6 P10 — per-fire shake cut 0.05 → 0.02. See the matching comment
     * in weapons_fire_hitscan_lag_comp for the rationale. */
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.02f);
    for (int k = 0; k < 3; ++k) {
        fx_spawn_spark(&w->fx, origin,
            (Vec2){ dir.x * 350.0f, dir.y * 350.0f }, w->rng);
    }
}

void weapons_fire_hitscan_lag_comp(World *w, int mid, uint64_t shot_at_tick) {
    if (shot_at_tick == 0 || shot_at_tick == (uint64_t)-1) {
        weapons_fire_hitscan(w, mid);
        return;
    }
    if (w->tick > shot_at_tick + LAG_HIST_TICKS) {
        weapons_fire_hitscan(w, mid);
        return;
    }

    Mech *me = &w->mechs[mid];
    const Weapon *wpn = weapon_def(me->weapon_id);
    if (!wpn || wpn->fire != WFIRE_HITSCAN) return;

    Vec2 hand = mech_hand_pos(w, mid);
    Vec2 dir  = apply_self_bink(w, me, mech_aim_dir(w, mid));
    const WeaponSpriteDef *wsp = weapon_sprite_def(me->weapon_id);
    Vec2 origin = weapon_muzzle_world(hand, dir, wsp, wpn->muzzle_offset);
    weapons_record_fire(w, mid, me->weapon_id, origin, dir);
    audio_play_at(audio_sfx_for_weapon(me->weapon_id), origin);
    float t_max = wpn->range_px;

    float wall_t;
    if (level_ray_hits(&w->level,
            origin,
            (Vec2){ origin.x + dir.x * wpn->range_px,
                    origin.y + dir.y * wpn->range_px },
            &wall_t)) {
        t_max = wall_t * wpn->range_px;
    }

    int   hit_mech = -1;
    int   hit_part = -1;
    float hit_t    = -1.0f;

    for (int i = 0; i < w->mech_count; ++i) {
        if (i == mid) continue;
        const Mech *t = &w->mechs[i];
        if (!t->alive) continue;
        float bx[PART_COUNT], by[PART_COUNT];
        if (!snapshot_lag_lookup(w, i, shot_at_tick, bx, by)) {
            for (int p = 0; p < PART_COUNT; ++p) {
                bx[p] = w->particles.pos_x[t->particle_base + p];
                by[p] = w->particles.pos_y[t->particle_base + p];
            }
        }
        for (int bi = 0; bi < NUM_BONES; ++bi) {
            const BoneSeg *b = &g_bones[bi];
            Vec2 va = { bx[b->parent], by[b->parent] };
            Vec2 vb = { bx[b->child],  by[b->child]  };
            float th = ray_seg_hit(origin, dir, t_max, va, vb, /*radius*/ 6.0f);
            if (th < 0.0f) continue;
            if (hit_t < 0.0f || th < hit_t) {
                hit_t    = th;
                hit_mech = i;
                hit_part = b->part_for_damage;
            }
        }
    }

    Vec2 final_end;
    if (hit_t >= 0.0f) {
        final_end = (Vec2){ origin.x + dir.x * hit_t, origin.y + dir.y * hit_t };
        SHOT_LOG("t=%llu fire mech=%d wpn=%d hit mech=%d part=%d at=(%.1f,%.1f) dmg=%.1f lag_comp=%llu",
                 (unsigned long long)w->tick, mid, me->weapon_id,
                 hit_mech, hit_part, final_end.x, final_end.y, wpn->damage,
                 (unsigned long long)shot_at_tick);
        if (w->authoritative) {
            w->mechs[hit_mech].last_killshot_weapon = me->weapon_id;
            mech_apply_damage(w, hit_mech, hit_part, wpn->damage, dir, mid);
        }
        audio_play_at(SFX_HIT_FLESH, final_end);
    } else {
        final_end = (Vec2){ origin.x + dir.x * t_max, origin.y + dir.y * t_max };
        SHOT_LOG("t=%llu fire mech=%d wpn=%d miss end=(%.1f,%.1f) wall=%d lag_comp=%llu",
                 (unsigned long long)w->tick, mid, me->weapon_id,
                 final_end.x, final_end.y,
                 (int)(t_max < wpn->range_px),
                 (unsigned long long)shot_at_tick);
        if (t_max < wpn->range_px) {
            for (int k = 0; k < 6; ++k) {
                fx_spawn_spark(&w->fx, final_end,
                    (Vec2){ -dir.x * 200.0f, -dir.y * 200.0f }, w->rng);
            }
            audio_play_at(SFX_HIT_CONCRETE, final_end);
        }
    }
    fx_spawn_tracer(&w->fx, origin, final_end);
    apply_bink_along_segment(w, mid, origin, final_end, wpn->bink, 80.0f);
    if (wpn->self_bink > 0.0f) {
        float sign = ((pcg32_next(w->rng) & 1u) ? 1.0f : -1.0f);
        me->aim_bink += sign * wpn->self_bink;
    }

    int hand_idx = me->particle_base + PART_R_HAND;
    w->particles.pos_x[hand_idx] -= dir.x * wpn->recoil_impulse;
    w->particles.pos_y[hand_idx] -= dir.y * wpn->recoil_impulse;
    me->recoil_kick = 1.0f;

    me->fire_cooldown = wpn->fire_rate_sec;
    /* M6 P10 — was 0.05; bursty per-shot accumulation made sustained fire
     * shake the camera too aggressively. Cut to 0.02 so even an SMG-rate
     * loop stays around shake_intensity ≈ 0.05 steady-state instead of
     * ≈ 0.6. Big-event shake (kill / explosion / hit) is unchanged. */
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.02f);
    for (int k = 0; k < 3; ++k) {
        fx_spawn_spark(&w->fx, origin,
            (Vec2){ dir.x * 350.0f, dir.y * 350.0f }, w->rng);
    }
}

void weapons_predict_local_fire(World *w, int mid) {
    Mech *me = &w->mechs[mid];
    const Weapon *wpn = weapon_def(me->weapon_id);
    if (!wpn) return;

    Vec2 hand = mech_hand_pos(w, mid);
    Vec2 dir  = apply_self_bink(w, me, mech_aim_dir(w, mid));
    const WeaponSpriteDef *wsp = weapon_sprite_def(me->weapon_id);
    Vec2 origin = weapon_muzzle_world(hand, dir, wsp, wpn->muzzle_offset);
    if (wpn->fire == WFIRE_HITSCAN) {
        float t_max = wpn->range_px;
        float wall_t;
        if (level_ray_hits(&w->level,
                origin,
                (Vec2){ origin.x + dir.x * wpn->range_px,
                        origin.y + dir.y * wpn->range_px },
                &wall_t)) {
            t_max = wall_t * wpn->range_px;
        }
        Vec2 end = { origin.x + dir.x * t_max, origin.y + dir.y * t_max };
        fx_spawn_tracer(&w->fx, origin, end);
        /* Predict-side fire SFX. The matching FIRE_EVENT will arrive
         * ~RTT later; client_handle_fire_event suppresses its own play
         * via the predict_drew gate so we don't double-trigger. */
        audio_play_at(audio_sfx_for_weapon(me->weapon_id), origin);
    }

    int hand_idx = me->particle_base + PART_R_HAND;
    w->particles.pos_x[hand_idx] -= dir.x * wpn->recoil_impulse;
    w->particles.pos_y[hand_idx] -= dir.y * wpn->recoil_impulse;
    me->recoil_kick = 1.0f;
    me->fire_cooldown = wpn->fire_rate_sec;
    /* M6 P10 — per-fire shake cut 0.05 → 0.02 (predict path; matches the
     * authoritative hitscan paths above). */
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.02f);
    for (int k = 0; k < 3; ++k) {
        fx_spawn_spark(&w->fx, origin,
            (Vec2){ dir.x * 350.0f, dir.y * 350.0f }, w->rng);
    }
}

/* ---- Projectile spawn paths ---------------------------------------- */

static void spawn_one_projectile(World *w, int mid, const Weapon *wpn,
                                 Vec2 origin, Vec2 dir, int weapon_id)
{
    ProjectileSpawn ps = {
        .kind = wpn->projectile_kind,
        .weapon_id = weapon_id,
        .owner_mech_id = mid,
        .owner_team = w->mechs[mid].team,
        .origin = origin,
        .velocity = (Vec2){ dir.x * wpn->projectile_speed_pxs,
                            dir.y * wpn->projectile_speed_pxs },
        .damage = wpn->damage,
        .aoe_radius = wpn->aoe_radius,
        .aoe_damage = wpn->aoe_damage,
        .aoe_impulse = wpn->aoe_impulse,
        .life = wpn->projectile_life_sec,
        .gravity_scale = wpn->projectile_grav_scale,
        .drag = wpn->projectile_drag,
        .bouncy = wpn->bouncy,
    };
    projectile_spawn(w, ps);
    /* One fire event per projectile (spread weapons emit one per
     * pellet, after the spread cone has been applied to dir). */
    weapons_record_fire(w, mid, weapon_id, origin, dir);
}

void weapons_spawn_projectiles(World *w, int mid, int weapon_id) {
    Mech *me = &w->mechs[mid];
    const Weapon *wpn = weapon_def(weapon_id);
    if (!wpn) return;

    Vec2 hand = mech_hand_pos(w, mid);
    Vec2 dir  = apply_self_bink(w, me, mech_aim_dir(w, mid));
    const WeaponSpriteDef *wsp = weapon_sprite_def(weapon_id);
    Vec2 origin = weapon_muzzle_world(hand, dir, wsp, wpn->muzzle_offset);

    if (wpn->fire == WFIRE_SPREAD) {
        int n = wpn->spread_pellets > 0 ? wpn->spread_pellets : 1;
        float cone = wpn->spread_cone_rad;
        for (int i = 0; i < n; ++i) {
            float r = ((float)pcg32_float01(w->rng) * 2.0f - 1.0f) * cone;
            float ca = cosf(r), sa = sinf(r);
            Vec2 d = { dir.x * ca - dir.y * sa,
                       dir.x * sa + dir.y * ca };
            spawn_one_projectile(w, mid, wpn, origin, d, weapon_id);
        }
    } else {
        spawn_one_projectile(w, mid, wpn, origin, dir, weapon_id);
    }

    /* Tracer-style FX from the muzzle so spectators see the fire even
     * before the projectile travels far. */
    Vec2 spark_end = { origin.x + dir.x * 80.0f,
                       origin.y + dir.y * 80.0f };
    fx_spawn_tracer(&w->fx, origin, spark_end);
    audio_play_at(audio_sfx_for_weapon(weapon_id), origin);

    /* Bink — projectile bink is applied as the projectile passes near
     * targets, but a small fire-time bink to anyone in the aim cone is
     * already useful. (Match the hitscan path for consistency.) */
    apply_bink_along_segment(w, mid, origin, spark_end, wpn->bink, 80.0f);
    if (wpn->self_bink > 0.0f) {
        float sign = ((pcg32_next(w->rng) & 1u) ? 1.0f : -1.0f);
        me->aim_bink += sign * wpn->self_bink;
    }

    /* Recoil. */
    int hand_idx = me->particle_base + PART_R_HAND;
    w->particles.pos_x[hand_idx] -= dir.x * wpn->recoil_impulse;
    w->particles.pos_y[hand_idx] -= dir.y * wpn->recoil_impulse;
    me->recoil_kick = 1.0f;
    me->fire_cooldown = wpn->fire_rate_sec;
    /* M6 P10 — projectile-spawn shake cut 0.04 → 0.015 (matches the
     * hitscan-fire reduction; the projectile's eventual *explosion*
     * still adds 0.4 unconditionally, which is the big-event shake the
     * user is OK with). */
    w->shake_intensity = fminf(1.0f, w->shake_intensity + 0.015f);
    for (int k = 0; k < 3; ++k) {
        fx_spawn_spark(&w->fx, origin,
            (Vec2){ dir.x * 350.0f, dir.y * 350.0f }, w->rng);
    }

    SHOT_LOG("t=%llu spawn_proj mech=%d wpn=%d kind=%d dir=(%.2f,%.2f) v=%.0f",
             (unsigned long long)w->tick, mid, weapon_id,
             wpn->projectile_kind, dir.x, dir.y, wpn->projectile_speed_pxs);
}

/* ---- Melee --------------------------------------------------------- */

void weapons_fire_melee(World *w, int mid, int weapon_id) {
    Mech *me = &w->mechs[mid];
    const Weapon *wpn = weapon_def(weapon_id);
    if (!wpn || wpn->fire != WFIRE_MELEE) return;

    Vec2 chest = mech_chest_pos(w, mid);
    Vec2 dir   = mech_aim_dir(w, mid);
    weapons_record_fire(w, mid, weapon_id, chest, dir);
    Vec2 end   = { chest.x + dir.x * wpn->range_px,
                   chest.y + dir.y * wpn->range_px };

    /* Find the closest mech bone the swing covers. */
    int   hit_mech = -1;
    int   hit_part = -1;
    float hit_t    = -1.0f;
    for (int i = 0; i < w->mech_count; ++i) {
        if (i == mid) continue;
        const Mech *t = &w->mechs[i];
        if (!t->alive) continue;
        for (int bi = 0; bi < NUM_BONES; ++bi) {
            int pa = t->particle_base + g_bones[bi].parent;
            int pb = t->particle_base + g_bones[bi].child;
            Vec2 va = { w->particles.pos_x[pa], w->particles.pos_y[pa] };
            Vec2 vb = { w->particles.pos_x[pb], w->particles.pos_y[pb] };
            float th = ray_seg_hit(chest, dir, wpn->range_px, va, vb, 8.0f);
            if (th < 0.0f) continue;
            if (hit_t < 0.0f || th < hit_t) {
                hit_t = th;
                hit_mech = i;
                hit_part = g_bones[bi].part_for_damage;
            }
        }
    }

    if (hit_mech >= 0 && w->authoritative) {
        Mech *t = &w->mechs[hit_mech];
        float dmg = wpn->damage;
        /* Backstab: dot of attacker's aim and victim's facing > 0.5. */
        Vec2 vd = { t->facing_left ? -1.0f : 1.0f, 0.0f };
        if (dir.x * vd.x + dir.y * vd.y > 0.5f) dmg *= 2.5f;
        t->last_killshot_weapon = weapon_id;
        mech_apply_damage(w, hit_mech, hit_part, dmg, dir, mid);
    }
    /* Audio: melee swing + flesh impact when the swing connects.
     * Played server-side and offline-solo; clients hear via
     * client_handle_fire_event for the swing and client_handle_hit_event
     * for the flesh hit. */
    audio_play_at(audio_sfx_for_weapon(weapon_id), chest);
    if (hit_mech >= 0) {
        Vec2 contact = { chest.x + dir.x * (hit_t * wpn->range_px),
                         chest.y + dir.y * (hit_t * wpn->range_px) };
        audio_play_at(SFX_HIT_FLESH, contact);
    }
    /* Visual swing — short tracer + sparks. */
    fx_spawn_tracer(&w->fx, chest, end);
    for (int k = 0; k < 4; ++k) {
        fx_spawn_spark(&w->fx, chest,
            (Vec2){ dir.x * 200.0f, dir.y * 200.0f }, w->rng);
    }
    me->fire_cooldown = wpn->fire_rate_sec;
}
