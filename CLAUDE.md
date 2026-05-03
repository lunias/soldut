# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

Soldut is a 2D side-scrolling multiplayer mech shooter in C, in the lineage of Soldat. The current build is **M1 — One mech, no network**: Verlet skeleton physics, one chassis (Trooper), pulse-rifle hitscan, ragdoll on death, blood particles + decal layer, HUD. Networking arrives at M2.

## Build & run

Native build (Linux/macOS host):

```bash
make                 # release build → ./soldut
./build.sh dev       # debug build with -O0 + ASan/UBSan
./build.sh release   # explicit release path through build.sh
make clean           # remove build artifacts (keeps third_party libs)
make distclean       # also rebuild raylib + enet from source
```

Cross-compile (requires `zig` on PATH):

```bash
make windows         # → build/windows/Soldut.exe (also runs cross-windows.sh)
make macos           # → build/macos/Soldut.app  (needs macOS SDK)
```

Note: `cross-windows.sh` wipes and re-builds `third_party/raylib` and `third_party/enet` for the Windows target, then *restores* the host-native raylib build at the end. If you interrupt it, run `make distclean && make raylib && make enet` to recover the host libs.

## Tests

There is one regression harness:

```bash
make test-physics            # builds + runs the headless physics tester
./build/headless_sim         # re-run without rebuilding
```

`tests/headless_sim.c` builds a real `World`, runs `simulate()` over five scripted phases (Spawn / Idle 2 s / Hold RIGHT 1 s / Release / Hold JET 0.5 s) and dumps particle positions. **It does not assert or return a non-zero exit code on regression** — humans read the output. CI builds on Linux/Windows-cross/macOS but does not run this test (see TRADE_OFFS.md → "No CI for physics correctness").

## Visual debugging — shot mode

For motion/physics questions where text dumps aren't enough, the binary
takes a `--shot path/to/script.txt` flag that drives a scripted scene
through the real renderer and writes PNGs to `build/shots/`.

```bash
./soldut --shot tests/shots/walk_right.shot
make shot                                    # default script above
make shot SCRIPT=tests/shots/your_case.shot  # any other .shot
```

