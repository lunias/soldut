#include "maps.h"

#include "arena.h"
#include "level.h"
#include "level_io.h"
#include "log.h"
#include "map_cache.h"  /* P08 — file CRC + size probes for serve-info */
#include "match.h"
#include "mech.h"     /* ArmorId values for default-map pickup variants */
#include "net.h"      /* MapDescriptor */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TILE_PX 32

/* Process-global runtime registry. Populated by map_registry_init from
 * the four code-built defaults plus every `assets/maps/<name>.lvl` on
 * disk. */
MapRegistry g_map_registry = {0};

const MapDef *map_def(int id) {
    if (id < 0 || id >= g_map_registry.count) {
        /* Out-of-range — clamp to entry 0 (Foundry). Callers that need
         * to detect the out-of-range case (e.g. client display fallback
         * to the host's pending_map.short_name) should check the index
         * against g_map_registry.count themselves before calling. */
        return &g_map_registry.entries[0];
    }
    return &g_map_registry.entries[id];
}

int map_id_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < g_map_registry.count; ++i) {
        const MapDef *d = &g_map_registry.entries[i];
        if (d->short_name  [0] && strcasecmp(name, d->short_name)   == 0) return i;
        if (d->display_name[0] && strcasecmp(name, d->display_name) == 0) return i;
    }
    return -1;
}

/* ---- P08b registry init: scan a directory for .lvl, populate g_map_registry --- */

/* Tiny endian-explicit readers — same as level_io.c. Duplicated here so
 * we don't widen level_io.h's surface for one helper used by one file. */
static uint16_t u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] <<  8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Filename stem (no extension) → lowercased into out. */
static void derive_short_name_from_path(const char *path, char *out,
                                        size_t out_cap) {
    if (!path || !out || out_cap == 0) {
        if (out && out_cap) out[0] = '\0';
        return;
    }
    const char *base = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    size_t i = 0;
    while (base[i] && base[i] != '.' && i + 1 < out_cap) {
        unsigned char c = (unsigned char)base[i];
        out[i] = (char)tolower(c);
        ++i;
    }
    out[i] = '\0';
}

/* "my_arena" → "My Arena". Used as the display fallback when META has
 * no name string. Splits on '_' / '-' / ' '; preserves the rest. */
static void titlecase_short_name(const char *src, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return;
    if (!src || !src[0]) { out[0] = '\0'; return; }
    size_t i = 0;
    int at_word_start = 1;
    while (src[i] && i + 1 < out_cap) {
        char c = src[i];
        if (c == '_' || c == '-') {
            out[i] = ' ';
            at_word_start = 1;
        } else if (at_word_start) {
            out[i] = (char)toupper((unsigned char)c);
            at_word_start = 0;
        } else {
            out[i] = c;
        }
        ++i;
    }
    out[i] = '\0';
}

/* Read a .lvl's header + META lump cheaply (no level_load). On success
 * fills `out` with short_name / display_name / mode_mask / crc / size /
 * tile_w / tile_h. Returns true if the file looked like a valid .lvl
 * header (magic OK + parsable directory). */
