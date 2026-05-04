# M5 — `.lvl` binary format

The on-disk level file. Everything that constitutes "a map" — tile grid, free polygons, spawn points, pickup spawners, decoration sprites, ambient zones, flag bases, audio environment — sits in here. The level editor writes it; the game's loader reads it.

This document specifies the file format. The editor that produces it is in [02-level-editor.md](02-level-editor.md); the runtime that consumes it is in [03-collision-polygons.md](03-collision-polygons.md).

## Why a custom binary

We had three choices for level serialization:

1. **JSON / TOML / similar.** Human-readable, slow to parse, large on disk, and we'd be wiring a third-party parser into the engine. Out per [01-philosophy.md](../01-philosophy.md) Rule 1 ("dependencies are taxes") and the existing position in [07-level-design.md](../07-level-design.md) ("no JSON, no XML").
2. **Tiled `.tmx` (XML).** Industry-standard 2D format. Free editor exists. We'd lose control of the format and pay a parser dependency. Stays as the **Plan B** if our editor never reaches "good enough" — see [02-level-editor.md](02-level-editor.md).
3. **A flat little-endian binary we own.** Ships in milliseconds, validates with a CRC, and the schema lives in `src/level_io.h`. This is what we do.

The decision is the same one every Soldat-tradition shooter has made: own the format. PMS, BSP, WAD all chose the same shape.

## File shape — Quake-WAD-style lump table

The header is fixed at offset 0; lump data sits in a directory of `(offset, size, name)` entries; each lump's payload follows in sequence. This is the WAD layout (https://doomwiki.org/wiki/WAD) with one-line modifications. Why this and not Soldat PMS's "everything inline":

- **Adding a section is "bump version, append a directory entry, append a lump."** PMS's inline padding required hand-counting bytes; the wiki specifically calls this out as a footgun.
- **A loader can read just one section.** When the editor opens a file but only wants the spawn points, it reads the directory + the SPWN lump and skips the tile grid. Useful for batched validation tools.
- **Diffing two `.lvl` files is sane.** A `binwalk`-style dump shows section boundaries; two files that differ only in tile grid have identical headers and identical non-TILE lumps.

```
+----------------------------------+
| 64-byte header                   |
| (magic, version, world dims,     |
|  string-table location, lump     |
|  directory)                      |
+----------------------------------+
| TILE   lump (tile grid)          |
+----------------------------------+
| POLY   lump (free polygons)      |
+----------------------------------+
| SPWN   lump (spawn points)       |
+----------------------------------+
| PICK   lump (pickup spawners)    |
+----------------------------------+
| DECO   lump (decoration sprites) |
+----------------------------------+
| AMBI   lump (ambient zones)      |
+----------------------------------+
| FLAG   lump (CTF flag bases)     |
+----------------------------------+
| META   lump (audio env, theme)   |
+----------------------------------+
| STRT   string table              |
+----------------------------------+
```

A lump's *payload* is a packed array of fixed-width records (or, for STRT, a blob of zero-terminated UTF-8). The directory tells the loader the byte offset of each lump's payload — the loader does not parse anything outside the lumps it knows.

A 100×60 tile map (3200 × 1920 px world) with 200 polygons, 32 spawns, 30 pickups, 100 decorations, 4 ambient zones, and 2 flag bases weighs ~25 KB before background art. Bigger maps (Citadel, 200×100) cap near 80 KB. Well under the 500 KB ceiling in [07-level-design.md](../07-level-design.md).

## Endianness

**Little-endian everywhere.** Quake BSP, Doom WAD, Soldat PMS all do the same. We compile only for little-endian targets (x86_64, arm64); a `static_assert` at the top of `level_io.c` catches a hypothetical big-endian host. Read/write helpers (`read_u32_le`, `write_u32_le`, etc.) make the byte order explicit at every callsite — even though the host happens to match, we don't rely on `memcpy` of a struct.

## Header (64 bytes, fixed)

```c
typedef struct {
    char     magic[4];           // "SDLV"  ("Soldut Level")
    uint32_t version;            // LVL_VERSION_CURRENT (1 at M5)
    uint32_t section_count;      // number of valid lumps below
    uint32_t flags;              // reserved, must be 0 at v1
    uint32_t world_w_tiles;
    uint32_t world_h_tiles;
    uint32_t tile_size_px;       // 32 px at v1, written explicitly
    uint32_t string_table_off;   // file offset of the STRT lump
    uint32_t string_table_size;  // bytes
    uint32_t crc32;              // checksum of the whole file (CRC field zeroed)
    uint8_t  reserved[24];       // pad to 64 bytes
} LvlHeader;
static_assert(sizeof(LvlHeader) == 64, "LvlHeader must be 64 bytes");
```

