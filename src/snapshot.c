#include "snapshot.h"

#include "log.h"
#include "mech.h"
#include "physics.h"

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
        e->armor    = quant_health(m->armor,  100.0f);
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
    }
    out->ent_count = cnt;
    out->valid = true;
    return true;
}

/* ---- Apply: write SnapshotFrame into the local World -------------- */

/* When the client sees a mech_id in the snapshot it doesn't have
 * locally yet, it spawns one at the snapshot position. (Server's
 * INITIAL_STATE delivery uses this.) */
static int ensure_mech_slot(World *w, int desired_id, Vec2 spawn, int team) {
    while (w->mech_count <= desired_id) {
        int got = mech_create(w, CHASSIS_TROOPER, spawn, team, /*is_dummy*/false);
        if (got < 0) return -1;
    }
    return desired_id;
}

void snapshot_apply(World *w, const SnapshotFrame *frame) {
    if (!frame || !frame->valid) return;

    /* Mark all current mechs "stale"; we'll un-stale anything the
     * snapshot mentions. Anything still stale at the end gets killed. */
    bool seen[MAX_MECHS] = {false};

    for (int i = 0; i < frame->ent_count; ++i) {
        const EntitySnapshot *e = &frame->ents[i];
        Vec2 spawn_pos = { dequant_pos(e->pos_x_q), dequant_pos(e->pos_y_q) };
        int mid = ensure_mech_slot(w, (int)e->mech_id, spawn_pos, (int)e->team);
        if (mid < 0) continue;
        seen[mid] = true;

        Mech *m = &w->mechs[mid];
        ParticlePool *pp = &w->particles;

        /* Translate the whole skeleton kinematically so the mech
         * "snaps" to the server position. We lerp internally during
         * smoothing; here we do the hard write. */
        float old_px = pp->pos_x[m->particle_base + PART_PELVIS];
        float old_py = pp->pos_y[m->particle_base + PART_PELVIS];
        float new_px = dequant_pos(e->pos_x_q);
        float new_py = dequant_pos(e->pos_y_q);
        float dx = new_px - old_px;
        float dy = new_py - old_py;
        for (int part = 0; part < PART_COUNT; ++part) {
            int idx = m->particle_base + part;
            physics_translate_kinematic(pp, idx, dx, dy);
        }

        /* Set per-tick velocity so Verlet picks the right (pos - prev). */
        float vx = dequant_vel(e->vel_x_q);
        float vy = dequant_vel(e->vel_y_q);
        int pelv = m->particle_base + PART_PELVIS;
        physics_set_velocity_x(pp, pelv, vx);
        physics_set_velocity_y(pp, pelv, vy);

        /* Aim: reconstruct from the angle. The pose drive needs an
         * aim_world point, not an angle; pick a long-range point. */
        float ang = dequant_angle(e->aim_q);
        m->aim_world = (Vec2){ new_px + cosf(ang) * 200.0f,
                               new_py + sinf(ang) * 200.0f };

        /* Combat & state. */
        m->health      = (e->health  / 255.0f) * m->health_max;
        m->armor       = (e->armor   / 255.0f) * 100.0f;
        m->weapon_id   = e->weapon_id;
        m->ammo        = e->ammo;
        m->team        = e->team;
        bool was_alive = m->alive;
        m->alive       = (e->state_bits & SNAP_STATE_ALIVE) != 0;
        m->grounded    = (e->state_bits & SNAP_STATE_GROUNDED) != 0;
        m->facing_left = (e->state_bits & SNAP_STATE_FACING_LEFT) != 0;
        if (e->state_bits & SNAP_STATE_JET) m->anim_id = ANIM_JET;
        else if (m->grounded)               m->anim_id = ANIM_STAND;
        else                                 m->anim_id = ANIM_FALL;

        if (was_alive && !m->alive) {
            /* Local cosmetic kill — drop the pose drive so render
             * shows ragdoll. */
            for (int s = 0; s < PART_COUNT; ++s) m->pose_strength[s] = 0.0f;
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