static bool scan_one_lvl(const char *path, MapDef *out) {
    if (!path || !out) return false;
    memset(out, 0, sizeof *out);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    /* Header — 64 bytes. Offsets per src/level_io.c. */
    uint8_t hdr[64];
    if (fread(hdr, 1, 64, f) != 64) { fclose(f); return false; }
    if (hdr[0] != 'S' || hdr[1] != 'D' ||
        hdr[2] != 'L' || hdr[3] != 'V') { fclose(f); return false; }

    uint32_t section_count = u32_le(hdr + 8);
    uint32_t world_w       = u32_le(hdr + 16);
    uint32_t world_h       = u32_le(hdr + 20);
    uint32_t strt_off      = u32_le(hdr + 28);
    uint32_t strt_size     = u32_le(hdr + 32);
    uint32_t crc           = u32_le(hdr + 36);

    /* Walk the directory looking for META. Each entry is 16 bytes:
     * 4-byte tag + 4-byte reserved + 4-byte offset + 4-byte size. */
    int meta_off = -1;
    int meta_sz  = 0;
    for (uint32_t i = 0; i < section_count && i < 64u; ++i) {
        uint8_t e[16];
        if (fseek(f, 64 + (long)i * 16, SEEK_SET) != 0) { fclose(f); return false; }
        if (fread(e, 1, 16, f) != 16) { fclose(f); return false; }
        if (e[0]=='M' && e[1]=='E' && e[2]=='T' && e[3]=='A') {
            meta_off = (int)u32_le(e + 8);
            meta_sz  = (int)u32_le(e + 12);
            break;
        }
    }

    /* META is 32 bytes: name_str_idx (u16, off 0), mode_mask (u16, off 12).
     * If META is missing or wrong-sized, fall back to mode_mask=FFA|TDM
     * (the safe code-built default — better than 0, which would lock the
     * map out of every mode). */
    uint16_t name_idx  = 0;
    uint16_t mode_mask = 0;
    if (meta_off > 0 && meta_sz == 32) {
        uint8_t meta[32];
        if (fseek(f, meta_off, SEEK_SET) == 0 && fread(meta, 1, 32, f) == 32) {
            name_idx  = u16_le(meta + 0);
            mode_mask = u16_le(meta + 12);
        }
    }

    /* Resolve display name from string table (STRT lump). The string
     * table is a packed sequence of null-terminated strings starting at
     * byte 0 (offset 0 = "empty string"). */
    char display[32] = {0};
    if (name_idx > 0 && strt_size > 0 && (uint32_t)name_idx < strt_size) {
        if (fseek(f, (long)strt_off + name_idx, SEEK_SET) == 0) {
            size_t n = fread(display, 1, sizeof display - 1, f);
            (void)n;
            display[sizeof display - 1] = '\0';
            /* Strings can run up to the next null; clip there. */
            for (size_t k = 0; k < sizeof display; ++k) {
                if (display[k] == '\0') break;
            }
        }
    }

    /* File size for the descriptor. */
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fclose(f);

    /* short_name from filename stem. */
    derive_short_name_from_path(path, out->short_name, sizeof out->short_name);

    /* display_name: META string if present, else titlecased short_name. */
    if (display[0]) {
        snprintf(out->display_name, sizeof out->display_name, "%s", display);
    } else {
        titlecase_short_name(out->short_name,
                             out->display_name, sizeof out->display_name);
    }

    out->blurb[0]        = '\0';
    out->tile_w          = (int)world_w;
    out->tile_h          = (int)world_h;
    out->mode_mask       = mode_mask
                           ? mode_mask
                           : (uint16_t)((1u << MATCH_MODE_FFA) |
                                        (1u << MATCH_MODE_TDM));
    out->has_lvl_on_disk = true;
    out->file_crc        = crc;
    out->file_size       = (flen > 0) ? (uint32_t)flen : 0u;
    return true;
}

static void seed_builtins(void) {
    /* Reserved indices: MAP_FOUNDRY (0) .. MAP_CROSSFIRE (3). Names +
     * blurbs match the M4 hand-authored versions; tile dims match the
     * code-built fallbacks. mode_mask values are the same the
     * `build_*` functions stamp at build time. */
    static const struct {
        const char *short_name;
        const char *display_name;
        const char *blurb;
        int         tile_w, tile_h;
        uint16_t    mode_mask;
    } seed[MAP_BUILTIN_COUNT] = {
        [MAP_FOUNDRY]    = { "foundry",    "Foundry",
                             "Open floor with cover columns. Ground-game.",
                             100, 40,
                             (1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM) },
        [MAP_SLIPSTREAM] = { "slipstream", "Slipstream",
                             "Stacked catwalks. Vertical jet beats.",
                             100, 50,
                             (1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM) },
        [MAP_REACTOR]    = { "reactor",    "Reactor",
                             "Central pillar, two flanking platforms.",
                             110, 42,
                             (1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM) },
        [MAP_CROSSFIRE]  = { "crossfire",  "Crossfire",
                             "Symmetric CTF arena. Two team bases, central run.",
                             140, 42,
                             (1u << MATCH_MODE_FFA) |
                             (1u << MATCH_MODE_TDM) |
                             (1u << MATCH_MODE_CTF) },
    };
    for (int i = 0; i < MAP_BUILTIN_COUNT; ++i) {
        MapDef *e = &g_map_registry.entries[i];
        memset(e, 0, sizeof *e);
        e->id = i;
        snprintf(e->short_name,   sizeof e->short_name,   "%s", seed[i].short_name);
        snprintf(e->display_name, sizeof e->display_name, "%s", seed[i].display_name);
        snprintf(e->blurb,        sizeof e->blurb,        "%s", seed[i].blurb);
        e->tile_w          = seed[i].tile_w;
        e->tile_h          = seed[i].tile_h;
        e->mode_mask       = seed[i].mode_mask;
        e->has_lvl_on_disk = false;
        e->file_crc        = 0;
        e->file_size       = 0;
    }
    g_map_registry.count = MAP_BUILTIN_COUNT;
}