`magic` is `'S','D','L','V'` to make the file recognizable in a hex dump. `version` is bumped on any breaking change to lump layout — old loaders refuse to load newer files. Forward compat: a loader that sees an *unknown lump name* skips it without erroring, so v2 can add sections that v1 ignores cleanly.

`crc32` is computed over the full file with the `crc32` field treated as zero. Loader recomputes; mismatch is a hard reject. (See "CRC32 placement" below for why footer-style was rejected.)

## Lump directory (after the header)

The lump directory is `section_count` entries of:

```c
typedef struct {
    char     name[8];            // 8-byte fixed name, padded with 0x00
    uint32_t offset;             // file offset of the payload
    uint32_t size;               // payload size in bytes
} LvlLumpEntry;
static_assert(sizeof(LvlLumpEntry) == 16, "LvlLumpEntry must be 16 bytes");
```

8-byte name slots match WAD. The loader builds a `name → (offset, size)` lookup once, then resolves sections by name. Names are exact-match, ASCII-only, case-sensitive: `TILE`, `POLY`, `SPWN`, `PICK`, `DECO`, `AMBI`, `FLAG`, `META`, `STRT`. Future names live in the same 8-byte slot.

## Sections

Each section's payload is a packed array of fixed-width records (POD, no padding bytes). The loader trusts `size` and reads `size / sizeof(record)` records. Loaders for unknown sections skip; loaders for known sections that find a `size` mismatch (e.g., a polygon lump that isn't a multiple of `LvlPoly`'s size) reject the file with `ERR_LEVEL_CORRUPT`.

### TILE — tile grid

```c
typedef struct {
    uint16_t id;                 // tile graphic + collision class
    uint16_t flags;              // bitmask: SOLID, ICE, DEADLY, ONE_WAY_TOP, BACKGROUND
} LvlTile;                        // 4 bytes per tile
```

Payload is `world_w_tiles * world_h_tiles` records, row-major. For a 100×60 map: 24 KB. For 200×100: 80 KB. The biggest single lump in any file.

`id` is a small int that maps to the tile sprite atlas (which resolves at runtime via `string_table_off` if maps end up needing per-map tile sets — at v1 we use one shared "industrial" sprite set, so `id` is the atlas index directly).

`flags` is the runtime collision flags. The TileKind enum in `world.h` becomes the low bits of `flags`; high bits are reserved. The current set:

```c
enum {
    TILE_F_EMPTY      = 0,
    TILE_F_SOLID      = 1u << 0,
    TILE_F_ICE        = 1u << 1,    // low friction
    TILE_F_DEADLY     = 1u << 2,    // 5 HP/s damage on contact
    TILE_F_ONE_WAY    = 1u << 3,    // pass-through from below
    TILE_F_BACKGROUND = 1u << 4,    // visual only, no collision
};
```

These match the level-design doc's polygon kinds; tiles get the same vocabulary so a designer can paint with them.

### POLY — free polygons (already triangulated)

```c
typedef struct {
    int16_t  v_x[3];             // triangle vertices, world-space px
    int16_t  v_y[3];
    int16_t  normal_x[3];        // edge normals, fixed-point Q1.15
    int16_t  normal_y[3];
    uint16_t kind;               // SOLID/ICE/DEADLY/ONE_WAY/BACKGROUND
    uint16_t group_id;           // for grouped destructibles (M5 reserves; not used)
    int16_t  bounce_q;           // restitution Q0.16: 0..65535 = 0..1
    uint16_t reserved;
} LvlPoly;                        // 32 bytes
```

The editor produces `LvlPoly` records directly: it triangulates the user's drawn polygons at save time (see [02-level-editor.md](02-level-editor.md) §"Polygon triangulation"). The runtime never re-triangulates — the cost is paid once at edit time. Edge normals are pre-baked too; the runtime normalizes-and-reuses.

A 200-polygon map is 6.4 KB. At [07-level-design.md](../07-level-design.md)'s 5000-polygon ceiling, 160 KB. Reasonable.

### SPWN — spawn points

