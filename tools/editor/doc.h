#pragma once

/*
 * EditorDoc — the editor's in-memory document.
 *
 * Mirrors src/world.h Level so we can hand pointers to level_save()
 * directly via a stub World. The editor uses stb_ds arrput-style
 * resizable arrays for everything that grows (polys, spawns, etc.) so
 * we don't reinvent dynamic arrays here. (Runtime sticks with fixed
 * pools — only the editor pays this dependency.)
 *
 * Lifecycle: doc_init -> doc_new (or doc_load) -> edits -> doc_save -> doc_free.
 */

#include "world.h"

#include <stdbool.h>
#include <stdint.h>

#define EDITOR_MAX_POLYS         8192
#define EDITOR_MAX_SPAWNS         128
#define EDITOR_MAX_PICKUPS        128
#define EDITOR_MAX_DECOS         1024
#define EDITOR_MAX_AMBIS           32
#define EDITOR_MAX_FLAGS            8

#define EDITOR_DEFAULT_TILE_SIZE   32
#define EDITOR_DEFAULT_W          100
#define EDITOR_DEFAULT_H           60

typedef struct EditorDoc {
    /* Mirrors Level. Tiles are a fixed (w*h) array allocated by doc_new
     * / doc_load; the rest are stb_ds arrput arrays. */
    int       width, height, tile_size;
    LvlTile  *tiles;             /* width * height records, malloc'd */

    LvlPoly  *polys;             /* arrput */
    LvlSpawn *spawns;            /* arrput */
    LvlPickup *pickups;          /* arrput */
    LvlDeco  *decos;             /* arrput */
    LvlAmbi  *ambis;             /* arrput */
    LvlFlag  *flags;             /* arrput */
    LvlMeta   meta;

    /* String table. Stored as a stb_ds char array; the file format's
     * STRT lump is a copy of this byte-blob. Offset 0 is reserved
     * "empty string" so we always start with a single \0 byte. */
    char     *str_pool;

    /* Editor-only state. */
    char      source_path[512];
    bool      dirty;
} EditorDoc;

void  doc_init    (EditorDoc *d);
void  doc_free    (EditorDoc *d);

/* Reset to a blank w*h grid + default META. width/height in tiles. */
void  doc_new     (EditorDoc *d, int width, int height);

/* Load via level_load() (re-uses runtime decoder), then copy into
 * stb_ds-backed arrays so the editor can mutate freely. Returns true
 * on success; on failure logs via LOG_E and leaves the doc in its
 * previous state. */
bool  doc_load    (EditorDoc *d, const char *path);

/* Encode + write via level_save() (re-uses runtime encoder). Returns
 * true on success; on failure logs via LOG_E. */
bool  doc_save    (EditorDoc *d, const char *path);

/* Append a NUL-terminated string to str_pool and return its byte
 * offset. If the string is empty or NULL, returns 0 (the reserved
 * empty-string offset). Does NOT deduplicate — the editor doesn't
 * need it; a typical map's STRT is a few hundred bytes. */
uint16_t doc_str_intern(EditorDoc *d, const char *s);

/* Look up a string by STRT offset. Returns "" for offset 0 or out of
 * range — the renderer / META display can call this without checking. */
const char *doc_str_lookup(const EditorDoc *d, uint16_t off);

/* Tile accessor (bounds-checked). Returns 0 (empty) for out-of-bounds
 * coords. Used by both render and validation. */
LvlTile doc_tile_at(const EditorDoc *d, int tx, int ty);
void    doc_tile_set(EditorDoc *d, int tx, int ty, LvlTile t);

/* World-pixel-space conversions — they take tile_size into account. */
int doc_world_w_px(const EditorDoc *d);
int doc_world_h_px(const EditorDoc *d);