void map_registry_init_from(const char *maps_dir) {
    seed_builtins();

    if (!maps_dir || !maps_dir[0]) {
        LOG_I("map_registry: no scan dir; %d builtins only",
              MAP_BUILTIN_COUNT);
        return;
    }

    DIR *d = opendir(maps_dir);
    if (!d) {
        LOG_I("map_registry: %s not present; %d builtins only",
              maps_dir, MAP_BUILTIN_COUNT);
        return;
    }

    int added = 0;
    int overrides = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t L = strlen(de->d_name);
        if (L < 5) continue;
        if (strcasecmp(de->d_name + L - 4, ".lvl") != 0) continue;

        char path[512];
        snprintf(path, sizeof path, "%s/%s", maps_dir, de->d_name);

        MapDef tmp;
        if (!scan_one_lvl(path, &tmp)) {
            LOG_W("map_registry: skipping unreadable %s", path);
            continue;
        }
        if (!tmp.short_name[0]) {
            LOG_W("map_registry: %s has empty short_name; skipping", path);
            continue;
        }

        /* Match against existing entries. A disk file with a builtin's
         * short_name overrides the builtin's CRC + size + mode_mask
         * (so map-share streams the on-disk file and modes reflect
         * what the file actually supports). */
        int match = -1;
        for (int i = 0; i < g_map_registry.count; ++i) {
            if (strcasecmp(g_map_registry.entries[i].short_name,
                           tmp.short_name) == 0) {
                match = i; break;
            }
        }
        if (match >= 0) {
            tmp.id = match;
            /* Preserve the builtin's blurb if the .lvl doesn't carry
             * one (P08b doesn't read META blurb_str_idx). */
            if (!tmp.blurb[0]) {
                snprintf(tmp.blurb, sizeof tmp.blurb, "%s",
                         g_map_registry.entries[match].blurb);
            }
            g_map_registry.entries[match] = tmp;
            ++overrides;
            LOG_I("map_registry: %s overrides builtin (crc=%08x, %u bytes)",
                  tmp.short_name, (unsigned)tmp.file_crc,
                  (unsigned)tmp.file_size);
        } else if (g_map_registry.count >= MAP_REGISTRY_MAX) {
            LOG_W("map_registry: cap %d reached, skipping %s",
                  MAP_REGISTRY_MAX, tmp.short_name);
        } else {
            tmp.id = g_map_registry.count;
            g_map_registry.entries[g_map_registry.count++] = tmp;
            ++added;
            LOG_I("map_registry: + %s (crc=%08x, %u bytes, mode_mask=0x%x)",
                  tmp.short_name, (unsigned)tmp.file_crc,
                  (unsigned)tmp.file_size, (unsigned)tmp.mode_mask);
        }
    }
    closedir(d);

    LOG_I("map_registry: %d entries (%d builtins, %d custom, %d overrides)",
          g_map_registry.count, MAP_BUILTIN_COUNT, added, overrides);
}

void map_registry_init(void) {
    map_registry_init_from("assets/maps");
}

static void set_tile(Level *l, int x, int y, TileKind k) {
    if (x < 0 || x >= l->width || y < 0 || y >= l->height) return;
    uint16_t f = (k == TILE_SOLID) ? TILE_F_SOLID : TILE_F_EMPTY;
    l->tiles[y * l->width + x] = (LvlTile){ .id = 0, .flags = f };
}

static void fill_rect(Level *l, int x0, int y0, int x1, int y1, TileKind k) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            set_tile(l, x, y, k);
}

/* Allocate a fixed-size pickup array on first use; subsequent calls
 * append (no-op if the cap is reached). Code-built maps hand-author
 * spawner positions so the user has something to grab in normal play
 * before P17 ships authored .lvl maps. Capacity 16 covers what fits
 * comfortably on the small M4 maps. */
#define MAP_PICKUP_FALLBACK_CAP 16
static void add_pickup_to_map(Level *L, Arena *arena, int wx, int wy,
                              uint8_t kind, uint8_t variant) {
    if (!L->pickups) {
        L->pickups = (LvlPickup *)arena_alloc(arena,
            sizeof(LvlPickup) * MAP_PICKUP_FALLBACK_CAP);
        if (!L->pickups) return;
        L->pickup_count = 0;
    }
    if (L->pickup_count >= MAP_PICKUP_FALLBACK_CAP) return;
    L->pickups[L->pickup_count++] = (LvlPickup){
        .pos_x      = (int16_t)wx,
        .pos_y      = (int16_t)wy,
        .category   = kind,
        .variant    = variant,
        .respawn_ms = 0,           /* use kind default */
        .flags      = 0,
        .reserved   = 0,
    };
}

static void map_alloc_tiles(Level *level, Arena *arena, int w, int h) {
    level->width     = w;
    level->height    = h;
    level->tile_size = TILE_PX;
    level->gravity   = (Vec2){0.0f, 1080.0f};
    int n = w * h;
    level->tiles = (LvlTile *)arena_alloc(arena, sizeof(LvlTile) * (size_t)n);
    if (!level->tiles) {
        LOG_E("map_alloc_tiles: arena out of memory (%dx%d=%zu bytes)",
              w, h, sizeof(LvlTile) * (size_t)n);
        return;
    }
    memset(level->tiles, 0, sizeof(LvlTile) * (size_t)n);
}

/* ---- MAP_FOUNDRY -------------------------------------------------- */
/* The existing tutorial map. Identical layout. */

