# M5 — Level editor

`tools/editor/soldut_editor` — a separate executable that authors `.lvl` files. Same engine, same allocator, same raylib build; different `main()`.

This document specifies the editor's UX, internal architecture, undo/redo, polygon drawing math, and how it cooperates with the running game for hot reload + test play.

The format the editor writes is in [01-lvl-format.md](01-lvl-format.md). The runtime that consumes those files is in [03-collision-polygons.md](03-collision-polygons.md) and the rest of M5.

## What we ship vs. what we use

We considered three options:

1. **Use Tiled** (https://www.mapeditor.org/) as-is and write a one-way `.tmx → .lvl` exporter. Tiled handles tiles, free polygons (objects), spawn points (object types), pickup spawners (object instances), decoration sprites (image layer + objects). Plan B if our editor stalls. Cost: 2 days of exporter work. Loss: the editor doesn't speak our format natively, so designers iterate slowly (export → reload).
2. **Build a thin custom editor on raylib + raygui.** Same engine code as the game; native `.lvl`; F5 test-play in-process. Cost: 1 week. Win: tight workflow for the one-author case we have at v1.
3. **Use TrenchBroom or similar.** Designed for 3D Quake-style maps, way too much editor for what we need.

We ship option 2. Plan B is documented in [00-overview.md](00-overview.md) §"Risks".

## Build & run

```bash
make editor                    # → build/soldut_editor
./build/soldut_editor          # blank canvas
./build/soldut_editor map.lvl  # open existing
```

The editor is built from a small set of `.c` files in `tools/editor/` plus most of the engine source. Specifically it links against:

- `src/level_io.{c,h}` — load/save
- `src/level.{c,h}` — runtime tile/polygon types
- `src/world.{h}` — type definitions
- `src/arena.{c,h}` — memory
- `src/log.{c,h}` — logging
- `src/math.{h}`, `src/hash.{c,h}`
- raylib + raygui

It does **not** link `src/mech.c`, `src/physics.c`, `src/net.c`, etc. — the editor doesn't simulate combat. F5 "test play" launches the *game* binary as a child process (see "Test play" below); the editor stays open.

## Editor file layout

```
tools/editor/
├── main.c                # event loop, raygui shell
├── doc.c, doc.h          # editor's in-memory document — superset of Level
├── tool.c, tool.h        # tool dispatch (tile-paint, polygon, spawn, pickup, ambient, deco)
├── poly.c, poly.h        # polygon drawing + ear-clipping triangulation
├── undo.c, undo.h        # command stack + tile-grid snapshot ring
├── palette.c, palette.h  # tile picker / pickup picker / polygon-kind picker
├── view.c, view.h        # camera, zoom, grid render, snap
├── play.c, play.h        # F5 test-play: spawn child process
├── files.c, files.h      # tinyfiledialogs wrapper
└── Makefile              # standalone target; the top-level Makefile delegates
```

Roughly 5 kLOC at v1, well under [09-codebase-architecture.md](../09-codebase-architecture.md)'s discomfort threshold.

## The document

The editor's in-memory state is a superset of `Level`:

```c
typedef struct EditorDoc {
    // Mirrors src/world.h Level — same types so we can copy directly
    // into a World on test-play.
    int       width, height, tile_size;
    LvlTile  *tiles;                       // packed
    LvlPoly  *polys;     int poly_count;   // already triangulated
    LvlSpawn *spawns;    int spawn_count;
    LvlPickup *pickups;  int pickup_count;
    LvlDeco  *decos;     int deco_count;
    LvlAmbi  *ambis;     int ambi_count;
    LvlFlag  *flags;     int flag_count;
    LvlMeta   meta;

    // Editor-only
    char      str_pool[8192];   // working STRT (UTF-8 blob; same shape as on-disk)
    int       str_pool_used;

    char      source_path[256]; // last save/load
    bool      dirty;             // any change since last save
} EditorDoc;
```

Storage lives in the editor's permanent arena. Resizable arrays use `stb_ds.h`'s `arrput` so we don't reinvent dynamic arrays. (This is the only place outside the runtime where `stb_ds` is used; the runtime stays on fixed-cap pools.)

## Tools and modes

A single click-drag tool grammar driven by which tool is active. Tools are picked from a left-side toolbar (raygui buttons) or via keyboard (T/P/S/I/A/D/M for Tile/Polygon/Spawn/pIckup/Ambient/Deco/Meta).

| Tool | Verb | Modifiers |
|---|---|---|
| **Tile paint (T)** | Left = paint, Right = erase | Shift = constrain to row/col, Ctrl = bucket fill, Scroll = pick tile id |
| **Polygon (P)** | Click-click-click-Enter to close | Shift = axis-lock new edge, Esc = abandon, Backspace = drop last vertex |
| **Spawn (S)** | Left-click to drop, Right-click to delete | Scroll = team |
| **Pickup (I)** | Left-click to drop, Right-click to delete | Scroll = category, Shift+Scroll = variant |
| **Ambient (A)** | Click-drag to draw rectangle | Scroll = kind |
| **Deco (D)** | Left-click to drop, Right-click to delete | Scroll = sprite, Shift+Scroll = layer |
| **Meta (M)** | Modal: name / music / background dropdowns | — |

The modifier-key gesture grammar is borrowed from Soldat PolyWorks (https://wiki.soldat.pl/index.php/Soldat_PolyWorks_Manual): one tool, modified meanings via Shift/Ctrl/Alt. This avoids a wide toolbar and keeps the muscle-memory thin.

### Universal verbs across tools

- `Ctrl+Z` — undo
- `Ctrl+Y` / `Ctrl+Shift+Z` — redo
- `Ctrl+S` — save
- `Ctrl+Shift+S` — save-as
- `Ctrl+O` — open
- `Ctrl+N` — new
- `F5` — test-play current map
- `Space + drag` — pan camera
- `Scroll while pan-modifier off` — context-sensitive (tool-specific)
- `Scroll while pan-modifier on` (Ctrl+Scroll) — zoom
- `G` — toggle 32 px grid
- `Shift+G` — toggle 4 px sub-grid (the snap target)
- `H` — toggle help overlay

### Snapping

Tile coords snap to 32 px (the tile size). Polygon vertices, spawn points, pickup spawners, and decoration anchors all snap to **4 px** by default. This is the cheap-but-effective polygon-degeneracy fix from the level-editor research (small-edge polygons cause sliver triangles that triangulation handles badly). Holding `Alt` disables snapping for sub-pixel placement (rare).

## Undo / redo

Editor commands are stored on a ring buffer of `EditCommand`:

```c
typedef enum { EC_TILE, EC_POLY_ADD, EC_POLY_DEL, EC_SPAWN_ADD, EC_SPAWN_DEL,
               EC_PICKUP_ADD, EC_PICKUP_DEL, /* ... */ } EditCommandKind;

typedef struct EditCommand {
    EditCommandKind kind;
    union {
        struct { uint16_t x, y; LvlTile before, after; } tile;
        struct { LvlPoly p; int idx; } poly;
        struct { LvlSpawn s; int idx; } spawn;
        // ... one variant per kind
    };
} EditCommand;
```

Two command shapes:

- **Tile-paint commands** are batched — a click-drag emits one command per painted tile, but the undo stack collapses consecutive `EC_TILE` commands of the same drag into one **stroke**, undone atomically. Stroke boundaries are mouse-press and mouse-release events.
- **Object commands** (polygon, spawn, pickup, deco, ambient) are atomic per-action.

The ring buffer is **64 strokes deep**. Older commands age out silently. This is enough for a session's worth of work; we don't ship infinite undo (memory grows linearly). When commit overflow is reached, the oldest command's "before" state is committed to a snapshot — see "Tile-grid snapshots" below.

### Tile-grid snapshots for big undos

For a multi-thousand-tile bucket-fill, storing per-tile `before/after` is 1 MB+ of undo data per stroke. Instead, bucket-fill (and any tile-region command bigger than 256 tiles) takes a **whole-tile-grid snapshot** before the operation: a single `memcpy` of the tile grid into the undo arena. The undo command stores `(snapshot_offset, snapshot_size)` and rewinds via memcpy.

For a 200×100 map: snapshot is 80 KB. With 64 strokes, worst-case snapshot memory is 5 MB. Live with it; trade-off entry: "editor undo is whole-tile-grid snapshot, not differential."

## Polygon tool

Click-click-click-Enter closes the polygon. As the user clicks, a preview line follows the cursor showing the next edge.

### Validation at draw time

Reject — visually highlight in red and refuse to commit — any polygon that:

- Has fewer than 3 vertices.
- Has any edge shorter than 8 px (after snap).
- Self-intersects (segment-segment test against all prior edges).
- Has reverse winding (would triangulate inverted).

The first three are easy. Self-intersection check is O(n²) but n is small (≤32 in practice). Winding is checked by the signed area sign.

### Triangulation

When the polygon is closed, run **ear-clipping** (Eberly's PDF, https://www.geometrictools.com/Documentation/TriangulationByEarClipping.pdf) to produce a list of triangles. Each triangle becomes one `LvlPoly` record in the document.

Implementation in `tools/editor/poly.c`, ~250 LOC of straight C:

```c
// Returns triangle count; triangles are written into out_tris (flat
// array of 3 verts each). Caller's responsibility to size out_tris
// for at least (n - 2) triangles.
int poly_triangulate(const Vec2 *verts, int n, Vec2 (*out_tris)[3]);
```

Algorithm: maintain a doubly-linked vertex ring. For each vertex, classify as ear (convex + no other vertex inside). Pop ears one at a time. O(n²); negligible for n ≤ 32.

Edge normals are computed at the same time and written into `LvlPoly.normal_x[i] / normal_y[i]` as Q1.15 fixed-point.

### Polygon kinds

The polygon palette has 5 entries: SOLID (default), ICE, DEADLY, ONE_WAY, BACKGROUND. The kind is set before drawing the polygon (scroll-wheel changes it; the cursor changes color to match).

ONE_WAY polygons need an "up" direction so the runtime knows which side is solid. The editor records the polygon's orientation (CCW vs CW) and the runtime treats CCW as "up=normal direction." Alternatively, the editor lets the user click an arrow on the polygon's centroid after closure to indicate the pass-through direction; that's simpler UX. Going with the latter.

## Spawn / pickup / decoration / ambient placement

Drag-and-drop from a palette. Each tool's palette is a vertical list on the right side (raygui's `GuiListView`).

The pickup palette shows category × variant grid:
```
HEALTH    [s] [m] [l]
AMMO      [primary] [secondary]
ARMOR     [light] [heavy] [reactive]
WEAPON    [each weapon icon]
POWERUP   [berserk] [invis] [godmode]
JET_FUEL  [single]
```
Each cell is a 32×32 button; hovering shows the respawn timer + audio cue.

Decoration palette is a scrollable grid of available sprite paths (read from `assets/decals/decoration_*.png` at editor startup). Adding a new sprite to the assets directory makes it available next editor restart — we don't watch the filesystem for new files (that bloats the editor with no win for v1).

## Camera + view

`view.c`. Standard 2D pan/zoom on a `Camera2D`:

- Space-drag pans.
- Ctrl-scroll zooms (multiplicative, 1.1× per notch).
- Default zoom = 1.0 (one-tile = 32 px on screen).
- World-bounds dim/border.
- Toggleable grid: 32 px (tile) and 4 px (snap).
- A small minimap top-right shows the whole map + camera viewport.

## Test play (F5)

When the user hits F5, the editor:

1. Saves to a temp file (`/tmp/soldut-editor-test-XXXXXX.lvl`).
2. Forks `./soldut --shot tools/editor/playtest.shot` with an environment variable `SOLDUT_TEST_LVL=<temp_path>` and a special `--test-play <temp_path>` flag.
3. The game-side `--test-play` flag bootstraps a single-player offline server with that map loaded, FFA mode, 1 minute round, just the local player + 3 wandering bots (or 0 — designer's choice via Shift+F5).
4. The editor stays running; when the child process exits, the editor returns to the front.

The child process runs in its own window (raylib's `InitWindow` is fine being called twice across distinct processes). On Linux the editor uses `posix_spawn`; on Windows, `CreateProcess`; macOS, `posix_spawn`. About 60 LOC in `tools/editor/play.c`.

The "wandering bots" in test-play are the same crude bot we use for the bake-test (see [07-maps.md](07-maps.md) §"Bake test"). They're not gameplay — they're targets that exercise spawn/pickup/CTF flows.

## Hot reload from the editor

Out of `Ctrl+S`, the editor:

1. Writes the `.lvl` to `assets/maps/<name>.lvl` (the same path the running game reads from).
2. The running game's mtime watcher (see "Hot reload" in [08-rendering.md](08-rendering.md) §"Hot reload of assets") notices and reloads.
3. If the running game is mid-round on that map, the host broadcasts a `LEVEL_RELOAD` event (new `NET_MSG_LEVEL_RELOAD` ID) and clients respond by reloading the same path. The round continues.

If the round is mid-active and a reload corrupts physics state (e.g., a player is now standing inside a freshly-painted solid tile), the host's collision system pushes them to the nearest empty tile. Documented as "in-development behavior" — public servers should not enable hot reload during a live round.

In production, the network's `LEVEL_RELOAD` is **gated to host-only debug builds** (`#ifdef DEV_BUILD`). Players can't trigger it.

## File picker

`tinyfiledialogs` (https://sourceforge.net/projects/tinyfiledialogs/) for OS-native open/save dialogs. Single-header (`tinyfiledialogs.h` + `tinyfiledialogs.c`), MIT license, ~3 kLOC. Cross-platform.

Vendored at `third_party/tinyfiledialogs/`. This is the **fourth** vendored dependency past raylib + ENet + stb. Documented per [01-philosophy.md](../01-philosophy.md) Rule 1: writing a custom file picker per OS is bigger than the dependency, and tinyfiledialogs is the smallest acceptable third-party.

## Polygon-tool keyboard verbs

Listed here because they're load-bearing and trip people up:

- `Click` — drop a vertex (snapped to 4 px).
- `Shift+Click` — drop a vertex with axis-lock (constrain to last edge's H or V).
- `Backspace` — undo last vertex (still inside the polygon).
- `Esc` — abandon the in-progress polygon.
- `Enter` or `Click on first vertex` — close and triangulate.
- `Tab` — switch polygon kind without aborting (cursor recolors).

## Validation on save

The editor refuses to save a file that fails any of:

- Tile grid contains data (size > 0 even if all empty).
- At least 1 spawn point exists.
- Every CTF flag base has a matching spawn point on the same team.
- Polygons triangulate cleanly (caught at edit time, but a final sweep runs).
- Pickup spawners aren't placed inside SOLID tiles.
- META section has a non-empty display name.
- **Alcove sizing** — every pickup spawner whose neighborhood is an enclosed nook (>2 SOLID walls within 96 px) must satisfy the alcove minimums per [07-maps.md](07-maps.md) §"Caves, alcoves, and hidden nooks": ≥3 tiles tall interior, ≥2 tiles deep, ≥2-tile mouth opening, ≥16 px clearance between the pickup centroid and any wall. The editor walks the empty-volume around each spawner and flags too-shallow / too-short enclosures.

The editor also adds three **alcove polygon presets** (edge alcove, jetpack alcove, slope-roof alcove) so designers don't hand-author the four-polygon C-shape every time. See [07-maps.md](07-maps.md) §"Editor presets" for the preset list.

Save failures pop a modal listing the problems with `Goto…` buttons that camera-snap to the offending object. Same modal style as raygui's `GuiMessageBox`.

## Asset-path validation

When the META "background" or "music" field references a STRT path, the editor attempts to load it (LoadTexture / LoadMusicStream) on the editor's own raylib context. If load fails, the field is highlighted red and a tooltip shows the underlying error. We don't block save on missing assets — a designer might be authoring against assets the artist hasn't shipped yet — but we warn loudly.

## What the editor does NOT have at v1

- **Multi-user collaborative editing.** Out per [07-level-design.md](../07-level-design.md). Single author, single document.
- **Region copy/paste across files.** A user wanting to reuse a chunk re-draws it. Documented as a known UX gap; v2.
- **Procedural decoration brushes.** Same.
- **Advanced terrain like Wang tiles or auto-tiling.** We use a flat tile palette.
- **Texture-painting on polygons.** Polygons are flat-colored by `kind`; no per-polygon texture override.
- **Live polygon CSG (boolean ops).** A wall is drawn or it's not; no "subtract this polygon."
- **History view of who changed what.** No version control beyond what `git` provides on the `.lvl` file.

## Done when

- `make editor` produces `build/soldut_editor`.
- A user can: open a blank doc, paint tiles, draw a polygon, place spawns + pickups, set META, save to disk, reopen, see the same content, F5 launches the game with that map, the game plays it without crashing.
- The 8 ship maps in [07-maps.md](07-maps.md) are authored using this editor (not as a Plan B fallback).
- A polygon with self-intersection is rejected at edit time; a polygon that triangulates correctly produces visually-equivalent triangles in the runtime.
- F5 test-play with 0 bots starts a round in <2 s on the developer machine.

## Trade-offs to log

- **Editor undo is whole-tile-grid snapshot for big strokes** (see "Tile-grid snapshots" above). 5 MB worst-case undo memory, well within the editor's permanent arena.
- **F5 test-play forks a child process** rather than running the simulation in-process. Simpler than refactoring `main.c` to be re-enterable from the editor; the cost is a brief process-start delay.
- **`tinyfiledialogs` is the fourth vendored dep.** Documented above.
- **No "import from Tiled" path.** If we need it later, it's 2 days of one-way exporter work; we don't pre-build it.
