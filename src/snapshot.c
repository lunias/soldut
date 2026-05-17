#include "snapshot.h"

#include "audio.h"
#include "log.h"
#include "mech.h"
#include "particle.h"
#include "physics.h"
#include "projectile.h"
#include "weapons.h"

#include <math.h>
#include <string.h>

/* ---- Quantization helpers ----------------------------------------- */
/*
 * pos: 1/4 px → int16 covers ±8190 px (was 1/8 px / ±4096 — see TRADE_OFFS).
 * vel: 1/16 px/tick (per-tick velocity, post-integrate). 16-bit
 *      signed covers ±2048 px/tick — way more than we'd ever see.
 * angle: uint16 fraction of 2π.
 */

/* Position quantization is i16 with a 4× sub-pixel factor:
 *   max representable px = 32760 / 4 ≈ 8190
 *   resolution           = 0.25 px (well under 1-px renderer interp jitter)
 *
 * The factor used to be 8.0f (max ~4096 px, 0.125 px res) — that clamped
 * any position east of x=4096 to x=4095 on the wire and made the
 * Crossfire map's BLUE base unreachable for clients (the local mech kept
 * snapping back to x=4095 every snapshot). When we ship a map wider
 * than ~8000 px we'll need a wider encoding here. See TRADE_OFFS.md
 * "snapshot pos quant factor 4× → 8 K px max world width". */
static int16_t quant_pos(float p) {
    float q = p * 4.0f;
    if (q >  32760.0f) q =  32760.0f;
    if (q < -32760.0f) q = -32760.0f;
    return (int16_t)(q < 0 ? q - 0.5f : q + 0.5f);
}
static float dequant_pos(int16_t q) { return (float)q / 4.0f; }

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

/* M6 — gait phase quantization. Maps [0, 1) → u16. Phase is wrapped
 * into [0, 1) before quantization so the encoder is robust to a stale
 * `gait_phase_l > 1` (which build_pose's `floorf` subtract should
 * already prevent, but defense in depth). */
