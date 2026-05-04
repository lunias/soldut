#include "snapshot.h"

#include "log.h"
#include "mech.h"
#include "particle.h"
#include "physics.h"
#include "weapons.h"

#include <math.h>
#include <string.h>

/* ---- Quantization helpers ----------------------------------------- */
/*
 * pos: 1/8 px → int16 covers ±4096 px, fine for our 3200 px world.
 * vel: 1/16 px/tick (per-tick velocity, post-integrate). 16-bit
 *      signed covers ±2048 px/tick — way more than we'd ever see.
 * angle: uint16 fraction of 2π.
 */

static int16_t quant_pos(float p) {
    float q = p * 8.0f;
    if (q >  32760.0f) q =  32760.0f;
    if (q < -32760.0f) q = -32760.0f;
    return (int16_t)(q < 0 ? q - 0.5f : q + 0.5f);
}
static float dequant_pos(int16_t q) { return (float)q / 8.0f; }

static int16_t quant_vel(float v) {
    float q = v * 16.0f;
    if (q >  32760.0f) q =  32760.0f;
    if (q < -32760.0f) q = -32760.0f;
    return (int16_t)(q < 0 ? q - 0.5f : q + 0.5f);
}
static float dequant_vel(int16_t q) { return (float)q / 16.0f; }

static uint16_t quant_angle(float a) {
    /* Normalize to [0, 2π). */
    const float TWO_PI = 6.28318530718f;
    while (a < 0.0f)      a += TWO_PI;
    while (a >= TWO_PI)   a -= TWO_PI;
    float q = (a / TWO_PI) * 65536.0f;
    if (q < 0.0f) q = 0.0f;
    if (q > 65535.0f) q = 65535.0f;
    return (uint16_t)q;
}
static float dequant_angle(uint16_t q) {
    return ((float)q / 65536.0f) * 6.28318530718f;
}

static uint8_t quant_health(float h, float h_max) {
    if (h_max <= 0.0f) return 0;
    float r = h / h_max;
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return (uint8_t)(r * 255.0f + 0.5f);
}

/* ---- Capture (server-side): live World → SnapshotFrame ------------ */

void snapshot_capture(const World *w, SnapshotFrame *out, uint16_t ack_input_seq) {
    memset(out, 0, sizeof *out);
    out->header.server_tick     = w->tick;
    out->header.server_time_ms  = (uint32_t)(w->tick * 1000ull / 60ull);
    out->header.baseline_tick   = 0;
    out->header.ack_input_seq   = ack_input_seq;

    int n = 0;
    for (int i = 0; i < w->mech_count; ++i) {
        const Mech *m = &w->mechs[i];
        EntitySnapshot *e = &out->ents[n++];
        e->mech_id = (uint16_t)i;

        int p = m->particle_base + PART_PELVIS;
        float px = w->particles.pos_x[p];
        float py = w->particles.pos_y[p];
        float vx = w->particles.pos_x[p] - w->particles.prev_x[p];
        float vy = w->particles.pos_y[p] - w->particles.prev_y[p];
        e->pos_x_q = quant_pos(px);
        e->pos_y_q = quant_pos(py);
        e->vel_x_q = quant_vel(vx);
        e->vel_y_q = quant_vel(vy);

        float aim_dx = m->aim_world.x - px;
        float aim_dy = m->aim_world.y - py;
        e->aim_q   = quant_angle(atan2f(aim_dy, aim_dx));
        e->torso_q = e->aim_q;

        e->health   = quant_health(m->health, m->health_max);
        e->armor    = quant_health(m->armor_hp, m->armor_hp_max > 0.0f ? m->armor_hp_max : 1.0f);
        e->weapon_id= (uint8_t)m->weapon_id;
        e->ammo     = (uint8_t)(m->ammo > 255 ? 255 : (m->ammo < 0 ? 0 : m->ammo));

        uint8_t bits = 0;
        if (m->alive)              bits |= SNAP_STATE_ALIVE;
        if (m->grounded)           bits |= SNAP_STATE_GROUNDED;
        if (m->facing_left)        bits |= SNAP_STATE_FACING_LEFT;
        if (m->anim_id == ANIM_JET)  bits |= SNAP_STATE_JET;
        if (m->anim_id == ANIM_FIRE) bits |= SNAP_STATE_FIRE;
        if (m->reload_timer > 0.0f)  bits |= SNAP_STATE_RELOAD;
        e->state_bits = bits;

        e->team      = (uint8_t)m->team;
        e->limb_bits = (uint16_t)m->dismember_mask;
        e->chassis_id     = (uint8_t)m->chassis_id;
        e->armor_id       = (uint8_t)m->armor_id;
        e->jetpack_id     = (uint8_t)m->jetpack_id;
        e->secondary_id   = (uint8_t)m->secondary_id;
        e->ammo_secondary = (uint8_t)(m->ammo_secondary > 255 ? 255
                                : (m->ammo_secondary < 0 ? 0 : m->ammo_secondary));
    }
    out->ent_count = n;
    out->valid = true;
}