```c
typedef struct {
    int16_t pos_x;               // world-space px
    int16_t pos_y;
    uint8_t team;                // 0 = any, 1 = red, 2 = blue
    uint8_t flags;               // PRIMARY (1), FALLBACK (2)
    uint8_t lane_hint;           // 0..255 — designer's intended lane order
    uint8_t reserved;
} LvlSpawn;                       // 8 bytes
```

Replaces the static lane tables in `src/maps.c::g_red_lanes`, etc. The spawn algorithm in `lobby_spawn_round_mechs` walks this array and picks one matching team + ≥800 px from the nearest enemy + not currently occupied.

`lane_hint` lets the designer say "use this one first" without sorting the array. The spawn algorithm sorts by `lane_hint` when picking among equally-eligible candidates.

In FFA mode all spawns are eligible regardless of `team` (team is treated as "preferred but not required"). In TDM/CTF, team is hard.

### PICK — pickup spawners

```c
typedef struct {
    int16_t  pos_x;
    int16_t  pos_y;
    uint8_t  category;           // PickupCategory: HEALTH/AMMO/ARMOR/WEAPON/POWERUP/JET_FUEL
    uint8_t  variant;            // small/med/large for HEALTH; weapon_id for WEAPON
    uint16_t respawn_ms;         // overrides category default; 0 = use default
    uint16_t flags;              // CONTESTED (1), RARE (2), HOST_ONLY (4)
    uint16_t reserved;
} LvlPickup;                      // 12 bytes
```

40 pickups per map = 480 bytes. Per-spawner override of respawn time (`respawn_ms != 0`) lets a map designer make a specific Mass Driver harder to hold. Flags reserved for future use; `RARE` was originally for power-ups but the category enum already covers them.

See [04-pickups.md](04-pickups.md) for category semantics.

### DECO — decoration sprites

```c
typedef struct {
    int16_t  pos_x;
    int16_t  pos_y;
    int16_t  scale_q;            // Q1.15 scale, default = 1.0 → 32768
    int16_t  rot_q;              // angle in 1/256 turns
    uint16_t sprite_str_idx;     // index into STRT — sprite asset path
    uint8_t  layer;              // 0 = far parallax, 1 = mid, 2 = near, 3 = foreground
    uint8_t  flags;              // FLIPPED_X, ADDITIVE
} LvlDeco;                        // 12 bytes
```

Decorations are visual-only: chain-link fences, pipes, hazard stripes, distant skyline silhouettes. They pass bullets (the foreground layer is `flags = ADDITIVE` for the dust/smoke kind that visually obscures sightlines without blocking). They're how a parallax background gets "glued" to a specific map without growing the file with a 4096×2048 PNG per map.

100 decorations = 1.2 KB. Most maps use 50–150.

### AMBI — ambient zones

```c
typedef struct {
    int16_t  rect_x, rect_y;     // top-left, world-space
    int16_t  rect_w, rect_h;
    uint16_t kind;               // AmbientKind: WIND/ZERO_G/ACID/FOG
    int16_t  strength_q;         // Q1.15 — kind-dependent magnitude
    int16_t  dir_x_q;            // Q1.15 unit vector — for WIND
    int16_t  dir_y_q;
    uint16_t reserved;
} LvlAmbi;                        // 16 bytes
```

Per [07-level-design.md](../07-level-design.md) §"Ambient zones": rare, designer-placed, never a constant gimmick. WIND pushes mechs sideways, ZERO_G turns off gravity, ACID does 5 HP/s, FOG is purely visual. M5 ships only these four kinds.

A 4-zone map is 64 bytes. Most maps have 0–2.

### FLAG — CTF flag bases

```c
typedef struct {
    int16_t  pos_x;
    int16_t  pos_y;
    uint8_t  team;               // 1 = red, 2 = blue
    uint8_t  reserved[3];
} LvlFlag;                        // 8 bytes
```

Two records per CTF map (one per team). Maps that aren't CTF-capable simply omit this lump or include zero records — see [06-ctf.md](06-ctf.md) for the runtime semantics.

### META — audio environment + map metadata