static uint16_t quant_phase(float phase) {
    if (!(phase == phase)) phase = 0.0f;   /* NaN guard */
    if (phase < 0.0f)  phase -= floorf(phase);
    if (phase >= 1.0f) phase -= floorf(phase);
    int v = (int)(phase * 65536.0f);
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    return (uint16_t)v;
}
static float dequant_phase(uint16_t q) {
    return (float)q * (1.0f / 65536.0f);
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

        uint16_t bits = 0;
        if (m->alive)              bits |= SNAP_STATE_ALIVE;
        if (m->grounded)           bits |= SNAP_STATE_GROUNDED;
        if (m->facing_left)        bits |= SNAP_STATE_FACING_LEFT;
        if (m->anim_id == ANIM_JET)    bits |= SNAP_STATE_JET;
        if (m->anim_id == ANIM_FIRE)   bits |= SNAP_STATE_FIRE;
        if (m->anim_id == ANIM_RUN)    bits |= SNAP_STATE_RUNNING;
        /* M6 — crouch / prone share the existing wire bits set aside
         * at M2. anim_id is the authoritative driver; the bits are
         * just the wire representation. */
        if (m->anim_id == ANIM_CROUCH) bits |= SNAP_STATE_CROUCH;
        if (m->anim_id == ANIM_PRONE)  bits |= SNAP_STATE_PRONE;
        if (m->reload_timer > 0.0f)  bits |= SNAP_STATE_RELOAD;
        if (m->is_dummy)           bits |= SNAP_STATE_IS_DUMMY;
        /* P05 — powerup state. Timers tick down server-side; the bit is
         * set whenever the timer is positive. Clients mirror to a
         * sentinel timer in snapshot_apply so render can read
         * `timer > 0` consistently. */
        if (m->powerup_berserk_remaining > 0.0f)  bits |= SNAP_STATE_BERSERK;
        if (m->powerup_invis_remaining   > 0.0f)  bits |= SNAP_STATE_INVIS;
        if (m->powerup_godmode_remaining > 0.0f)  bits |= SNAP_STATE_GODMODE;
        /* M6 P02 — burst-jet boost on the wire. Drives the FX plume
         * spike + leading-edge SFX_JET_BOOST cue on remote mechs. */
        if (m->boost_timer > 0.0f)               bits |= SNAP_STATE_BOOSTING;

        /* P06 — Grapple state. The trailing 8-byte suffix is only on
         * the wire when SNAP_STATE_GRAPPLING is set; idle stays flat.
         * For ATTACHED with a tile anchor, anchor_pos is the source of
         * truth for the rope endpoint. For a bone anchor, the receiver
         * looks up the live particle position; we still ship anchor_pos
         * for fallback / mid-flight FLYING-state handling. */
        if (m->grapple.state != GRAPPLE_IDLE) {
            bits |= SNAP_STATE_GRAPPLING;
            e->grapple_state       = m->grapple.state;
            e->grapple_anchor_mech = (int8_t)m->grapple.anchor_mech;
            e->grapple_anchor_part = m->grapple.anchor_part;
            e->grapple_anchor_x_q  = (int16_t)m->grapple.anchor_pos.x;
            e->grapple_anchor_y_q  = (int16_t)m->grapple.anchor_pos.y;
        }
        e->state_bits = bits;

        e->team      = (uint8_t)m->team;
        e->limb_bits = (uint16_t)m->dismember_mask;
        e->chassis_id     = (uint8_t)m->chassis_id;
        e->armor_id       = (uint8_t)m->armor_id;
        e->jetpack_id     = (uint8_t)m->jetpack_id;
        e->primary_id     = (uint8_t)m->primary_id;
        e->secondary_id   = (uint8_t)m->secondary_id;
        e->ammo_secondary = (uint8_t)(m->ammo_secondary > 255 ? 255
                                : (m->ammo_secondary < 0 ? 0 : m->ammo_secondary));
        /* M6 — gait phase. Source is `gait_phase_l` which build_pose
         * updates each tick in the ANIM_RUN case (clamped to [0, 1)).
         * Outside ANIM_RUN it stays at 0 — non-RUN mechs ship a 0 here
         * which the client's pose function correctly treats as
         * "no gait motion." */
        e->gait_phase_q = quant_phase(m->gait_phase_l);
    }
    out->ent_count = n;

    /* M6 P12 — capture replicated projectiles. Only bouncy projectiles
     * ride the snapshot today (see ProjectileSnapshot doc in snapshot.h);
     * everything else stays on the FIRE_EVENT / NET_MSG_EXPLOSION path.
     * `net_id == 0` is the sentinel for "spawn happened on the client
     * (via FIRE_EVENT visual) and the server has no authoritative twin"
     * — those shouldn't appear here because this runs on the
     * authoritative World whose projectile_spawn always assigns a
     * non-zero net_id, but defensively skip them. */
    int pn = 0;
    const ProjectilePool *pp = &w->projectiles;
    for (int i = 0; i < pp->count && pn < SNAPSHOT_PROJECTILE_CAP; ++i) {
        if (!pp->alive[i])      continue;
        if (!pp->bouncy[i])     continue;     /* non-bouncy on FIRE_EVENT path */
        if (pp->net_id[i] == 0) continue;     /* unassigned — shouldn't happen server-side */
        ProjectileSnapshot *ps = &out->projs[pn++];
        ps->net_id     = pp->net_id[i];
        ps->kind       = pp->kind[i];
        ps->owner_mech = (uint8_t)((pp->owner_mech[i] >= 0
                                    && pp->owner_mech[i] < MAX_MECHS)
                                       ? pp->owner_mech[i] : 0);
        ps->pos_x_q    = quant_pos(pp->pos_x[i]);
        ps->pos_y_q    = quant_pos(pp->pos_y[i]);
        /* Velocity quant scale matches mechs (1/16 px/tick). Server
         * stores vel in px/sec; divide by 60 (Hz) for the wire. */
        ps->vel_x_q    = quant_vel(pp->vel_x[i] * (1.0f / 60.0f));
        ps->vel_y_q    = quant_vel(pp->vel_y[i] * (1.0f / 60.0f));
        uint8_t flags = 0;
        if (pp->bouncy[i])    flags |= PROJ_SNAP_F_BOUNCY;
        if (pp->exploded[i])  flags |= PROJ_SNAP_F_EXPLODED;
        ps->flags      = flags;
        /* fuse_ticks: clamp life (seconds) × 60 to 255. */
        float lt = pp->life[i] * 60.0f;
        if (lt < 0.0f)   lt = 0.0f;
        if (lt > 255.0f) lt = 255.0f;
        ps->fuse_ticks = (uint8_t)(lt + 0.5f);
    }
    out->proj_count = pn;
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
        const EntitySnapshot *e = &cur->ents[i];
        bool has_grapple = (e->state_bits & SNAP_STATE_GRAPPLING) != 0;
        int  ent_bytes   = ENTITY_SNAPSHOT_WIRE_BYTES
                         + (has_grapple ? ENTITY_SNAPSHOT_GRAPPLE_BYTES : 0);
        if (p + ent_bytes > end) {
            LOG_E("snapshot_encode: buffer overflow at ent %d/%d",
                  i, cur->ent_count);
            return 0;
        }
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
        /* health, armor, weapon_id, ammo, state_bits(2), team */
        *p++ = e->health;
        *p++ = e->armor;
        *p++ = e->weapon_id;
        *p++ = e->ammo;
        *p++ = (uint8_t)e->state_bits;
        *p++ = (uint8_t)(e->state_bits >> 8);
        *p++ = e->team;
        /* limb_bits(2) */
        *p++ = (uint8_t)e->limb_bits; *p++ = (uint8_t)(e->limb_bits >> 8);
        /* loadout (M3 + P10-followup): chassis, armor, jetpack,
         * primary, secondary, ammo_secondary. `primary_id` joins
         * the wire so the client doesn't have to infer it from
         * the active-slot weapon at first-snapshot time. */
        *p++ = e->chassis_id;
        *p++ = e->armor_id;
        *p++ = e->jetpack_id;
        *p++ = e->primary_id;
        *p++ = e->secondary_id;
        *p++ = e->ammo_secondary;
        /* M6 — gait_phase_q(2). Rides every entity record (not gated
         * on RUN-anim) so the decoder layout stays fixed-width. */
        *p++ = (uint8_t)e->gait_phase_q;
        *p++ = (uint8_t)(e->gait_phase_q >> 8);
        /* P06 — Optional grapple suffix (8 bytes when SNAP_STATE_GRAPPLING). */
        if (has_grapple) {
            *p++ = e->grapple_state;
            *p++ = (uint8_t)e->grapple_anchor_mech;
            *p++ = e->grapple_anchor_part;
            *p++ = 0;  /* reserved */
            *p++ = (uint8_t)e->grapple_anchor_x_q;
            *p++ = (uint8_t)((uint16_t)e->grapple_anchor_x_q >> 8);
            *p++ = (uint8_t)e->grapple_anchor_y_q;
            *p++ = (uint8_t)((uint16_t)e->grapple_anchor_y_q >> 8);
        }
    }

    /* M6 P12 — ProjectileSnapshot array. u16 count + N × 14 bytes. */
    int proj_count = cur->proj_count;
    if (proj_count > SNAPSHOT_PROJECTILE_CAP) proj_count = SNAPSHOT_PROJECTILE_CAP;
    if (p + 2 + proj_count * PROJECTILE_SNAPSHOT_WIRE_BYTES > end) {
        LOG_E("snapshot_encode: buffer overflow at proj_count=%d", proj_count);
        return 0;
    }
    *p++ = (uint8_t)proj_count;
    *p++ = (uint8_t)(proj_count >> 8);
    for (int i = 0; i < proj_count; ++i) {
        const ProjectileSnapshot *ps = &cur->projs[i];
        *p++ = (uint8_t)ps->net_id;
        *p++ = (uint8_t)(ps->net_id >> 8);
        *p++ = ps->kind;
        *p++ = ps->owner_mech;
        *p++ = (uint8_t)ps->pos_x_q; *p++ = (uint8_t)((uint16_t)ps->pos_x_q >> 8);
        *p++ = (uint8_t)ps->pos_y_q; *p++ = (uint8_t)((uint16_t)ps->pos_y_q >> 8);
        *p++ = (uint8_t)ps->vel_x_q; *p++ = (uint8_t)((uint16_t)ps->vel_x_q >> 8);
        *p++ = (uint8_t)ps->vel_y_q; *p++ = (uint8_t)((uint16_t)ps->vel_y_q >> 8);
        *p++ = ps->flags;
        *p++ = ps->fuse_ticks;
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
    /* Lower-bound check: every entity contributes at least the fixed
     * 28-byte record. The optional 8-byte grapple suffix is checked
     * per-entity below when its state_bits says it's present. */
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
        e->state_bits = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
        e->team       = *p++;
        e->limb_bits  = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
        e->chassis_id     = *p++;
        e->armor_id       = *p++;
        e->jetpack_id     = *p++;
        e->primary_id     = *p++;
        e->secondary_id   = *p++;
        e->ammo_secondary = *p++;
        /* M6 — gait_phase_q. */
        e->gait_phase_q   = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
        /* P06 — Optional grapple suffix. */
        if (e->state_bits & SNAP_STATE_GRAPPLING) {
            if (end - p < ENTITY_SNAPSHOT_GRAPPLE_BYTES) {
                LOG_E("snapshot_decode: short grapple suffix at ent %d", i);
                return false;
            }
            e->grapple_state       = *p++;
            e->grapple_anchor_mech = (int8_t)*p++;
            e->grapple_anchor_part = *p++;
            p++;  /* reserved */
            e->grapple_anchor_x_q  = (int16_t)((uint16_t)(p[0] | (p[1] << 8))); p += 2;
            e->grapple_anchor_y_q  = (int16_t)((uint16_t)(p[0] | (p[1] << 8))); p += 2;
        }
    }
    out->ent_count = cnt;

    /* M6 P12 — ProjectileSnapshot array. Treat absence as 0 — pre-S0LL
     * (or a malformed body trimmed at the entity boundary) just yields
     * proj_count = 0 and the client's per-projectile snap rings stay
     * untouched. */
    out->proj_count = 0;
    if (end - p >= 2) {
        uint16_t pcnt = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
        if (pcnt > SNAPSHOT_PROJECTILE_CAP) {
            LOG_E("snapshot_decode: proj_count %u > cap %d",
                  (unsigned)pcnt, SNAPSHOT_PROJECTILE_CAP);
            return false;
        }
        if (end - p < (int)pcnt * PROJECTILE_SNAPSHOT_WIRE_BYTES) {
            LOG_E("snapshot_decode: short projectile body (%d left, need %d)",
                  (int)(end - p), (int)pcnt * PROJECTILE_SNAPSHOT_WIRE_BYTES);
            return false;
        }
        for (int i = 0; i < (int)pcnt; ++i) {
            ProjectileSnapshot *ps = &out->projs[i];
            ps->net_id     = (uint16_t)(p[0] | (p[1] << 8));      p += 2;
            ps->kind       = *p++;
            ps->owner_mech = *p++;
            ps->pos_x_q    = (int16_t)((uint16_t)(p[0] | (p[1] << 8))); p += 2;
            ps->pos_y_q    = (int16_t)((uint16_t)(p[0] | (p[1] << 8))); p += 2;
            ps->vel_x_q    = (int16_t)((uint16_t)(p[0] | (p[1] << 8))); p += 2;
            ps->vel_y_q    = (int16_t)((uint16_t)(p[0] | (p[1] << 8))); p += 2;
            ps->flags      = *p++;
            ps->fuse_ticks = *p++;
        }
        out->proj_count = (int)pcnt;
    }

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
            /* Out-of-range fallbacks match server_handle_lobby_loadout's
             * clamps in src/net.c: an unknown-on-the-wire armor/jet
             * surfaces as ARMOR_LIGHT / JET_STANDARD (working defaults),
             * not ARMOR_NONE / JET_NONE (no armor + Baseline thrust).
             * Pre-fix the client and server defaulted differently on
             * out-of-range bytes, producing silent loadout divergence. */
            lo.armor_id     = e->armor_id     < ARMOR_COUNT   ? e->armor_id     : ARMOR_LIGHT;
            lo.jetpack_id   = e->jetpack_id   < JET_COUNT     ? e->jetpack_id   : JET_STANDARD;
            /* P10-followup — `primary_id` rides the wire directly so we
             * use it as-is. Pre-fix this used `e->weapon_id` (the active
             * slot's weapon), which broke when a mid-round-join client
             * received a first snapshot whose mech had its secondary
             * equipped (active_slot==1) — the inferred primary_id ended
             * up equal to the secondary's weapon, which then made the
             * `active_slot == (weapon_id == primary_id)` derivation in
             * snapshot_apply return 0 (wrong) for the entire session. */
            lo.primary_id   = e->primary_id   < WEAPON_COUNT  ? e->primary_id   : WEAPON_PULSE_RIFLE;
            lo.secondary_id = e->secondary_id < WEAPON_COUNT  ? e->secondary_id : WEAPON_SIDEARM;
        }
        /* DIAG-sync: log the entity values vs the clamped loadout used to
         * spawn this mech. Compares to the server's
         * `DIAG-sync: lobby_spawn_round_mechs` line for the same mech_id —
         * any mismatch reveals where the loadout went wrong on the way
         * across the wire. desired_id is the index ensure_mech_slot was
         * asked for; the actual created mech_count grows by 1 per loop
         * iteration so multiple mechs share an `e` only when the loop
         * runs more than once (which would itself be a bug — see while
         * condition above). */
        LOG_I("DIAG-sync: ensure_mech_slot desired_id=%d mech_count=%d "
              "entity{chassis=%d armor=%d jet=%d primary=%d secondary=%d} "
              "clamped{chassis=%d armor=%d jet=%d primary=%d secondary=%d}",
              desired_id, w->mech_count,
              e ? (int)e->chassis_id : -1,
              e ? (int)e->armor_id   : -1,
              e ? (int)e->jetpack_id : -1,
              e ? (int)e->primary_id : -1,
              e ? (int)e->secondary_id : -1,
              lo.chassis_id, lo.armor_id, lo.jetpack_id,
              lo.primary_id, lo.secondary_id);
        int got = mech_create_loadout(w, lo, spawn, team, /*is_dummy*/false);
        if (got < 0) return -1;
    }
    return desired_id;
}

