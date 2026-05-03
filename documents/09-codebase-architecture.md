# 09 — Codebase Architecture

This document specifies the **layout** of the source: folders, modules, what each `.c` and `.h` is allowed to know, and how memory flows. It is the structural counterpart to [01-philosophy.md](01-philosophy.md): philosophy says *how to think*; this says *how to file*.

## Top-level layout

```
soldut/
├── src/                    # game source — about 50 .c/.h files at v1
├── tools/                  # support utilities (level cooker, replay extractor)
├── assets/                 # ships with the binary
├── third_party/
│   ├── raylib/             # vendored, with prebuilt static libs per platform
│   ├── enet/               # vendored, builds locally
│   ├── stb_ds.h            # the only stb header we vendor directly
│   └── stb_sprintf.h
├── documents/              # the docs you're reading
├── tests/                  # unit / regression tests
├── Makefile                # native build
├── build.sh                # one-shot orchestration
├── cross-windows.sh        # cross-compile to Windows
├── cross-macos.sh          # cross-compile to macOS
└── README.md               # build instructions, contributor pointer
```

`src/` is the only place game source lives. Tools have their own `tools/<name>/` subfolders, each with its own tiny Makefile. Assets ship verbatim — no asset-pack format at v1.

## `src/` modules

The game is decomposed into modules. Each module is **one `.h` + one `.c`**. The `.h` is the public surface; the `.c` is the implementation. If a module grows past ~1500 LOC, we split into `<name>.c` + `<name>_internal.c`, but we do not split the public header.

```
src/
├── main.c                  # entry point, top-level loop
├── platform.{c,h}          # window, input, time — wraps raylib for our use
├── game.{c,h}              # top-level Game struct, init/shutdown
├── world.{c,h}             # World struct, the entire simulation state
├── simulate.{c,h}          # the pure simulate() function
├── physics.{c,h}           # Verlet, constraints, collision
├── mech.{c,h}              # Mech struct, chassis, animation
├── weapons.{c,h}           # weapon table, fire logic
├── projectile.{c,h}        # bullets, grenades, rockets
├── particle.{c,h}          # blood, sparks, smoke
├── decal.{c,h}             # splat layer
├── pickup.{c,h}            # map pickups
├── level.{c,h}             # .lvl loader, tile grid, polygons
├── render.{c,h}            # high-level draw orchestration
├── hud.{c,h}               # screen-space UI
├── lobby.{c,h}             # waiting room, chat, ready-up
├── server_browser.{c,h}    # discovery + direct connect
├── net.{c,h}               # ENet wrapper, packet schemas
├── snapshot.{c,h}          # snapshot encode/decode + delta + interp
├── reconcile.{c,h}         # client prediction + reconciliation
├── input.{c,h}             # input bitmask + buffer
├── audio.{c,h}             # sound aliases, attenuation, mix
├── arena.{c,h}             # arena allocator
├── pool.{c,h}              # fixed-size pool allocator
├── log.{c,h}               # logger
├── math.{c,h}              # math helpers beyond raymath (rare)
├── hash.{c,h}              # PCG, FNV, simple hash table
├── ds.c                    # one .c that #defines STB_DS_IMPLEMENTATION
├── hotreload.{c,h}         # mtime watcher
└── version.h               # version constants
```

That is the entire public structure. Anything we want to add asks: which existing module owns this? If none, it gets a new `name.{c,h}` pair, never a folder.

## `Game *g` — the spine

There is exactly one mutable global concept: the `Game` struct, passed by pointer through every layer that needs it. raylib's globals are an exception we accept (one process owns the GL context); our gameplay is testable.

```c
typedef struct Game {
    // Allocators — owned by Game, used by everyone.
    Arena permanent;
    Arena level_arena;
    Arena frame_arena;

    // Pools — fixed at startup.
    ParticlePool particles;
    ProjectilePool projectiles;
    DecalPool decals;

    // Subsystems.
    Platform platform;
    NetState net;
    AudioState audio;

    // World — the simulation state.
    World world;

    // Local-client state (host runs this too).
    LocalClient local;

    // Lobby state when in lobby; world state when in match.
    GameMode mode;            // LOBBY, MATCH, SUMMARY

    // Mode-specific.
    LobbyState lobby;
    MatchState match;
} Game;
```

`Game *g` is the first parameter to most public functions. We do not pass `World *`, `Platform *`, `NetState *` separately if we already need `g`.

## Memory model

Three arenas, one frame pool, several object pools.

### Arenas

```c
typedef struct {
    uint8_t *base;
    size_t   size;
    size_t   used;
} Arena;

void *arena_alloc(Arena *a, size_t bytes);
void  arena_reset(Arena *a);
```

