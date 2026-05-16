# M6 follow-up — Per-tick projectile position replication

## Prompt for Claude

You are picking up a network-sync improvement for bouncy projectiles.
Read [`11-frag-grenade-tuning.md`](11-frag-grenade-tuning.md) first
to understand the current mechanic; this doc is the spec for the
remaining client/server sync work.

### Problem

Bouncy AOE projectiles (currently `PROJ_FRAG_GRENADE`; in the future
any `wpn->bouncy == true` projectile) diverge between the client's
local simulation and the server's authoritative simulation over the
projectile's lifetime. The two sims start from the same FIRE_EVENT
(same initial pos, vel, owner) and run the same deterministic
physics, but:

- Collision tests against mech bones differ. Remote mech particles
  on the client are positioned by `snapshot_interp_remotes` (lagged
  ~INTERP_DELAY_MS behind the server's authoritative state). The
  bone the projectile sees on the client may be a few pixels off
  from where the server sees it. A grenade that just barely misses
  a bone on the client may direct-hit on the server (or vice
  versa). After one missed/extra hit the trajectories diverge.

- The bounce reflection is sensitive to floating-point velocity
  inputs (`fabsf(vel.x) > fabsf(vel.y)` axis pick, surface-normal
  dot products). After three or four bounces the client and server
  positions can be hundreds of pixels apart.

### Current workarounds (see `net.c::client_handle_explosion`,
`projectile.c::detonate`)

- Client predicts its own AOE detonations and pushes an
  `EXPL_SRC_PREDICTED` record; wire-receive finds the record and
  dedupes so we don't double-spawn FX or overwrite
  `last_explosion_pos`. The dedupe window is bumped to 600 ticks
  (~10 s) to cover bouncy-fuse + sim divergence.
- When no predict matches (server detonated first or divergence
  exceeds the dedupe window), the client snaps the visible
  projectile's pos to the server's broadcast pos for the dying
  frame so the sprite "slides" to where the explosion actually
  fires. Small divergence: invisible. Large divergence: visible
  hop, but at least the FX center matches the damage center.

These keep the FX cosmetically aligned but they don't fix the root
divergence. Symptoms the user still reports occasionally:

- Grenade visually rolls past a mech, no damage (server saw it
  detonate elsewhere).
- Grenade visually contacts a mech's face, no damage.
- Grenade vanishes mid-air on the client, then explosion FX appears
  somewhere off-screen.

## Goal

Make the client's bouncy-projectile visual and the server's
authoritative state STAY IN SYNC tick-to-tick by replicating
projectile positions over the wire. After this work:

- The client's rendered grenade is at the server's last-broadcast
  position (interpolated between snapshot ticks).
- Collision and damage outcomes match what the player sees.
- "Grenade in the face → damage" is reliable.

## Approach (recommended)

Add projectile entries to the existing snapshot stream. Build on
the same wire / interp infrastructure that already replicates
mechs.

### Wire changes

`world.h` likely needs (verify by reading current code):

```c
typedef struct {
    uint16_t id;          /* projectile slot index — STABLE for the
                             projectile's lifetime, not the pool slot */
    uint8_t  kind;        /* ProjectileKind */
    uint8_t  owner_mech;
    int16_t  pos_x_q;     /* 1/4-px fixed-point, matches mech wire
                             quantization in EntitySnapshot */
    int16_t  pos_y_q;
    int16_t  vel_x_q;     /* px/s in some quantization */
    int16_t  vel_y_q;
    uint8_t  flags;       /* bouncy, exploded, alive — packed */
    uint8_t  fuse_ticks;  /* remaining life in ticks, clamped 255 */
} ProjectileSnapshot;
```

12 bytes per projectile is workable. Mechs are 29-31 bytes per
EntitySnapshot, so this is comparable density.

Update protocol id (`S0LK` → `S0LL`) so old clients reject the
new stream rather than misparse.

Add a count + array of `ProjectileSnapshot` to each snapshot
packet, after the EntitySnapshot array. Cap at a sensible limit
(64 active projectiles is plenty — current pool capacity is
larger but per-tick alive count is typically <10).

### Wire-encode (server)

In `snapshot.c::snapshot_encode`-equivalent, after writing the
mech entries, iterate `world.projectiles`, skip dead / non-replicate
kinds (hitscan tracers don't need this; grapple head already has
its own state path), write one entry per alive AOE projectile.

