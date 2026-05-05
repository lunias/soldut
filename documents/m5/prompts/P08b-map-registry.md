# P08b — Custom map registry: scan `assets/maps/` and surface user-authored maps in the lobby UI

## What this prompt does

Replaces the hardcoded `MapId` enum (4 entries: Foundry / Slipstream / Reactor / Crossfire) with a runtime **map registry** that includes:

1. The 4 code-built fallbacks (preserved).
2. **Every `assets/maps/*.lvl`** the host finds at startup, with display name + mode mask read from the file's META lump.

Result: a designer who saves `assets/maps/my_arena.lvl` from the editor (P04) sees `My Arena` in the host's lobby map-cycle on next launch, and friends who connect download it via the P08 protocol the same way they do for any other map. No protocol changes — `MapDescriptor` already carries `short_name + crc + size`, and the host's `match.map_id` is a host-local index that the client uses only as a display hint (the descriptor is the truth).

Resolves the "Custom map names not in lobby rotation (P08 follow-up)" trade-off (added at P08).

Independent of P09 (controls). Can run before or after.

## Required reading

1. `CLAUDE.md`
2. `documents/07-level-design.md` §"Map size targets" — the ≤500 KB / ≤40 pickups budgets the registry should validate against
3. `documents/m5/01-lvl-format.md` — header + META lump layout (so we can read display_name + mode_mask without level_load)
4. `documents/m5/10-map-sharing.md` — confirms the wire is content-addressed (CRC), not name-addressed; the registry is host-local
5. `TRADE_OFFS.md` — entry "Custom map names not in lobby rotation (P08 follow-up)"
6. `src/maps.{c,h}` — `MapId` enum, `g_maps[]`, `map_def`, `map_id_from_name`, `map_build`, `maps_refresh_serve_info`, `map_build_for_descriptor`
7. `src/config.{c,h}` — `map_rotation[]` parsing + `config_pick_map`
8. `src/lobby_ui.c` — `host_setup_screen_run` map cycle (line ~190–230), in-lobby map cycle (line ~853–935)
9. `src/level_io.{c,h}` — header offsets (especially `HDR_OFF_CRC32 = 36`, `HDR_OFF_STRT_OFF = 28`, `HDR_OFF_STRT_SIZE = 32`) so the registry scan can pluck CRC + meta cheaply without a full `level_load`
10. `src/map_cache.{c,h}` — `map_cache_file_crc` / `map_cache_file_size` shape; the registry scan is the same flavor of lightweight probe
11. `src/main.c::bootstrap_host`, `bootstrap_client` — call sites for the registry init

## Background

P08 shipped the wire to **stream** any `.lvl` from server to client. But the host's lobby UI still picks maps from a 4-entry enum baked at compile time:

```c
// src/maps.h — current
typedef enum {
    MAP_FOUNDRY = 0,
    MAP_SLIPSTREAM,
    MAP_REACTOR,
    MAP_CROSSFIRE,
    MAP_COUNT
} MapId;
```

A designer's `assets/maps/my_arena.lvl` can only get into a real match by **overwriting** one of those four reserved slots (e.g., saving as `foundry.lvl`). That's the workaround `tests/net/run_map_share.sh` and `tests/shots/net/run_meet_custom.sh` use today. It's hostile to anyone who actually wants to keep Foundry **and** play a custom map.

