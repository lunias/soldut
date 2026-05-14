#include "doc.h"

#include "arena.h"
#include "level_io.h"
#include "log.h"
#include "map_thumb.h"

#include "raylib.h"
#include "stb_ds.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#define DOC_MKDIR(p) _mkdir(p)
#else
#define DOC_MKDIR(p) mkdir((p), 0755)
#endif

/* mkdir -p the parent directory of `path`. The editor's default save
 * target is `assets/maps/<name>.lvl`; on a fresh checkout that
 * directory doesn't exist (it's gitignored), so fopen(... "wb")
 * returns NULL and `level_save` reports LVL_ERR_IO. Walk the
 * separators, create each segment ignoring "already exists" errors. */
static void doc_ensure_parent_dir(const char *path) {
    if (!path || !*path) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    /* Find last separator and truncate. */
    char *last = NULL;
    for (char *p = buf; *p; ++p) {
        if (*p == '/' || *p == '\\') last = p;
    }
    if (!last || last == buf) return;   /* no parent dir or already root */
    *last = '\0';
    /* Walk and create each segment. Skip a leading slash on POSIX so
     * mkdir("") doesn't fire on an absolute path. */
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            DOC_MKDIR(buf);
            *p = saved;
        }
    }
    DOC_MKDIR(buf);
}

/* Sized arena used for level_load / level_save scratch. The editor
 * keeps one of these around for the lifetime of the process; each
 * load/save resets it. 4 MB covers the 1 MB file cap with comfortable
 * headroom for the broadphase grid. */
#define DOC_SCRATCH_BYTES (4u * 1024u * 1024u)

static Arena *g_doc_scratch(void) {
    static Arena a = {0};
    static int   inited = 0;
    if (!inited) {
        arena_init(&a, DOC_SCRATCH_BYTES, "editor-scratch");
        inited = 1;
    }
    return &a;
}

void doc_init(EditorDoc *d) {
    memset(d, 0, sizeof(*d));
    d->tile_size = EDITOR_DEFAULT_TILE_SIZE;
    /* Reserved offset 0 in STRT is the empty string. */
    arrput(d->str_pool, '\0');
}

void doc_free(EditorDoc *d) {
    free(d->tiles);             d->tiles = NULL;
    arrfree(d->polys);          d->polys = NULL;
    arrfree(d->spawns);         d->spawns = NULL;
    arrfree(d->pickups);        d->pickups = NULL;
    arrfree(d->decos);          d->decos = NULL;
    arrfree(d->ambis);          d->ambis = NULL;
    arrfree(d->flags);          d->flags = NULL;
    arrfree(d->str_pool);       d->str_pool = NULL;
    d->width = d->height = 0;
}

void doc_new(EditorDoc *d, int width, int height) {
    /* Reset to a blank document. */
    doc_free(d);
    d->width      = width;
    d->height     = height;
    d->tile_size  = EDITOR_DEFAULT_TILE_SIZE;
    d->tiles = (LvlTile *)calloc((size_t)(width * height), sizeof(LvlTile));
    arrput(d->str_pool, '\0');
    /* META defaults: empty name; allow all match modes. */
    d->meta = (LvlMeta){0};
    d->meta.name_str_idx = doc_str_intern(d, "Untitled");
    d->meta.mode_mask    = 1u | 2u | 4u;        /* FFA | TDM | CTF */
    d->source_path[0] = 0;
    d->dirty = false;
    LOG_I("editor: new document %dx%d", width, height);
}

uint16_t doc_str_intern(EditorDoc *d, const char *s) {
    if (!s || !s[0]) return 0;
    size_t n = arrlenu(d->str_pool);
    if (n > 65535) return 0;             /* STRT can't address past 16 bits */
    uint16_t off = (uint16_t)n;
    for (const char *p = s; *p; ++p) arrput(d->str_pool, *p);
    arrput(d->str_pool, '\0');
    return off;
}

const char *doc_str_lookup(const EditorDoc *d, uint16_t off) {
    if (!d->str_pool) return "";
    if (off == 0)     return "";
    size_t n = arrlenu(d->str_pool);
    if (off >= n)     return "";
    return d->str_pool + off;
}

int  doc_world_w_px(const EditorDoc *d) { return d->width  * d->tile_size; }
int  doc_world_h_px(const EditorDoc *d) { return d->height * d->tile_size; }

LvlTile doc_tile_at(const EditorDoc *d, int tx, int ty) {
    if (tx < 0 || tx >= d->width || ty < 0 || ty >= d->height) return (LvlTile){0};
    return d->tiles[ty * d->width + tx];
}

void doc_tile_set(EditorDoc *d, int tx, int ty, LvlTile t) {
    if (tx < 0 || tx >= d->width || ty < 0 || ty >= d->height) return;
    d->tiles[ty * d->width + tx] = t;
    d->dirty = true;
}

/* ---- Load: route through level_load and copy into stb_ds arrays. */

/* Stub World — level_load only writes through `world->level`, so we
 * don't need to set up any other fields. Sized to whatever layout
 * world.h has at compile time; we pass a pointer. */
typedef struct {
    /* Mirror enough of World for level_load. We only ever read .level
     * after the call. */
    char pad_before[offsetof(World, level)];
    Level level;
    char pad_after[sizeof(World) - offsetof(World, level) - sizeof(Level)];
} StubWorld;

