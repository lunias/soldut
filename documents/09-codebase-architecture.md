# 09 — Codebase Architecture

This document specifies the **layout** of the source: folders, modules, what each `.c` and `.h` is allowed to know, and how memory flows. It is the structural counterpart to [01-philosophy.md](01-philosophy.md): philosophy says *how to think*; this says *how to file*.

## Top-level layout

```
soldut/
├── src/                    # game source — 70 .c/.h files (38 modules) post-M5 P10
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
├── main.c                  # entry point, top-level loop, 60 Hz accumulator
├── platform.{c,h}          # window, input, time — wraps raylib for our use
├── game.{c,h}              # top-level Game struct, init/shutdown
├── world.h                 # World struct + all simulation-state typedefs (header-only)
├── simulate.{c,h}          # the pure simulate() function
├── physics.{c,h}           # Verlet, constraints, collision
├── mech.{c,h}              # Mech struct, chassis, animation
├── mech_sprites.{c,h}      # per-chassis sprite atlas + render parts (M5 P10)
├── weapons.{c,h}           # weapon table, fire logic
├── projectile.{c,h}        # bullets, grenades, rockets
├── particle.{c,h}          # blood, sparks, smoke
├── decal.{c,h}             # splat layer
├── pickup.{c,h}            # pickup spawners + state machine + per-kind apply (M5 P05)
├── ctf.{c,h}               # CTF flag entities + capture / pickup / return rules (M5 P07)
├── level.{c,h}             # tile grid + ray helpers
├── level_io.{c,h}          # `.lvl` binary format loader/saver + CRC32 (M5 P01)
├── maps.{c,h}              # code-built map fallbacks + `map_build` dispatcher
├── match.{c,h}             # match phases, scoring, mode rules
├── render.{c,h}            # high-level draw orchestration
├── hud.{c,h}               # screen-space UI (HP/jet/ammo/kill feed)
├── ui.{c,h}                # small immediate-mode raylib UI helpers (4K-aware scaling)
├── lobby.{c,h}             # lobby state, slot table, chat, ready-up
├── lobby_ui.{c,h}          # title / server browser / lobby / summary screens
├── net.{c,h}               # ENet wrapper, packet schemas, LAN discovery
├── map_cache.{c,h}         # content-addressed `.lvl` cache + LRU eviction (M5 P08)
├── map_download.{c,h}      # client-side chunked map download + reassembly (M5 P08)
├── snapshot.{c,h}          # snapshot encode/decode + delta + interp
├── reconcile.{c,h}         # client prediction + reconciliation
├── input.h                 # input bitmask (header-only)
├── config.{c,h}            # `soldut.cfg` key=value parser
├── arena.{c,h}             # arena allocator
├── pool.{c,h}              # fixed-size pool allocator
├── log.{c,h}               # logger (incl. SHOT_LOG())
├── math.h                  # math helpers beyond raymath (header-only, mostly inline)
├── hash.{c,h}              # PCG, FNV, simple hash table
├── ds.c                    # one .c that #defines STB_DS_IMPLEMENTATION
├── shotmode.{c,h}          # scriptable scene runner that drives the real
│                           #   sim + renderer and writes PNG + log pairs
│                           #   for visual debugging — see CLAUDE.md
└── version.h               # version constants
```

That is the entire public structure. Anything we want to add asks: which existing module owns this? If none, it gets a new `name.{c,h}` pair, never a folder.

Modules that the design canon expects but that haven't shipped yet:
- `audio.{c,h}` — lands at M5 P14 (per `documents/m5/09-audio.md`).
- `hotreload.{c,h}` — never built; data hot-reload is deferred indefinitely.

Shipped at M5: `level_io.{c,h}` (P01), `pickup.{c,h}` (P05), `ctf.{c,h}` (P07), `map_cache.{c,h}` + `map_download.{c,h}` (P08), `mech_sprites.{c,h}` (P10).

`server_browser.{c,h}` was originally planned as its own module; LAN discovery folded into `net.{c,h}` and the browser screen lives in `lobby_ui.{c,h}`.

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
                    /  \
              simulate  net ── map_cache, map_download
                |        ↓
              physics  snapshot
              mech     reconcile
              mech_sprites          (P10; consumed by render)
              weapons
              projectile
              pickup
              ctf
              particle
              decal
              level
              level_io
              maps
              match
              render
                ↑
              hud
              ui
              lobby
              lobby_ui
                ↑
              input

              arena, pool, log, math, hash, ds, config   (leaf utilities)
              shotmode                                    (test harness; see CLAUDE.md)