static void build_foundry(Level *L, Arena *arena) {
    map_alloc_tiles(L, arena, 100, 40);
    /* Floor. */
    fill_rect(L, 0, L->height - 4, L->width, L->height, TILE_SOLID);
    /* Outer walls. */
    fill_rect(L, 0,            L->height - 12, 2,            L->height - 4, TILE_SOLID);
    fill_rect(L, L->width - 2, L->height - 12, L->width,     L->height - 4, TILE_SOLID);
    /* Spawn platform. */
    fill_rect(L, 12, L->height - 10, 22, L->height - 9, TILE_SOLID);
    /* Mid wall (cover column). */
    fill_rect(L, 55, L->height - 9,  56, L->height - 4, TILE_SOLID);
    /* Right-side dummy platform. */
    fill_rect(L, 70, L->height - 8,  80, L->height - 7, TILE_SOLID);

    /* Pickups so normal play (no editor-authored .lvl) has something
     * to grab. P17 replaces this with authored placements; for now we
     * scatter four representative kinds along the natural traffic
     * paths. Tile-coord helpers: floor row is `height-4`, spawn
     * platform top sits at row `height-10`, dummy platform top at
     * row `height-8`. */
    int floor_y = (L->height - 4) * 32 - 16;
    int spawn_top_y = (L->height - 10) * 32 - 16;
    int dummy_top_y = (L->height - 8)  * 32 - 16;
    add_pickup_to_map(L, arena,  17 * 32, spawn_top_y, PICKUP_HEALTH,        HEALTH_MEDIUM);
    add_pickup_to_map(L, arena,  30 * 32, floor_y,     PICKUP_POWERUP,       POWERUP_INVISIBILITY);
    add_pickup_to_map(L, arena,  45 * 32, floor_y,     PICKUP_AMMO_PRIMARY,  0);
    add_pickup_to_map(L, arena,  75 * 32, dummy_top_y, PICKUP_POWERUP,       POWERUP_BERSERK);
    add_pickup_to_map(L, arena,  90 * 32, floor_y,     PICKUP_ARMOR,         ARMOR_LIGHT);

    L->meta.mode_mask = (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM));
    LOG_I("map: foundry built (%dx%d, %d pickups, mode_mask=0x%x)",
          L->width, L->height, L->pickup_count, (unsigned)L->meta.mode_mask);
}

/* ---- MAP_SLIPSTREAM ----------------------------------------------- */
/* Three vertical layers — basement, main floor, catwalks. Player spawns
 * are on opposite sides; the catwalks reward jet hops. */

static void build_slipstream(Level *L, Arena *arena) {
    map_alloc_tiles(L, arena, 100, 50);
    /* Basement floor — bottom 4 rows. */
    fill_rect(L, 0, L->height - 4, L->width, L->height, TILE_SOLID);
    /* Outer walls (full vertical). */
    fill_rect(L, 0,            0, 2,        L->height - 4, TILE_SOLID);
    fill_rect(L, L->width - 2, 0, L->width, L->height - 4, TILE_SOLID);
    /* Mid floor — main combat layer. */
    fill_rect(L, 4,  L->height - 18, 36, L->height - 17, TILE_SOLID);
    fill_rect(L, 64, L->height - 18, 96, L->height - 17, TILE_SOLID);
    /* Mid-floor central island (smaller — encourages jet). */
    fill_rect(L, 46, L->height - 18, 54, L->height - 17, TILE_SOLID);
    /* Upper catwalks. */
    fill_rect(L, 8,  L->height - 32, 28, L->height - 31, TILE_SOLID);
    fill_rect(L, 72, L->height - 32, 92, L->height - 31, TILE_SOLID);
    /* Connecting beam at the top. */
    fill_rect(L, 30, L->height - 38, 70, L->height - 37, TILE_SOLID);
    /* Two cover blocks on the main floor. */
    fill_rect(L, 30, L->height - 8,  31, L->height - 4, TILE_SOLID);
    fill_rect(L, 69, L->height - 8,  70, L->height - 4, TILE_SOLID);
    L->meta.mode_mask = (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM));
    LOG_I("map: slipstream built (%dx%d, mode_mask=0x%x)",
          L->width, L->height, (unsigned)L->meta.mode_mask);
}

/* ---- MAP_REACTOR -------------------------------------------------- */
/* Central solid pillar with two flanking elevated platforms. Plays as
 * a wider "Foundry" with a strong contested midpoint. */

