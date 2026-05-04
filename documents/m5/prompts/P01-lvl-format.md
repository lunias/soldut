# P01 — `.lvl` binary format + loader/saver + tests

## What this prompt does

Implements the on-disk level format the rest of M5 depends on. New module `src/level_io.{c,h}` provides `level_load()` and `level_save()` plus a CRC32 helper. New `tests/level_io_test.c` round-trips a synthetic level. The three M4 code-built maps (Foundry / Slipstream / Reactor) get exporter scripts so they can be rebuilt as `.lvl` files in a later prompt.

This is the foundation prompt. Nothing else in M5 ships without it.

## Required reading (in this order)

Read these before doing anything:

1. `CLAUDE.md` — project conventions (build commands, conventions that bite)
2. `documents/01-philosophy.md` — the C11 rules, allocation rules, naming
3. `documents/09-codebase-architecture.md` — module layout
4. `documents/07-level-design.md` — the design canon for what a level is
5. `documents/m5/00-overview.md` — M5 strategic view
6. **`documents/m5/01-lvl-format.md`** — the spec for this prompt
7. `src/level.h` and `src/level.c` — the M1 tile-grid loader you're extending
8. `src/world.h` — the `Level` struct definition (you'll add fields)
9. `src/maps.c` — the `build_foundry`, `build_slipstream`, `build_reactor` paths
10. `src/arena.h` and `src/arena.c` — the level arena allocator you'll use
11. `tests/headless_sim.c` — the existing test scaffold (model your tests after this shape)

## Background

The M4 build ships maps as code (`maps.c::build_foundry` etc.). The runtime calls `map_build(MapId, Level*, Arena*)` and the function fills the level arena with a hand-coded tile grid. There's no `.lvl` file format, no loader, no editor. Everything M5 does — editor, custom maps, network map sharing — assumes the format exists.

The format is specified in `documents/m5/01-lvl-format.md`: a 64-byte header, a Quake-WAD-style lump directory, packed records per section (TILE / POLY / SPWN / PICK / DECO / AMBI / FLAG / META / STRT), CRC32 footer-style over the whole file with the CRC field zeroed. Little-endian everywhere.

## Concrete tasks

### Task 1 — extend `Level` struct in `src/world.h`

Add the fields described in `documents/m5/03-collision-polygons.md` §"What changes":

- `LvlPoly *polys; int poly_count;` (free-floating polygons)
- `LvlSpawn *spawns; int spawn_count;`
- `LvlPickup *pickups; int pickup_count;` (just data; transient instances live in P05)
- `LvlDeco *decos; int deco_count;`
- `LvlAmbi *ambis; int ambi_count;`
- `LvlFlag *flags; int flag_count;`
- `LvlMeta meta;`
- `int *poly_grid; int *poly_grid_off;` (broadphase index — populated in P02; declare here)

Define the `LvlTile`, `LvlPoly`, `LvlSpawn`, `LvlPickup`, `LvlDeco`, `LvlAmbi`, `LvlFlag`, `LvlMeta` structs in `src/world.h` per the byte layouts in `documents/m5/01-lvl-format.md`. **Sizes must match the spec exactly** — `static_assert(sizeof(LvlPoly) == 32)` etc.

Update existing M4 callers that touch `Level.tiles`. The current code uses `uint8_t *tiles`; the new spec uses `LvlTile *tiles` (with `id` + `flags` per tile, 4 bytes). This is a breaking change — fix every callsite. Search:

```bash
grep -rn "level.tiles\|level->tiles\|L->tiles" src/
```

For backwards compat with M4's `TILE_SOLID = 1`, keep the existing `TileKind` enum but read from `level->tiles[i].flags & TILE_F_SOLID` everywhere. Old code that sets `tiles[i] = TILE_SOLID` becomes `tiles[i] = (LvlTile){.id = 0, .flags = TILE_F_SOLID}`.

### Task 2 — write `src/level_io.{h,c}`

Public API per `documents/m5/01-lvl-format.md` §"Loader API":

```c
typedef enum {
    LVL_OK = 0,
    LVL_ERR_FILE_NOT_FOUND, LVL_ERR_TOO_LARGE, LVL_ERR_BAD_MAGIC,
    LVL_ERR_BAD_VERSION, LVL_ERR_BAD_CRC, LVL_ERR_BAD_DIRECTORY,
    LVL_ERR_BAD_SECTION, LVL_ERR_OOM,
} LvlResult;

LvlResult level_load(struct World *world, struct Arena *level_arena, const char *path);
LvlResult level_save(const struct World *world, struct Arena *scratch, const char *path);
```

Implementation strategy in `documents/m5/01-lvl-format.md` §"Loader implementation strategy" — fread whole file into level arena, walk header, build directory, copy records into `World` fields, leave string-table blob in place (records reference into it).

CRC32 implementation: the standard polynomial 0xEDB88320, table-based, ~150 LOC. **Don't pull in zlib.** Compute over the full file with the `crc32` field zeroed.

Endianness: explicit `r_u16/r_u32/w_u16/w_u32` helpers per the spec. `static_assert` little-endian host.

### Task 3 — wire `map_build` to use `level_load`

In `src/maps.c`, modify `map_build` per `documents/m5/01-lvl-format.md` §"Loader API":

```c
void map_build(MapId id, Level *level, Arena *arena) {
    const MapDef *def = map_def(id);
    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", def->short_name);
    LvlResult r = level_load(/*world*/&g->world, arena, path);
    if (r != LVL_OK) {
        LOG_W("map_build(%s): load failed (%d) — falling back to code-built",
              def->short_name, (int)r);
        build_fallback(id, level, arena);   // existing build_foundry/etc.
    }
}
```

The fallback path keeps `build_foundry / build_slipstream / build_reactor` so a fresh checkout without `assets/maps/` still boots. P17 will create the actual .lvl files; for now `level_load` always fails and the fallback fires.

### Task 4 — write `tests/level_io_test.c`

Round-trip test. Build a synthetic World programmatically with one of every section populated (a tile grid, 3 polygons, 2 spawns, 1 pickup, 2 decorations, 1 ambient zone, 2 flag bases, a META with a name string). Run `level_save` to a temp file. Call `level_load` into a fresh World. Bytewise-compare every section.

Plus two corruption tests:

- Bit-flip a random byte in the saved file → `level_load` returns `LVL_ERR_BAD_CRC`.
- Truncate the saved file at a random byte → `level_load` returns `LVL_ERR_BAD_DIRECTORY` or `LVL_ERR_BAD_SECTION` (don't crash).

Add a Makefile target `make test-level-io` that runs the test. Modeled after `make test-physics`.

### Task 5 — `tools/cook_maps/` (one-shot exporter — STUB ONLY in this prompt)

Don't actually generate `.lvl` files yet. Create the directory `tools/cook_maps/` with a placeholder `cook_maps.c` that has the `int main()` entry point and a TODO listing the three M4 maps to export. P17 fills this in by calling `level_save` against synthetic Worlds initialized via `build_foundry / build_slipstream / build_reactor`.

## Done when

- `src/level_io.{h,c}` exists with the API above.
- `static_assert(sizeof(LvlHeader) == 64)`, `sizeof(LvlPoly) == 32)`, `sizeof(LvlSpawn) == 8)`, etc. — the byte layouts compile-check.
- `make` builds clean (no warnings; remember `-Werror`).
- `make test-level-io` produces a synthetic level, saves, reloads, byte-compares, passes.
- The corruption tests detect CRC mismatch and truncated files.
- `level_load` is wired into `map_build` with fallback to the existing code-built path.
- A bit-flipped `.lvl` rejects with a clear log message; the host falls back.
- The headless sim still passes: `make test-physics` and inspect output.
- `tests/net/run.sh` still passes (the format change shouldn't affect networking).

## Out of scope (don't do these here)

- Polygon broadphase (`poly_grid` population) — that's P02. Just declare the fields.
- Actually creating `.lvl` files for the three M4 maps — that's P17.
- The level editor — P04.
- Defensive parser hardening beyond CRC + lump-bound checks — keep it simple.
- Rendering polygons — P02 will do collision; rendering polygons is in P10.
- Any new TileKind beyond `TILE_F_SOLID` (those land in P02).
- Loading background PNGs referenced from META — that's P13.

## How to verify

```bash
make                              # clean build, no warnings
make test-level-io                # round-trip test
make test-physics                 # existing physics regression
./tests/net/run.sh                # existing networking smoke test
./soldut --host 23073             # boots; falls back to code-built map; works
```

Visual: launch the host, single-player, observe the M4 fallback Foundry plays normally (capsule mech, three-tile-tall cover wall, dummy on the right platform). The .lvl file format is in place but the content isn't loaded yet.

## Close-out

1. Update `CURRENT_STATE.md`:
   - In the milestones table, add a **Status** row for **M5** with the format-loader-only first deliverable noted.
   - Add a section "M5 progress" under "Recently fixed" or similar listing what's done and what's pending.

2. Update `TRADE_OFFS.md`:
   - **Don't delete** any entries yet — the .lvl format alone doesn't resolve them. P17 (when assets are exported) will close out the "Hard-coded tutorial map" entry.
   - Add a new entry "**`.lvl` v1 format is locked in**" as a forward-disclosure: bumping versions costs editor + loader churn.

3. Don't commit unless explicitly asked.

## Common pitfalls

- **`static_assert` failures on struct size** — pad explicitly with `uint8_t reserved[N]` to hit the spec'd byte counts. Compiler may insert padding without it.
- **Endianness assumptions** — use the explicit `r_u32/w_u32` helpers everywhere; don't `memcpy` whole structs and call it good (struct layout in memory is not guaranteed to match wire layout even on LE hosts).
- **CRC computation order** — compute over the whole file with the CRC field treated as zero. The save path: write file with CRC=0, compute CRC over written bytes, seek back to header offset of CRC field, write the actual CRC, close.
- **Magic mismatch** — the bytes are `'S','D','L','V'` exactly, in that order. Don't swap.
- **Forgetting the static_assert little-endian check** at the top of `level_io.c`.

## Trade-offs to log when shipping

If you discover something forced during implementation, add a TRADE_OFFS entry. Pre-disclosed candidates:

- "No compression in `.lvl` files" (already documented in 01-lvl-format)
- "No per-lump CRC; single footer-style CRC over whole file" (already documented)
- "No per-tile sprite override" (already documented)