The fix is small: build a runtime registry at process start, populate it with the code-built defaults plus every `.lvl` we find on disk, point everything that previously walked `0..MAP_COUNT` at the registry instead. Wire format is unchanged — `MapDescriptor` already round-trips `short_name + crc + size`, and `match.map_id` is a u8 hint that a client uses only when looking up a display name (and falls back to `pending_map.short_name` when the index is out of its local registry's range).

## Concrete tasks

### Task 1 — runtime `MapRegistry` in `src/maps.{c,h}`

Replace the file-scope `g_maps[MAP_COUNT]` table with a runtime registry. The `MapId` enum stays (the four reserved indices are load-bearing for code paths like `build_fallback`'s switch), but `MAP_COUNT` becomes `MAP_BUILTIN_COUNT` and a new constant `MAP_REGISTRY_MAX` caps the runtime list.

```c
// src/maps.h
typedef enum {
    MAP_FOUNDRY = 0,
    MAP_SLIPSTREAM,
    MAP_REACTOR,
    MAP_CROSSFIRE,
    MAP_BUILTIN_COUNT,    /* = 4; reserved indices for code-built fallbacks */
} MapId;

#define MAP_REGISTRY_MAX 32       /* hard cap; bumps live in TRADE_OFFS */

typedef struct MapDef {
    int        id;                /* matches index into g_map_registry.entries */
    char       short_name[24];    /* filename stem, lowercased, ASCII */
    char       display_name[32];  /* META display, or short_name title-cased */
    char       blurb[64];         /* META blurb, or "" */
    int        tile_w, tile_h;    /* from .lvl header (or code-built default) */
    uint16_t   mode_mask;         /* FFA=1, TDM=2, CTF=4 */
    bool       has_lvl_on_disk;   /* false → code-built fallback only */
    uint32_t   file_crc;          /* 0 if no .lvl on disk */
    uint32_t   file_size;         /* 0 if no .lvl on disk */
} MapDef;

typedef struct MapRegistry {
    MapDef entries[MAP_REGISTRY_MAX];
    int    count;
} MapRegistry;

/* Process-global registry. Built once in game_init via
 * map_registry_init; subsequent module calls (map_def, map_id_from_name,
 * config_pick_map, lobby UI cycle) read from this. */
extern MapRegistry g_map_registry;

/* Populate registry: 4 code-built defaults first, then scan
 * assets/maps/*.lvl and append (or override-by-short-name) every file
 * found. Idempotent — safe to call multiple times for hot-reload-style
 * "the editor just saved a file" rescans (P08b ships startup-only;
 * rescan trigger is a future task). */
void map_registry_init(void);
```

The four code-built entries seed the registry with `mode_mask = FFA|TDM` (Foundry / Slipstream / Reactor) or `FFA|TDM|CTF` (Crossfire) so the registry has correct mode-affinity even on a fresh checkout with no `.lvl` files.

`map_def(int id)` returns `&g_map_registry.entries[id]` (clamped to `[0, count)`). `map_id_from_name(const char *name)` walks the registry comparing `short_name` (case-insensitive) and falls back to `display_name`.

### Task 2 — disk scan that reads META without `level_load`

The registry needs `display_name + mode_mask` for every `.lvl`, **without** the cost of a full `level_load` per entry. The scan reads the 64-byte header, walks the lump directory to find META, reads the 32-byte META record, and looks up the display-name string-table offset.

```c
/* src/maps.c — internal helper */
static bool scan_one_lvl(const char *path, MapDef *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    /* Header: 64 bytes. We need world_w/h (offsets 16/20), tile_size (24),
     * STRT offset (28) + size (32), CRC (36), section count (8). */
    uint8_t hdr[64];
    if (fread(hdr, 1, 64, f) != 64) { fclose(f); return false; }
    if (hdr[0] != 'S' || hdr[1] != 'D' || hdr[2] != 'L' || hdr[3] != 'V') {
        fclose(f); return false;
    }
    /* Walk directory looking for META; pluck its offset + size. */
    uint32_t section_count = read_u32_le(hdr + 8);
    uint32_t strt_off      = read_u32_le(hdr + 28);
    uint32_t strt_size     = read_u32_le(hdr + 32);
    uint32_t crc           = read_u32_le(hdr + 36);

    int meta_off = -1;
    for (uint32_t i = 0; i < section_count; ++i) {
        uint8_t e[16];
        if (fseek(f, 64 + i * 16, SEEK_SET) != 0) { fclose(f); return false; }
        if (fread(e, 1, 16, f) != 16) { fclose(f); return false; }
        if (e[0]=='M' && e[1]=='E' && e[2]=='T' && e[3]=='A') {
            meta_off = (int)read_u32_le(e + 8);
            break;
        }
    }

    /* META is 32 bytes: 7 × u16 (name, blurb, bg, music, ambient,
     * reverb, mode_mask) + 9 × u16 reserved. We need name_str_idx
     * (offset 0) and mode_mask (offset 12). */
    uint16_t name_idx  = 0;
    uint16_t mode_mask = 0;
    if (meta_off > 0) {
        uint8_t meta[32];
        if (fseek(f, meta_off, SEEK_SET) == 0 && fread(meta, 1, 32, f) == 32) {
            name_idx  = read_u16_le(meta + 0);
            mode_mask = read_u16_le(meta + 12);
        }
    }

    /* Resolve display name from string table. STRT is a packed
     * sequence of null-terminated strings starting at byte 0. */
    char display[32] = {0};
    if (name_idx > 0 && strt_size > 0 && (uint32_t)name_idx < strt_size) {
        if (fseek(f, strt_off + name_idx, SEEK_SET) == 0) {
            fread(display, 1, sizeof display - 1, f);
            display[sizeof display - 1] = '\0';
        }
    }

    /* Stat for size. */
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fclose(f);

    /* Fill out. */
    /* short_name = filename stem, lowercased */
    derive_short_name_from_path(path, out->short_name, sizeof out->short_name);
    snprintf(out->display_name, sizeof out->display_name, "%s",
             display[0] ? display : titlecase(out->short_name));
    out->has_lvl_on_disk = true;
    out->file_crc        = crc;
    out->file_size       = (flen > 0) ? (uint32_t)flen : 0u;
    out->tile_w          = (int)read_u32_le(hdr + 16);
    out->tile_h          = (int)read_u32_le(hdr + 20);
    out->mode_mask       = mode_mask ? mode_mask
                                     : (uint16_t)(1u << MATCH_MODE_FFA |
                                                  1u << MATCH_MODE_TDM);
    out->blurb[0] = '\0';
    return true;
}
```

`map_registry_init` flow:

```c
void map_registry_init(void) {
    g_map_registry.count = 0;

    /* 1. Seed with code-built defaults. */
    static const MapDef builtins[MAP_BUILTIN_COUNT] = {
        { .id = MAP_FOUNDRY,    .short_name = "foundry",    .display_name = "Foundry",
          .blurb = "Open floor with cover columns. Ground-game.", .tile_w = 100, .tile_h = 40,
          .mode_mask = (1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM) },
        { .id = MAP_SLIPSTREAM, .short_name = "slipstream", .display_name = "Slipstream",
          .blurb = "Stacked catwalks. Vertical jet beats.", .tile_w = 100, .tile_h = 50,
          .mode_mask = (1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM) },
        { .id = MAP_REACTOR,    .short_name = "reactor",    .display_name = "Reactor",
          .blurb = "Central pillar, two flanking platforms.", .tile_w = 110, .tile_h = 42,
          .mode_mask = (1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM) },
        { .id = MAP_CROSSFIRE,  .short_name = "crossfire",  .display_name = "Crossfire",
          .blurb = "Symmetric CTF arena. Two team bases, central run.",
          .tile_w = 140, .tile_h = 42,
          .mode_mask = (1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM) | (1u << MATCH_MODE_CTF) },
    };
    for (int i = 0; i < MAP_BUILTIN_COUNT; ++i) {
        g_map_registry.entries[i] = builtins[i];
        g_map_registry.entries[i].id = i;
    }
    g_map_registry.count = MAP_BUILTIN_COUNT;

    /* 2. Scan assets/maps/*.lvl. For files matching a builtin's short
     * name, OVERRIDE the builtin entry with disk metadata (display
     * name from META, real CRC + size, real mode_mask). For files
     * with new names, APPEND. */
    DIR *d = opendir("assets/maps");
    if (!d) {
        LOG_I("map_registry: no assets/maps dir; using %d builtins only",
              MAP_BUILTIN_COUNT);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t L = strlen(de->d_name);
        if (L < 5 || strcmp(de->d_name + L - 4, ".lvl") != 0) continue;

        char path[512];
        snprintf(path, sizeof path, "assets/maps/%s", de->d_name);

        MapDef tmp = {0};
        if (!scan_one_lvl(path, &tmp)) {
            LOG_W("map_registry: skipping unreadable %s", path);
            continue;
        }
        /* Match against existing entries (overrides a builtin). */
        int match = -1;
        for (int i = 0; i < g_map_registry.count; ++i) {
            if (strcasecmp(g_map_registry.entries[i].short_name, tmp.short_name) == 0) {
                match = i; break;
            }
        }
        if (match >= 0) {
            tmp.id = match;
            g_map_registry.entries[match] = tmp;
            LOG_I("map_registry: %s overrides builtin (crc=%08x, %u bytes)",
                  tmp.short_name, (unsigned)tmp.file_crc, (unsigned)tmp.file_size);
        } else if (g_map_registry.count >= MAP_REGISTRY_MAX) {
            LOG_W("map_registry: cap %d reached, skipping %s",
                  MAP_REGISTRY_MAX, tmp.short_name);
        } else {
            tmp.id = g_map_registry.count;
            g_map_registry.entries[g_map_registry.count++] = tmp;
            LOG_I("map_registry: + %s (crc=%08x, %u bytes, mode_mask=0x%x)",
                  tmp.short_name, (unsigned)tmp.file_crc,
                  (unsigned)tmp.file_size, (unsigned)tmp.mode_mask);
        }
    }
    closedir(d);

    LOG_I("map_registry: %d entries total (%d builtins + %d custom)",
          g_map_registry.count, MAP_BUILTIN_COUNT,
          g_map_registry.count - MAP_BUILTIN_COUNT);
}
```

Wire the call in `game_init` (after arenas are up; the registry lives in static storage, no allocation needed). Both host and client run it — client uses it for display fallback when the host advertises a map the client also has locally.

### Task 3 — replace every `MAP_COUNT` walk with `g_map_registry.count`

Sites:
- `src/lobby_ui.c::setup_next_map_for_mode` — `for (int step = 1; step <= MAP_COUNT; ++step)` → `... <= g_map_registry.count`, and `(cur + step) % MAP_COUNT` → `% g_map_registry.count`.
- `src/lobby_ui.c::lobby_screen_run` (in-lobby mode change auto-skip + map cycle button, lines ~860 and ~915) — same substitution.
- `src/main.c::start_round` — the CTF mode-mask validation walks `for (int step = 1; step <= MAP_COUNT; ++step)`. Same substitution.
- Anywhere else that iterates the enum range; one grep `grep -rn "MAP_COUNT" src/`.

The 4 named constants (`MAP_FOUNDRY` etc.) stay; `build_fallback`'s switch stays as-is — it dispatches the four code-built builders for indices 0..3.

### Task 4 — `map_id_from_name` searches the runtime registry

Replace the static-array walk:

```c
int map_id_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < g_map_registry.count; ++i) {
        const MapDef *d = &g_map_registry.entries[i];
        if (strcasecmp(name, d->short_name)   == 0) return i;
        if (strcasecmp(name, d->display_name) == 0) return i;
    }
    return -1;
}
```

`config_pick_map` is unchanged — it returns `cfg->map_rotation[round_index % cfg->map_rotation_count]`, which is now a registry index in `[0, g_map_registry.count)`.

### Task 5 — `map_build` for non-builtin entries

`map_build(MapId id, World *, Arena *)` currently builds the path as `assets/maps/<short>.lvl`, tries `level_load`, and falls back to `build_fallback(id, ...)` on failure. With registry IDs > `MAP_BUILTIN_COUNT`, `build_fallback`'s switch hits the `default` arm (Foundry). That's wrong — for a custom map that mysteriously fails to load (CRC mismatch, truncation), we'd silently boot players into Foundry without telling them.

Add a "no fallback for custom maps" branch:

```c
void map_build(MapId id, World *world, Arena *arena) {
    const MapDef *def = map_def(id);
    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", def->short_name);
    LvlResult r = level_load(world, arena, path);
    if (r == LVL_OK) return;

    if (id < MAP_BUILTIN_COUNT) {
        if (r == LVL_ERR_FILE_NOT_FOUND) {
            LOG_I("map_build(%s): no .lvl on disk — using code-built fallback",
                  def->short_name);
        } else {
            LOG_W("map_build(%s): level_load failed (%s) — using code-built fallback",
                  def->short_name, level_io_result_str(r));
        }
        build_fallback((MapId)id, &world->level, arena);
    } else {
        /* Custom map registered at startup but can't be loaded now —
         * file deleted or corrupted between scan and play. Hard fail
         * to Foundry's code-built. */
        LOG_E("map_build(%s): custom map's .lvl unavailable (%s) — falling back to Foundry",
              def->short_name, level_io_result_str(r));
        build_fallback(MAP_FOUNDRY, &world->level, arena);
    }
}
```

### Task 6 — host-setup screen layout doesn't break with >4 entries

The host-setup screen's current map cycle button (`host_setup_screen_run` in `src/lobby_ui.c`) just shows the current pick and advances on click — no rendering changes needed; it works for any `g_map_registry.count`. Same for the in-lobby map cycle button.

The "vote three random maps" picker (P09's task — separate prompt) becomes more interesting with custom maps in the pool: it'll randomly draw 3 from the registry. P09 already accepts `g_map_registry.count` parameterization once this lands.

### Task 7 — client-side display when the host's index is out of range

When the host advertises `match.map_id = 5` (their 6th entry, a custom map) and the client's local registry has only 4 entries, `map_def(5)` would clamp to entry 0 and the client UI would read "Foundry." Fix:

```c
// src/lobby_ui.c — wherever match.map_id is rendered
const char *map_label = (g->match.map_id < g_map_registry.count)
                        ? g_map_registry.entries[g->match.map_id].display_name
                        : (g->pending_map.short_name[0]
                           ? g->pending_map.short_name
                           : "(custom)");
```

`g->pending_map.short_name` is filled by INITIAL_STATE / NET_MSG_MAP_DESCRIPTOR (see P08). The client now displays the host's actual map name even when its local registry doesn't contain it.

### Task 8 — `tests/map_registry_test.c` unit test

```c
// Builds a tmp dir with a few synthetic .lvl files (via the existing
// synth_map flow), points the registry init at it (factor out a
// `map_registry_init_from(const char *dir)` for testability), and
// asserts:
//   - 4 builtins always present
//   - synthesized "my_arena.lvl" appears as a 5th entry with crc + size populated
//   - synthesized "foundry.lvl" with custom CRC OVERRIDES builtin (crc differs from 0)
//   - duplicate-named files don't double-add (last write wins)
//   - cap of 32 honored
//   - a malformed .lvl (bad magic) is skipped without crashing
```

Wire as `make test-map-registry` alongside the existing test targets.

### Task 9 — end-to-end shot-test extension

Extend `tests/shots/net/run_meet_custom.sh` (or write a sibling `run_meet_named.sh`) that:

1. Editor shot writes `<host_dir>/assets/maps/my_arena.lvl` (NOT overwriting `foundry.lvl`).
2. Host's `soldut.cfg` carries `map_rotation=my_arena`.
3. Host log asserts `map_registry: + my_arena` and `match: round begin (... map=4 ...)` (or whichever index `my_arena` got).
4. Client's `pending_map.short_name == "my_arena"`, downloads, plays.
5. Both walk + meet, screenshots from each side.

This proves the full editor-to-multiplayer round-trip works with a non-reserved map name.

## Done when

- `make` clean.
- `g_map_registry` populates correctly: 4 builtins on a fresh checkout, +1 per `assets/maps/*.lvl` file present (without overwriting builtin behavior).
- A user saving `assets/maps/my_arena.lvl` from the editor, then launching `./soldut --host`, sees "My Arena" in the lobby's map cycle on the next click of the cycle button.
- `make test-map-registry` passes (unit tests on registry shape).
- `make test-meet-custom` (or new `make test-meet-named`) passes end-to-end with a non-reserved map name.
- The host-setup screen + in-lobby map cycle iterate `g_map_registry.count`, not `MAP_COUNT`.
- Client UI shows the right name even when the host advertises a registry index the client doesn't know.
- TRADE_OFFS entry "Custom map names not in lobby rotation (P08 follow-up)" is **deleted**.

## Out of scope

- **Hot-rescan when the editor saves**: the registry init runs once at process start. F5 from the editor (P04) already restarts the game with `--test-play`, so the new file is picked up by the next `map_registry_init`. Live "the running host noticed I just saved a new map" is a future task.
- **Map preview thumbnails**: P13's art pass renders thumbnails. The registry has the path; thumbnail loading hangs off it.
- **Server-side filename → CRC manifest**: optional optimization to cache the scan; the current scan is sub-millisecond per file at our scale.
- **Map sub-directories**: `assets/maps/` is flat. Sub-dirs (e.g. `assets/maps/community/foo.lvl`) are out of scope — designers can prefix the filename instead.
- **Removing maps from disk while the host is running**: undefined; the registry has stale entries until restart. Don't catch fire (`map_build` already errors gracefully); leave a TRADE_OFFS entry if it's a real concern.

## How to verify

```bash
make
make test-map-registry          # new unit test
make test-meet-custom           # existing — proves overwrite path still works
make test-meet-named            # new — proves non-reserved name path works
./tests/net/run_map_share.sh    # existing P08 test must still pass
./tests/net/run.sh              # existing — unchanged behavior on builtin maps
```

Manual:

```bash
make && make editor
./build/soldut_editor   # save assets/maps/my_arena.lvl
./soldut --host         # lobby map cycle should include "my_arena"
# Friend connects; their client downloads my_arena via P08;
# round runs; screenshots show both players on the same arena.
```

## Close-out

1. Update `CURRENT_STATE.md`: P08b — custom map registry shipped; lobby UI cycles arbitrary `assets/maps/*.lvl` files.
2. Update `TRADE_OFFS.md`:
   - **Delete** "Custom map names not in lobby rotation (P08 follow-up)".
3. Don't commit unless explicitly asked.

## Common pitfalls

- **`MAP_COUNT` removal misses one site**: grep before declaring done. A leftover `MAP_COUNT` references the now-removed enum value and the build breaks.
- **`build_fallback` switch covers only the 4 builtins**: that's correct. Custom map IDs (>= 4) hit `map_build`'s explicit no-fallback branch (Task 5). Don't try to extend `build_fallback` to fabricate a level for an unknown short_name.
- **Filename casing**: `derive_short_name_from_path` lowercases everything. `assets/maps/MyArena.lvl` and `assets/maps/myarena.lvl` would collide. Document; let the second-encountered win (deterministic = first by `readdir` order, which is unspecified).
- **Empty display name from META**: META's name_str_idx is 0 by default → empty string. Fall back to `titlecase(short_name)` — `my_arena` → `My_arena` (or split on `_`/`-` to `My Arena`; either reads OK).
- **Registry init order**: must run AFTER `arena_init` (so `LOG_I` is safe) but BEFORE `bootstrap_host` / `bootstrap_client`'s `map_build` calls. Insert in `game_init` immediately before the closing `LOG_I("game_init: ok ...")`.
- **Both host and client init the registry**: yes. Client uses it for display name lookups. The two registries can legitimately differ (different `assets/maps/` contents); the wire's descriptor is the source of truth for what's actually being played.
- **`config.c::config_load` warns on unknown map names**: with the registry populated AT THE TIME the config parses, a `map_rotation=my_arena` in `soldut.cfg` resolves correctly. Init order matters: registry → config → bootstrap. Verify in `main.c`.