static void build_reactor(Level *L, Arena *arena) {
    map_alloc_tiles(L, arena, 110, 42);
    /* Floor. */
    fill_rect(L, 0, L->height - 4, L->width, L->height, TILE_SOLID);
    /* Outer walls. */
    fill_rect(L, 0,            L->height - 18, 2,        L->height - 4, TILE_SOLID);
    fill_rect(L, L->width - 2, L->height - 18, L->width, L->height - 4, TILE_SOLID);
    /* Big central pillar (the reactor core). */
    fill_rect(L, 51, L->height - 16, 59, L->height - 4,  TILE_SOLID);
    /* Two flanking platforms at mid height. */
    fill_rect(L, 16, L->height - 12, 38, L->height - 11, TILE_SOLID);
    fill_rect(L, 72, L->height - 12, 94, L->height - 11, TILE_SOLID);
    /* High overlooks. */
    fill_rect(L, 22, L->height - 22, 32, L->height - 21, TILE_SOLID);
    fill_rect(L, 78, L->height - 22, 88, L->height - 21, TILE_SOLID);
    L->meta.mode_mask = (uint16_t)((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM));
    LOG_I("map: reactor built (%dx%d, mode_mask=0x%x)",
          L->width, L->height, (unsigned)L->meta.mode_mask);
}

/* ---- MAP_CROSSFIRE (M5 P07) ---------------------------------------- */
/* The CTF map. Symmetric 140×42 layout — two team bases on far left and
 * far right with a small back wall, an elevated forward platform per
 * team, and a central depression with cover columns to funnel fights.
 *
 * The flag at each base sits on top of the rear elevated platform,
 * accessible only from the front (so a defender can sit between flag
 * and approach). Spawn lanes are biased to each team's back area so
 * the carrier has a legitimate run home.
 *
 * mode_mask carries FFA|TDM|CTF so the rotation can mix it in with
 * non-CTF rounds. */
