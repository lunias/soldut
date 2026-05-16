# CLAUDE.md

This file is the entry point for working in this repo. It tells you
what the project is, how to build/run/test it, where the load-bearing
docs live, and which conventions will bite if you ignore them. It is
**not** a changelog — git log + `documents/m5/` + `documents/m6/`
already cover what shipped when.

## What this project is

Soldut is a 2D side-scrolling multiplayer mech shooter in C, in the
lineage of Soldat. Authoritative server + client prediction over UDP
(ENet), Verlet-skeleton mech physics, lobby → match → summary →
next round, FFA / TDM / CTF, eight ship maps, level editor that
shares its engine with the game. Ships as a **single static binary
per platform** (plus a separate editor binary). No DLLs, no scripting
runtime.

Active milestone: **M6** (stability + ship prep + game-feel). The
shape of the current phase lives in `CURRENT_STATE.md`; the phase
plan + decisions live in `documents/m6/`.

## Where to look next

These are not optional reading for non-trivial work.

| File | What it tells you |
|---|---|
| `documents/01-philosophy.md` | How to write code here. Wins on style disagreements. |
| `documents/09-codebase-architecture.md` | Module layout, memory model, cross-module rules. |
| `documents/` (00–13) | Design canon. Source of truth for *intent*. |
| `documents/m5/`, `documents/m6/` | Per-phase plan + implementation notes. |
| `CURRENT_STATE.md` | What the build actually does *right now*, with tunables. Update when behavior changes. |
| `TRADE_OFFS.md` | Deliberate compromises, with revisit triggers. Add an entry *before* shipping a "for now" hack; *delete* the entry when the proper fix lands (it's a queue, not a changelog). |
| `git log` | Per-commit history. Use this — not CLAUDE.md — for "what shipped." |

If the docs disagree with the code about *what* it does, the code
wins; fix the doc. If they disagree about *how* code should be
written, the philosophy doc wins.

## Build & run

Native build (Linux/macOS host):

```bash
make                 # release build → ./soldut
./build.sh dev       # debug build with -O0 + ASan/UBSan
make clean           # remove build artifacts (keeps third_party libs)
make distclean       # also rebuild raylib + enet from source
make help            # list every Make target
```

Cross-compile (requires `zig` on PATH):

```bash
make windows         # → build/windows/Soldut.exe (runs cross-windows.sh)
make macos           # → build/macos/Soldut.app  (needs macOS SDK)
```

`cross-windows.sh` wipes and rebuilds `third_party/raylib` + `enet`
for the Windows target, then restores the host-native raylib build.
If interrupted, run `make distclean && make raylib && make enet` to
recover host libs.

The editor builds via `make editor` → `build/soldut_editor`. Same
engine, different `main()`.

## Tests

All asserting tests run via `make`:

```bash
make test-level-io           # .lvl round-trip + CRC32
make test-pickups            # pickup state machine
make test-ctf                # CTF rules
make test-snapshot           # snapshot encode/decode round-trip
make test-spawn              # spawn placement
make test-prefs              # soldut-prefs.cfg round-trip
make test-map-chunks         # chunk reassembly + bit-flip CRC fail
make test-map-registry       # MapRegistry scan
make test-map-share          # end-to-end host → client .lvl stream
make test-mech-ik            # procedural pose math
make test-pose-compute       # pose_compute determinism
make test-damage-numbers     # paired host/client damage-number sync
make test-atmosphere-parity  # paired host/client atmosphere sync
make test-frag-grenade       # paired-dedi grenade sync
make test-editor             # editor binary smoke
make test-grapple-ceiling    # grapple vs angled-ceiling polys
# …see `make help` for the full list
```

`make test-physics` is the **outlier**: it dumps particle positions
for human inspection and does *not* assert. Every other `make test-*`
target returns non-zero on failure.

Paired-process flows (`tests/net/run.sh`, `tests/shots/net/run.sh
<name>`) spawn a real host + client over loopback and assert on log
lines or paired screenshots. New multiplayer features need a
networked test — single-process verification is insufficient for
wire-format work.

CI builds on Linux/Windows-cross/macOS but does not run the test
suite (see `TRADE_OFFS.md` → "No CI for physics correctness").

## Visual debugging — shot mode

For motion/physics/render questions where text dumps aren't enough,
the binary takes a `--shot <script>` flag that drives a scripted
scene through the real sim + renderer and writes PNGs + a `.log`
file to `build/shots/`.

```bash
./soldut --shot tests/shots/walk_right.shot
make shot                                    # default script
make shot SCRIPT=tests/shots/your_case.shot
```

Scripts are line-based (`#` comments). Header directives: `window <w>
<h>`, `seed <hi> <lo>`, `out <dir>`, `map <name>`, `spawn_at <wx>
<wy>`, plus one of `aim <wx> <wy>` (fixed world target) or `mouse
<sx> <sy>` (screen cursor; same conversion path as real play, so
camera-follow moves the world target with the mech).

Tick-stamped events: `at <tick> press|release|tap <button>`,
`at <tick> aim|mouse ...`, `at <tick> shot <name>` (PNG after that
tick's render), `at <tick> end`. Range: `burst <prefix> from <t0>
to <t1> every <k>`, `mouse_lerp <sx> <sy> from <t0> to <t1>`.
`contact_sheet <name> [cols C] [cell W H]` composites every shot
from the run into a grid PNG — many frames at the token cost of
one image when sharing with an LLM.

Buttons: `left right jump jet crouch prone fire reload melee use
swap dash`.

The runner uses fixed `dt = 1/60`, doesn't read wall-clock time, and
re-seeds the World RNG when a `seed` directive is present — re-runs
produce byte-identical PNGs. See `src/shotmode.c`.

The `.log` file gets a per-tick line for the local mech plus event
lines for anim transitions, grounded toggles, jet ceiling taper,
post-physics anchor recoveries, fire/hit/miss, damage, dismember,
kills. Add events with the `SHOT_LOG()` macro from `src/log.h` —
gated on `g_shot_mode` (one-branch no-op outside shot mode), free
to sprinkle into hot paths.

**Do not** use `LOG_I` for per-tick diagnostics — production play
must never emit per-tick debug lines. Route everything through
`SHOT_LOG`.

## Architecture in a paragraph

`main.c` runs a fixed-step 60 Hz accumulator; each tick calls
`simulate(World*, ClientInput, dt)` (in `src/simulate.c`), the pure
simulation step — no globals, no wall-clock reads, just the World's
seeded PCG RNG. Per tick: pose drive (`mech_step_drive`) → try-fire
(`mech_try_fire` dispatches by `WFIRE_*` kind) → latch prev_buttons
→ gravity → Verlet integrate → 12-iter constraint+collision
relaxation interleaved in one loop (`physics_constrain_and_collide`)
→ `mech_post_physics_anchor` (the standing-pose hack — see
TRADE_OFFS.md) → `projectile_step` (integrate, sweep-collide vs
tiles + bones, detonate AOE) → `pose_compute` (procedural IK, see
M6 P01) → FX update → kill-feed aging → bookkeeping. Render reads
the latest `World` and draws; render never writes authoritative
state. Module dependency graph is a DAG with `world` at the top
and `arena`/`pool`/`log`/`math`/`hash`/`ds` as leaves;
`render`/`hud`/`lobby` are consumers only. `weapons.c` and
`projectile.c` are peers — weapons spawn projectiles; projectiles
call back into `mech_apply_damage` and `explosion_spawn`.

## Conventions that bite if you ignore them

- **C11 only.** `-std=c11 -Wall -Wextra -Wpedantic -Werror`. No
  C++, no scripting, no codegen, no threads at v1 (the in-process
  server thread in `proc_spawn.c` is the one accepted exception —
  see TRADE_OFFS.md), no `void *` in public APIs for typed data.
- **One `.h` + one `.c` per module.** Public surface in the header;
  everything else is implementation. No `_internal.h`, `_types.h`,
  `_priv.h` splits unless a `.c` exceeds ~1500 LOC.
- **Allocate up front.** `malloc` is called at `arena_init` / pool
  init at startup and at level load — nowhere else. Three arenas:
  `permanent` (process), `level_arena` (round), `frame_arena` (one
  tick, reset every tick). Pools have stable indices used as
  identifiers.
- **SoA where it matters.** `ParticlePool` is parallel arrays of
  `pos_x`, `pos_y`, `prev_x`, `prev_y`, `inv_mass`, `flags`. Don't
  refactor it into AoS to "look cleaner."
- **No malloc in inner loops.** Grow the right arena instead.
- **Naming tells cost.** `physics_solve_world` is expensive;
  `mech_predict_local` is cheap; `level_load_from_disk` is slow + IO.
  Refactor the name first if it lies.
- **Pose drive is kinematic translate, not position-only.** When
  you move a particle to fit a pose, update **both** `pos` and
  `prev` by the same delta (use `physics_translate_kinematic`) —
  otherwise Verlet reads the displacement as injected velocity.
  This burned a full debug round in M1; CURRENT_STATE.md → "Recently
  fixed" has the post-mortem.
- **`Game *g` is the only spine.** Everything threads through it;
  no module-level mutable globals (raylib's globals are the one
  accepted exception).
- **Map-build helpers must `memset(Level, 0, ...)` at the top.**
  `level_load` and `map_alloc_tiles` already do — anything that
  builds into a freshly arena-reset `Level` and forgets will alias
  stale pointers from the previously-loaded map and ship ghost
  geometry. (Cost us a round of "two clients render different
  Crossfire" debugging at M5 P17.)

## Tunables live in code

The numbers driving feel are `#define`s in `src/physics.h`,
`src/mech.c`, `src/weapons.c`, and (atmospherics) `src/atmosphere.c`.
`CURRENT_STATE.md` mirrors them for quick reference but the C files
are authoritative. The simulation runs at **60 Hz**, not 120 Hz as
`documents/03-physics-and-mechs.md` specifies — see TRADE_OFFS.md.

## Vendored dependencies

Do not add a fifth without writing it down.

- `third_party/raylib/` — built into `libraylib.a` per platform via
  its own Makefile.
- `third_party/enet/` — `*.c` compiled into `libenet.a` by our
  top-level Makefile.
- `third_party/stb_ds.h`, `third_party/stb_sprintf.h` — header-only;
  `src/ds.c` is the one file that `#define STB_DS_IMPLEMENTATION`s.
- `third_party/raygui/raygui.h` — header-only immediate-mode UI on
  top of raylib. Used **only** by the level editor (`tools/editor/`);
  the game proper does not link it. License: zlib/libpng (same as
  raylib).