/* Forward declaration — definition lives in the "Remote-mech
 * interpolation" section below. snapshot_apply + the new
 * snapshot_apply_remote_ring_only both call it. */
static void remote_snap_push(Mech *m, uint32_t stx,
                             float px, float py,
                             float vx, float vy);

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

        float new_px = dequant_pos(e->pos_x_q);
        float new_py = dequant_pos(e->pos_y_q);
        float vx     = dequant_vel(e->vel_x_q);
        float vy     = dequant_vel(e->vel_y_q);

        if (is_local) {
            /* Full snap. Reconciliation (in reconcile.c) replays
             * unacked inputs after this call to bring the body
             * forward to "now". Anything else would break that
             * contract.
             *
             * Spawn case: when ensure_mech_slot just created this
             * mech, existing pos equals new_px/new_py so the
             * translate is a no-op — no concern. */
            float old_px = pp->pos_x[m->particle_base + PART_PELVIS];
            float old_py = pp->pos_y[m->particle_base + PART_PELVIS];
            float dx = new_px - old_px;
            float dy = new_py - old_py;
            for (int part = 0; part < PART_COUNT; ++part) {
                int idx = m->particle_base + part;
                physics_translate_kinematic(pp, idx, dx, dy);
                physics_set_velocity_x(pp, idx, vx);
                physics_set_velocity_y(pp, idx, vy);
            }
        } else {
            /* P03 — push to the per-mech snapshot ring; the actual
             * particle write happens each tick in
             * snapshot_interp_remotes, lerping between bracketing
             * ring entries at `render_time - INTERP_DELAY_MS`. The
             * per-particle velocity matches the snapshot's velocity
             * so the constraint solver doesn't fight body-frame
             * velocity mismatches between snapshots.
             *
             * Large corrections (>200 px — respawn, teleport) clear
             * the ring and snap fully so we don't slide across the
             * level for a hundred ms. */
            float old_px = pp->pos_x[m->particle_base + PART_PELVIS];
            float old_py = pp->pos_y[m->particle_base + PART_PELVIS];
            float dx_full = new_px - old_px;
            float dy_full = new_py - old_py;
            bool big_correction = (dx_full * dx_full + dy_full * dy_full)
                                  > 200.0f * 200.0f;
            if (big_correction) {
                /* Snap fully + clear ring; the next snapshot lands
                 * fresh and interp picks back up from the new
                 * position. */
                for (int part = 0; part < PART_COUNT; ++part) {
                    int idx = m->particle_base + part;
                    physics_translate_kinematic(pp, idx, dx_full, dy_full);
                    physics_set_velocity_x(pp, idx, vx);
                    physics_set_velocity_y(pp, idx, vy);
                }
                m->remote_snap_count = 0;
                m->remote_snap_head  = 0;
                for (int s = 0; s < REMOTE_SNAP_RING; ++s)
                    m->remote_snap_ring[s].valid = false;
            }
            /* Push the snapshot into the ring (always, including the
             * just-snapped case — the next tick's interp will see the
             * fresh entry and stay put on it). */
            remote_snap_push(m, frame->header.server_time_ms,
                             new_px, new_py, vx, vy);
        }

        /* Aim: reconstruct from the angle. The pose drive needs an
         * aim_world point, not an angle; pick a long-range point. */
        float ang = dequant_angle(e->aim_q);
        m->aim_world = (Vec2){ new_px + cosf(ang) * 200.0f,
                               new_py + sinf(ang) * 200.0f };

        /* Combat & state. Per-hit blood / sparks ride NET_MSG_HIT_EVENT
         * (broadcast by the server from mech_apply_damage) — the
         * client spawns FX at the actual hit point with the actual
         * hit direction. Spawning blood here from snapshot
         * health-decrease would double-up (HIT_EVENT already fired)
         * AND show wrong position (we'd default to chest +
         * facing-derived spray). */
        m->health      = (e->health  / 255.0f) * m->health_max;
        if (m->armor_hp_max > 0.0f) {
            m->armor_hp = (e->armor / 255.0f) * m->armor_hp_max;
        }
        m->weapon_id     = e->weapon_id;
        m->ammo          = e->ammo;
        m->team          = e->team;
        /* P10-followup — keep primary/secondary in lockstep with the
         * host's authoritative loadout so a mid-round PICKUP_WEAPON or
         * a delayed first snapshot with active_slot==1 doesn't strand
         * the client on a stale primary inferred at spawn time. */
        m->primary_id    = e->primary_id;
        m->secondary_id  = e->secondary_id;
        m->ammo_secondary= e->ammo_secondary;
        /* Active slot: derived from which weapon_id matches. */
        m->active_slot   = (e->weapon_id == m->primary_id) ? 0 : 1;

        /* DIAG-sync fix: also re-sync armor_id + jetpack_id + their
         * derived caps (armor_hp_max, fuel_max) from the snapshot.
         * Pre-fix these were set ONLY at spawn (in ensure_mech_slot's
         * mech_create_loadout call), so any state where the
         * spawn-time loadout disagreed with the server's authoritative
         * value — a race in the slot's loadout being stale when
         * lobby_spawn_round_mechs ran, an out-of-range byte falling
         * to JET_NONE in the old clamp default, or a first-snapshot
         * encoding glitch — stuck the client's mech with the wrong
         * caps for the entire round (empty jet meter, wrong armor
         * capacity). The wire ships the authoritative values every
         * snapshot; consuming them is cheap and idempotent in the
         * common case (values match → no-op).
         *
         * chassis_id we do NOT re-sync mid-round: changing it would
         * orphan the existing skeleton's particle positions against
         * the new chassis's bone-length table. We log a WARN if a
         * mismatch is ever detected so future debugging has a clear
         * signal, but we hold the spawn-time chassis. (In practice
         * chassis doesn't change mid-round; if it ever does we'd
         * rebuild the mech entirely.) */
        int new_jet   = e->jetpack_id < JET_COUNT   ? e->jetpack_id : JET_STANDARD;
        int new_armor = e->armor_id   < ARMOR_COUNT ? e->armor_id   : ARMOR_LIGHT;
        if (m->jetpack_id != new_jet || m->armor_id != new_armor) {
            const Chassis *cur_ch = mech_chassis((ChassisId)m->chassis_id);
            const Jetpack *new_jp = jetpack_def(new_jet);
            const Armor   *new_ar = armor_def(new_armor);
            float old_fuel_max = m->fuel_max;
            float fuel_frac    = (old_fuel_max > 0.0f) ? (m->fuel / old_fuel_max) : 1.0f;
            m->jetpack_id   = new_jet;
            m->armor_id     = new_armor;
            m->fuel_max     = cur_ch->fuel_max * new_jp->fuel_mult;
            m->fuel         = m->fuel_max * fuel_frac;
            if (m->fuel > m->fuel_max) m->fuel = m->fuel_max;
            if (new_ar->hp > 0.0f) {
                /* If the new armor's cap is non-zero, scale current
                 * armor_hp into the new range. If old armor_hp_max
                 * was 0 (no armor) and new is non-zero, start at the
                 * new max. */
                float armor_frac = (m->armor_hp_max > 0.0f)
                                       ? (m->armor_hp / m->armor_hp_max) : 1.0f;
                m->armor_hp_max = new_ar->hp;
                m->armor_hp     = m->armor_hp_max * armor_frac;
                m->armor_charges = new_ar->reactive_charges;
            } else {
                m->armor_hp_max = 0.0f;
                m->armor_hp     = 0.0f;
                m->armor_charges = 0;
            }
            LOG_I("DIAG-sync: snapshot_apply re-synced mid=%d "
                  "jet=%s(%d) armor=%s(%d) fuel_max=%.3f armor_hp_max=%.1f",
                  mid, new_jp->name, new_jet, new_ar->name, new_armor,
                  (double)m->fuel_max, (double)m->armor_hp_max);
        }
        if (e->chassis_id < CHASSIS_COUNT && (int)e->chassis_id != m->chassis_id) {
            LOG_W("DIAG-sync: chassis mismatch mid=%d local=%d snapshot=%d "
                  "(not re-synced mid-round)",
                  mid, m->chassis_id, (int)e->chassis_id);
        }
        bool was_alive = m->alive;
        bool now_alive = (e->state_bits & SNAP_STATE_ALIVE) != 0;
        /* Mid-round respawn: when a mech transitions from dead back
         * to alive (CTF respawn timer fired on the server), the
         * client needs to undo the host's death-side state. Call
         * mech_respawn directly with the snapshot's pelvis position
         * — same path the server uses, so the skeleton, dismember
         * state, constraints, damage decals, hit-flash, pinned FX
         * emitters, and interp ring all reset identically on both
         * sides. The translate-by-(dx, dy) below then sees dx=dy=0
         * (pelvis already at new_pelv) and stays put.
         *
         * Applies to BOTH the local mech AND remote mechs — pre-fix
         * the gate `mid != local_mech_id` left the local player's
         * respawned body wearing the previous life's decals + smoke
         * trail while remote viewers saw a clean body (the user-
         * reported sync bug). The local-mech reconcile path doesn't
         * touch decals / dismember / FX, so the reset is safe. */
        if (!was_alive && now_alive) {
            Vec2 new_pelv = (Vec2){ new_px, new_py };
            mech_respawn(w, mid, new_pelv);
        }
        m->alive       = now_alive;
        m->grounded    = (e->state_bits & SNAP_STATE_GROUNDED) != 0;
        m->facing_left = (e->state_bits & SNAP_STATE_FACING_LEFT) != 0;
        m->is_dummy    = (e->state_bits & SNAP_STATE_IS_DUMMY) != 0;
        /* P05 — mirror powerup bits to a local sentinel timer. The
         * client doesn't know the exact remaining seconds (the server
         * doesn't ship a float for this), but render and self-modulation
         * paths only need "is the powerup active?" — which the bit
         * answers. mech_step_drive ticks the sentinel down each frame;
         * the next snapshot re-arms it while the host's authoritative
         * timer is still positive, then clears it when the host's bit
         * goes 0. */
        m->powerup_berserk_remaining = (e->state_bits & SNAP_STATE_BERSERK) ? 1.0f : 0.0f;
        m->powerup_invis_remaining   = (e->state_bits & SNAP_STATE_INVIS)   ? 1.0f : 0.0f;
        m->powerup_godmode_remaining = (e->state_bits & SNAP_STATE_GODMODE) ? 1.0f : 0.0f;
        /* M6 P02 — Visual jet state for remote mechs. Owner-side mechs
         * (server, or this client's local mech) populate jet_state_bits
         * in mech_step_drive from real input + boost_timer; for remote
         * mechs on the client the snapshot's state bits are the only
         * source. The MECH_JET_IGNITION_TICK edge is derived locally
         * from m->jet_prev_grounded vs the just-applied m->grounded so
         * the trigger lands on the same tick host and client agree the
         * mech left the ground. Leading-edge SFX_JET_BOOST fires before
         * we overwrite jet_state_bits, mirroring the owner-side audio
         * fired from mech_step_drive. */
        bool snap_jet   = (e->state_bits & SNAP_STATE_JET)      != 0;
        bool snap_boost = (e->state_bits & SNAP_STATE_BOOSTING) != 0;
        if (!is_local) {
            bool prev_boost = (m->jet_state_bits & MECH_JET_BOOSTING) != 0;
            if (snap_boost && !prev_boost) {
                audio_play_at(SFX_JET_BOOST, mech_chest_pos(w, mid));
            }
            bool was_grounded_jb = m->jet_prev_grounded != 0;
            uint8_t new_bits = 0;
            if (snap_jet) {
                new_bits |= MECH_JET_ACTIVE;
                if (was_grounded_jb && !m->grounded) {
                    new_bits |= MECH_JET_IGNITION_TICK;
                }
                if (snap_boost) new_bits |= MECH_JET_BOOSTING;
            }
            m->jet_state_bits    = new_bits;
            m->jet_prev_grounded = m->grounded ? 1u : 0u;
        }
        /* P06 — Grapple. Clients never allocate a constraint (the
         * pull is felt through the snapshot's pelvis_pos updates);
         * we just store state + anchor for the rope renderer. */
        if (e->state_bits & SNAP_STATE_GRAPPLING) {
            m->grapple.state          = e->grapple_state;
            m->grapple.anchor_mech    = e->grapple_anchor_mech;
            m->grapple.anchor_part    = e->grapple_anchor_part;
            m->grapple.anchor_pos     = (Vec2){
                (float)e->grapple_anchor_x_q,
                (float)e->grapple_anchor_y_q,
            };
            m->grapple.constraint_idx = -1;
        } else {
            m->grapple.state          = GRAPPLE_IDLE;
            m->grapple.anchor_mech    = -1;
            m->grapple.constraint_idx = -1;
        }
        /* M6 — gait phase. Authoritative side updates `gait_phase_l`
         * each tick in `mech_update_gait`'s ANIM_RUN case; the
         * snapshot mirrors it here so the procedural pose function
         * reads the same value the server saw. Outside RUN the field
         * is 0 (suppresses spurious gait drive).
         *
         * For REMOTE mechs on the client we ALSO fire the swing→stance
         * footstep SFX here — `mech_update_gait` is gated to
         * auth+local on the client, so the snapshot path is the only
         * place that observes the wrap for remote players. The
         * trigger condition + plant location mirror what
         * `mech_update_gait` does on the auth side, so audio cues are
         * symmetric across all viewers. */
        float new_phase_l = dequant_phase(e->gait_phase_q);
        float new_phase_r = new_phase_l + 0.5f;
        if (new_phase_r >= 1.0f) new_phase_r -= 1.0f;
        if (!w->authoritative && !is_local && m->grounded
            && m->anim_id == ANIM_RUN)
        {
            const Chassis *ch = mech_chassis((ChassisId)m->chassis_id);
            float pelvis_x = pp->pos_x[m->particle_base + PART_PELVIS];
            float pelvis_y = pp->pos_y[m->particle_base + PART_PELVIS];
            float foot_y   = pelvis_y + ch->bone_thigh + ch->bone_shin;
            float dir      = m->facing_left ? -1.0f : 1.0f;
            float front    = 14.0f * dir;   /* STRIDE/2 = 14 */
            if (m->gait_phase_l > 0.5f && new_phase_l < 0.5f) {
                audio_play_at(SFX_FOOTSTEP_CONCRETE,
                              (Vec2){ (pelvis_x - 7.0f) + front, foot_y });
            }
            if (m->gait_phase_r > 0.5f && new_phase_r < 0.5f) {
                audio_play_at(SFX_FOOTSTEP_CONCRETE,
                              (Vec2){ (pelvis_x + 7.0f) + front, foot_y });
            }
        }
        m->gait_phase_l = new_phase_l;
        m->gait_phase_r = new_phase_r;

        /* wan-fixes-3 followup — anim_id mirrors the server's
         * authoritative classification (SNAP_STATE_JET / RUNNING) so
         * the client doesn't have to guess from velocity (which
         * flickered RUN → STAND mid-stride) or grounded (which
         * flickered FALL → STAND on each gait foot-lift). build_pose
         * treats ANIM_FALL / FIRE / STAND identically (same default
         * pose case), so we just distinguish RUN / JET / "default."
         * Hysteresis for FALL lives on the server in mech_step_drive's
         * air_ticks counter and only affects the snapshot's RUNNING
         * bit indirectly via the server's anim_id. */
        /* Priority mirrors mech_step_drive's authoritative branch:
         * JET overrides everything (airborne); PRONE outranks CROUCH;
         * RUN outranks STAND. */
        if (e->state_bits & SNAP_STATE_JET) {
            m->anim_id = ANIM_JET;
        } else if (e->state_bits & SNAP_STATE_PRONE) {
            m->anim_id = ANIM_PRONE;
        } else if (e->state_bits & SNAP_STATE_CROUCH) {
            m->anim_id = ANIM_CROUCH;
        } else if (e->state_bits & SNAP_STATE_RUNNING) {
            m->anim_id = ANIM_RUN;
        } else {
            m->anim_id = ANIM_STAND;
        }
        m->air_ticks = 0;   /* unused on client now; kept for struct compat */

        /* (per-hit blood/sparks now ride NET_MSG_HIT_EVENT — see
         * the long comment above where prev_health is captured) */

        if (was_alive && !m->alive) {
            /* Local cosmetic kill — once `m->alive` flips off the
             * procedural pose step in simulate skips the mech (alive
             * guard), so Verlet free-runs the ragdoll body. Spawn the
             * death blood fountain so the client matches the host's
             * visual on kill. */
            int chest = m->particle_base + PART_CHEST;
            Vec2 at = { pp->pos_x[chest], pp->pos_y[chest] };
            Vec2 base_dir = { (m->facing_left ? 100.0f : -100.0f), -120.0f };
            for (int k = 0; k < 32; ++k)
                fx_spawn_blood(&w->fx, at, base_dir, w->rng);
            for (int k = 0; k < 8; ++k)
                fx_spawn_spark(&w->fx, at, base_dir, w->rng);
        }

        /* Apply dismemberment by deactivating constraints in the new
         * masks that weren't already inactive locally. P12: route
         * through mech_dismember for any newly-set bits so the client
         * runs the same constraint-deactivation + 64-particle blood
         * spray + FX_STUMP pinned emitter + limb-HP-zero side effects
         * the host fires inside `mech_apply_damage`. mech_dismember
         * has an internal "already gone" guard so re-asserting an
         * already-set bit is a no-op. The trailing assignment is
         * defensive — covers any future bit added to the mask but
         * not iterated here (e.g. a 6th limb). */
        uint8_t new_mask = (uint8_t)e->limb_bits;
        uint8_t added    = (uint8_t)(new_mask & ~m->dismember_mask);
        if (added) {
            static const uint8_t s_limb_bits[] = {
                LIMB_HEAD, LIMB_L_ARM, LIMB_R_ARM, LIMB_L_LEG, LIMB_R_LEG,
            };
            for (int k = 0;
                 k < (int)(sizeof s_limb_bits / sizeof s_limb_bits[0]);
                 ++k)
            {
                if (added & s_limb_bits[k]) {
                    mech_dismember(w, mid, (int)s_limb_bits[k]);
                }
            }
        }
        m->dismember_mask = new_mask;
    }

    /* Kill anything not in this snapshot. */
    for (int i = 0; i < w->mech_count; ++i) {
        if (!seen[i]) {
            w->mechs[i].alive = false;
        }
    }

    /* M6 P12 — Apply replicated projectiles. Match by stable `net_id`
     * (not pool slot — server's pool layout is independent of ours).
     * Missing matches spawn a fresh local visual; orphaned local
     * slots (ours but not in this frame) are left alone — the
     * NET_MSG_EXPLOSION event drives their dying frame, same path as
     * pre-M6-P12. The two-slot interp pair (snap_a, snap_b) gets a
     * standard demote-on-update; duplicates (same server_time_ms)
     * drop. The client's pool capacity (512) is ≫ the snapshot cap
     * (64) so spawning never fails in steady state.
     *
     * Local-only projectiles (`net_id == 0` — non-bouncy AOE,
     * pellets, hitscan tracers visualised via FIRE_EVENT) are left
     * untouched. */
    ProjectilePool *pp = &w->projectiles;
    for (int i = 0; i < frame->proj_count; ++i) {
        const ProjectileSnapshot *ps = &frame->projs[i];
        if (ps->net_id == 0) continue;

        /* Match by stable id. Track both alive matches (steady state)
         * and dead-but-id-matched slots: when our local projectile
         * has already detonated (fuse-expire predict) but the server
         * is still sending the grenade in its dying-frame snapshot
         * tick, the wire entry would otherwise allocate a NEW local
         * slot that would also fuse-expire-detonate, double-firing
         * the explosion FX. The dead-slot branch below silences that
         * by ignoring the re-arriving entry — the matching
         * NET_MSG_EXPLOSION drives any remaining visual via the
         * existing record/dedupe path.
         *
         * Scan the full pool capacity (not just `pp->count`):
         * projectile_step trims `count` back to last_alive+1 each
         * tick, which can hide a dead net_id'd slot from a `count`-
         * bounded loop. Without this widening the next snapshot for
         * that net_id would skip the dead-slot match, allocate a
         * fresh slot via proj_alloc — which would happily REUSE the
         * trimmed-out slot 2 — and detonate again on fuse-expire,
         * yielding the same double-spawn bug. The scan cost is
         * 512 array reads per projectile per snapshot — trivial.
         * (See documents/m6/12-projectile-snapshot-replication.md
         * for context.) */
        int slot = -1;
        int slot_dead = -1;
        for (int k = 0; k < PROJECTILE_CAPACITY; ++k) {
            if (pp->net_id[k] != ps->net_id) continue;
            if (pp->alive[k]) { slot = k; break; }
            slot_dead = k;
        }
        if (slot < 0 && slot_dead >= 0) {
            /* Already-killed local twin — drop. */
            continue;
        }

        float new_x  = dequant_pos(ps->pos_x_q);
        float new_y  = dequant_pos(ps->pos_y_q);
        /* Wire vel is 1/16 px/TICK; rehydrate to px/sec for the
         * projectile pool's units (which matches weapon spawn). */
        float new_vx = dequant_vel(ps->vel_x_q) * 60.0f;
        float new_vy = dequant_vel(ps->vel_y_q) * 60.0f;
        uint32_t snap_time = frame->header.server_time_ms;

        if (slot < 0) {
            /* First time we've seen this id — spawn a local visual.
             * We look up a weapon whose `projectile_kind` matches so
             * the dying-frame FX (driven by client_handle_explosion's
             * weapon_def lookup) and the projectile's render path
             * agree. For PROJ_FRAG_GRENADE that resolves to
             * WEAPON_FRAG_GRENADE. */
            int wid = 0;
            const Weapon *wpn = NULL;
            for (int wi = 0; wi < WEAPON_COUNT; ++wi) {
                const Weapon *cand = weapon_def(wi);
                if (cand && cand->projectile_kind == ps->kind) {
                    wid = wi;
                    wpn = cand;
                    break;
                }
            }
            float aoe_r   = wpn ? wpn->aoe_radius  : 0.0f;
            float aoe_d   = wpn ? wpn->aoe_damage  : 0.0f;
            float aoe_imp = wpn ? wpn->aoe_impulse : 0.0f;
            float grav    = wpn ? wpn->projectile_grav_scale : 0.0f;
            float drg     = wpn ? wpn->projectile_drag       : 0.0f;
            float life    = (float)ps->fuse_ticks / 60.0f;

            int owner_mech = (int)ps->owner_mech;
            int owner_team = (owner_mech >= 0 && owner_mech < w->mech_count)
                                 ? (int)w->mechs[owner_mech].team : 0;

            ProjectileSpawn spawn = {
                .kind           = ps->kind,
                .weapon_id      = wid,
                .owner_mech_id  = owner_mech,
                .owner_team     = owner_team,
                .origin         = (Vec2){ new_x, new_y },
                .velocity       = (Vec2){ new_vx, new_vy },
                .damage         = wpn ? wpn->damage : 0.0f,
                .aoe_radius     = aoe_r,
                .aoe_damage     = aoe_d,
                .aoe_impulse    = aoe_imp,
                .life           = life,
                .gravity_scale  = grav,
                .drag           = drg,
                .bouncy         = (ps->flags & PROJ_SNAP_F_BOUNCY) != 0,
            };
            slot = projectile_spawn(w, spawn);
            if (slot < 0) continue;
            pp->net_id[slot] = ps->net_id;
            /* Initialise the interp pair with the b slot only — the
             * next snapshot will populate a. snapshot_interp_projectiles
             * clamps to b for state == 1 so the first rendered frame
             * sits exactly at the snapshot pos. */
            pp->snap_a_time_ms[slot] = 0;
            pp->snap_a_x      [slot] = 0.0f; pp->snap_a_y [slot] = 0.0f;
            pp->snap_a_vx     [slot] = 0.0f; pp->snap_a_vy[slot] = 0.0f;
            pp->snap_b_time_ms[slot] = snap_time;
            pp->snap_b_x      [slot] = new_x; pp->snap_b_y [slot] = new_y;
            pp->snap_b_vx     [slot] = new_vx; pp->snap_b_vy[slot] = new_vy;
            pp->snap_state    [slot] = 1;
            /* Also seed pos + render_prev so the renderer has the
             * right pos on the very first frame before the interp pass
             * runs. */
            pp->pos_x[slot]          = new_x;
            pp->pos_y[slot]          = new_y;
            pp->vel_x[slot]          = new_vx;
            pp->vel_y[slot]          = new_vy;
            pp->render_prev_x[slot]  = new_x;
            pp->render_prev_y[slot]  = new_y;
        } else {
            /* Existing local slot — push to the (a, b) interp pair.
             * Dup-drop on same server_time (rare, but a peer that
             * received the same snapshot twice via reorder would
             * otherwise overwrite the more recent b with the older
             * dup). */
            if (pp->snap_state[slot] >= 1 &&
                pp->snap_b_time_ms[slot] == snap_time) {
                /* duplicate, ignore */
            } else if (pp->snap_state[slot] == 0) {
                pp->snap_b_time_ms[slot] = snap_time;
                pp->snap_b_x      [slot] = new_x; pp->snap_b_y [slot] = new_y;
                pp->snap_b_vx     [slot] = new_vx; pp->snap_b_vy[slot] = new_vy;
                pp->snap_state    [slot] = 1;
            } else if (snap_time < pp->snap_b_time_ms[slot]) {
                /* Out-of-order arrival older than current b — only
                 * useful if it lands between a and b. Treat as a if
                 * newer than current a; else drop. Keeps interp pair
                 * span tight without elaborate ring logic. */
                if (pp->snap_state[slot] == 1 ||
                    snap_time > pp->snap_a_time_ms[slot])
                {
                    pp->snap_a_time_ms[slot] = snap_time;
                    pp->snap_a_x      [slot] = new_x; pp->snap_a_y [slot] = new_y;
                    pp->snap_a_vx     [slot] = new_vx; pp->snap_a_vy[slot] = new_vy;
                    pp->snap_state    [slot] = 2;
                }
            } else {
                /* Monotonic case: demote b → a, write new to b. */
                pp->snap_a_time_ms[slot] = pp->snap_b_time_ms[slot];
                pp->snap_a_x      [slot] = pp->snap_b_x      [slot];
                pp->snap_a_y      [slot] = pp->snap_b_y      [slot];
                pp->snap_a_vx     [slot] = pp->snap_b_vx     [slot];
                pp->snap_a_vy     [slot] = pp->snap_b_vy     [slot];
                pp->snap_b_time_ms[slot] = snap_time;
                pp->snap_b_x      [slot] = new_x;  pp->snap_b_y [slot] = new_y;
                pp->snap_b_vx     [slot] = new_vx; pp->snap_b_vy[slot] = new_vy;
                pp->snap_state    [slot] = 2;
            }
        }

        /* Refresh fuse so the fuse-expire fallback (in projectile_step
         * if NET_MSG_EXPLOSION is dropped on the wire) detonates near
         * when the server would have. The dying-frame trigger still
         * comes from the explosion msg in steady state. */
        pp->life[slot] = (float)ps->fuse_ticks / 60.0f;
        /* Server-set exploded bit drives the dying frame even if the
         * (reliable) NET_MSG_EXPLOSION is still in flight. */
        if (ps->flags & PROJ_SNAP_F_EXPLODED) pp->exploded[slot] = 1;
    }
}

