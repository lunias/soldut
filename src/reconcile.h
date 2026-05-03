#pragma once

#include "input.h"
#include "snapshot.h"
#include "world.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Client-side prediction + reconciliation. (See [05-networking.md] §3.)
 *
 * Each client tick:
 *   1. Sample input.
 *   2. Tag with sequence number.
 *   3. Latch onto the local mech and call simulate() — the world advances
 *      with zero perceived input latency.
 *   4. Push the input into a circular buffer of unacked inputs.
 *   5. Send to server.
 *
 * When a snapshot arrives:
 *   1. Look up the snapshot's ack_input_seq for this client.
 *   2. Drop any inputs in the buffer with seq <= ack_input_seq.
 *   3. Overwrite the local world from the snapshot (truth).
 *   4. Replay remaining buffered inputs through simulate() so the local
 *      mech catches back up to "now."
 *   5. Smooth the visible position toward the corrected one over
 *      ~6 frames so corrections aren't visually jarring.
 */

/* Capacity of the unacked-input ring. At 60 Hz this gives 1 second of
 * runway — way more than any real RTT we care about. */
#define RECON_INPUT_BUF 60

typedef struct {
    ClientInput inputs[RECON_INPUT_BUF];
    int         head;             /* next write slot */
    int         count;            /* # items currently buffered */
} InputRing;

typedef struct {
    InputRing input_ring;

    /* Visual smoothing: the last position the local mech rendered at,
     * vs. its server-corrected current position. We blend toward
     * `target` over `smooth_t` frames. */
    Vec2     visual_offset;       /* current applied offset (added to render pos) */
    Vec2     visual_offset_target;/* always (0,0) — lerped toward */
    int      smooth_frames_left;
} Reconcile;

/* Init: zero the ring + smoother. */
void reconcile_init(Reconcile *r);

/* Push an input we just sampled onto the ring. Tags it with `seq`. */
void reconcile_push_input(Reconcile *r, ClientInput in);

/* Drop already-acked inputs (seq <= ack_seq). Called after a snapshot
 * applies. Wraps cleanly via uint16 arithmetic. */
void reconcile_drop_acked(Reconcile *r, uint16_t ack_seq);

/* Apply `snap` to the world (overwrite all mech states from server),
 * then replay the still-unacked inputs through simulate() to bring
 * the local mech back up to "now". Records the visual delta on
 * the local mech so render can smooth the correction over a few
 * frames. */
void reconcile_apply_snapshot(Reconcile *r, World *w,
                              const SnapshotFrame *snap,
                              uint16_t ack_input_seq,
                              float dt);

/* Per-frame smoothing tick. Decays visual_offset toward zero. */
void reconcile_tick_smoothing(Reconcile *r);