static void build_crossfire(Level *L, Arena *arena) {
    map_alloc_tiles(L, arena, 140, 42);

    int W = L->width, H = L->height;
    /* Floor across the full width. */
    fill_rect(L, 0, H - 4, W, H, TILE_SOLID);

    /* Outer side walls (full height). */
    fill_rect(L, 0,     0, 2,     H - 4, TILE_SOLID);
    fill_rect(L, W - 2, 0, W,     H - 4, TILE_SOLID);

    /* RED base (left) — back platform 12 tiles deep, raised 6 tiles. */
    fill_rect(L, 4,  H - 10, 18, H - 9,  TILE_SOLID);   /* platform top */
    fill_rect(L, 16, H - 14, 18, H - 9,  TILE_SOLID);   /* short rear wall */

    /* BLUE base (right) — mirror of red. */
    fill_rect(L, W - 18, H - 10, W - 4, H - 9,  TILE_SOLID);
    fill_rect(L, W - 18, H - 14, W - 16, H - 9, TILE_SOLID);

    /* Forward elevated platforms — per-team approach to the central
     * battleground. RED's faces right, BLUE's faces left. */
    fill_rect(L, 26, H - 8, 36, H - 7, TILE_SOLID);
    fill_rect(L, W - 36, H - 8, W - 26, H - 7, TILE_SOLID);

    /* Central cover columns + a low mid platform. The map's "name"
     * (Crossfire) refers to the central exchange where both teams'
     * forward platforms have line-of-sight. */
    int mid = W / 2;
    fill_rect(L, mid - 12, H - 6, mid - 11, H - 4, TILE_SOLID);
    fill_rect(L, mid + 11, H - 6, mid + 12, H - 4, TILE_SOLID);
    fill_rect(L, mid - 4,  H - 7, mid + 4,  H - 6, TILE_SOLID);   /* mid bridge */

    /* High overlooks — a sniper perch per team, accessible by jet. */
    fill_rect(L, 12, H - 22, 22, H - 21, TILE_SOLID);
    fill_rect(L, W - 22, H - 22, W - 12, H - 21, TILE_SOLID);

    /* ---- Authored spawns (LvlSpawn) — preferred over the
     * hardcoded g_red/g_blue_lanes by map_spawn_point's M5 path.
     * Stagger horizontally inside each team's back third. */
    int n_spawns = 8;
    L->spawns = (LvlSpawn *)arena_alloc(arena, sizeof(LvlSpawn) * (size_t)n_spawns);
    if (L->spawns) {
        L->spawn_count = n_spawns;
        const int floor_y = (H - 4) * 32;        /* feet sit on top of row H-4 */
        const int spawn_y = floor_y - 40;        /* pelvis above the floor */
        const int plat_top_y = (H - 10) * 32 - 40;
        /* RED side: two on platform, two on floor between base and mid. */
        L->spawns[0] = (LvlSpawn){ .pos_x =  6 * 32, .pos_y = (int16_t)plat_top_y, .team = 1, .flags = 1, .lane_hint = 0 };
        L->spawns[1] = (LvlSpawn){ .pos_x = 12 * 32, .pos_y = (int16_t)plat_top_y, .team = 1, .flags = 1, .lane_hint = 1 };
        L->spawns[2] = (LvlSpawn){ .pos_x = 22 * 32, .pos_y = (int16_t)spawn_y,   .team = 1, .flags = 1, .lane_hint = 2 };
        L->spawns[3] = (LvlSpawn){ .pos_x = 30 * 32, .pos_y = (int16_t)spawn_y,   .team = 1, .flags = 1, .lane_hint = 3 };
        /* BLUE side: mirror. */
        L->spawns[4] = (LvlSpawn){ .pos_x = (int16_t)((W -  6) * 32), .pos_y = (int16_t)plat_top_y, .team = 2, .flags = 1, .lane_hint = 0 };
        L->spawns[5] = (LvlSpawn){ .pos_x = (int16_t)((W - 12) * 32), .pos_y = (int16_t)plat_top_y, .team = 2, .flags = 1, .lane_hint = 1 };
        L->spawns[6] = (LvlSpawn){ .pos_x = (int16_t)((W - 22) * 32), .pos_y = (int16_t)spawn_y,   .team = 2, .flags = 1, .lane_hint = 2 };
        L->spawns[7] = (LvlSpawn){ .pos_x = (int16_t)((W - 30) * 32), .pos_y = (int16_t)spawn_y,   .team = 2, .flags = 1, .lane_hint = 3 };
    }

    /* ---- Flags — one per team, hovering at chest height above each
     * team's back platform. Touch detection in ctf_step uses
     * mech_chest_pos against flag.home_pos with a 36 px radius
     * (FLAG_TOUCH_RADIUS_PX) — so flags need to sit near where a
     * standing mech's chest would actually be. The platform's top
     * tile row is `H-10`, top edge at y = (H-10)*32 = 1024. Pelvis
     * sits at floor - 36 = 988, chest ~30 px above pelvis = 958.
     * Setting flag y to platform_top - 50 = 974 puts the flag
     * staff/pennant in chest-overlap range (dy ≈ 16 → in radius). */
    L->flags = (LvlFlag *)arena_alloc(arena, sizeof(LvlFlag) * 2);
    if (L->flags) {
        L->flag_count = 2;
        const int chest_y = (H - 10) * 32 - 50;
        L->flags[0] = (LvlFlag){
            .pos_x = (int16_t)(10 * 32),         /* RED base */
            .pos_y = (int16_t)chest_y,
            .team  = 1,
        };
        L->flags[1] = (LvlFlag){
            .pos_x = (int16_t)((W - 10) * 32),   /* BLUE base */
            .pos_y = (int16_t)chest_y,
            .team  = 2,
        };
    }

    /* ---- Pickups — Health + Ammo on each side near mid; one armor
     * pack at center for risky teamplay. */
    int floor_y = (H - 4) * 32 - 16;
    int plat_y  = (H - 8)  * 32 - 16;
    add_pickup_to_map(L, arena,  40 * 32, floor_y, PICKUP_HEALTH,       HEALTH_SMALL);
    add_pickup_to_map(L, arena, (W - 40) * 32, floor_y, PICKUP_HEALTH,  HEALTH_SMALL);
    add_pickup_to_map(L, arena,  31 * 32, plat_y,  PICKUP_AMMO_PRIMARY, 0);
    add_pickup_to_map(L, arena, (W - 31) * 32, plat_y, PICKUP_AMMO_PRIMARY, 0);
    add_pickup_to_map(L, arena,  mid * 32, floor_y, PICKUP_ARMOR,       ARMOR_LIGHT);

    L->meta.mode_mask = (uint16_t)((1u << MATCH_MODE_FFA) |
                                    (1u << MATCH_MODE_TDM) |
                                    (1u << MATCH_MODE_CTF));
    LOG_I("map: crossfire built (%dx%d, %d spawns, %d flags, %d pickups, mode_mask=0x%x)",
          W, H, L->spawn_count, L->flag_count, L->pickup_count,
          (unsigned)L->meta.mode_mask);
}

static void build_fallback(MapId id, Level *level, Arena *arena) {
    switch (id) {
        case MAP_FOUNDRY:    build_foundry(level, arena);    break;
        case MAP_SLIPSTREAM: build_slipstream(level, arena); break;
        case MAP_REACTOR:    build_reactor(level, arena);    break;
        case MAP_CROSSFIRE:  build_crossfire(level, arena);  break;
        default:             build_foundry(level, arena);    break;
    }
}

