#include "mech_ik.h"

#include "mech.h"
#include "particle.h"
#include "physics.h"

#include <stdbool.h>

/* ---- 2-bone analytic IK -------------------------------------------- */

bool mech_ik_2bone(Vec2 s, Vec2 t, float L1, float L2,
                   float bend_sign, Vec2 *out_joint)
{
    const float EPS = 1e-4f;
    float dx = t.x - s.x;
    float dy = t.y - s.y;
    float d2 = dx*dx + dy*dy;
    float d  = sqrtf(d2);

    /* Out of reach: target farther than L1 + L2. Joint sits on the
     * s→t line at L1 from s — the chain extends straight toward `t`
     * but the end-effector falls short. */
    if (d >= L1 + L2 - EPS) {
        float inv = (d > EPS) ? 1.0f / d : 0.0f;
        out_joint->x = s.x + dx * inv * L1;
        out_joint->y = s.y + dy * inv * L1;
        return false;
    }
    /* Collapsed: target closer than |L1 - L2|. Same placement —
     * degenerate but stable straight chain pointed at t. */
    float min_d = fabsf(L1 - L2);
    if (d <= min_d + EPS) {
        float inv = (d > EPS) ? 1.0f / d : 0.0f;
        out_joint->x = s.x + dx * inv * L1;
        out_joint->y = s.y + dy * inv * L1;
        return false;
    }

    /* Normal case — law of cosines. `a` is the signed projection of
     * (j - s) onto the s→t unit; `h` is the perpendicular distance
     * from j to the s→t line. */
    float a  = (L1*L1 - L2*L2 + d2) / (2.0f * d);
    float h2 = L1*L1 - a*a;
    float h  = (h2 > 0.0f) ? sqrtf(h2) : 0.0f;

    float fx = dx / d, fy = dy / d;
    /* Right-perpendicular in screen coords (y-down): rotate forward
     * by +90° → (-fy, fx). bend_sign flips to the other side. */
    float px = -fy, py = fx;

    out_joint->x = s.x + a*fx + bend_sign * h * px;
    out_joint->y = s.y + a*fy + bend_sign * h * py;
    return true;
}

/* ---- Procedural pose function -------------------------------------- */
/*
 * Pure function from (pelvis, aim, anim_id, gait_phase, facing,
 * chassis, active_slot, dismember_mask, foregrip, grapple) to bone
 * positions. No history, no iteration. Implements §7 of
 * documents/m6/01-ik-and-pose-sync.md.
 *
 * Sign conventions (screen coords, y-down — rotate forward by +90°
 * gives right-perpendicular = (-fy, fx)):
 *   - Aim arm bend: `facing_left ? -1 : +1` places the elbow on the
 *     screen-down side of the aim line for a right-facing shooter
 *     (i.e., elbow drops below the shoulder behind the arm).
 *   - Foregrip arm bend: same as aim arm — both elbows drop below the
 *     respective arm's line for symmetric rifle hold.
 *   - Knee bend: `+1` for both legs — knees on the screen-down /
 *     forward side of the hip→foot line.
 */
