# 01 — Philosophy

This document is the part of the codebase you cannot grep for. It tells you **how** to write code in this project, not what to write. When the docs and the code disagree about *what* to do, the code wins. When the docs and the code disagree about *how* to do it, this document wins.

## The three engineers we channel

### Sean Barrett — the stb mindset

Sean Barrett's contribution to game programming is not just the stb single-header libraries; it's **the cultural argument that less code is better code**. The stb headers exist because adding a dependency on the early-2000s game-engine treadmill was so painful that most programmers reinvented image loading, font rasterization, and rect packing instead. Sean's response was not to advocate a better build system — it was to ship a single `.h` file that you drop in.

What we take from him:

- **One file is the unit of trust.** A subsystem that lives in one `.c` and one `.h` is one you can audit. Anything bigger needs justification.
- **Dependencies are taxes.** Every `#include` of someone else's code is a bet that they will not break you. We pay this tax for **raylib**, **enet**, and a small set of **stb headers**. We do not add a fourth without writing it down here.
- **Public domain or permissive.** Our own code will ship under MIT or 0BSD. We do not pull GPL code into the binary.
- **Configuration knobs are a sign of weakness.** If you find yourself adding a `#define ENABLE_X`, ask whether either the X path or the not-X path can be deleted. Usually one can.
- **Boring code wins.** No clever templates, no metaclasses, no DSL. C is enough. If C looks ugly, it is because the problem is ugly; making it pretty hides the cost.

### Casey Muratori — compression-oriented programming

Casey's *Handmade Hero* taught a generation of programmers to **type the code first and abstract second**. His "compression-oriented programming" thesis is short: write the specific case. When you have written the specific case three times, *then* extract the abstraction — and only as far as the third case demands. You will be wrong about the abstraction the first two times. Pretending otherwise costs you weeks.

What we take from him:

- **Make it usable before you make it reusable.** No `Mech` class with virtual methods on day one. We have `mech_update(Mech *m, Input in)` and we duplicate code freely until a pattern proves itself.
- **Understand the machine.** When we read a particle pool, we know cache lines are 64 bytes and we lay particles out so a hot loop reads contiguous memory. We do not sprinkle `std::vector<std::shared_ptr<Particle>>`-equivalents and hope the optimizer saves us. (Also: there is no `std::` here; this is C.)
- **Allocate up front.** Each subsystem reserves its memory at startup. Per-frame allocations come from a frame arena, reset to zero between frames. No `malloc` in the inner loop.
- **The simulation is data, not behavior.** Particles, bullets, mechs, decals — they are arrays of structs (or structs of arrays where it matters). Functions operate on the arrays. We do not hide loops behind iterators.
- **Iteration speed is the metric.** A clean build of the whole game should be **under 10 seconds** on a developer laptop. An incremental build should be **under 1 second**. If those numbers slip, we stop and fix the build before we continue with features.

### Jonathan Blow — game-feel above all, and your tools are tools

Jonathan Blow built *Braid* and *The Witness* and is building *Sokoban* and a language called Jai. His public stance comes down to two things: **the player's experience is what justifies any of this**, and **your tools should serve you, not the other way around**. He has spent fifteen years building Jai because the C++ build pipeline was slowing him down and he refused to accept that.

What we take from him:

- **Game-feel is engineering.** When a hit feels weak, the answer is in the code, not in the art. Recoil curves, screen shake, hit-pause, blood spawn count, audio mix — these are all numbers in C files. We tune them like we tune the netcode.
- **Avoid frameworks that hide behavior.** raylib is a framework, but a small one whose source we can read in an afternoon. We do not bring in Unity, Unreal, or Godot. We do not write our own scripting language. We do not embed Lua "for designers" until we have a designer asking for it.
- **The compiler is your tool.** We compile with `-Wall -Wextra -Wpedantic -Werror`. We use sanitizers in dev builds. We do not silence warnings; we fix them. We do not use IDEs that hide the build commands.
- **Don't lie about what's happening.** If a function allocates, it should look like it allocates (`mech_create`, not `mech_get`). If a function takes 4 ms, that should be obvious in its name (`physics_solve_constraints_full`, not `tick`).

## How this becomes code

The translation from philosophy to code is concrete. Here are our **rules**, ordered roughly by how often they bite.

### Rule 1 — Data layout is API design.

When you design a subsystem, design the **arrays** first. The functions follow. If you find yourself writing the function before you can describe its arrays, stop and write the arrays.

```c
// Particle pool — SoA for a reason.
typedef struct {
    float    *pos_x, *pos_y;     // hot, read every frame
    float    *vel_x, *vel_y;     // hot
    float    *life;              // hot
    uint16_t *type;              // medium
    uint32_t *color;             // cold, only on draw
    int       count, capacity;
    int       head;              // ring buffer for spawns
} ParticlePool;
```

Not:

```c
typedef struct Particle { float x,y,vx,vy,life; ...; struct Particle *next; } Particle;
typedef struct ParticlePool { Particle *first; size_t count; } ParticlePool;
```