void map_build(MapId id, World *world, Arena *arena) {
    const MapDef *def = map_def(id);
    char path[256];
    snprintf(path, sizeof path, "assets/maps/%s.lvl", def->short_name);

    LvlResult r = level_load(world, arena, path);
    if (r == LVL_OK) return;

    /* P08b — fallback handling diverges by registry slot.
     *   - Reserved indices (< MAP_BUILTIN_COUNT) have a code-built
     *     fallback in `build_fallback`'s switch. P17 will produce
     *     assets/maps/<short>.lvl files for these; until then a fresh
     *     checkout always ends up here for builtins, and "file not
     *     found" is the expected case (LOG_I), not WARN.
     *   - Custom indices (>= MAP_BUILTIN_COUNT) registered at startup
     *     mean a .lvl WAS on disk at scan time. Reaching this branch
     *     means the file was deleted, truncated, or its CRC went bad
     *     between scan and play. Log loudly and hard-fall-back to
     *     Foundry's code-built (better than booting players into a
     *     map they didn't pick). */
    if ((int)id < MAP_BUILTIN_COUNT) {
        if (r == LVL_ERR_FILE_NOT_FOUND) {
            LOG_I("map_build(%s): no .lvl on disk — using code-built fallback",
                  def->short_name);
        } else {
            LOG_W("map_build(%s): level_load failed (%s) — using code-built fallback",
                  def->short_name, level_io_result_str(r));
        }
        build_fallback(id, &world->level, arena);
    } else {
        LOG_E("map_build(%s): custom map's .lvl unavailable (%s) — falling back to Foundry",
              def->short_name, level_io_result_str(r));
        build_fallback(MAP_FOUNDRY, &world->level, arena);
    }
}

bool map_build_from_path(World *world, Arena *arena, const char *path) {
    if (!path || !path[0]) return false;
    LvlResult r = level_load(world, arena, path);
    if (r == LVL_OK) {
        LOG_I("map_build_from_path: loaded %s", path);
        return true;
    }
    LOG_E("map_build_from_path(%s): level_load failed (%s) — falling back to Foundry",
          path, level_io_result_str(r));
    build_fallback(MAP_FOUNDRY, &world->level, arena);
    return false;
}

/* ---- Spawn-point selection --------------------------------------- */
/* Stagger horizontally so successive spawns from the same team don't
 * telefrag. We pick from a per-map lane table. Y is derived from the
 * floor-y so any map with floor at `height-4` works. */

static const int g_red_lanes [16] = { 8, 12, 16, 10, 14, 6, 18, 20, 4, 22, 24, 26, 28, 30, 32, 34 };
static const int g_blue_lanes[16] = { 92, 88, 84, 90, 86, 94, 82, 80, 96, 78, 76, 74, 72, 70, 68, 66 };
static const int g_ffa_lanes [16] = { 16, 80, 30, 70, 24, 76, 12, 88, 44, 56, 20, 84, 36, 64, 50, 60 };

Vec2 map_spawn_point(MapId id, const Level *level, int slot_index,
                     int team, MatchModeId mode)
{
    (void)id;
    const float feet_below_pelvis = 36.0f;
    const float foot_clearance    = 4.0f;

    /* M5 P04+: prefer the .lvl's authored SPWN points when present.
     * Each `LvlSpawn` already specifies its world-pixel coords + team
     * affinity, so the runtime doesn't need to re-derive lanes.
     *
     * Match logic:
     *   - FFA: any spawn matches; sort by lane_hint and round-robin
     *     across slot_index.
     *   - TDM/CTF: prefer same-team or team=0 (any). If none match,
     *     fall back to the first authored spawn so the round still
     *     starts (better than the M4 hardcoded-lanes fallback). */
    if (level->spawn_count > 0 && level->spawns) {
        int n = level->spawn_count;
        int eligible[64];        /* small map cap; we have no map with >64 spawns */
        int e_count = 0;
        for (int i = 0; i < n && e_count < (int)(sizeof eligible / sizeof eligible[0]); ++i) {
            const LvlSpawn *s = &level->spawns[i];
            if (mode == MATCH_MODE_FFA ||
                s->team == 0 ||
                (int)s->team == team) {
                eligible[e_count++] = i;
            }
        }
        if (e_count == 0) {
            /* No team-matching spawn — accept the first one. */
            const LvlSpawn *s = &level->spawns[0];
            return (Vec2){ (float)s->pos_x, (float)s->pos_y };
        }
        const LvlSpawn *s = &level->spawns[eligible[slot_index % e_count]];
        return (Vec2){ (float)s->pos_x, (float)s->pos_y };
    }

    /* Pre-M5 / fallback: hardcoded per-team lane tables on top of the
     * (height - 4) floor row. */
    float floor_y = (float)(level->height - 4) * (float)level->tile_size
                  - feet_below_pelvis - foot_clearance;

    /* Mode wins over team in FFA: MATCH_TEAM_FFA aliases MATCH_TEAM_RED
     * (both = 1) so a naive team-only check would jam every FFA player
     * onto the red lanes (clustered at x=8..14). FFA lanes are spread
     * across the full map width so two players spawn ~64 tiles apart
     * instead of 4. */
    const int *lanes;
    if (mode == MATCH_MODE_FFA)            lanes = g_ffa_lanes;
    else if (team == MATCH_TEAM_BLUE)      lanes = g_blue_lanes;
    else if (team == MATCH_TEAM_RED)       lanes = g_red_lanes;
    else                                    lanes = g_ffa_lanes;

    int lane_count = 16;
    int tx = lanes[slot_index % lane_count];
    if (tx < 4) tx = 4;
    if (tx >= level->width - 4) tx = level->width - 5;

    return (Vec2){ (float)tx * (float)level->tile_size + 8.0f, floor_y };
}