void snapshot_apply_remote_ring_only(World *w, const SnapshotFrame *frame) {
    if (!frame || !frame->valid) return;
    int local_id = w->local_mech_id;
    for (int i = 0; i < frame->ent_count; ++i) {
        const EntitySnapshot *e = &frame->ents[i];
        int mid = (int)e->mech_id;
        if (mid < 0 || mid >= w->mech_count) continue;     /* skip ensure_mech_slot */
        if (mid == local_id) continue;                     /* never touch local */
        Mech *m = &w->mechs[mid];
        if (m->remote_snap_count <= 0) continue;            /* no ring to merge into */
        float new_px = dequant_pos(e->pos_x_q);
        float new_py = dequant_pos(e->pos_y_q);
        float vx     = dequant_vel(e->vel_x_q);
        float vy     = dequant_vel(e->vel_y_q);
        remote_snap_push(m, frame->header.server_time_ms,
                         new_px, new_py, vx, vy);
    }
}

/* ---- Remote-mech interpolation ----------------------------------- */

/* wan-fixes-20 — insert (stx, px, py, vx, vy) into the per-mech
 * snapshot ring with reorder-tolerant eviction. The head/count
 * scheme used by the original M5 P03 code assumes monotonic
 * arrivals: head advances each push and the oldest slot is the
 * one being overwritten. That's wrong for out-of-order arrivals,
 * where head may point past a NEWER entry — overwriting it would
 * silently throw away a valid future sample.
 *
 * Eviction policy here:
 *   1. If `stx` already exists in the ring → no-op (dup).
 *   2. If any slot is invalid → write there.
 *   3. Else (ring full): find the slot with the smallest
 *      server_time. If that's older than `stx`, overwrite it.
 *      If `stx` is older than every existing slot, drop —
 *      we have no useful insertion to make.
 *
 * head/count are still maintained but their precise meaning
 * shifts: head is just "next slot for the simple monotonic
 * write path"; the count tracks valid entries. pick_bracket
 * walks all slots and orders by server_time regardless of
 * physical layout, so eviction order doesn't affect correctness. */