/* ---- Encode: SnapshotFrame → wire bytes --------------------------- */

int snapshot_encode(const SnapshotFrame *cur,
                    const SnapshotFrame *baseline,
                    uint8_t *buf, int buf_cap)
{
    if (buf_cap < (int)sizeof(SnapshotHeader)) return 0;
    uint8_t *p = buf;
    uint8_t *end = buf + buf_cap;

    SnapshotHeader h = cur->header;
    h.baseline_tick = (baseline && baseline->valid) ? baseline->header.server_tick : 0;
    h.entity_count = (uint16_t)cur->ent_count;
    /* Header — packed wire layout. */
    /* server_tick (8) */
    for (int i = 0; i < 8; ++i) *p++ = (uint8_t)(h.server_tick >> (i*8));
    /* server_time_ms (4) */
    for (int i = 0; i < 4; ++i) *p++ = (uint8_t)(h.server_time_ms >> (i*8));
    /* baseline_tick (8) */
    for (int i = 0; i < 8; ++i) *p++ = (uint8_t)(h.baseline_tick >> (i*8));
    /* ack_input_seq (2) */
    *p++ = (uint8_t)h.ack_input_seq;
    *p++ = (uint8_t)(h.ack_input_seq >> 8);
    /* entity_count (2) */
    *p++ = (uint8_t)h.entity_count;
    *p++ = (uint8_t)(h.entity_count >> 8);

    /* For M2 we ship full snapshots (no delta). The baseline_tick is
     * always 0 on the wire even if the caller passed a baseline; we
     * leave the field as a forward-compat hook. (See TRADE_OFFS.md
     * "no snapshot delta encoding at M2".) */
    (void)baseline;

    for (int i = 0; i < cur->ent_count; ++i) {
        if (p + ENTITY_SNAPSHOT_WIRE_BYTES > end) {
            LOG_E("snapshot_encode: buffer overflow at ent %d/%d",
                  i, cur->ent_count);
            return 0;
        }
        const EntitySnapshot *e = &cur->ents[i];
        /* Pack the EntitySnapshot fields explicitly (don't rely on
         * the C struct layout, which can pad differently across
         * compilers). 22 bytes total. */
        /* mech_id(2) */
        *p++ = (uint8_t)e->mech_id; *p++ = (uint8_t)(e->mech_id >> 8);
        /* pos_x_q(2), pos_y_q(2), vel_x_q(2), vel_y_q(2) */
        *p++ = (uint8_t)e->pos_x_q; *p++ = (uint8_t)((uint16_t)e->pos_x_q >> 8);
        *p++ = (uint8_t)e->pos_y_q; *p++ = (uint8_t)((uint16_t)e->pos_y_q >> 8);
        *p++ = (uint8_t)e->vel_x_q; *p++ = (uint8_t)((uint16_t)e->vel_x_q >> 8);
        *p++ = (uint8_t)e->vel_y_q; *p++ = (uint8_t)((uint16_t)e->vel_y_q >> 8);
        /* aim_q(2), torso_q(2) */
        *p++ = (uint8_t)e->aim_q;   *p++ = (uint8_t)(e->aim_q >> 8);
        *p++ = (uint8_t)e->torso_q; *p++ = (uint8_t)(e->torso_q >> 8);
        /* health, armor, weapon_id, ammo, state_bits, team */
        *p++ = e->health;
        *p++ = e->armor;
        *p++ = e->weapon_id;
        *p++ = e->ammo;
        *p++ = e->state_bits;
        *p++ = e->team;
        /* limb_bits(2) */
        *p++ = (uint8_t)e->limb_bits; *p++ = (uint8_t)(e->limb_bits >> 8);
        /* loadout (M3): chassis, armor, jetpack, secondary, ammo_secondary */
        *p++ = e->chassis_id;
        *p++ = e->armor_id;
        *p++ = e->jetpack_id;
        *p++ = e->secondary_id;
        *p++ = e->ammo_secondary;
    }

    return (int)(p - buf);
}

/* ---- Decode ------------------------------------------------------- */