A script is line-based; comments start with `#`. Header directives:
`window <w> <h>`, `seed <hi> <lo>`, `out <dir>`, `spawn_at <wx> <wy>`
(teleport the player after world build, before tick 0 — useful for
focusing tests on a specific location), plus one of
`aim <wx> <wy>` (world-space, fixed target) or `mouse <sx> <sy>`
(screen-space cursor; converted via the live camera each tick — same
path as real play, so camera-follow changes the world target as the
mech moves). Tick-stamped events: `at <tick> press|release|tap
<button>`, `at <tick> aim <wx> <wy>`, `at <tick> mouse <sx> <sy>`,
`at <tick> shot <name>` (writes `<out>/<name>.png` after that
tick's render), `at <tick> end`. Range directives: `burst <prefix>
from <t0> to <t1> every <k>` (one shot per k ticks) and
`mouse_lerp <sx> <sy> from <t0> to <t1>` (linear cursor sweep). The
`contact_sheet <name> [cols <C>] [cell <W> <H>]` directive composites
every shot from the run into a single grid PNG at `<out>/<name>.png`
— useful for sending many frames to an LLM at the token cost of one
image (default 320×180 cells × 4 cols).
Buttons: `left right jump jet crouch prone fire reload melee use
swap dash`. Use `mouse` for accurate gameplay reproduction; `aim`
when you specifically need a world target that doesn't move with the
camera.

The runner uses a fixed `dt = 1/60`, doesn't read wall-clock time, and
re-seeds the World RNG when a `seed` directive is present — so a re-run
produces the same PNGs. See `src/shotmode.c`.

Each run also writes a `<scriptname>.log` next to its PNGs, with a
per-tick summary line for the local mech plus event lines for anim
transitions, grounded toggles, jet ceiling taper, post-physics anchor
recoveries, pose-sweep clamps, inside-tile escapes, hitscan
fire/hit/miss, damage, dismemberment, and kills. Use the `SHOT_LOG()`
macro from `src/log.h` to add more events; it's gated on
`g_shot_mode` (a one-branch no-op outside shot mode), so it's free to
sprinkle into hot paths.

## Required reading for non-trivial work

Three files near the root are not optional:

- **`documents/`** — the design canon. 12 numbered docs, read in order. Source of truth for *intent*. `documents/01-philosophy.md` and `documents/09-codebase-architecture.md` are the load-bearing ones for code style.
- **`CURRENT_STATE.md`** — what the build actually does *right now*, including tunable values and the most recent debugging round. Update this when behavior changes.
- **`TRADE_OFFS.md`** — deliberate compromises between intent and reality, with revisit triggers. Add an entry *before* shipping a "for now" hack; *delete* the entry when the proper fix lands (this is a queue, not a changelog).

If the docs disagree with the code about *what* it does, the code wins and the doc is wrong — fix the doc. If they disagree about *how* code should be written, the philosophy doc wins.

## Architecture in a paragraph

`main.c` runs a fixed-step 60 Hz accumulator; each tick calls `simulate(World*, ClientInput, dt)` (in `src/simulate.c`), which is the pure simulation step — no globals, no wall-clock reads, just the World's seeded PCG RNG. The simulate step does pose drive (`mech_step_drive`) → fire → gravity → Verlet integrate → 12-iter constraint+collision relaxation (interleaved in one loop, see `physics_constrain_and_collide`) → `mech_post_physics_anchor` (the standing-pose hack — see TRADE_OFFS.md) → FX update → bookkeeping. Render reads the latest `World` and draws; render never writes authoritative state. Module dependency graph is a DAG with `world` at the top and `arena`/`pool`/`log`/`math`/`hash`/`ds` as leaves; `render`/`hud`/`lobby` are consumers only.

## Conventions that bite if you ignore them

- **C11 only.** `-std=c11 -Wall -Wextra -Wpedantic -Werror`. No C++, no scripting, no codegen, no threads at v1, no `void *` in public APIs for typed data.
- **One `.h` + one `.c` per module.** Public surface in the header; everything else is implementation. No `_internal.h`, `_types.h`, `_priv.h` splits unless a `.c` exceeds ~1500 LOC.
- **Allocate up front.** `malloc` is called only at `arena_init` / pool init at startup and at level load. Three arenas: `permanent` (process), `level_arena` (round), `frame_arena` (one tick, reset every tick). Pools have stable indices used as identifiers.
- **SoA where it matters.** `ParticlePool` is parallel arrays of `pos_x`, `pos_y`, `prev_x`, `prev_y`, `inv_mass`, `flags`. Don't refactor it into AoS to "look cleaner."
- **No malloc in inner loops.** Grow the right arena instead.
- **Naming tells cost.** `physics_solve_world` is expensive; `mech_predict_local` is cheap; `level_load_from_disk` is slow + IO. Refactor the name first if it lies.
- **Pose drive is kinematic translate, not position-only.** When you move a particle to fit a pose, update **both** `pos` and `prev` by the same delta (use `physics_translate_kinematic`) — otherwise Verlet reads the displacement as injected velocity. This bug class burned a full debug round in M1; see CURRENT_STATE.md → "Recently fixed."
- **`Game *g` is the only spine.** Everything threads through it; no module-level mutable globals (raylib's globals are the one accepted exception).

## Tunables live in code

The numbers driving feel are `#define`s in `src/physics.h`, `src/mech.c`, and `src/weapons.c`. `CURRENT_STATE.md` mirrors them in a table for quick reference but the C files are authoritative. The simulation runs at **60 Hz**, not 120 Hz as `documents/03-physics-and-mechs.md` specifies — this is a tracked trade-off.

## Vendored dependencies (do not add a fourth without writing it down)

- `third_party/raylib/` — built into `libraylib.a` per platform via its own Makefile
- `third_party/enet/` — `*.c` compiled into `libenet.a` by our top-level Makefile
- `third_party/stb_ds.h` and `third_party/stb_sprintf.h` — header-only; `src/ds.c` is the one file that `#define STB_DS_IMPLEMENTATION`s

The game ships as a single static binary per platform. No DLLs, no plugins, no scripting runtime.