bool doc_load(EditorDoc *d, const char *path) {
    if (!d || !path || !path[0]) return false;
    Arena *scr = g_doc_scratch();
    arena_reset(scr);

    /* level_load writes to world->level. We pass a real World on the
     * stack — it's heavy but cheap to zero, and avoids pointer math. */
    World *w = (World *)calloc(1, sizeof(World));
    if (!w) return false;
    LvlResult r = level_load(w, scr, path);
    if (r != LVL_OK) {
        LOG_E("editor: doc_load(%s): %s", path, level_io_result_str(r));
        free(w);
        return false;
    }

    /* Wipe the doc and copy data out of the arena into stb_ds storage
     * (so future arena_reset() calls don't yank our buffers). */
    doc_free(d);
    d->width     = w->level.width;
    d->height    = w->level.height;
    d->tile_size = w->level.tile_size;
    int n_tiles  = d->width * d->height;
    d->tiles     = (LvlTile *)malloc(sizeof(LvlTile) * (size_t)n_tiles);
    if (n_tiles > 0 && w->level.tiles) {
        memcpy(d->tiles, w->level.tiles, sizeof(LvlTile) * (size_t)n_tiles);
    } else if (n_tiles > 0) {
        memset(d->tiles, 0, sizeof(LvlTile) * (size_t)n_tiles);
    }

    for (int i = 0; i < w->level.poly_count;   ++i) arrput(d->polys,   w->level.polys[i]);
    for (int i = 0; i < w->level.spawn_count;  ++i) arrput(d->spawns,  w->level.spawns[i]);
    for (int i = 0; i < w->level.pickup_count; ++i) arrput(d->pickups, w->level.pickups[i]);
    for (int i = 0; i < w->level.deco_count;   ++i) arrput(d->decos,   w->level.decos[i]);
    for (int i = 0; i < w->level.ambi_count;   ++i) arrput(d->ambis,   w->level.ambis[i]);
    for (int i = 0; i < w->level.flag_count;   ++i) arrput(d->flags,   w->level.flags[i]);
    d->meta = w->level.meta;

    /* Copy STRT verbatim. The arena owns the original bytes; we
     * arrput-copy into our own pool so the pointers survive
     * arena_reset on the next load. */
    if (w->level.string_table_size > 0) {
        for (int i = 0; i < w->level.string_table_size; ++i) {
            arrput(d->str_pool, w->level.string_table[i]);
        }
    } else {
        arrput(d->str_pool, '\0');
    }

    snprintf(d->source_path, sizeof d->source_path, "%s", path);
    d->dirty = false;

    LOG_I("editor: loaded %s (%dx%d, %td polys, %td spawns)",
          path, d->width, d->height,
          arrlen(d->polys), arrlen(d->spawns));
    free(w);
    return true;
}

bool doc_save(EditorDoc *d, const char *path) {
    if (!d || !path || !path[0]) return false;
    Arena *scr = g_doc_scratch();
    arena_reset(scr);

    /* Build a stub World pointing at our doc's storage. level_save
     * reads from world->level only. */
    World *w = (World *)calloc(1, sizeof(World));
    if (!w) return false;
    Level *L = &w->level;
    L->width      = d->width;
    L->height     = d->height;
    L->tile_size  = d->tile_size;
    L->tiles      = d->tiles;
    L->polys      = d->polys;       L->poly_count   = (int)arrlen(d->polys);
    L->spawns     = d->spawns;      L->spawn_count  = (int)arrlen(d->spawns);
    L->pickups    = d->pickups;     L->pickup_count = (int)arrlen(d->pickups);
    L->decos      = d->decos;       L->deco_count   = (int)arrlen(d->decos);
    L->ambis      = d->ambis;       L->ambi_count   = (int)arrlen(d->ambis);
    L->flags      = d->flags;       L->flag_count   = (int)arrlen(d->flags);
    L->meta       = d->meta;
    L->string_table      = d->str_pool;
    L->string_table_size = (int)arrlen(d->str_pool);

    /* Ensure the destination directory exists before fopen. */
    doc_ensure_parent_dir(path);

    /* Encode the thumb once so it can ride inside the .lvl as a THMB
     * lump AND drop on disk as a sidecar PNG. Sidecar feeds the host's
     * lobby vote picker directly; THMB lump rides over the wire to
     * peers that download the map. */
    int thumb_size = 0;
    unsigned char *thumb_bytes = map_thumb_encode_png(L, &thumb_size);
    L->thumb_png_data = thumb_bytes;
    L->thumb_png_size = thumb_size;

    LvlResult r = level_save(w, scr, path);
    L->thumb_png_data = NULL;
    L->thumb_png_size = 0;
    if (r != LVL_OK) {
        if (thumb_bytes) MemFree(thumb_bytes);
        free(w);
        LOG_E("editor: doc_save(%s): %s", path, level_io_result_str(r));
        return false;
    }

    /* Sidecar thumbnail next to the .lvl. Non-fatal on failure — the
     * .lvl saved cleanly and the embedded lump still lets peers see
     * the preview. */
    char thumb_path[512];
    size_t n = strlen(path);
    if (n >= 4 && strcmp(path + n - 4, ".lvl") == 0
              && n - 4 < sizeof thumb_path - 12) {
        memcpy(thumb_path, path, n - 4);
        memcpy(thumb_path + n - 4, "_thumb.png", 11);
        if (thumb_bytes) {
            SaveFileData(thumb_path, thumb_bytes, thumb_size);
        } else if (!map_thumb_write_png(L, thumb_path)) {
            LOG_W("editor: thumb write to %s failed", thumb_path);
        }
    }

    if (thumb_bytes) MemFree(thumb_bytes);
    free(w);
    snprintf(d->source_path, sizeof d->source_path, "%s", path);
    d->dirty = false;
    LOG_I("editor: saved %s", path);
    return true;
}