bool snapshot_decode(const uint8_t *buf, int len,
                     const SnapshotFrame *baseline,
                     SnapshotFrame *out)
{
    (void)baseline;
    if (len < SNAPSHOT_HEADER_WIRE_BYTES) return false;
    memset(out, 0, sizeof *out);
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;

    /* Header */
    uint64_t st = 0;
    for (int i = 0; i < 8; ++i) st |= (uint64_t)(*p++) << (i*8);
    out->header.server_tick = st;
    uint32_t sm = 0;
    for (int i = 0; i < 4; ++i) sm |= (uint32_t)(*p++) << (i*8);
    out->header.server_time_ms = sm;
    uint64_t bt = 0;
    for (int i = 0; i < 8; ++i) bt |= (uint64_t)(*p++) << (i*8);
    out->header.baseline_tick = bt;
    uint16_t ack;
    ack = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
    out->header.ack_input_seq = ack;
    uint16_t cnt;
    cnt = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
    out->header.entity_count = cnt;

    if (cnt > MAX_MECHS) {
        LOG_E("snapshot_decode: count %u > MAX_MECHS", (unsigned)cnt);
        return false;
    }
    if (end - p < (int)cnt * ENTITY_SNAPSHOT_WIRE_BYTES) {
        LOG_E("snapshot_decode: short body (%d left, need %d)",
              (int)(end - p), (int)cnt * ENTITY_SNAPSHOT_WIRE_BYTES);
        return false;
    }

    for (int i = 0; i < cnt; ++i) {
        EntitySnapshot *e = &out->ents[i];
        e->mech_id  = (uint16_t)(p[0] | (p[1] << 8));   p += 2;
        e->pos_x_q  = (int16_t)((uint16_t)(p[0] | (p[1] << 8))); p += 2;
        e->pos_y_q  = (int16_t)((uint16_t)(p[0] | (p[1] << 8))); p += 2;
        e->vel_x_q  = (int16_t)((uint16_t)(p[0] | (p[1] << 8))); p += 2;
        e->vel_y_q  = (int16_t)((uint16_t)(p[0] | (p[1] << 8))); p += 2;
        e->aim_q    = (uint16_t)(p[0] | (p[1] << 8));   p += 2;
        e->torso_q  = (uint16_t)(p[0] | (p[1] << 8));   p += 2;
        e->health   = *p++;
        e->armor    = *p++;
        e->weapon_id= *p++;
        e->ammo     = *p++;
        e->state_bits = *p++;
        e->team       = *p++;
        e->limb_bits  = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
        e->chassis_id     = *p++;
        e->armor_id       = *p++;
        e->jetpack_id     = *p++;
        e->secondary_id   = *p++;
        e->ammo_secondary = *p++;
    }
    out->ent_count = cnt;
    out->valid = true;
    return true;
}

/* ---- Apply: write SnapshotFrame into the local World -------------- */

/* When the client sees a mech_id in the snapshot it doesn't have
 * locally yet, it spawns one at the snapshot position. The loadout in
 * the snapshot tells us which chassis/armor/jet/secondary to spawn so
 * the local rendering and physics match the server. */
static int ensure_mech_slot(World *w, int desired_id, Vec2 spawn,
                            int team, const EntitySnapshot *e)
{
    while (w->mech_count <= desired_id) {
        MechLoadout lo = mech_default_loadout();
        if (e) {
            lo.chassis_id   = e->chassis_id   < CHASSIS_COUNT ? e->chassis_id   : CHASSIS_TROOPER;
            lo.armor_id     = e->armor_id     < ARMOR_COUNT   ? e->armor_id     : ARMOR_NONE;
            lo.jetpack_id   = e->jetpack_id   < JET_COUNT     ? e->jetpack_id   : JET_NONE;
            lo.primary_id   = e->weapon_id    < WEAPON_COUNT  ? e->weapon_id    : WEAPON_PULSE_RIFLE;
            lo.secondary_id = e->secondary_id < WEAPON_COUNT  ? e->secondary_id : WEAPON_SIDEARM;
        }
        int got = mech_create_loadout(w, lo, spawn, team, /*is_dummy*/false);
        if (got < 0) return -1;
    }
    return desired_id;
}