static void remote_snap_push(Mech *m,
                             uint32_t stx,
                             float px, float py,
                             float vx, float vy)
{
    /* Dup check + invalid-slot scan + oldest-slot tracking, one pass. */
    int  empty_slot = -1;
    int  oldest_slot = -1;
    uint32_t oldest_t = (uint32_t)-1;
    for (int s = 0; s < REMOTE_SNAP_RING; ++s) {
        if (!m->remote_snap_ring[s].valid) {
            if (empty_slot < 0) empty_slot = s;
            continue;
        }
        if (m->remote_snap_ring[s].server_time_ms == stx) return;  /* dup */
        if (m->remote_snap_ring[s].server_time_ms < oldest_t) {
            oldest_t   = m->remote_snap_ring[s].server_time_ms;
            oldest_slot = s;
        }
    }

    int target;
    if (empty_slot >= 0) {
        target = empty_slot;
    } else {
        /* Ring is full. Only evict if incoming is newer than the
         * current oldest — otherwise the incoming is itself the
         * oldest and inserting it would just overwrite something
         * more useful. */
        if (stx < oldest_t) return;
        target = oldest_slot;
    }

    m->remote_snap_ring[target] = (RemoteSnapBuf){
        .server_time_ms = stx,
        .pelvis_x       = px,
        .pelvis_y       = py,
        .vel_x          = vx,
        .vel_y          = vy,
        .valid          = true,
    };
    m->remote_snap_head = (target + 1) % REMOTE_SNAP_RING;
    if (m->remote_snap_count < REMOTE_SNAP_RING)
        m->remote_snap_count++;
}