### Wire-decode (client)

`snapshot.c::snapshot_apply`-equivalent: read the projectile array,
and for each entry decide:

- **Match by `id`**: a stable per-projectile id (not pool slot).
  Server assigns at spawn time (monotonic counter, wrap at 16 bits
  is fine — collisions inside the dedupe window are vanishingly
  unlikely).
- If the client has an entry with this id: update its position
  toward the server's via interp (same `client_render_time_ms`
  scheme as `snapshot_interp_remotes`). Don't snap directly —
  interp between bracketing snapshots for smooth motion.
- If the client has no entry: spawn one (this is how mid-air joins
  see grenades already in flight, AND the steady-state path for
  bouncy projectiles since FIRE_EVENT's spawn happens elsewhere
  per the current wan-fixes-10 path).
- If a previously-seen id is missing from a snapshot: mark dying
  if the snapshot is fresh (server killed it). Don't immediately
  remove — wait for the matching `NET_MSG_EXPLOSION` to drive the
  dying-frame + FX, same as today.

### Stop client-side bouncy physics

With server pos replicated each tick, the client no longer needs
to simulate bouncy physics locally. Two options:

- **Hard stop**: in `projectile_step`, skip the
  drag/gravity/integrate/collide passes for bouncy projectiles
  when `!w->authoritative`. Position comes entirely from snapshot
  interp.
- **Predict + correct**: keep the local sim for between-snapshot
  smoothness, but each snapshot-apply tick reconciles the local
  position toward the server's. Smoother visually but more code.

Recommend hard-stop for v1 — easier to verify, and snapshot at
60 Hz gives 16 ms of inherent jitter which interp smooths out.

### Keep the predict-record dedupe

Don't rip out `EXPL_SRC_PREDICTED`. The client may still detonate
LOCALLY (via fuse expiry during the snapshot lag window) before
the server's NET_MSG_EXPLOSION arrives — the record still dedupes
that case, just with much smaller divergence now that the position
came from the server every tick.

### Direct hits

With positions in lockstep, the bone-collision test runs against
matching state on both sides. Direct-hit damage is consistent.

## Acceptance

- `tests/shots/net/run_frag_concourse.sh` passes and the host log
  shows the SAME damage events the client log shows
  client_handle_explosion arriving for (no asymmetric damage).
- A new paired test `tests/shots/net/run_frag_sync.sh` that throws
  3 grenades, asserts the client's `client_handle_explosion`
  positions are within ≤ 4 px (1 snapshot of motion) of the
  server's `explosion at` positions.
- Existing `tests/shots/net/run_frag_grenade.sh` (wan-fixes-10
  regression) goes from 7/9-or-9/9 flake to consistent 9/9 (the
  current flake is wire-event delivery timing under sim
  divergence; with synced positions it stabilizes).
- All other paired + unit tests still green: `run_frag_charge.sh`,
  `2p_basic`, `2p_atmosphere_parity`, `test-snapshot`,
  `test-frag-grenade`, `test-mech-ik`, `test-pose-compute`,
  `test-ctf`, `test-pickups`, `test-damage-numbers`,
  `test-grapple-ceiling`, `test-atmosphere-parity`,
  `test-map-share`, `test-spawn`, `test-prefs`, `test-level-io`.

## Out of scope

- Snapshot bandwidth optimization (delta encoding per projectile).
  At 12 bytes × 10 projectiles × 60 Hz = 7.2 KB/s of new traffic,
  worth it for the gameplay fix; revisit if perf-baseline shows
  pressure.
- Grapple head replication. Already handled via the
  `GRAPPLE_FLYING / ATTACHED` state on the firer's mech snapshot.
- Hitscan tracer replication. Not needed — hitscan resolves on the
  same tick.
- Snapshot lag-comp for projectile collisions. Hitscan already has
  this (`weapons_fire_hitscan_lag_comp`); not needed for bouncy
  projectiles since their flight time dominates RTT and the
  player isn't expected to land instant-frame hits with a thrown
  weapon.

## Estimated size

~300-400 LOC across `snapshot.c/h`, `net.c`, `projectile.c`,
`world.h`. Plus the new paired test (`~150 LOC`). Plus protocol-id
bump touches a handful of `.h` constants. Plan for 1-2 sessions
including paired-shot iteration.