| Arena | Lifetime | Reset when |
|---|---|---|
| `permanent` | process | never |
| `level_arena` | match | round end |
| `frame_arena` | one tick | every tick |

Strings, scratch buffers, snapshot encoding scratch — all use `frame_arena`. Level geometry, decal chunks, pickup spawners — `level_arena`. Asset handles, network state, audio buffers — `permanent`.

### Pools

```c
typedef struct {
    void   *items;
    int     stride;        // bytes per item
    int     capacity;
    int     count;
    int     free_head;     // index of first free item, or -1
} Pool;
```

Pools have stable indices. Freeing an item flips it to the free list; allocating pops from the free list. **Item indices are stable across operations** — we use them as identifiers for projectiles, mechs, particles.

For SoA pools (particles, projectiles), the `Pool` macro generates the parallel arrays.

### Allocation rules

- `malloc` / `free` are called **only** during init (allocating arenas + pools) and shutdown.
- Frame allocations come from `frame_arena` — reset every simulate-tick at zero cost.
- Long-lived allocations come from `level_arena` — reset on level change.
- Permanent state lives in `permanent`.
- We do not call `malloc` in any inner loop. If we find ourselves wanting to, we grow the right arena.

## Cross-module rules

### Allowed dependencies

Each module's `.h` declares what it depends on. The dependency graph is a DAG; we enforce it socially (and with a one-line script that greps `#include "..."` and detects cycles in CI).

```
                         main
                           ↓
                         game
                       /       \
                    world      platform
                    /  \           ↓
              simulate  net    audio
                |        ↓
              physics  snapshot
              mech     reconcile
              weapons
              projectile
              particle
              decal
              pickup
              level
              render
                ↑
              hud
              lobby
              server_browser
                ↑
              input

              arena, pool, log, math, hash, ds   (leaf utilities)
```

`render`, `hud`, `lobby`, `server_browser` are *consumers* of `world` and `net`; they never write to authoritative state.

### Forbidden patterns

- **No circular includes.** A module never includes a module that includes it.
- **No `extern` of another module's data.** If you need it, the owning module exposes a getter.
- **No `void *` in public APIs** for typed data.
- **No "manager" classes.** `mech_create(g, chassis)` returns a `MechHandle` (an index). We don't have a `MechManager`.
- **No header-only public modules** (apart from leaf utilities like `math.h`).

## Naming conventions

- **Module functions**: `<module>_<verb>[_<noun>]`. `mech_create`, `mech_destroy`, `mech_apply_damage`, `physics_step`, `net_send_unreliable`.
- **Types**: `PascalCase`. `Mech`, `World`, `ParticlePool`, `LobbyState`.
- **Enum constants**: `SCREAM_CASE`. `MECH_FLAG_GROUNDED`, `WEAPON_PULSE_RIFLE`.
- **Variables and fields**: `snake_case`. `mech.health`, `bullet.vel_x`.
- **Constants from configs**: `kSnakeCase` is **not** used. We use `SCREAM_CASE` if compile-time, lower-case static if runtime-loaded.
- **File names**: `snake_case.c` / `.h`.

## Headers

A typical header:

```c
// src/mech.h
#pragma once
#include "world.h"          // for World
#include "math.h"           // for Vec2

typedef int MechId;          // index into world.mechs

typedef struct Mech {
    MechId      id;
    int         chassis_id;
    Vec2        pos;            // pelvis position (cached)
    float       health, armor;
    uint8_t     team;
    bool        alive;
    // ... see [03-physics-and-mechs.md]
} Mech;

MechId mech_create(struct Game *g, int chassis_id, Vec2 spawn);
void   mech_destroy(struct Game *g, MechId id);
void   mech_apply_damage(struct Game *g, MechId id, int part, float dmg, Vec2 dir);
bool   mech_is_alive(const struct Game *g, MechId id);
```

The header is the API the rest of the codebase sees. Implementation details — internal helpers, intermediate state — never appear in the header.

## C standard

**C11**. We use:
- Designated initializers (`(Mech){.id = id, .pos = pos}`).
- Compound literals.
- `static_assert`.
- `_Alignof`, `alignas` where it matters.
- `<stdatomic.h>` if we ever need atomics (not at v1).

We avoid:
- C++ everything — we are C.
- VLA in production code (stack overflow risk; arena instead).
- `goto` for control flow (allowed for cleanup-on-error in init paths only).
- Bitfields in struct (compiler-defined ordering).

## Logging

```c
typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;
void log_msg(LogLevel l, const char *fmt, ...);
```

One logger. Goes to stderr by default and to `soldut.log` next to the executable. No structured logging, no log targets, no log filters at v1. If we need log filtering, we grep.

## Asserts