/* Pick the bracket pair around `t` from the per-mech ring. Returns
 * indices into the ring or sets *a == *b for clamp cases. */
static void pick_bracket(const Mech *m, uint32_t t, int *out_a, int *out_b) {
    /* Walk the ring in chronological order (oldest first). */
    int newest = -1; uint32_t newest_t = 0;
    int oldest = -1; uint32_t oldest_t = (uint32_t)-1;
    for (int i = 0; i < REMOTE_SNAP_RING; ++i) {
        if (!m->remote_snap_ring[i].valid) continue;
        uint32_t st = m->remote_snap_ring[i].server_time_ms;
        if (newest < 0 || st > newest_t) { newest = i; newest_t = st; }
        if (oldest < 0 || st < oldest_t) { oldest = i; oldest_t = st; }
    }
    if (newest < 0) { *out_a = -1; *out_b = -1; return; }
    if (t <= oldest_t) { *out_a = oldest; *out_b = oldest; return; }
    if (t >= newest_t) { *out_a = newest; *out_b = newest; return; }
    /* Find the pair (ai, bi) with ai.t <= t <= bi.t. */
    int   best_a = oldest;
    uint32_t best_a_t = oldest_t;
    int   best_b = newest;
    uint32_t best_b_t = newest_t;
    for (int i = 0; i < REMOTE_SNAP_RING; ++i) {
        if (!m->remote_snap_ring[i].valid) continue;
        uint32_t st = m->remote_snap_ring[i].server_time_ms;
        if (st <= t && st > best_a_t) { best_a = i; best_a_t = st; }
        if (st >= t && st < best_b_t) { best_b = i; best_b_t = st; }
    }
    *out_a = best_a;
    *out_b = best_b;
}

