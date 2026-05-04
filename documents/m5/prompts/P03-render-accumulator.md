# P03 — Render-side accumulator + interpolation alpha + reconcile smoothing + is_dummy bit

## What this prompt does

Lands four small but coupled visual-fidelity fixes, all consequences of M5's new feedback layers (hit-flash tint, smoke from damaged limbs, slope physics) interacting with the M1/M2 frame-pacing trade-offs. After this prompt, render-rate is decoupled from sim-rate (vsync-fast no longer accelerates physics), the local mech smoothly interpolates over reconciliation snaps, remote mechs render from a proper two-snapshot buffer ~100 ms in the past, and the practice-dummy `is_dummy` bit rides snapshots so remote dummies don't aim.

This prompt is independent of P01/P02 and can run in parallel — it doesn't touch level/physics paths. But run it before P10–P12 (rendering work) so the renderer has the alpha lerp in place when sprite atlases land.

## Required reading

1. `CLAUDE.md` — project conventions
2. `documents/01-philosophy.md` — Rule 7 (commit to numbers)
3. `documents/03-physics-and-mechs.md` — fixed-step accumulator + render interpolation
4. `documents/05-networking.md` — §"Snapshot interpolation", §"Server reconciliation"
5. `CURRENT_STATE.md` — "Vsync / FPS mismatch", the M4 reconcile/jitter bugs in "Recently fixed"
6. `TRADE_OFFS.md` — "60 Hz simulation, not 120 Hz", "No render-to-sim interpolation alpha", "Vsync / frame-pacing leak", "No client-side visual smoothing of reconciliation jumps", "Snapshot interp is a 35% lerp, not a full two-snapshot buffer", "Dummy is a non-dummy on the client"
7. **`documents/m5/13-controls-and-residuals.md`** §"Trade-offs picked up at M5" — the spec for this prompt
8. **`documents/m5/08-rendering.md`** §"Render-side interpolation alpha" — implementation detail
9. `src/main.c` — current accumulator loop in MODE_MATCH
10. `src/world.h` — `ParticlePool`, `Mech`, `EntitySnapshot`
11. `src/render.c` — `renderer_draw_frame`, `draw_mech` (every read of `pos_x/pos_y`)
12. `src/snapshot.{c,h}` — `snapshot_apply`, `SNAP_STATE_*`
13. `src/reconcile.{c,h}` — `Reconcile.visual_offset` (computed but unused)

## Background

The M4 build calls `simulate()` once per render frame with `dt = wall-clock since last render`. On a 144 Hz vsync display, sim runs faster than 60 Hz. Hit-flash timers, animation phase, projectile motion all accelerate with frame rate. The user has already noticed this in playtest ("jet feels different in fullscreen" — see CURRENT_STATE.md).

Reconciliation in M2 lands the local mech to the server's authoritative position, but at WAN ping the snap is visible. The current 35% lerp on remote mechs is fine on LAN but wobbles on a real internet connection.

The practice-dummy spawned at M5 (P05) has `is_dummy = true` only on the host; on remote clients it spawns as a regular mech that tries to drive its right arm to its (snapshot-set) aim_world. Cosmetic but visible.

## Concrete tasks

### Task 1 — Per-particle previous-frame snapshot

Add to `ParticlePool` in `src/world.h`:

```c
float *render_prev_x;     // pos_x at the start of the most-recent simulate tick
float *render_prev_y;
```

**Names matter.** `prev_x/prev_y` is Verlet's previous position (`pos - prev = velocity`). `render_prev_x/_y` is the render snapshot — the value `pos_x` had at the *start* of the most-recent simulate tick. Decoupled.

Allocate alongside the existing pool fields at startup.

### Task 2 — Snapshot at start of simulate tick

In `src/simulate.c::simulate_step`, at the very top before any work:

```c
ParticlePool *pp = &w->particles;
for (int i = 0; i < pp->count; ++i) {
    pp->render_prev_x[i] = pp->pos_x[i];
    pp->render_prev_y[i] = pp->pos_y[i];
}
```