void pose_compute(const PoseInputs *in, PoseBones out) {
    const Chassis *ch = mech_chassis((ChassisId)in->chassis_id);
    if (!ch) ch = mech_chassis(CHASSIS_TROOPER);

    Vec2 pelvis = in->pelvis;
    float face_dir = in->facing_left ? -1.0f : 1.0f;

    /* §7.1 — PELVIS is the anchor. */
    out[PART_PELVIS] = pelvis;

    /* §7.2 — Hips and shoulders. Numbers from build_pose.
     *
     * M6 — Body orientation differs across anims:
     *   STAND/RUN/JET/FALL/FIRE/CROUCH: torso extends UP from pelvis.
     *     CROUCH keeps the torso at FULL LENGTH and just bends the
     *     knees — shortening the torso violated the skeleton's
     *     cross-brace `PELVIS-L_SHOULDER` / `PELVIS-R_SHOULDER`
     *     distance constraints (rest set at standing height), which
     *     fired every tick and produced a tiny but compounding
     *     horizontal drift on the pelvis.
     *   PRONE: torso extends FORWARD horizontally from pelvis along
     *     the ground. The PELVIS-shoulder cross-brace WILL fight this
     *     orientation (rest assumes torso-up); see the new trade-off
     *     entry. For now we accept the resulting tug — prone is
     *     transient and the drift is along facing, not perpendicular. */
    bool is_prone  = (in->anim_id == ANIM_PRONE);
    bool is_crouch = (in->anim_id == ANIM_CROUCH);

    if (is_prone) {
        /* PRONE: rotate the entire skeleton 90° so the body's
         * "up axis" lies along the world's forward direction. With
         * face_dir = +1, STAND offset (ox, oy) becomes prone offset
         * (-oy * face_dir, ox * face_dir). face_dir = -1 mirrors it
         * for facing-left. This rotation preserves every inter-bone
         * distance — the cross-brace constraints
         * (`PELVIS-L_SHOULDER`, `PELVIS-R_SHOULDER`, `L_HIP-R_HIP`,
         * etc.) stay at their standing rest lengths, so the solver
         * doesn't tug the pelvis sideways while the player is prone. */
        #define PRONE_PT(ox, oy) (Vec2){                          \
            pelvis.x + (-(oy)) * face_dir,                        \
            pelvis.y + (ox) * face_dir                            \
        }
        out[PART_L_HIP]      = PRONE_PT(-7.0f, 0.0f);
        out[PART_R_HIP]      = PRONE_PT(+7.0f, 0.0f);
        out[PART_CHEST]      = PRONE_PT(0.0f, -ch->torso_h);
        out[PART_NECK]       = PRONE_PT(0.0f, -ch->torso_h - ch->neck_h * 0.5f);
        out[PART_HEAD]       = PRONE_PT(0.0f, -ch->torso_h - ch->neck_h - 8.0f);
        out[PART_L_SHOULDER] = PRONE_PT(-10.0f, -ch->torso_h + 4.0f);
        out[PART_R_SHOULDER] = PRONE_PT(+10.0f, -ch->torso_h + 4.0f);
        #undef PRONE_PT
    } else {
        out[PART_L_HIP] = (Vec2){ pelvis.x - 7, pelvis.y };
        out[PART_R_HIP] = (Vec2){ pelvis.x + 7, pelvis.y };
        /* Standing / crouch: full-height torso. Crouch is signaled by
         * the leg block below (knees bent) + the lower pelvis_y from
         * mech_post_physics_anchor — not by squashing the torso. */
        out[PART_L_SHOULDER] = (Vec2){ pelvis.x - 10, pelvis.y - ch->torso_h + 4 };
        out[PART_R_SHOULDER] = (Vec2){ pelvis.x + 10, pelvis.y - ch->torso_h + 4 };
        out[PART_CHEST] = (Vec2){ pelvis.x, pelvis.y - ch->torso_h };
        out[PART_NECK]  = (Vec2){ pelvis.x, pelvis.y - ch->torso_h - ch->neck_h * 0.5f };
        out[PART_HEAD]  = (Vec2){ pelvis.x, pelvis.y - ch->torso_h - ch->neck_h - 8.0f };

        /* Chassis quirks. */
        switch ((ChassisId)in->chassis_id) {
            case CHASSIS_SCOUT:
                out[PART_CHEST].x += face_dir * 2.0f;
                break;
            case CHASSIS_SNIPER:
                out[PART_HEAD].x  += face_dir * 2.0f;
                out[PART_HEAD].y  += 3.0f;
                break;
            case CHASSIS_HEAVY:
            case CHASSIS_ENGINEER:
            case CHASSIS_TROOPER:
            default:
                break;
        }
    }

    /* §7.4 — Right arm. Drives the aim chain unless:
     *   - mech is a practice dummy (no aim target — arm dangles);
     *   - Engineer holding secondary (tool/wrench, not rifle); or
     *   - R_ARM dismembered (free-flying Verlet runs the particles).
     * Grapple ATTACHED state overrides the aim target with the anchor
     * position (rope endpoint == hand, see §7.10). */
    bool dismember_r_arm = (in->dismember_mask & LIMB_R_ARM) != 0;
    bool engineer_secondary =
        (in->chassis_id == CHASSIS_ENGINEER && in->active_slot == 1);
    bool grapple_attached = (in->grapple_state == GRAPPLE_ATTACHED);

    if (grapple_attached) {
        /* R_HAND points TOWARD the anchor but is clamped to the arm's
         * chain reach. Placing the hand AT the anchor (which is
         * typically 200+ px away) stretches the R_ARM far beyond its
         * rest length and renders as a giraffe-arm distortion that
         * also breaks the IK elbow placement. The grapple rope
         * renderer draws a line from R_HAND to the anchor, which
         * already covers the visual "hand reaching for the rope." */
        Vec2 sho = out[PART_R_SHOULDER];
        Vec2 anchor = in->grapple_anchor;
        float arm_reach = ch->bone_arm + ch->bone_forearm;
        float dxh = anchor.x - sho.x;
        float dyh = anchor.y - sho.y;
        float d   = sqrtf(dxh*dxh + dyh*dyh);
        Vec2 hand;
        if (d <= arm_reach + 1e-3f) {
            hand = anchor;
        } else {
            float inv = arm_reach / d;
            hand = (Vec2){ sho.x + dxh * inv, sho.y + dyh * inv };
        }
        Vec2 elbow;
        mech_ik_2bone(sho, hand,
                      ch->bone_arm, ch->bone_forearm,
                      in->facing_left ? -1.0f : +1.0f, &elbow);
        out[PART_R_ELBOW] = elbow;
        out[PART_R_HAND]  = hand;
    } else if (!engineer_secondary && !dismember_r_arm && !in->is_dummy) {
        float arm_reach = ch->bone_arm + ch->bone_forearm;
        Vec2 hand = { out[PART_R_SHOULDER].x + in->aim_dir.x * arm_reach,
                      out[PART_R_SHOULDER].y + in->aim_dir.y * arm_reach };
        Vec2 elbow;
        mech_ik_2bone(out[PART_R_SHOULDER], hand,
                      ch->bone_arm, ch->bone_forearm,
                      in->facing_left ? -1.0f : +1.0f, &elbow);
        out[PART_R_ELBOW] = elbow;
        out[PART_R_HAND]  = hand;
    } else {
        /* Rest-dangle for Engineer-with-tool or severed arm. */
        out[PART_R_ELBOW] = (Vec2){ out[PART_R_SHOULDER].x + face_dir * 2,
                                     out[PART_R_SHOULDER].y + ch->bone_arm };
        out[PART_R_HAND]  = (Vec2){ out[PART_R_ELBOW].x + face_dir * 2,
                                     out[PART_R_ELBOW].y + ch->bone_forearm };
    }

    /* §7.5 — Left arm. Dangles by default (M1 baseline). If the caller
     * supplies a foregrip world position (two-handed weapon), IK toward
     * it. When the foregrip is past the chain's reach (the common case
     * — rifle is ~70 px from L_SHOULDER, chain ≈ 30 px), `mech_ik_2bone`
     * returns false and the elbow sits on the s→t ray at L1 — the arm
     * extends straight toward the foregrip, and we put L_HAND at the
     * chain's max-reach point along the same ray so the hand
     * geometrically lines up with the rifle silhouette. */
    bool dismember_l_arm = (in->dismember_mask & LIMB_L_ARM) != 0;
    bool drive_foregrip  = in->foregrip_world && !dismember_l_arm
                           && !engineer_secondary && !grapple_attached;
    if (drive_foregrip) {
        Vec2 fg = *in->foregrip_world;
        Vec2 elbow;
        bool reachable = mech_ik_2bone(out[PART_L_SHOULDER], fg,
                                       ch->bone_arm, ch->bone_forearm,
                                       in->facing_left ? -1.0f : +1.0f,
                                       &elbow);
        out[PART_L_ELBOW] = elbow;
        if (reachable) {
            out[PART_L_HAND] = fg;
        } else {
            /* Out-of-reach: L_HAND at L_SHOULDER + chain * unit(fg - L_SHOULDER). */
            float dx = fg.x - out[PART_L_SHOULDER].x;
            float dy = fg.y - out[PART_L_SHOULDER].y;
            float d  = sqrtf(dx*dx + dy*dy);
            float chain = ch->bone_arm + ch->bone_forearm;
            if (d > 1e-4f) {
                float inv = chain / d;
                out[PART_L_HAND] = (Vec2){ out[PART_L_SHOULDER].x + dx * inv,
                                            out[PART_L_SHOULDER].y + dy * inv };
            } else {
                out[PART_L_HAND] = out[PART_L_SHOULDER];
            }
        }
    } else {
        out[PART_L_ELBOW] = (Vec2){ out[PART_L_SHOULDER].x - face_dir * 2,
                                     out[PART_L_SHOULDER].y + ch->bone_arm };
        out[PART_L_HAND]  = (Vec2){ out[PART_L_ELBOW].x - face_dir * 2,
                                     out[PART_L_ELBOW].y + ch->bone_forearm };
    }

    /* §7.6 / 7.7 / 7.8 — Legs by anim_id. */
    Vec2 lhip = out[PART_L_HIP];
    Vec2 rhip = out[PART_R_HIP];

    if (in->anim_id == ANIM_RUN) {
        /* §7.7 — RUN gait. Constants match build_pose so the gait
         * shape stays the same. Phase is normalized into [0, 1) before
         * splitting the foot pair 180° apart in the cycle. */
        const float STRIDE  = 28.0f;
        const float LIFT_H  = 9.0f;
        float foot_y_ground = lhip.y + ch->bone_thigh + ch->bone_shin;
        float dir   = in->facing_left ? -1.0f : 1.0f;
        float front =  STRIDE * 0.5f * dir;
        float back  = -STRIDE * 0.5f * dir;

        float p_l = in->gait_phase;
        if (p_l < 0.0f)  p_l -= floorf(p_l);
        if (p_l >= 1.0f) p_l -= floorf(p_l);
        float p_r = p_l + 0.5f;
        if (p_r >= 1.0f) p_r -= 1.0f;

        Vec2 lfoot, rfoot;
        if (p_l < 0.5f) {
            /* Stance: foot slides back along ground. */
            float u = p_l * 2.0f;
            lfoot = (Vec2){ lhip.x + front + (back - front) * u, foot_y_ground };
        } else {
            /* Swing: foot lifts in an arc, comes back to front. */
            float u = (p_l - 0.5f) * 2.0f;
            lfoot = (Vec2){ lhip.x + back + (front - back) * u,
                            foot_y_ground - LIFT_H * sinf(u * PI) };
        }
        if (p_r < 0.5f) {
            float u = p_r * 2.0f;
            rfoot = (Vec2){ rhip.x + front + (back - front) * u, foot_y_ground };
        } else {
            float u = (p_r - 0.5f) * 2.0f;
            rfoot = (Vec2){ rhip.x + back + (front - back) * u,
                            foot_y_ground - LIFT_H * sinf(u * PI) };
        }

        Vec2 lknee, rknee;
        mech_ik_2bone(lhip, lfoot, ch->bone_thigh, ch->bone_shin, +1.0f, &lknee);
        mech_ik_2bone(rhip, rfoot, ch->bone_thigh, ch->bone_shin, +1.0f, &rknee);
        out[PART_L_KNEE] = lknee;
        out[PART_L_FOOT] = lfoot;
        out[PART_R_KNEE] = rknee;
        out[PART_R_FOOT] = rfoot;
    } else if (in->anim_id == ANIM_JET) {
        /* §7.8 — JET legs swept back opposite to facing. */
        float dir = in->facing_left ? 1.0f : -1.0f;   /* opposite to facing */
        Vec2 lfoot = { lhip.x + dir * 12,
                       lhip.y + ch->bone_thigh + ch->bone_shin - 4 };
        Vec2 rfoot = { rhip.x + dir * 12,
                       rhip.y + ch->bone_thigh + ch->bone_shin - 4 };
        Vec2 lknee, rknee;
        mech_ik_2bone(lhip, lfoot, ch->bone_thigh, ch->bone_shin, +1.0f, &lknee);
        mech_ik_2bone(rhip, rfoot, ch->bone_thigh, ch->bone_shin, +1.0f, &rknee);
        out[PART_L_KNEE] = lknee;
        out[PART_L_FOOT] = lfoot;
        out[PART_R_KNEE] = rknee;
        out[PART_R_FOOT] = rfoot;
    } else if (is_prone) {
        /* PRONE legs — rotate the STAND leg offsets by 90° so they
         * extend BACKWARD from the pelvis along the ground. Each
         * hip→knee and knee→foot distance stays at its rest length
         * (thigh / shin), and the rotated L_HIP / R_HIP positions
         * already match the cross-brace constraint above.
         *
         * STAND L_KNEE = lhip + (-1, thigh).
         * STAND L_FOOT = lhip + (-1, thigh + shin).
         * Rotated by face_dir as in the torso block: (ox, oy) →
         * (-oy * face_dir, ox * face_dir). */
        #define PRONE_FROM_HIP(hip, ox, oy) (Vec2){             \
            (hip).x + (-(oy)) * face_dir,                        \
            (hip).y + (ox) * face_dir                            \
        }
        out[PART_L_KNEE] = PRONE_FROM_HIP(lhip, -1.0f, ch->bone_thigh);
        out[PART_L_FOOT] = PRONE_FROM_HIP(lhip, -1.0f, ch->bone_thigh + ch->bone_shin);
        out[PART_R_KNEE] = PRONE_FROM_HIP(rhip, +1.0f, ch->bone_thigh);
        out[PART_R_FOOT] = PRONE_FROM_HIP(rhip, +1.0f, ch->bone_thigh + ch->bone_shin);
        #undef PRONE_FROM_HIP
    } else if (is_crouch) {
        /* M6 — CROUCH: pelvis is lifted to ~55% of chain length above
         * the feet by post_physics_anchor, so hip→foot at chain*0.55
         * gives the leg the right compression. IK then places the
         * knee on the FORWARD side (facing direction) for the
         * "squatted" silhouette. The IK output is at rest length, so
         * the Verlet constraint doesn't pull the pelvis backward like
         * a raw forced-bend would. */
        float crouch_h = (ch->bone_thigh + ch->bone_shin) * 0.55f;
        Vec2 lfoot = { lhip.x + face_dir * 2.0f, lhip.y + crouch_h };
        Vec2 rfoot = { rhip.x + face_dir * 2.0f, rhip.y + crouch_h };
        Vec2 lknee, rknee;
        mech_ik_2bone(lhip, lfoot, ch->bone_thigh, ch->bone_shin,
                      in->facing_left ? +1.0f : -1.0f, &lknee);
        mech_ik_2bone(rhip, rfoot, ch->bone_thigh, ch->bone_shin,
                      in->facing_left ? +1.0f : -1.0f, &rknee);
        out[PART_L_KNEE] = lknee;
        out[PART_L_FOOT] = lfoot;
        out[PART_R_KNEE] = rknee;
        out[PART_R_FOOT] = rfoot;
    } else {
        /* §7.6 — STAND / FALL / FIRE: straight down with a slight bias. */
        out[PART_L_KNEE] = (Vec2){ lhip.x - 1, lhip.y + ch->bone_thigh };
        out[PART_L_FOOT] = (Vec2){ lhip.x - 1, lhip.y + ch->bone_thigh + ch->bone_shin };
        out[PART_R_KNEE] = (Vec2){ rhip.x + 1, rhip.y + ch->bone_thigh };
        out[PART_R_FOOT] = (Vec2){ rhip.x + 1, rhip.y + ch->bone_thigh + ch->bone_shin };
    }
}