void snapshot_apply(World *w, const SnapshotFrame *frame) {
    if (!frame || !frame->valid) return;

    /* Mark all current mechs "stale"; we'll un-stale anything the
     * snapshot mentions. Anything still stale at the end gets killed. */
    bool seen[MAX_MECHS] = {false};

    int local_id = w->local_mech_id;

    for (int i = 0; i < frame->ent_count; ++i) {
        const EntitySnapshot *e = &frame->ents[i];
        Vec2 spawn_pos = { dequant_pos(e->pos_x_q), dequant_pos(e->pos_y_q) };
        int mid = ensure_mech_slot(w, (int)e->mech_id, spawn_pos, (int)e->team, e);
        if (mid < 0) continue;
        seen[mid] = true;

        Mech *m = &w->mechs[mid];
        ParticlePool *pp = &w->particles;
        bool is_local = (mid == local_id);

        /* Translate the whole skeleton kinematically so the mech
         * "snaps" to the server position.
         *
         * For the LOCAL mech: full snap. Reconciliation (in
         * reconcile.c) then replays unacked inputs to bring the body
         * forward to "now". Anything else would break that contract.
         *
         * For REMOTE mechs: a hard snap every snapshot looks like
         * visible jitter to the user (each snapshot is ~33 ms apart;
         * gravity + stale-input simulation between snapshots drifts
         * the body, then we snap it back). Smooth the correction
         * with a 35% lerp toward the server position — the body
         * still tracks the server but without the per-snapshot
         * pop. Over a few snapshots we converge. This is a poor-
         * man's snapshot interpolation; full
         * "render-100-ms-in-the-past" interp is M2 carry-forward
         * (see TRADE_OFFS.md → "no client-side mid-tick interp").
         *
         * Spawn case: when ensure_mech_slot has just created this
         * mech, the existing position equals new_px/new_py so dx/dy
         * are zero — no lerp damping concern. */
        float old_px = pp->pos_x[m->particle_base + PART_PELVIS];
        float old_py = pp->pos_y[m->particle_base + PART_PELVIS];
        float new_px = dequant_pos(e->pos_x_q);
        float new_py = dequant_pos(e->pos_y_q);
        float dx_full = new_px - old_px;
        float dy_full = new_py - old_py;
        float lerp = is_local ? 1.0f : 0.35f;
        /* For very large remote-mech corrections (>200 px — likely a
         * teleport / round-start respawn) skip the lerp and snap
         * fully. Without this a respawn would take seconds to slide
         * into position. */
        if (!is_local && (dx_full * dx_full + dy_full * dy_full) > 200.0f * 200.0f) {
            lerp = 1.0f;
        }
        float dx = dx_full * lerp;
        float dy = dy_full * lerp;
        for (int part = 0; part < PART_COUNT; ++part) {
            int idx = m->particle_base + part;
            physics_translate_kinematic(pp, idx, dx, dy);
        }

        /* Per-tick velocity. We set EVERY particle (not just the
         * pelvis) to the snapshot's pelvis velocity. The old code
         * only touched the pelvis, leaving the rest of the skeleton
         * with whatever velocity it had before the snap — the
         * mismatch made the constraint solver fight to reconcile
         * shapes between particles moving at different rates, which
         * the user saw as a per-tick jitter. With every particle
         * synced to the same body-frame velocity the skeleton moves
         * as a rigid unit between snapshots and the constraints
         * just maintain shape, not bleed energy. (The local-mech
         * pose drive in mech_step_drive will overwrite per-particle
         * velocities for the next tick anyway, so there's no
         * downstream side effect.) */
        float vx = dequant_vel(e->vel_x_q);
        float vy = dequant_vel(e->vel_y_q);
        for (int part = 0; part < PART_COUNT; ++part) {
            int idx = m->particle_base + part;
            physics_set_velocity_x(pp, idx, vx);
            physics_set_velocity_y(pp, idx, vy);
        }

        /* Aim: reconstruct from the angle. The pose drive needs an
         * aim_world point, not an angle; pick a long-range point. */
        float ang = dequant_angle(e->aim_q);
        m->aim_world = (Vec2){ new_px + cosf(ang) * 200.0f,
                               new_py + sinf(ang) * 200.0f };

        /* Combat & state. Capture pre-snapshot health so we can
         * spawn local FX (blood / sparks) when the server reports a
         * hit — those FX live in the client's local fx pool and
         * aren't carried in the snapshot wire format. Without this
         * the client sees no blood when remote players are damaged
         * (only the host's view shows hits). */
        float prev_health = m->health;
        m->health      = (e->health  / 255.0f) * m->health_max;
        if (m->armor_hp_max > 0.0f) {
            m->armor_hp = (e->armor / 255.0f) * m->armor_hp_max;
        }
        m->weapon_id     = e->weapon_id;
        m->ammo          = e->ammo;
        m->team          = e->team;
        m->secondary_id  = e->secondary_id;
        m->ammo_secondary= e->ammo_secondary;
        /* Active slot: derived from which weapon_id matches. */
        m->active_slot   = (e->weapon_id == m->primary_id) ? 0 : 1;
        bool was_alive = m->alive;
        m->alive       = (e->state_bits & SNAP_STATE_ALIVE) != 0;
        m->grounded    = (e->state_bits & SNAP_STATE_GROUNDED) != 0;
        m->facing_left = (e->state_bits & SNAP_STATE_FACING_LEFT) != 0;
        if (e->state_bits & SNAP_STATE_JET) m->anim_id = ANIM_JET;
        else if (m->grounded)               m->anim_id = ANIM_STAND;
        else                                 m->anim_id = ANIM_FALL;

        /* Health decreased → server registered a hit; mirror the
         * blood/spark FX locally so the client sees the same visual
         * feedback as the host. Spawn count scales loosely with
         * damage (~4 blood per ~5 % health lost), capped to keep
         * the fx pool from saturating on multi-hit bursts.
         *
         * Position: at the chest, with a small spray direction (we
         * don't know which side the bullet came from on the wire,
         * so default upward + outward is a reasonable proxy). */
        if (m->alive && m->health < prev_health) {
            float dmg = prev_health - m->health;
            int n_blood = (int)(dmg / 4.0f) + 2;
            if (n_blood > 12) n_blood = 12;
            int chest = m->particle_base + PART_CHEST;
            Vec2 at = { pp->pos_x[chest], pp->pos_y[chest] };
            Vec2 spray = { (m->facing_left ? 60.0f : -60.0f), -40.0f };
            for (int k = 0; k < n_blood; ++k)
                fx_spawn_blood(&w->fx, at, spray, w->rng);
            for (int k = 0; k < 2; ++k)
                fx_spawn_spark(&w->fx, at, spray, w->rng);
        }

        if (was_alive && !m->alive) {
            /* Local cosmetic kill — drop the pose drive so render
             * shows ragdoll. Also spawn the death blood fountain so
             * the client matches the host's visual on kill. */
            for (int s = 0; s < PART_COUNT; ++s) m->pose_strength[s] = 0.0f;
            int chest = m->particle_base + PART_CHEST;
            Vec2 at = { pp->pos_x[chest], pp->pos_y[chest] };
            Vec2 base_dir = { (m->facing_left ? 100.0f : -100.0f), -120.0f };
            for (int k = 0; k < 32; ++k)
                fx_spawn_blood(&w->fx, at, base_dir, w->rng);
            for (int k = 0; k < 8; ++k)
                fx_spawn_spark(&w->fx, at, base_dir, w->rng);
        }

        /* Apply dismemberment by deactivating constraints in the new
         * masks that weren't already inactive locally. M1 only ships
         * left-arm dismemberment; the same predicate works. */
        m->dismember_mask = (uint8_t)e->limb_bits;
    }

    /* Kill anything not in this snapshot. */
    for (int i = 0; i < w->mech_count; ++i) {
        if (!seen[i]) {
            w->mechs[i].alive = false;
        }
    }
}