/* ---- M5 P08 — client-side build by descriptor -------------------- */

void map_build_for_descriptor(World *world, Arena *arena,
                              const struct MapDescriptor *desc,
                              MapId fallback_id)
{
    if (!world || !arena) return;
    if (!desc || (desc->crc32 == 0 && desc->size_bytes == 0)) {
        map_build(fallback_id, world, arena);
        return;
    }
    /* 1. shipped */
    char shipped[256];
    if (map_cache_assets_path(desc->short_name, shipped, sizeof(shipped))) {
        if (map_cache_file_crc(shipped) == desc->crc32 &&
            map_cache_file_size(shipped) == desc->size_bytes) {
            if (map_build_from_path(world, arena, shipped)) return;
        }
    }
    /* 2. cache */
    if (map_cache_has(desc->crc32)) {
        const char *cached = map_cache_path(desc->crc32);
        if (map_cache_file_crc(cached) == desc->crc32 &&
            map_cache_file_size(cached) == desc->size_bytes) {
            if (map_build_from_path(world, arena, cached)) return;
        }
    }
    /* 3. fallback. The host shipped a descriptor we can't load — log
     * and use the rotation map. start_round on the client built before
     * the descriptor arrived will use this branch. */
    LOG_W("map_build_for_descriptor: no local file for crc=%08x; falling back to MapId %d",
          (unsigned)desc->crc32, (int)fallback_id);
    map_build(fallback_id, world, arena);
}

/* ---- M5 P08 — host serve-info refresh ---------------------------- */

/* Strip directory + extension from a filename. Used to derive a
 * short_name for test-play maps where the host loads from an absolute
 * path. */
static void basename_no_ext(const char *path, char *out, size_t out_cap) {
    if (!path || !out || out_cap == 0) {
        if (out && out_cap) out[0] = '\0';
        return;
    }
    const char *base = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    size_t i = 0;
    while (base[i] && base[i] != '.' && i + 1 < out_cap) {
        out[i] = base[i];
        ++i;
    }
    out[i] = '\0';
}

void maps_refresh_serve_info(const char *short_name,
                             const char *serve_path_in,
                             struct MapDescriptor *out_desc,
                             char *out_serve_path,
                             size_t out_serve_path_cap)
{
    if (!out_desc || !out_serve_path || out_serve_path_cap == 0) return;

    memset(out_desc, 0, sizeof(*out_desc));
    out_serve_path[0] = '\0';

    /* Pick the candidate path the host will stream from. */
    char candidate[256];
    if (serve_path_in && *serve_path_in) {
        snprintf(candidate, sizeof(candidate), "%s", serve_path_in);
    } else if (short_name && *short_name) {
        snprintf(candidate, sizeof(candidate), "assets/maps/%s.lvl", short_name);
    } else {
        candidate[0] = '\0';
    }

    /* Probe the file. */
    uint32_t size = candidate[0] ? map_cache_file_size(candidate) : 0;
    uint32_t crc  = (size > 0)   ? map_cache_file_crc (candidate) : 0;

    if (size == 0 || size > NET_MAP_MAX_FILE_BYTES) {
        /* No .lvl on disk (code-built fallback) or oversized — emit
         * an empty descriptor. crc/size = 0 tells clients to use their
         * own MapId-rotation fallback. */
        if (size > NET_MAP_MAX_FILE_BYTES) {
            LOG_W("maps: %s is %u bytes (> %u cap) — not advertising",
                  candidate, (unsigned)size, (unsigned)NET_MAP_MAX_FILE_BYTES);
        }
        out_desc->crc32      = 0;
        out_desc->size_bytes = 0;
        out_desc->short_name_len = 0;
        return;
    }

    /* Derive the short_name. For test-play we use the file's basename;
     * otherwise the caller's short_name. Both get null-padded into the
     * 24-byte slot. */
    char nm[24];
    if (serve_path_in && *serve_path_in) {
        basename_no_ext(serve_path_in, nm, sizeof(nm));
    } else if (short_name) {
        snprintf(nm, sizeof(nm), "%s", short_name);
    } else {
        nm[0] = '\0';
    }
    size_t nlen = strlen(nm);
    if (nlen > 23) nlen = 23;

    out_desc->crc32          = crc;
    out_desc->size_bytes     = size;
    out_desc->short_name_len = (uint8_t)nlen;
    memset(out_desc->short_name, 0, sizeof(out_desc->short_name));
    memcpy(out_desc->short_name, nm, nlen);

    snprintf(out_serve_path, out_serve_path_cap, "%s", candidate);
    LOG_I("maps: serve %s (crc=%08x, %u bytes)", candidate,
          (unsigned)crc, (unsigned)size);
}