/* ---- Write computed bones into the particle pool ------------------- */

/* Map PART_* → LIMB_* bit for dismember gating. PART_PELVIS / CHEST /
 * NECK / HEAD belong to the torso/head; only HEAD has a corresponding
 * dismember bit (LIMB_HEAD), and even then only PART_HEAD is severed
 * (NECK remains as the stump anchor). The L_ARM / R_ARM / L_LEG /
 * R_LEG limbs each take three particles per limb. */
static uint8_t limb_bit_for_part(int part) {
    switch (part) {
        case PART_HEAD:       return LIMB_HEAD;
        case PART_L_SHOULDER:
        case PART_L_ELBOW:
        case PART_L_HAND:     return LIMB_L_ARM;
        case PART_R_SHOULDER:
        case PART_R_ELBOW:
        case PART_R_HAND:     return LIMB_R_ARM;
        case PART_L_HIP:
        case PART_L_KNEE:
        case PART_L_FOOT:     return LIMB_L_LEG;
        case PART_R_HIP:
        case PART_R_KNEE:
        case PART_R_FOOT:     return LIMB_R_LEG;
        default:              return 0;
    }
}

void pose_write_to_particles(World *w, int mech_id, const PoseBones bones) {
    if (mech_id < 0 || mech_id >= w->mech_count) return;
    Mech *m = &w->mechs[mech_id];
    if (!m->alive) return;

    ParticlePool *p = &w->particles;
    int base = m->particle_base;
    uint8_t mask = m->dismember_mask;

    /* `physics_translate_kinematic_swept` does TWO things we need:
     *   1. Translate pos AND prev by the same delta — preserves
     *      `(pos - prev) = velocity`. Critical for gravity / jet /
     *      grapple swing to accelerate properly across ticks (the
     *      old `prev = pos` write killed velocity each tick).
     *   2. Sweep the move against solid tiles — if a bone's pose
     *      target would land it inside a wall, clamp the move so it
     *      stops just shy of the tile surface. Without the sweep,
     *      pose_compute's deterministic offsets from the pelvis can
     *      stuff body bones into a wall the mech is hugging (which
     *      then stays clipped because pose runs last and overwrites
     *      whatever physics-side collision would have pushed out). */
    for (int i = 0; i < PART_COUNT; ++i) {
        uint8_t bit = limb_bit_for_part(i);
        if (bit && (mask & bit)) {
            /* Dismembered: leave free-flying Verlet to evolve the
             * particle (it's no longer in the live skeleton). */
            continue;
        }
        int idx = base + i;
        float dx = bones[i].x - p->pos_x[idx];
        float dy = bones[i].y - p->pos_y[idx];
        physics_translate_kinematic_swept(p, &w->level, idx, dx, dy);
    }
}