The second is what a programmer who hasn't thought about cache writes. We do not write it.

### Rule 2 — Pure functions where possible.

The simulation step is `simulate(World *w, Input *inputs, int n_inputs, float dt)`. It mutates `w` in place. It does not read globals. It does not look at wall-clock time. It does not call random — it calls `world_rand(w)` which uses a seeded PCG. This means the simulation is *replay-able* and easy to test, even though we are not pursuing strict determinism (see [05-networking.md](05-networking.md)).

### Rule 3 — Allocate once.

```c
typedef struct Game {
    Arena   frame_arena;     // reset each frame
    Arena   level_arena;     // reset each level
    Arena   permanent;       // never reset
    World   world;           // size known at startup
} Game;
```

`malloc` is called from `arena_init` at startup and at level load. **Nothing else allocates from the C heap at runtime.** When you find yourself wanting a malloc in the inner loop, the answer is to grow the right arena.

### Rule 4 — One header per subsystem; the implementation is the rest.

Every public-facing module has exactly one header in `src/`. The header is short — the API surface a caller needs. The implementation lives in one `.c` (sometimes split into a `_internal.c` if it grows past ~1500 lines, but only then).

Example:

```
src/net.h         // 80 lines: net_init, net_send_*, net_poll, types
src/net.c         // 1200 lines: implementation
```

We do not have `net.h`, `net_types.h`, `net_internal.h`, `net_priv.h`, `net_compat.h`. If we did, it would be a sign we'd over-abstracted.

### Rule 5 — No premature reusable code.

A function that is called from one place is `static`. A function that is called from two places stays `static` and gets duplicated when the call sites diverge. A function called from three places earns a non-static life. **The bar for cross-module function is high.**

### Rule 6 — Names tell you what costs what.

- `mech_predict_local(Mech *m, Input in)` — cheap, runs on every input.
- `physics_solve_world(World *w, float dt)` — expensive, runs once per tick.
- `level_load_from_disk(const char *path)` — slow, allocates, IO.

If the name doesn't tell you the cost class, the name is wrong. Refactor the name first.

### Rule 7 — We commit to numbers.

Every subsystem document includes its budget: bytes, microseconds, kilobits. (See [10-performance-budget.md](10-performance-budget.md).) When we exceed a budget, we either increase it on purpose (and write down why) or we fix the code. We do not silently exceed budgets.

### Rule 8 — Hot reload of data.

Code does not need to hot-reload (we can recompile in 1 second). **Data must hot-reload.** Maps, weapon stats, mech parameters, audio mix, color palettes — change them on disk, see the change in the running game in under a second. This is the single biggest accelerator of game-feel iteration. We build a tiny file watcher (`src/hotreload.c`) that polls mtimes; we do not pull in a library for this.

### Rule 9 — One way to do each thing.

There is one logger. There is one math vector type. There is one allocator interface. There is one input bitmask. There is one error-return convention. We do not have `Vec2` and `vec2_t` and `Vector2` competing in the same codebase. (raylib provides `Vector2`; we use raylib's. Done.)

### Rule 10 — The code is the spec.

These documents describe intent. The code is the truth. When in doubt, read the code. When the doc disagrees with the code in a way that matters, file an issue or fix the doc. We do not maintain three sources of truth.

## Anti-patterns we forbid

These are not opinions. These are **rules**, in priority order.

1. **No global mutable state outside the `Game *g` you pass around.** No `current_player`, no `g_renderer`. (raylib's globals are an exception we accept; we wrap calls behind our own thin shims when it matters.)
2. **No `void *` payloads to "leave the type for later."** If you don't know the type, you don't have the design.
3. **No exception-style error handling via `setjmp/longjmp`** in gameplay code. Errors are return values.
4. **No threads in v1.** When we add threads, we add them deliberately — for the network thread and the audio thread, with explicit message queues. We do not add threads as a performance hammer.
5. **No header-only "just include it" files for our own code.** stb gets to do that because it's a leaf. Our own modules have a `.c` so we control link order and rebuild times.
6. **No build-time code generation in v1.** No Python preprocessing scripts, no protoc, no embedded DSL compilers. If we need codegen, we write it in C and check in the output.
7. **No language other than C in shipped binaries.** Tools may be in Python or shell. The game is C.
8. **No abstraction we cannot delete in an afternoon.** If a layer can't be ripped out, it is load-bearing in a way that's invisible — and that's exactly the layer that bites you in year three.

## When you are tempted to break a rule

You will be. We expect it. The rule is: **write the temptation down**, including why, on a comment in the relevant file. If three of those comments accumulate in the same area, that is the signal that the rule needs to evolve. Adjust this document and the code together.

The goal is not to be rigid. The goal is to be **intentional**. Every line we write is a line that someone has to read, debug, and ship. The smaller and clearer the codebase, the more room we have to do the actual interesting work — making mechs come apart in beautiful, painful ways across a network.