```c
typedef struct {
    uint16_t name_str_idx;            // STRT — display name ("Foundry")
    uint16_t blurb_str_idx;           // STRT — short description
    uint16_t background_str_idx;      // STRT — background PNG path (3-layer set baked under one name)
    uint16_t music_str_idx;           // STRT — OGG-Vorbis music track
    uint16_t ambient_loop_str_idx;    // STRT — ambient SFX loop
    uint16_t reverb_amount_q;         // Q0.16 — 0..1
    uint16_t mode_mask;               // bitmask of MatchModeId allowed (FFA=1, TDM=2, CTF=4)
    uint16_t reserved[8];
} LvlMeta;                             // 32 bytes
```

The map's "identity" — name, theme, audio, modes. `mode_mask` is enforced at server start (a host config that picks a CTF map without FLAG records gets a warning and the map is skipped from the rotation).

### STRT — string table

A single blob of `\0`-terminated UTF-8. Records reference strings by *byte offset from the start of STRT*. Identical to ELF's `.strtab` and Quake's `texinfo`.

A 64-byte header offset of 0 is reserved as the "empty string" — references to offset 0 are treated as "no string." This means **STRT always begins with `\0`**. The editor enforces this on save.

Strings are case-sensitive, no length cap (terminated by `\0`). Asset paths are relative to the binary's `assets/` directory. The audio module / renderer resolves them via `assets/<string>`.

Typical STRT for a map: 200–500 bytes (a dozen asset paths + the display name + blurb).

## CRC32 placement

We use a **single header CRC32** over the full file, computed with the `crc32` field zeroed. Loader recomputes and rejects on mismatch.

Per-lump CRC (PNG-style) was considered and rejected for v1: the per-lump check tells you *which* section corrupted, but the only consumer of that information would be the editor's "auto-recover" flow, which we don't ship. Single CRC is one write at save (seek back to header, write CRC) and one verify at load. Documented as a non-issue: if a level fails CRC, it fails CRC.

CRC32 polynomial is the standard one (0xEDB88320, table-based, ~150 LOC). We don't need cryptographic strength here — this is corruption detection, not signing.

## Validation

The loader runs five checks in this order, returning `ERR_LEVEL_CORRUPT` (or a more specific code) on any failure. The editor runs the same checks at *save* time, refusing to save a file that fails them — so a `.lvl` on disk is always loader-valid:

