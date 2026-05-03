#include "reconcile.h"

#include "log.h"
#include "simulate.h"

#include <math.h>
#include <string.h>

void reconcile_init(Reconcile *r) {
    memset(r, 0, sizeof *r);
    r->visual_offset_target = (Vec2){0,0};
}

void reconcile_push_input(Reconcile *r, ClientInput in) {
    InputRing *ring = &r->input_ring;
    ring->inputs[ring->head] = in;
    ring->head = (ring->head + 1) % RECON_INPUT_BUF;
    if (ring->count < RECON_INPUT_BUF) ring->count++;
}

/* Drop everything in the ring whose seq is <= ack_seq. We keep the
 * ring stored newest-last (head points one past the last stored
 * input), so we walk the buffered inputs and copy out only those
 * still unacked. */
void reconcile_drop_acked(Reconcile *r, uint16_t ack_seq) {
    InputRing *ring = &r->input_ring;
    if (ring->count == 0) return;

    /* Walk in seq order: oldest first. */
    int start = (ring->head - ring->count + RECON_INPUT_BUF) % RECON_INPUT_BUF;
    ClientInput tmp[RECON_INPUT_BUF];
    int kept = 0;
    for (int i = 0; i < ring->count; ++i) {
        int idx = (start + i) % RECON_INPUT_BUF;
        ClientInput in = ring->inputs[idx];
        /* uint16 wrap-aware compare: keep `in` if (in.seq - ack_seq)
         * is positive in signed-16-bit arithmetic. */
        int16_t delta = (int16_t)(in.seq - ack_seq);
        if (delta > 0) tmp[kept++] = in;
    }
    /* Rewrite the ring tightly. */
    for (int i = 0; i < kept; ++i) ring->inputs[i] = tmp[i];
    ring->head  = kept % RECON_INPUT_BUF;
    ring->count = kept;
}

void reconcile_apply_snapshot(Reconcile *r, World *w,
                              const SnapshotFrame *snap,
                              uint16_t ack_input_seq,
                              float dt)
{
    if (!snap || !snap->valid) return;
    int local_id = w->local_mech_id;

    /* Capture pre-correction visible position so we can render-smooth
     * the jump. Only the pelvis is enough — the body is rigid via
     * constraints. */
    Vec2 pre = {0,0};
    if (local_id >= 0 && local_id < w->mech_count) {
        const Mech *m = &w->mechs[local_id];
        int p = m->particle_base + PART_PELVIS;
        pre = (Vec2){ w->particles.pos_x[p], w->particles.pos_y[p] };
    }

    /* Authoritative truth from the server. */
    snapshot_apply(w, snap);

    /* Drop already-acked inputs from the ring, then replay the
     * remainder so the local mech's predicted state catches back up
     * to "now." Each replay tick reuses the existing simulate_step
     * pipeline — same physics, same pose drive, same constraints. */
    reconcile_drop_acked(r, ack_input_seq);

    InputRing *ring = &r->input_ring;
    int start = (ring->head - ring->count + RECON_INPUT_BUF) % RECON_INPUT_BUF;
    for (int i = 0; i < ring->count; ++i) {
        int idx = (start + i) % RECON_INPUT_BUF;
        if (local_id >= 0 && local_id < w->mech_count) {
            w->mechs[local_id].latched_input = ring->inputs[idx];
        }
        simulate_step(w, dt);
    }

    /* Compute visible delta and start a smooth-blend toward the
     * corrected position. We don't actually move the renderer — the
     * delta is exposed via reconcile_visual_offset() and applied as a
     * camera-side sub-pixel nudge. M2 keeps it simple by just logging
     * the magnitude; the smoothing path is wired but the renderer
     * hasn't been told yet. (See TRADE_OFFS.md "no visual smoothing
     * of reconciliation jumps"). */
    if (local_id >= 0 && local_id < w->mech_count) {
        const Mech *m = &w->mechs[local_id];
        int p = m->particle_base + PART_PELVIS;
        Vec2 post = { w->particles.pos_x[p], w->particles.pos_y[p] };
        Vec2 d = { pre.x - post.x, pre.y - post.y };
        float mag2 = d.x * d.x + d.y * d.y;
        if (mag2 > 1.0f) {       /* > 1 px — worth smoothing */
            r->visual_offset = d;
            r->smooth_frames_left = 6;
        }
    }
}

void reconcile_tick_smoothing(Reconcile *r) {
    if (r->smooth_frames_left <= 0) return;
    /* Geometric decay over 6 frames toward (0,0). */
    r->visual_offset.x *= 0.6f;
    r->visual_offset.y *= 0.6f;
    r->smooth_frames_left--;
    if (fabsf(r->visual_offset.x) < 0.5f && fabsf(r->visual_offset.y) < 0.5f) {
        r->visual_offset = (Vec2){0,0};
        r->smooth_frames_left = 0;
    }
}
