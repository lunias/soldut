# P04 — Level editor (`tools/editor/`)

## What this prompt does

Builds the standalone level editor as a separate executable. Same engine code, plus an editor-specific UI. Tile painting, polygon drawing with ear-clipping triangulation, spawn / pickup / decoration / ambient placement, save / load round-trip with the `.lvl` format from P01, F5 test-play that forks the game binary, hot-reload integration.

This is the largest single prompt by code volume (~5 kLOC). Plan to split across two sessions if needed.

Depends on P01 (.lvl format), P02 (polygon collision so the editor's preview matches runtime).

## Required reading

1. `CLAUDE.md` — project conventions
2. `documents/01-philosophy.md` — Rule 1 (one file per module, no codegen, etc.)
3. `documents/07-level-design.md` — what a level should be
4. **`documents/m5/02-level-editor.md`** — the spec for this prompt
5. `documents/m5/01-lvl-format.md` — the file format the editor produces
6. `documents/m5/03-collision-polygons.md` §"Slopes, hills, valleys" + §"Editor presets for slope-friendly geometry"
7. `documents/m5/07-maps.md` §"Caves, alcoves, and hidden nooks" §"Editor presets" + §"Sizing — the mech has to fit"
8. `src/level_io.{h,c}` — load/save (P01)
9. `src/world.h` — `LvlPoly`, `LvlSpawn`, `LvlPickup`, `LvlDeco`, `LvlAmbi`, `LvlFlag`, `LvlMeta`
10. `src/arena.{c,h}` — allocator
11. `src/log.{c,h}` — logger
12. raylib + raygui: `third_party/raylib/src/raylib.h`, `third_party/raylib/src/raygui.h` (or similar)
13. `Makefile` — build system pattern

## Concrete tasks

### Task 1 — `tools/editor/` skeleton

```
tools/editor/
├── main.c                # event loop, raygui shell, dispatcher
├── doc.{c,h}             # editor's in-memory document — superset of Level
├── tool.{c,h}            # tool dispatch (tile-paint, polygon, spawn, pickup, ambient, deco)
├── poly.{c,h}            # polygon drawing + ear-clipping triangulation
├── undo.{c,h}            # command stack + tile-grid snapshot ring
├── palette.{c,h}         # tile picker / pickup picker / polygon-kind picker
├── view.{c,h}            # camera, zoom, grid render, snap
├── play.{c,h}            # F5 test-play: spawn child process
├── files.{c,h}           # tinyfiledialogs wrapper
└── Makefile              # standalone target
```

Top-level Makefile gets a `make editor` target that delegates to `tools/editor/Makefile`. Editor binary: `build/soldut_editor`.

Editor links: `src/level_io.{c,h}`, `src/world.h`, `src/arena.{c,h}`, `src/log.{c,h}`, `src/math.h`, `src/hash.{c,h}`, raylib + raygui + tinyfiledialogs. **Does NOT link** `mech.c`, `physics.c`, `net.c`.

### Task 2 — `tinyfiledialogs` vendoring

Add `third_party/tinyfiledialogs/` (https://sourceforge.net/projects/tinyfiledialogs/, single file `tinyfiledialogs.c` + `.h`, MIT license). Cross-platform OS-native file dialogs. ~3 kLOC.

This is the **fourth** vendored dependency. Document in CLAUDE.md.

### Task 3 — `EditorDoc`

Per `documents/m5/02-level-editor.md` §"The document":

```c
typedef struct EditorDoc {
    int       width, height, tile_size;
    LvlTile  *tiles;
    LvlPoly  *polys;     int poly_count;
    LvlSpawn *spawns;    int spawn_count;
    LvlPickup *pickups;  int pickup_count;
    LvlDeco  *decos;     int deco_count;
    LvlAmbi  *ambis;     int ambi_count;
    LvlFlag  *flags;     int flag_count;
    LvlMeta   meta;
    char      str_pool[8192];
    int       str_pool_used;
    char      source_path[256];
    bool      dirty;
} EditorDoc;
```

Use `stb_ds.h`'s `arrput` for resizable arrays. This is the **only** place outside the runtime where stb_ds is used; runtime stays on fixed-cap pools.

### Task 4 — Tools + tool dispatch

Per `documents/m5/02-level-editor.md` §"Tools and modes":

Each tool implements a small interface:

```c
typedef struct ToolVTable {
    void (*on_press)(EditorDoc*, ToolCtx*, Vec2 world);
    void (*on_drag)(EditorDoc*, ToolCtx*, Vec2 world);
    void (*on_release)(EditorDoc*, ToolCtx*, Vec2 world);
    void (*draw_overlay)(const EditorDoc*, const ToolCtx*);
    bool (*on_key)(EditorDoc*, ToolCtx*, int key);
} ToolVTable;
```

7 tools: tile-paint, polygon, spawn, pickup, ambient, deco, meta-modal.

Universal verbs (Ctrl+Z/Y, Ctrl+S, Ctrl+O, F5, Space-pan, etc.) are handled in `main.c` before tool dispatch.

### Task 5 — Snap discipline

Tiles snap to 32 px (the tile size). Polygon vertices, spawn points, pickup spawners, decorations all snap to **4 px** by default. Alt held disables snapping.

The 4-px snap is the cheap-but-effective polygon-degeneracy fix from the research. Don't skip it.

### Task 6 — Undo / redo

Per `documents/m5/02-level-editor.md` §"Undo / redo":

Ring buffer of 64 strokes. Tile-paint commands collapse to strokes (mouse-press to mouse-release). Big tile ops (>256 tiles, including bucket fill) take a whole-grid snapshot. Object commands are atomic per action.

### Task 7 — Polygon tool + ear-clipping

Per `documents/m5/02-level-editor.md` §"Polygon tool":

Click-click-click-Enter to close. Validation at draw time:
- ≥3 vertices
- No edge < 8 px (after snap)
- No self-intersection (segment-segment test against all prior edges)
- CCW winding (or auto-flip)

On close, run ear-clipping (Eberly's algorithm — see `documents/m5/02-level-editor.md` §"Triangulation"). ~250 LOC of straight C in `tools/editor/poly.c`.

ONE_WAY polygons need an "up" direction — after closure, prompt with a small arrow on the polygon's centroid; user clicks to indicate pass-through direction.

### Task 8 — Polygon palette presets (slopes + alcoves)

Per `documents/m5/03-collision-polygons.md` §"Editor presets" + `documents/m5/07-maps.md` §"Editor presets":

Slope presets (3 angles × heights):
- ramp_up_30°, ramp_up_45°, ramp_up_60° (1, 1.5, 2 tile heights)
- ramp_down_*: mirrors
- bowl_30°, bowl_45° (two opposing ramps + flat bottom)
- overhang_30°, overhang_45°, overhang_60° (ceiling angles)

Alcove presets (3 archetypes):
- edge_alcove (2 deep × 3 tall, opens horizontally)
- jetpack_alcove (3 wide × 3 tall × 3 deep, mouth facing inward)
- slope_roof_alcove (4 wide × 4 tall at entrance, 1 at back)

User picks preset → drops 4-N polygons forming the shape. Editable afterward.

### Task 9 — Spawn / pickup / decoration / ambient placement

Drag-and-drop from a palette per `documents/m5/02-level-editor.md` §"Spawn / pickup / decoration / ambient placement".

The pickup palette shows category × variant grid; hovering shows respawn timer + audio cue.

### Task 10 — Validation on save

Per `documents/m5/02-level-editor.md` §"Validation on save":

- Tile grid populated.
- ≥1 spawn point.
- CTF flag bases have matching team-sided spawn points.
- Polygons triangulate cleanly.
- Pickup spawners not inside SOLID tiles.
- META has a non-empty display name.
- **Alcove sizing** per `documents/m5/07-maps.md` §"Sizing — the mech has to fit": every pickup spawner whose neighborhood is enclosed (>2 SOLID walls within 96 px) satisfies ≥3 tiles tall × ≥2 tiles deep × ≥2 tiles mouth × ≥16 px clearance from any wall.

Save failures pop a modal listing problems with `Goto…` buttons.

### Task 11 — Camera + view

`view.c`: pan (Space-drag), zoom (Ctrl-scroll), 32 px / 4 px grid toggles (G / Shift+G), minimap top-right.

### Task 12 — Test-play (F5)

Per `documents/m5/02-level-editor.md` §"Test play (F5)":

1. Save to temp file.
2. Fork `./soldut --test-play <temp_path>` (you'll add this CLI flag in `main.c` of the *game* binary).
3. Editor stays running; child returns to editor on exit.

`posix_spawn` on Linux/macOS, `CreateProcess` on Windows.

### Task 13 — File picker via tinyfiledialogs

`Ctrl+O` → `tinyfd_openFileDialog`; `Ctrl+S` (no source path) → `tinyfd_saveFileDialog`.

## Done when

- `make editor` builds `build/soldut_editor`.
- Open editor, paint tiles, draw a polygon, place spawn + pickup, set META, Ctrl+S, reopen file → exact same content.
- F5 launches the game with the saved map; round runs.
- Polygon with self-intersection: rejected at edit time.
- Pickup placed inside a SOLID tile: rejected on save.
- Pickup placed in a 1-tile-tall enclosure: rejected on save with the alcove-sizing message.
- Ctrl+Z undoes; Ctrl+Y redoes; tile-paint strokes collapse to one undo step.
- Slope preset drops a 45° ramp polygon; ear-clipping produces 1-2 triangles.

## Out of scope

- Multi-user collaborative editing.
- Region copy/paste across files.
- Auto-tiling / Wang tiles.
- Live polygon CSG (boolean ops).
- A history view of who changed what.

## How to verify

```bash
make editor
./build/soldut_editor                        # blank doc
./build/soldut_editor assets/maps/foundry.lvl  # opens existing
```

Manual: paint tiles, draw a slope polygon, place spawn at (300, 800), Ctrl+S to `/tmp/test.lvl`. Run `./soldut --test-play /tmp/test.lvl`. Round plays.

## Close-out

1. Update `CURRENT_STATE.md`: editor functional.
2. Update `TRADE_OFFS.md`:
   - **Add** "Editor undo is whole-tile-grid snapshot for big strokes" (pre-disclosed).
   - **Add** "F5 test-play forks a child process" (pre-disclosed).
   - **Add** "tinyfiledialogs is the fourth vendored dep" (pre-disclosed).
3. Don't commit unless explicitly asked.

## Common pitfalls

- **Editor links a subset of src/** — easy to accidentally pull in mech.c via dependency. Inspect linker output.
- **Ear-clipping rejects collinear edges**: handle the case where 3 consecutive vertices are collinear; treat the middle as a non-ear.
- **Self-intersection check is O(n²)** for small n; don't optimize prematurely.
- **`stb_ds` arrput macro pitfalls**: see stb_ds.md for gotchas (e.g., array realloc invalidates pointers; use indices not pointers).
- **F5 path resolution**: the temp .lvl path needs to be absolute or the child can't find it. Use `realpath`.
- **Window focus on F5**: when child closes, focus may not return to editor on all platforms. Acceptable; user clicks back.