Also snapshot equivalents for projectile positions and FX particle positions (you'll need to add `render_prev_*` fields to those pools too — same pattern).

### Task 3 — Renderer reads alpha + lerps

`src/main.c` already computes `alpha = (float)(accum / TICK_DT)` and passes it to `renderer_draw_frame` (look around `MODE_MATCH`). The renderer currently ignores it.

In `src/render.c`, add an inline helper:

```c
static inline Vec2 particle_render_pos(const ParticlePool *p, int i, float alpha) {
    return (Vec2){
        p->render_prev_x[i] + (p->pos_x[i] - p->render_prev_x[i]) * alpha,
        p->render_prev_y[i] + (p->pos_y[i] - p->render_prev_y[i]) * alpha,
    };
}
```

Replace every read of `p->pos_x[idx]/p->pos_y[idx]` in `draw_mech`, projectile draw, FX draw, decal draw, etc. with `particle_render_pos(p, idx, alpha)`. Plumb `alpha` through the draw functions that need it.

`hud_draw` doesn't need alpha — HUD reads `mech` fields, not particle pool, and those don't tween mid-tick.

### Task 4 — Reconcile visual offset

`src/reconcile.c` already computes `visual_offset` and decays it (look for `visual_smoothing.target` or similar). The renderer doesn't read it. Wire that up:

When drawing the local mech, apply `reconcile.visual_offset` as an additive offset to all the mech's particles' rendered positions. The offset decays toward zero over ~100 ms (~6 frames at 60 Hz). On a snapshot apply with a sub-pixel correction, the offset is invisible; on a 9-pixel correction, it smooths over 6 frames instead of snapping.

### Task 5 — Two-snapshot interpolation buffer for remote mechs

Per `documents/05-networking.md` §"Snapshot interpolation": render remote mechs ~100 ms in the past, lerping between the two most recent snapshots.

Replace the M2 35% lerp (look in `src/snapshot.c::snapshot_apply` for the `0.35f` constant) with:

1. Keep the last 3 snapshots per remote mech in a ring (`SnapshotFrame ring[3]; int ring_head;`).
2. Render time = `now - 100 ms` (configurable; per the doc).
3. Find the bracket pair (snapshot just before render-time + snapshot just after).
4. Interpolate: `t = (render_time - snap_a.server_time_ms) / (snap_b.server_time_ms - snap_a.server_time_ms)`. Clamp to `[0, 1]`.
5. For each particle in each remote mech, lerp position between `snap_a.pos` and `snap_b.pos`.

Local mech still snaps fully (the reconcile path replays inputs from a known-truth anchor). The 100 ms lookback applies to remote mechs only.

Fall back to the most recent single snapshot if only one is available (early in a connection).

### Task 6 — `is_dummy` bit on the wire

Per `documents/m5/13-controls-and-residuals.md` §"is_dummy bit on the wire":

In `src/snapshot.h`, add to the SNAP_STATE_* enum:

```c
SNAP_STATE_IS_DUMMY = 1u << 11,
```

The `state_bits` field is u16 — there's room.

In `snapshot_capture` (server), set the bit if `m->is_dummy`. In `snapshot_apply` (client), set `m->is_dummy = (snap->state_bits & SNAP_STATE_IS_DUMMY) != 0`. Bump `SOLDUT_PROTOCOL_ID` from `S0LF` to `S0LG` if you haven't already (P05 may bump it for powerup state — check `version.h`).

Verify: client-side dummy doesn't try to drive its right arm aim. Look at `src/mech.c::build_pose` for the `if (!m->is_dummy)` gate.

### Task 7 — Update the close-window path (housekeeping)

While here, ensure `src/main.c`'s shutdown path handles the new pool fields cleanly. The M1 fix (`_exit(0)` after the loop) bypasses arena destroy — that's fine. Just make sure no NULL deref on the new pointer fields if shutdown triggers before init completes.

## Done when

- `make` builds clean.
- Running `./soldut --host 23073` on a 144 Hz display: sim no longer accelerates with frame rate. (Verify with `tests/shots/m5_vsync.shot` — capture pelvis position over 600 ticks; should match the 60 Hz reference.)
- A WAN-simulated reconciliation snap (use `tc qdisc add dev lo root netem delay 80ms` on Linux to simulate latency, run `./tests/net/run.sh -k`) shows smooth local-mech motion, not a hard pop.
- A remote mech walking past at 280 px/s renders smoothly with no visible jitter — the two-snapshot buffer hides the 33 ms snapshot interval.
- Practice dummy on a remote client (after P05 lands, retest) doesn't aim its right arm.
- The headless sim's reference output matches pre-change values to FP precision (the snapshot-at-start-of-tick is deterministic).
- Performance: snapshot pass < 50 µs, render lerp adds < 100 µs total per frame. Inside slack.

## Out of scope

- The 120 Hz toggle. Accumulator infrastructure lands here; the actual rate stays 60 Hz. Decision deferred to playtest after maps ship.
- Server-side entity culling. Tracked in TRADE_OFFS.md, deferred past M5.
- Snapshot delta encoding. Same.
- HMAC-SHA256. Same.

## How to verify

```bash
make
./tests/net/run.sh
./tests/net/run_3p.sh
./tests/shots/net/run.sh 2p_jitter   # the existing jitter test — should still pass + look smoother
```

A new shot test `tests/shots/net/m5_smoothing.shot` (write it as part of this prompt) walks a remote mech across the screen and captures every 10 ticks. Diff against pre-change PNG output: smoother motion, no pop.

## Close-out

1. Update `CURRENT_STATE.md`:
   - Note the accumulator + interp alpha is in.
   - Note the reconcile smoothing + two-snapshot buffer.
   - Note `is_dummy` bit on the wire.

2. Update `TRADE_OFFS.md`:
   - **Delete** "No render-to-sim interpolation alpha".
   - **Delete** "Vsync / frame-pacing leak".
   - **Delete** "No client-side visual smoothing of reconciliation jumps".
   - **Delete** "Snapshot interp is a 35% lerp, not a full two-snapshot buffer".
   - **Delete** "Dummy is a non-dummy on the client".
   - **Update** "60 Hz simulation, not 120 Hz" to note the accumulator is in place; the rate decision is deferred.

3. Bump `SOLDUT_PROTOCOL_ID` in `src/version.h` if not already done by another prompt.

4. Don't commit unless explicitly asked.

## Common pitfalls

- **Forgetting to initialize `render_prev_*` at pool creation** — first-tick reads will pull garbage. Memset to zero or init to current pos.
- **Forgetting to plumb alpha through all draw functions** — the project uses `void renderer_draw_frame(... float alpha ...)` already; pass it down to every helper that reads particle positions.
- **Lerping projectile draw without snapshotting projectile prev positions** — every pool you read in render needs the prev-frame snapshot. Forgetting this means projectiles stutter.
- **Reconcile.visual_offset is per-mech**, not per-particle. Apply it as an offset added to every particle of the local mech's draw — don't try to re-derive per-particle.
- **Two-snapshot buffer wraparound**: the ring is small (3); when a snapshot drops, the buffer might not have a "before" entry. Fall back to extrapolation OR clamp to the most recent snapshot. Don't render at undefined position.
- **Protocol ID bump triggers handshake reject** between mismatched-version client/server. That's intentional but expect existing test scripts to break until restarted.
