# 10 — Performance Budget

This document is the **numbers** doc. Everything here is a target, every target is concrete, and exceeding a target is a decision we make on purpose, not by drift.

## The headline targets

| Target | Value | Measured on |
|---|---|---|
| Frame time | **≤ 16.6 ms** (60 FPS) | mid-range laptop, integrated GPU |
| Simulate tick | **≤ 4.0 ms** | same |
| Render | **≤ 8.0 ms** | same |
| Network downstream per client | **≤ 80 kbps** typical | 32-player match, 30 Hz snapshots |
| Host upstream | **≤ 2.0 Mbps** | 32 players, 30 Hz snapshots |
| Resident memory | **≤ 256 MB** | warm, mid-match |
| Cold disk | **≤ 50 MB** | binary + assets |
| Cold start to title | **≤ 2.0 s** | NVMe SSD |
| Cold start to title | **≤ 5.0 s** | spinning HDD |
| Connect-to-lobby | **≤ 1.5 s** | on a LAN; +RTT for internet |
| Build time (cold) | **≤ 10 s** | 8-core dev laptop |
| Build time (incremental) | **≤ 1 s** | one-file change |

Mid-range laptop reference: Intel i5-8300H (4-core, 2018-era), 16 GB RAM, integrated UHD 630. If we hit 60 FPS there, we hit 60 FPS everywhere current that anyone is buying.

## Frame budget breakdown

```
16.6 ms total
├── Simulate tick (every other frame on average — 60 Hz sim, ~120 FPS render)
│   ├── Input sample           0.05 ms
│   ├── Pose drive             0.10 ms
│   ├── Verlet integrate       0.20 ms
│   ├── Constraints (8 iter)   0.70 ms
│   ├── Map collision          1.00 ms
│   ├── Bullet sweeps (200)    0.50 ms
│   ├── Particles (4000)       0.30 ms
│   ├── Decals + bookkeeping   0.20 ms
│   ├── AI (bots only)         0.20 ms
│   ├── Game logic             0.30 ms
│   └── Subtotal               3.55 ms (budget: 4.0)
│
├── Net poll + receive          0.30 ms
│   ├── ENet poll
│   ├── Packet decode
│   └── Snapshot apply
│
├── Render                      8.00 ms budget
│   ├── Background parallax     0.30 ms
│   ├── Decal splat layer       0.20 ms
│   ├── World geometry          0.80 ms
│   ├── Mech bodies (32)        2.50 ms
│   ├── Projectiles             0.40 ms
│   ├── Particles               1.20 ms
│   ├── Pickups                 0.20 ms
│   ├── Post-FX                 0.50 ms
│   ├── HUD                     0.80 ms
│   ├── Chat / kill feed        0.30 ms
│   └── Buffer swap + vsync     1.00 ms (mostly wait)
│
└── Slack                       4.45 ms
```

The 4.45 ms slack is **deliberate**. We size the budget for 60 FPS at ~70% of frame time so we have headroom for spikes (a sudden explosion spawning hundreds of particles, a level-load tail, an OS hiccup).

## Memory budget

| Category | Budget | Notes |
|---|---|---|
| Permanent arena | 32 MB | Asset handles, network state, audio aliases |
| Level arena | 24 MB | Map polygons, decals chunks, pickup spawners |
| Frame arena | 4 MB | Per-tick scratch |
| Particle pool (4096) | 256 KB | SoA |
| Projectile pool (512) | 96 KB | |
| Decal RT layer | 16 MB | One 2048×2048 RGBA8 |
| Texture atlases | 80 MB | Mech sprites, particles, UI |
| Audio (decoded) | 30 MB | All SFX in RAM |
| Music streaming buffer | 1 MB | OGG decode |
| Font atlas | 4 MB | One TTF, three sizes |
| ENet buffers | 4 MB | Send/receive queues |
| Splat layer | 16 MB (overlap with decal RT) | |
| Stack | 8 MB | OS default |
| Misc / OS | 14 MB | |
| **Total** | **~213 MB** | Under 256 MB target |

## Network budget

### Per-client downstream (delta-encoded snapshot, 30 Hz, 32 players)

```
Average dirty entities per snapshot:    ~16
Bytes per dirty entity (after delta):   ~12
Snapshot payload:                       ~192 B
+ Snapshot header:                       ~12 B
+ ENet/UDP header:                       ~28 B
Total per snapshot:                     ~232 B
× 30 Hz:                              ~7000 B/s
                                     = 56 kbps  ✓ (target ≤ 80 kbps)
```

Plus reliable channel traffic (events, chat, lobby): ~5 kbps avg, bursting to ~20 kbps on round transitions. Total typical: ~60 kbps downstream.

### Host upstream

`56 kbps × 31 clients = 1.74 Mbps`. **Target ≤ 2 Mbps.**

If a host's measured uplink can't sustain that, the server lowers snapshot rate to 20 Hz automatically:
- 192 B/snapshot × 20 Hz × 31 clients = 1.16 Mbps. Cushion.

### Bandwidth fall-back ladder