```c
#define ASSERT(x) ((x) ? (void)0 : assert_fail(#x, __FILE__, __LINE__))
```

Asserts are **on in dev builds, off in release**. We use them liberally for invariants we believe must hold: pool indices in range, particle counts non-negative, weapon IDs valid. They are not a substitute for error handling at I/O boundaries (file load, network read) — those return error codes.

## Error handling

Errors are **return values**. No exceptions, no `setjmp`/`longjmp` in gameplay.

```c
typedef enum {
    OK = 0,
    ERR_FILE_NOT_FOUND,
    ERR_VERSION_MISMATCH,
    ERR_NET_TIMEOUT,
    // ...
} Result;

Result level_load(World *w, const char *path);
```

Caller checks. Logging happens at the point of failure or at a call-site that adds context.

## Testing

`tests/` contains tiny unit tests for the parts that benefit from them:
- `tests/physics_test.c` — Verlet step, constraint relaxation
- `tests/snapshot_test.c` — encode/decode round-trip
- `tests/level_test.c` — `.lvl` parse for known-good fixtures
- `tests/arena_test.c` — allocator invariants

Each test is a single executable with a tiny test harness (50-line header, no external dependencies). Run via `make test`.

We do **not** unit test:
- Render code (visual diffing is over-engineered for v1)
- Audio code (manual ear test)
- Anything tightly coupled to raylib runtime state

We **do** integration test:
- A "headless server" that loads a level, spawns 8 bots, runs 60 seconds, asserts no crash.
- A round-trip test: client connects, sends 60 inputs, server processes, snapshots arrive, client reconciles within tolerance.

These run in CI on Linux.

## Profiling

In dev builds we expose a per-frame timing overlay (toggle with F3):

```
SIMULATE      2.4 ms   ████████████░░░░░░░
  PHYSICS     1.6 ms   ████████░░░░░░░░░░░
  AI          0.2 ms   █░░░░░░░░░░░░░░░░░░
NET           0.7 ms   ███░░░░░░░░░░░░░░░░
RENDER        4.2 ms   █████████████████░░
  WORLD       3.1 ms
  HUD         0.3 ms
  POST        0.8 ms
TOTAL        13.8 ms  /16.6 budget
```

Implemented as a tiny `prof.h` with `PROF_BLOCK("PHYSICS") { ... }` macros that push/pop a stack of named timing scopes. Aggregates over the last 60 frames. Cost: <0.05 ms.

We do **not** integrate Tracy or Optick at v1. If we need deep traces, we add them per-investigation, then remove.

## File size targets

| Module | Approx LOC at v1 | Notes |
|---|---|---|
| platform.c | 200 | thin raylib wrapper |
| game.c | 300 | top-level loop |
| world.c | 200 | data definitions |
| simulate.c | 600 | the pure step |
| physics.c | 1100 | Verlet + constraints + collision |
| mech.c | 800 | chassis + animation drive |
| weapons.c | 700 | weapon table + fire logic |
| projectile.c | 400 | bullets/grenades |
| particle.c | 300 | pool + draw |
| decal.c | 200 | splat layer |
| level.c | 500 | loader + grid |
| render.c | 800 | orchestration |
| hud.c | 600 | screen-space UI |
| lobby.c | 700 | waiting room |
| server_browser.c | 400 | LAN discovery |
| net.c | 800 | ENet wrap + packet codec |
| snapshot.c | 700 | encode/decode |
| reconcile.c | 300 | predict + replay |
| input.c | 200 | bitmask |
| audio.c | 400 | mix + aliases |
| arena.c | 80 | |
| pool.c | 150 | |
| log.c | 100 | |
| hash.c | 200 | |
| ds.c | 5 | one #define |
| hotreload.c | 200 | |
| **Total** | **~10,800 LOC** | comfortable for one or two engineers |

If we substantially exceed these, the module is doing too much — we look for an extraction.

## What we are NOT building

- **No ECS.** Mechs are an array of structs; particles are SoA pools. We do not need archetypes, sparse sets, or query DSLs at our scale.
- **No reflection / RTTI.** No struct introspection.
- **No scripting.** No Lua, no Wren, no JS. C is the language.
- **No DLLs / plug-ins.** One static binary per platform.
- **No service-locator pattern.** `Game *g` is the only "locator," passed explicitly.
- **No auto-generated bindings.** No SWIG, no protobuf, no flatbuffers at v1. Hand-coded packet codecs.

## References

- *Handmade Hero* — code organization for a self-built game.
- The stb headers themselves — examples of single-file disciplined C.
- Box2D source — for reference on how a serious C++ codebase organizes physics modules.
- raylib source at `third_party/raylib/src/` — the canonical "small framework in C" layout.