1. **Magic** — exact match `'S','D','L','V'`.
2. **Version** — `<= LVL_VERSION_CURRENT`. (Future versions won't downgrade.)
3. **CRC32** — recompute with crc field zero; reject mismatch.
4. **Lump directory bounds** — every `(offset, size)` lies fully within the file; no overlap. (We don't enforce contiguous packing; gaps are tolerated for compatibility but the editor doesn't produce them.)
5. **Per-section integrity** — known lumps' `size` is a multiple of their record size; world dims sane (1 ≤ w,h ≤ 512); polygon vertex coords inside `world_w_tiles * tile_size_px` ± 64 px (small slop for off-edge decoration); pickup categories valid; flag bases ≤ 2 per team.

A failure logs which check failed and at what offset, so the editor can show "fix at byte 0x4830" for a corrupted file.

## Loader API

```c
// src/level_io.h
typedef enum {
    LVL_OK = 0,
    LVL_ERR_FILE_NOT_FOUND,
    LVL_ERR_TOO_LARGE,           // > 1 MB hard cap
    LVL_ERR_BAD_MAGIC,
    LVL_ERR_BAD_VERSION,
    LVL_ERR_BAD_CRC,
    LVL_ERR_BAD_DIRECTORY,
    LVL_ERR_BAD_SECTION,
    LVL_ERR_OOM,
} LvlResult;

LvlResult level_load(struct World *world, struct Arena *level_arena,
                     const char *path);

// Editor-only — produces a `.lvl` byte-for-byte. world is read-only;
// arena is used for the encode buffer. The editor calls this with its
// own permanent arena, the game's loader does not call it.
LvlResult level_save(const struct World *world, struct Arena *scratch,
                     const char *path);
```

`level_load` is **the** map-load entry point at M5. The current `map_build` path becomes:

```c
// src/maps.c — runtime lookup + delegation
void map_build(MapId id, Level *level, Arena *arena) {
    const MapDef *def = map_def(id);
    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", def->short_name);
    LvlResult r = level_load(/*world*/&g->world, arena, path);
    if (r != LVL_OK) {
        LOG_E("map_build(%s): load failed (%d) — falling back to code-built",
              def->short_name, (int)r);
        // build_foundry / etc. as a Plan B for fresh checkouts that
        // haven't shipped the .lvl assets yet
        build_fallback(id, level, arena);
    }
}
```

This keeps the existing call sites unchanged. The "fallback" path stays in `maps.c` for the three M4 maps so a fresh checkout without `assets/maps/` still boots — it lights up a hard-coded Foundry. Once the .lvl assets ship, the fallback is dead code we delete in M6.

## Loader implementation strategy

The loader is one `level_io.c` file, ~600 LOC. The high-level shape:

1. `fopen` + `fread` the whole file into the `level_arena` (one allocation, capped at 1 MB).
2. Parse the header from the first 64 bytes (no allocation).
3. Build the lump directory lookup (walk `section_count` entries, populate a small `LvlLumpEntry` array on the stack).
4. Walk known lumps, copy their records into `World` fields. The tile grid copies into `world->level.tiles` (level arena); polygons into `world->level.polys` (new field, see [03-collision-polygons.md](03-collision-polygons.md)); spawns into `world->level.spawns`; pickups into `world->pickups.spawners` (new pool, see [04-pickups.md](04-pickups.md)); decorations + ambient + flag bases similarly.
5. Resolve string-table offsets into pointers into the (level-arena-owned) STRT blob. The blob *is* the storage for those strings — we never copy strings out. They live for the round, get freed when the level arena resets.

No malloc calls outside the arena. The original `fread` buffer stays alive as the level arena's first allocation; record arrays are second allocations sliced off the same arena. Total level memory at runtime: <100 KB even for the biggest map.

Endianness handling is centralized in three helpers:

```c
static inline uint16_t r_u16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t r_u32(const uint8_t *p) {
    return (uint32_t)p[0]       |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16)|
           ((uint32_t)p[3] << 24);
}
static inline int16_t  r_i16(const uint8_t *p) { return (int16_t)r_u16(p); }
```

Every record-decode goes through these. The editor uses symmetric `w_u16/w_u32/w_i16` for save.

## Versioning

`LVL_VERSION_CURRENT = 1` at M5 ship. Bump on any of:

- A record's size changes.
- A new field is added that the loader must parse (vs. a new lump that's safely skipped by old loaders).
- The header changes shape.

A future v2 might widen `LvlPoly` to carry per-vertex texture coords for the parallax foreground sprite layer. That's a v2 feature, not v1 — v1 ships with the schema as specified above and **we do not change it during M5** unless we discover a fatal flaw.

## Forward compat strategy

When v2 ships, old `.lvl v1` files load with a "fill in missing fields with defaults" pass:

- Missing FLAG section → no CTF support on this map.
- Missing META section → use `MapDef` defaults (display name = filename, no music, no parallax).
- Missing AMBI section → no ambient zones.

Any *required* lump (TILE, SPWN) missing is a hard error. The required set at v1 is `TILE` + `SPWN` + `META`. POLY/PICK/DECO/AMBI/FLAG are optional.

## Bake test for the format itself

Round-trip test in `tests/level_io_test.c`:

1. Build a synthetic `World` with one of every section populated.
2. `level_save` to a temp file.
3. `level_load` it back into a fresh `World`.
4. Bytewise-compare every section.
5. Bit-flip a random byte; loader should fail with `LVL_ERR_BAD_CRC`.
6. Truncate the file at random byte; loader should fail with `LVL_ERR_BAD_DIRECTORY` or `LVL_ERR_BAD_SECTION`.

Run via `make test-level-io`. CI runs this on every push.

## Done when

- `src/level_io.{c,h}` exists with the API above.
- `level_save` round-trips through `level_load` for a synthetic world (test passes in CI).
- All three M4 maps (Foundry / Slipstream / Reactor) have an `assets/maps/<name>.lvl` checked in, produced by a one-shot exporter from the existing `build_foundry` / `build_slipstream` / `build_reactor` paths.
- The runtime loads the `.lvl` files at match start; the M3 hard-coded fallback in `maps.c` is *retained* but unreached on a normal play session.
- A bit-flipped `.lvl` rejects with a clear log message; the host falls back to the next map in rotation.

## Trade-offs to log when shipping

- **No per-tile sprite override.** Every tile of a given `id` shares a sprite. If a designer wants two visually distinct floor patterns in one map, they pick two different `id`s. We don't ship a shader-side variant system at v1.
- **CRC32 over the full file, not per-lump.** Single check at load. Documented above.
- **No compression.** Files are well under 100 KB; gzip would shrink them by 3× and make the format un-greppable in hex. Skipped.