void snapshot_interp_remotes(World *w, uint32_t render_time_ms) {
    int local_id = w->local_mech_id;
    ParticlePool *pp = &w->particles;
    for (int mi = 0; mi < w->mech_count; ++mi) {
        if (mi == local_id) continue;
        Mech *m = &w->mechs[mi];
        if (m->remote_snap_count <= 0) continue;

        int ai, bi;
        pick_bracket(m, render_time_ms, &ai, &bi);
        if (ai < 0) continue;

        const RemoteSnapBuf *a = &m->remote_snap_ring[ai];
        const RemoteSnapBuf *b = &m->remote_snap_ring[bi];

        float target_x, target_y, target_vx, target_vy;

        /* wan-fixes-20 — forward extrapolation. When pick_bracket
         * clamps to newest (ai == bi) AND render_time is past that
         * sample, the original M5 P03 code froze the mech at
         * a->pelvis until a fresher snapshot arrived. On real WAN
         * with bursty delivery, render_time outpaces newest_t for
         * windows of 30–80 ms multiple times per minute → visible
         * stutter (mech freezes, then catches up). Instead, dead-
         * reckon forward using the last known velocity, capped at
         * EXTRAP_CAP_MS so we don't fling the mech off a cliff if
         * the link drops entirely. */
        const float EXTRAP_CAP_MS = 100.0f;
        if (ai == bi && a->server_time_ms < render_time_ms) {
            float extrap_ms = (float)(render_time_ms - a->server_time_ms);
            if (extrap_ms > EXTRAP_CAP_MS) extrap_ms = EXTRAP_CAP_MS;
            target_x  = a->pelvis_x + a->vel_x * (extrap_ms / 1000.0f);
            target_y  = a->pelvis_y + a->vel_y * (extrap_ms / 1000.0f);
            target_vx = a->vel_x;
            target_vy = a->vel_y;
        } else {
            float t = 0.0f;
            if (a != b) {
                uint32_t span = b->server_time_ms - a->server_time_ms;
                if (span > 0u) {
                    t = (float)((double)(render_time_ms - a->server_time_ms) /
                                (double)span);
                }
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
            }
            target_x  = a->pelvis_x + (b->pelvis_x - a->pelvis_x) * t;
            target_y  = a->pelvis_y + (b->pelvis_y - a->pelvis_y) * t;
            target_vx = a->vel_x    + (b->vel_x    - a->vel_x   ) * t;
            target_vy = a->vel_y    + (b->vel_y    - a->vel_y   ) * t;
        }

        int pelv = m->particle_base + PART_PELVIS;
        float dx = target_x - pp->pos_x[pelv];
        float dy = target_y - pp->pos_y[pelv];
        for (int part = 0; part < PART_COUNT; ++part) {
            int idx = m->particle_base + part;
            physics_translate_kinematic(pp, idx, dx, dy);
            physics_set_velocity_x(pp, idx, target_vx);
            physics_set_velocity_y(pp, idx, target_vy);
        }
    }
}