The server adapts to its uplink:
1. Default: 30 Hz snapshots, full delta encoding, full visibility.
2. Tight: 20 Hz snapshots.
3. Tighter: 20 Hz + entity culling (skip players outside any other player's viewport).
4. Emergency: 10 Hz + cull + drop low-priority entities (pickups, decals).

We log the ladder steps so debugging connection problems is easy.

## CPU budget detail (per simulate tick)

The 3.55 ms simulate budget assumes:
- 32 active mechs (16 alive, 16 in various states of dying / sleeping)
- 200 active projectiles
- 4000 active particles
- 50 active decals being authored into the splat layer
- 8 bots running (in single-player practice)

Sleeping mechs (typical: 30–50% of the dead) cost effectively zero. As more mechs die and settle, the budget gets *easier*, not harder.

### Hot loops

The hottest loops, in order:

1. **Constraint relaxation**: `for cstr in constraints: solve(cstr)` × 8 iterations. ~5600 calls/tick. Each call is ~10 flops + 4 cache-line touches. SoA layout makes this cheap. **Profile target: 0.7 ms.**
2. **Verlet integrate**: `for particle in particles: integrate(p, dt)`. ~600 calls. **Profile target: 0.2 ms.**
3. **Particle integrate**: `for p in particle_pool: integrate(p, dt)`. ~4000 calls, simpler than mech particles. **Profile target: 0.3 ms.**
4. **Collision narrowphase**: `for particle: for nearby_poly: closest_point(...)`. Most expensive single operation. **Profile target: 1.0 ms.**

If we exceed targets, the order of investigation:

1. **Measure** — never speculate. Sampling profiler (perf, vtune, tracy in dev).
2. **Sleep more aggressively** — mechs that haven't moved enough.
3. **Reduce constraint iterations** off-screen / for distant mechs.
4. **Cull** particles offscreen earlier.
5. **SIMD the integrate loops** (last, because it costs readability).

## Render budget detail

8 ms render is generous for our scope but realistic on integrated GPUs. The big items:

- **Mech draws** (2.5 ms): 32 mechs × ~14 sprite parts × 1 draw call = ~448 draws. raylib batches these, so we end up with maybe a dozen real GL draw calls. The cost is mostly CPU-side — submitting vertices to the batch.
- **Particles** (1.2 ms): up to 4000 quads. One draw call total via batch + atlas. CPU-bound on submission.
- **World geometry** (0.8 ms): tile sprites visible (typically ~1500). Drawn as one batched textured-quad pass.

We do **not** instance via shaders at v1. raylib's CPU-side batching is enough. If we hit a wall, we switch the mech and particle draws to true instancing (`glDrawElementsInstanced`) — that's a 1-day refactor under raylib's `rlgl` lower layer.

## Asset budget

Each asset has a hard cap:

| Asset | Cap | Notes |
|---|---|---|
| Mech part sprite | 256×256 RGBA8 | 256 KB max |
| Particle | 64×64 | atlased into one 256×256 |
| UI element | 256×256 | atlased |
| Background | 4096×2048 | 32 MB |
| Map .lvl | 500 KB | Max 100 polygons + 10000 tiles |
| Sound effect | 200 KB WAV (16-bit mono) | ~2 sec at 22 kHz |
| Music | streamed OGG, no cap | But shipped ≤4 MB |
| Font | 1 TTF per project | 1–4 MB |

If we want to ship art that exceeds the cap, the cap goes up *on purpose*, not because someone exported at 4K thinking it'd be fine.

## Build performance

| Build step | Target | Strategy |
|---|---|---|
| Single .c → .o | <100 ms | Small TUs, minimal includes |
| Full link | <2 s | Static linking, no LTO at v1 |
| Cold build | <10 s | Parallel `-j` |
| Incremental | <1 s | Make's dependency tracking |

If a header is included by 30+ TUs and changes regularly, we move its volatile parts behind opaque pointers (forward-declare the struct in the public header, define in the .c). Saves recompile fan-out.

## Telemetry (none)

We do not collect player telemetry at v1. **No phone-home.** Not for crashes, not for analytics, not for "feature usage." If we ever do, it is opt-in, documented, and disabled in the default build flag.

## Profiling we DO use, in development

- **F3** in-game frame timing overlay (described in [09-codebase-architecture.md](09-codebase-architecture.md)).
- **F4** in-game network stats overlay: round-trip, jitter, packet loss, downstream/upstream bytes.
- **F5** in-game memory dump: arena fill rates, pool occupancy.
- **`perf record`** on Linux for deep CPU traces.
- **`heaptrack`** on Linux for allocation traces during dev (we expect *zero* allocations per frame in steady state).

## Performance regressions

Every commit that touches a performance-sensitive module includes a one-line note on the impact, measured. If we don't have a number, we say so:

> "physics: split constraint loop into per-color groups. Measured 0.6 → 0.5 ms in 32-mech profile bench (laptop reference)."

We don't merge regressions on faith. We don't merge optimizations on faith either.

## What we are NOT optimizing for

- **Steam Deck** at v1. We will probably run fine on it; we don't profile it.
- **8K resolutions**. Internal world-render resolution is capped at the
  `internal_res_h` setting (default 1080 lines, soldut.cfg / soldut-prefs.cfg
  / `--internal-h` CLI override). World + post-process pass run at the capped
  height; the result is bilinear-upscaled to the window. HUD draws at window
  resolution on top, unshaded. See `documents/m6/03-perf-4k-enhancements.md`.
- **120+ FPS**. We design for 60 FPS lock. If your monitor is 144 Hz, you get 60 FPS sim with 144 Hz interpolated render — perfectly fine.
- **GPU-bound workloads**. We are CPU-bound by design (lots of small draws, lots of physics). Optimizing the GPU before the CPU is optimizing the wrong thing.
- **Per-platform CPU intrinsics**. We write portable C. SIMD via auto-vectorization. If we ever need explicit SIMD, we wrap it in `<simd.h>` with platform fallbacks — but we expect not to.

## Slack and decision-making

The 4.45 ms frame slack is **mortgageable**. If a feature wants 1 ms, we can grant it — but explicitly, with a number. We do not let slack erode invisibly.

This document is the contract. When the code disagrees with this document, the code is wrong (per [01-philosophy.md](01-philosophy.md), Rule 7). Either bring the code back inside budget or update the budget on purpose, with a one-paragraph justification.