```

`render`, `hud`, `lobby`, `lobby_ui` are *consumers* of `world` and `net`; they never write to authoritative state.

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

| Module | Current LOC (post-M4 + M5 P01–P10) | Notes |
|---|---|---|
| main.c | 1594 | top-level loop + accumulator + CLI + P03 event broadcast loops + P04 `--test-play` + P05 `broadcast_new_pickups` + P07 CTF mode-mask validation + `broadcast_flag_state_if_dirty` + `ctf_step` hookup + TDM/CTF team auto-balance + P08 host map-ready gate / serve-info refresh + P09 `host_start_map_vote` + summary-screen vote routing + P10 `mech_sprites_load_all` after `platform_init` |
| platform.c | 100 | thin raylib wrapper (P09: RMB → `BTN_FIRE_SECONDARY`) |
| game.c | 143 | Game lifecycle (P08b `map_registry_init` call) |
| simulate.c | 251 | the pure step (P03 render_prev snapshot, P05 pickup_step hook) |
| physics.c | 753 | Verlet + constraints + tile/poly collision (M5 P02 grew this; P06 added `solve_fixed_anchor`) |
| mech.c | 1904 | chassis + animation drive — past split threshold (P05 powerups + Engineer deployable + Burst SMG cadence; P06 grapple lifecycle + ATTACHED retract block; P07 carrier penalties + `ctf_drop_on_death` hook in `mech_kill`; P09 `fire_other_slot_one_shot` RMB one-shot dispatch; P10 per-chassis bone-length distinctness in `g_chassis[]` + per-chassis posture quirks in `build_pose`) |
| weapons.c | 676 | weapon table + fire logic + P03 fire-event recording + P06 WFIRE_GRAPPLE branch |
| projectile.c | 590 | bullets/grenades + P06 `PROJ_GRAPPLE_HEAD` lifecycle |
| particle.c | 190 | pool + draw |
| decal.c | 80 | splat layer |
| level.c | 260 | tile-grid helpers + ray queries + poly broadphase (M5 P02) |
| level_io.c | 833 | `.lvl` loader/saver + CRC32 + poly broadphase build (M5 P01–P02) + P08 `level_compute_buffer_crc` for download verify |
| maps.c | 837 | code-built map fallbacks + `map_build` + `map_build_from_path` + P05 default pickups + P07 `MAP_CROSSFIRE` (CTF arena) + per-map `meta.mode_mask` + P08 `map_build_for_descriptor` + `maps_refresh_serve_info` + P08b `MapRegistry` + `map_registry_init`/`_from` + cheap META scan + custom-map fallback in `map_build` |
| match.c | 301 | match phases, scoring, mode rules |
| render.c | 692 | orchestration (P02 polygon stopgap, P03 alpha-lerp threading, P05 invis alpha-mod + pickup placeholder, P06 `draw_grapple_rope`, P07 `draw_flags`, P10 sprite-or-capsule dispatch + 17-entry `g_render_parts` z-order table + L↔R swap helpers + `draw_held_weapon_line` extracted) |
| hud.c | 283 | HP / jet / ammo / kill feed + P05 active-powerup pill + P07 CTF flag pips + off-screen compass arrows |
| ui.c | 306 | immediate-mode UI helpers |
| lobby.c | 598 | lobby state, slots, chat, ready-up + P09 `lobby_load_bans` / `lobby_save_bans` (`bans.txt` round-trip) |
| lobby_ui.c | 1826 | title / browser / lobby / summary screens — host-setup screen + lobby MATCH panel + per-player TEAM picker + P08 download progress strip + P08b `g_map_registry.count` walks + P09 host `[Kick] [Ban]` modal + 3-card vote picker + Controls modal — past split threshold |
| net.c | 2532 | ENet wrap + packet codec + P03 hit/fire events + P05 pickup-state + P07 flag-state encode/decode + INITIAL_STATE flag suffix + P08 four new lobby-channel messages (MAP_REQUEST/MAP_CHUNK/MAP_READY/MAP_DESCRIPTOR) + chunk reassembly hooks + P09 `net_server_kick_or_ban_slot` host-side direct call — well past split threshold |
| snapshot.c | 660 | encode/decode + per-mech remote interp ring (P03) + powerup bits (P05) + P06 `SNAP_STATE_GRAPPLING` trailing suffix + post-P07 quant 8× → 4× |
| reconcile.c | 114 | predict + replay |
| pickup.c | 343 | new at P05 — spawner pool + state machine + per-kind apply |
| ctf.c | 285 | new at P07 — flag entities + capture rule + auto-return + carrier helpers |
| map_cache.c | 324 | new at P08 — content-addressed `<XDG_DATA_HOME>/soldut/maps/<crc>.lvl` cache + 64 MB LRU + atomic write |
| map_download.c | 125 | new at P08 — per-process MapDownload (2 MB buffer + chunk bitmap + 30 s stall watchdog) |
| mech_sprites.c | 111 | new at P10 — `g_chassis_sprites[CHASSIS_COUNT]` + 22-entry placeholder sub-rect/pivot table + `assets/sprites/<chassis>.png` loader |
| config.c | 178 | `soldut.cfg` parser |
| arena.c | 51 | |
| pool.c | 65 | |
| log.c | 91 | |
| hash.c | 49 | |
| ds.c | 20 | one #define |
| shotmode.c | 2245 | scriptable test runner + P05 `give_invis` debug directive + P07 `mode`/`map`/`flag_carry` directives + config_load + ctf_init_round/ctf_step hookup + post-P07 `arm_carry`/`kill_peer`/`team_change` directives + P09 `fire_secondary` button + P10 `extra_chassis` directive + dummy spawn loop + `mech_sprites_unload_all` before each `platform_shutdown` — well past split threshold |
| **Total .c** | **~19,410 LOC** | + ~4,070 LOC of headers |

`net.c`, `mech.c`, `lobby_ui.c`, and `shotmode.c` are all well past the
~1500 line split guideline. P08 added ~415 LOC to `net.c` (lobby-channel
map-share handlers + chunk reassembly); P09 added ~106 more
(`net_server_kick_or_ban_slot` + lobby vote-state plumbing) and pushed
`lobby_ui.c` over the threshold (host-controls modal + 3-card vote
picker + Controls modal). P10 grew `render.c` (+183 LOC for the sprite
path + render-parts table) and `shotmode.c` (+53 LOC for the
`extra_chassis` directive). The extraction (e.g., `net_lobby.c` /
`net_map_share.c`; `lobby_ui_match.c` / `lobby_ui_summary.c`) is worth
scheduling before the audio module (M5 P14) adds its own surface to
`net.c`. `audio.{c,h}` is not yet built — it lands at M5 P14 and will
add to this table.

If a module substantially exceeds the rule of thumb, it's doing too much — we
look for an extraction.

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