/* ---- Lag-compensation history ------------------------------------ */

void snapshot_record_lag_hist(World *w, int mech_id) {
    if (mech_id < 0 || mech_id >= w->mech_count) return;
    Mech *m = &w->mechs[mech_id];
    int slot = m->lag_hist_head % LAG_HIST_TICKS;
    BoneHistFrame *f = &m->lag_hist[slot];
    f->tick  = w->tick;
    f->valid = true;
    for (int p = 0; p < PART_COUNT; ++p) {
        int idx = m->particle_base + p;
        f->pos_x[p] = w->particles.pos_x[idx];
        f->pos_y[p] = w->particles.pos_y[idx];
    }
    m->lag_hist_head = (m->lag_hist_head + 1) % LAG_HIST_TICKS;
}

bool snapshot_lag_lookup(const World *w, int target_mech_id,
                         uint64_t target_tick,
                         float *out_x, float *out_y)
{
    if (target_mech_id < 0 || target_mech_id >= w->mech_count) return false;
    const Mech *m = &w->mechs[target_mech_id];
    /* Find the frame whose tick is closest to target_tick (within
     * LAG_HIST_TICKS). */
    const BoneHistFrame *best = NULL;
    uint64_t best_diff = (uint64_t)-1;
    for (int i = 0; i < LAG_HIST_TICKS; ++i) {
        const BoneHistFrame *f = &m->lag_hist[i];
        if (!f->valid) continue;
        uint64_t diff = f->tick > target_tick ? (f->tick - target_tick)
                                              : (target_tick - f->tick);
        if (diff < best_diff) { best_diff = diff; best = f; }
    }
    if (!best || best_diff > LAG_HIST_TICKS) return false;
    for (int p = 0; p < PART_COUNT; ++p) {
        out_x[p] = best->pos_x[p];
        out_y[p] = best->pos_y[p];
    }
    return true;
}