/* M6 P12 — Interpolate snapshot-replicated projectiles. For each
 * alive pool slot with `net_id != 0`, lerp pos/vel between the per-
 * projectile (snap_a, snap_b) pair at render_time_ms. State semantics:
 *   0 → no snapshots yet → leave pos untouched.
 *   1 → only b is valid (first snapshot just landed) → clamp pos to b.
 *   2 → both a and b valid → linear interp between them. When
 *        render_time outpaces b (jitter / drop) extrapolate forward
 *        with b's velocity for up to EXTRAP_CAP_MS so the projectile
 *        keeps flying instead of freezing mid-air (mirrors the
 *        wan-fixes-20 mech-interp behaviour). */
void snapshot_interp_projectiles(World *w, uint32_t render_time_ms) {
    ProjectilePool *pp = &w->projectiles;
    /* Same cap as mech interp — projectiles share the WAN-jitter
     * domain so the same forward-dead-reckon budget applies. */
    const float EXTRAP_CAP_MS = 100.0f;
    for (int i = 0; i < pp->count; ++i) {
        if (!pp->alive[i])      continue;
        if (pp->net_id[i] == 0) continue;
        uint8_t st = pp->snap_state[i];
        if (st == 0) continue;

        float target_x, target_y, target_vx, target_vy;
        if (st == 1 || render_time_ms <= pp->snap_a_time_ms[i]) {
            /* Pre-history clamp to a (or to b when state==1). */
            uint32_t bt = pp->snap_b_time_ms[i];
            if (st == 1 || render_time_ms >= bt) {
                target_x  = pp->snap_b_x [i];
                target_y  = pp->snap_b_y [i];
                target_vx = pp->snap_b_vx[i];
                target_vy = pp->snap_b_vy[i];
                /* Extrapolate forward past b (capped) so a brief
                 * snapshot gap doesn't freeze the grenade mid-arc. */
                if (st == 2 && render_time_ms > bt) {
                    float dt_ms = (float)(render_time_ms - bt);
                    if (dt_ms > EXTRAP_CAP_MS) dt_ms = EXTRAP_CAP_MS;
                    float dt_s = dt_ms * 0.001f;
                    target_x += target_vx * dt_s;
                    target_y += target_vy * dt_s;
                }
            } else {
                target_x  = pp->snap_a_x [i];
                target_y  = pp->snap_a_y [i];
                target_vx = pp->snap_a_vx[i];
                target_vy = pp->snap_a_vy[i];
            }
        } else {
            uint32_t at = pp->snap_a_time_ms[i];
            uint32_t bt = pp->snap_b_time_ms[i];
            if (render_time_ms >= bt) {
                target_x  = pp->snap_b_x [i];
                target_y  = pp->snap_b_y [i];
                target_vx = pp->snap_b_vx[i];
                target_vy = pp->snap_b_vy[i];
                float dt_ms = (float)(render_time_ms - bt);
                if (dt_ms > EXTRAP_CAP_MS) dt_ms = EXTRAP_CAP_MS;
                float dt_s = dt_ms * 0.001f;
                target_x += target_vx * dt_s;
                target_y += target_vy * dt_s;
            } else {
                uint32_t span = (bt > at) ? (bt - at) : 1u;
                float t = (float)((double)(render_time_ms - at) / (double)span);
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                target_x  = pp->snap_a_x [i] + (pp->snap_b_x [i] - pp->snap_a_x [i]) * t;
                target_y  = pp->snap_a_y [i] + (pp->snap_b_y [i] - pp->snap_a_y [i]) * t;
                target_vx = pp->snap_a_vx[i] + (pp->snap_b_vx[i] - pp->snap_a_vx[i]) * t;
                target_vy = pp->snap_a_vy[i] + (pp->snap_b_vy[i] - pp->snap_a_vy[i]) * t;
            }
        }

        pp->pos_x[i] = target_x;
        pp->pos_y[i] = target_y;
        pp->vel_x[i] = target_vx;
        pp->vel_y[i] = target_vy;
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
