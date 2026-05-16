#include "level_io.h"

#include "arena.h"
#include "log.h"
#include "world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Endian sanity ------------------------------------------------ */
/* Sanity check: every supported target is little-endian. */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#  error "level_io requires a little-endian host"
#endif

/* ---- Header / directory layout (offsets, in bytes) --------------- */

#define LVL_HEADER_BYTES        64
#define LVL_DIR_ENTRY_BYTES     16
#define LVL_NAME_BYTES           8

#define HDR_OFF_MAGIC            0     /* 4 bytes */
#define HDR_OFF_VERSION          4     /* u32 */
#define HDR_OFF_SECTION_COUNT    8     /* u32 */
#define HDR_OFF_FLAGS           12     /* u32 (reserved) */
#define HDR_OFF_WORLD_W         16     /* u32 */
#define HDR_OFF_WORLD_H         20     /* u32 */
#define HDR_OFF_TILE_SIZE       24     /* u32 */
#define HDR_OFF_STRT_OFF        28     /* u32 */
#define HDR_OFF_STRT_SIZE       32     /* u32 */
#define HDR_OFF_CRC32           36     /* u32 */
/* 24 reserved bytes from offset 40 to 63 */

#define DIR_OFF_NAME             0     /* 8 bytes */
#define DIR_OFF_OFFSET           8     /* u32 */
#define DIR_OFF_SIZE            12     /* u32 */

#define LVL_TILE_REC_BYTES       4
#define LVL_POLY_REC_BYTES      32
#define LVL_SPAWN_REC_BYTES      8
#define LVL_PICKUP_REC_BYTES    12
#define LVL_DECO_REC_BYTES      12
#define LVL_AMBI_REC_BYTES      16
#define LVL_FLAG_REC_BYTES       8
#define LVL_META_REC_BYTES      32

/* ---- Endian helpers (explicit little-endian; never memcpy struct) - */

static inline uint16_t r_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t r_u32(const uint8_t *p) {
    return (uint32_t)p[0]        |
           ((uint32_t)p[1] <<  8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline int16_t  r_i16(const uint8_t *p) { return (int16_t)r_u16(p); }

static inline void w_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}
static inline void w_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v        & 0xff);
    p[1] = (uint8_t)((v >>  8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}
static inline void w_i16(uint8_t *p, int16_t v) { w_u16(p, (uint16_t)v); }

/* ---- CRC32 (polynomial 0xEDB88320, table-based) ------------------- */

static uint32_t g_crc_table[256];
static int      g_crc_table_init = 0;

static void crc_table_build(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
    g_crc_table_init = 1;
}

uint32_t level_crc32(const uint8_t *data, int n) {
    if (!g_crc_table_init) crc_table_build();
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; ++i) {
        c = g_crc_table[(c ^ data[i]) & 0xffu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* CRC over a buffer with a 4-byte window zeroed (for the header CRC
 * field, which is itself part of the file). */
uint32_t level_compute_buffer_crc(const uint8_t *buf, int n) {
    if (!buf || n < (int)(HDR_OFF_CRC32 + 4)) return 0;
    if (!g_crc_table_init) crc_table_build();
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; ++i) {
        uint8_t b = buf[i];
        if (i >= HDR_OFF_CRC32 && i < HDR_OFF_CRC32 + 4) b = 0;
        c = g_crc_table[(c ^ b) & 0xffu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t crc32_with_zeroed_window(const uint8_t *buf, int n,
                                         int window_off) {
    if (!g_crc_table_init) crc_table_build();
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; ++i) {
        uint8_t b = buf[i];
        if (i >= window_off && i < window_off + 4) b = 0;
        c = g_crc_table[(c ^ b) & 0xffu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* ---- Result strings ----------------------------------------------- */

const char *level_io_result_str(LvlResult r) {
    switch (r) {
        case LVL_OK:                  return "ok";
        case LVL_ERR_FILE_NOT_FOUND:  return "file not found";
        case LVL_ERR_TOO_LARGE:       return "file > 1 MB";
        case LVL_ERR_BAD_MAGIC:       return "bad magic (not SDLV)";
        case LVL_ERR_BAD_VERSION:     return "unsupported version";
        case LVL_ERR_BAD_CRC:         return "crc mismatch";
        case LVL_ERR_BAD_DIRECTORY:   return "lump directory out of bounds";
        case LVL_ERR_BAD_SECTION:     return "section size or content invalid";
        case LVL_ERR_OOM:             return "out of memory (level arena)";
        case LVL_ERR_IO:              return "i/o error";
    }
    return "unknown";
}

/* ---- Lump name comparison ---------------------------------------- */

static int name_eq(const uint8_t *raw_name, const char *want) {
    /* Names are 8 bytes, NUL-padded ASCII. */
    char buf[LVL_NAME_BYTES + 1];
    memcpy(buf, raw_name, LVL_NAME_BYTES);
    buf[LVL_NAME_BYTES] = 0;
    return strncmp(buf, want, LVL_NAME_BYTES) == 0;
}

static void name_pack(uint8_t *raw_name, const char *src) {
    memset(raw_name, 0, LVL_NAME_BYTES);
    for (int i = 0; i < LVL_NAME_BYTES && src[i]; ++i) raw_name[i] = (uint8_t)src[i];
}

/* ---- Per-record decoders ------------------------------------------ */

static void decode_tile(const uint8_t *p, LvlTile *out) {
    out->id    = r_u16(p + 0);
    out->flags = r_u16(p + 2);
}
static void decode_poly(const uint8_t *p, LvlPoly *out) {
    out->v_x[0] = r_i16(p +  0); out->v_x[1] = r_i16(p +  2); out->v_x[2] = r_i16(p +  4);
    out->v_y[0] = r_i16(p +  6); out->v_y[1] = r_i16(p +  8); out->v_y[2] = r_i16(p + 10);
    out->normal_x[0] = r_i16(p + 12); out->normal_x[1] = r_i16(p + 14); out->normal_x[2] = r_i16(p + 16);
    out->normal_y[0] = r_i16(p + 18); out->normal_y[1] = r_i16(p + 20); out->normal_y[2] = r_i16(p + 22);
    out->kind     = r_u16(p + 24);
    out->group_id = r_u16(p + 26);
    out->bounce_q = r_i16(p + 28);
    out->reserved = r_u16(p + 30);
}
static void decode_spawn(const uint8_t *p, LvlSpawn *out) {
    out->pos_x     = r_i16(p + 0);
    out->pos_y     = r_i16(p + 2);
    out->team      = p[4];
    out->flags     = p[5];
    out->lane_hint = p[6];
    out->reserved  = p[7];
}
static void decode_pickup(const uint8_t *p, LvlPickup *out) {
    out->pos_x      = r_i16(p +  0);
    out->pos_y      = r_i16(p +  2);
    out->category   = p[4];
    out->variant    = p[5];
    out->respawn_ms = r_u16(p +  6);
    out->flags      = r_u16(p +  8);
    out->reserved   = r_u16(p + 10);
}
static void decode_deco(const uint8_t *p, LvlDeco *out) {
    out->pos_x          = r_i16(p +  0);
    out->pos_y          = r_i16(p +  2);
    out->scale_q        = r_i16(p +  4);
    out->rot_q          = r_i16(p +  6);
    out->sprite_str_idx = r_u16(p +  8);
    out->layer          = p[10];
    out->flags          = p[11];
}
static void decode_ambi(const uint8_t *p, LvlAmbi *out) {
    out->rect_x     = r_i16(p +  0);
    out->rect_y     = r_i16(p +  2);
    out->rect_w     = r_i16(p +  4);
    out->rect_h     = r_i16(p +  6);
    out->kind       = r_u16(p +  8);
    out->strength_q = r_i16(p + 10);
    out->dir_x_q    = r_i16(p + 12);
    out->dir_y_q    = r_i16(p + 14);
}
static void decode_flag(const uint8_t *p, LvlFlag *out) {
    out->pos_x = r_i16(p + 0);
    out->pos_y = r_i16(p + 2);
    out->team  = p[4];
    out->reserved[0] = p[5];
    out->reserved[1] = p[6];
    out->reserved[2] = p[7];
}
static void decode_meta(const uint8_t *p, LvlMeta *out) {
    out->name_str_idx         = r_u16(p +  0);
    out->blurb_str_idx        = r_u16(p +  2);
    out->background_str_idx   = r_u16(p +  4);
    out->music_str_idx        = r_u16(p +  6);
    out->ambient_loop_str_idx = r_u16(p +  8);
    out->reverb_amount_q      = r_u16(p + 10);
    out->mode_mask            = r_u16(p + 12);
    /* M6 P09 — atmosphere fields (formerly reserved[]). Old files
     * carry zeros here, which the runtime interprets as "use theme
     * defaults" via atmosphere_resolve(). */
    out->theme_id             = r_u16(p + 14);
    out->sky_top_rgb565       = r_u16(p + 16);
    out->sky_bot_rgb565       = r_u16(p + 18);
    out->fog_density_q        = r_u16(p + 20);
    out->fog_color_rgb565     = r_u16(p + 22);
    out->vignette_q           = r_u16(p + 24);
    out->sun_angle_q          = r_u16(p + 26);
    out->weather_kind         = r_u16(p + 28);
    out->weather_density_q    = r_u16(p + 30);
    /* 9 atmosphere u16s consume exactly the 18-byte reserved[9] slot
     * the format reserved at M5 P01; LvlMeta total stays 32 B and the
     * _Static_assert at world.h:953 still holds. */
}

/* ---- Per-record encoders ------------------------------------------ */

static void encode_tile(uint8_t *p, const LvlTile *t) {
    w_u16(p + 0, t->id);
    w_u16(p + 2, t->flags);
}
static void encode_poly(uint8_t *p, const LvlPoly *q) {
    w_i16(p +  0, q->v_x[0]); w_i16(p +  2, q->v_x[1]); w_i16(p +  4, q->v_x[2]);
    w_i16(p +  6, q->v_y[0]); w_i16(p +  8, q->v_y[1]); w_i16(p + 10, q->v_y[2]);
    w_i16(p + 12, q->normal_x[0]); w_i16(p + 14, q->normal_x[1]); w_i16(p + 16, q->normal_x[2]);
    w_i16(p + 18, q->normal_y[0]); w_i16(p + 20, q->normal_y[1]); w_i16(p + 22, q->normal_y[2]);
    w_u16(p + 24, q->kind);
    w_u16(p + 26, q->group_id);
    w_i16(p + 28, q->bounce_q);
    w_u16(p + 30, q->reserved);
}
static void encode_spawn(uint8_t *p, const LvlSpawn *s) {
    w_i16(p + 0, s->pos_x);
    w_i16(p + 2, s->pos_y);
    p[4] = s->team;
    p[5] = s->flags;
    p[6] = s->lane_hint;
    p[7] = s->reserved;
}
static void encode_pickup(uint8_t *p, const LvlPickup *s) {
    w_i16(p +  0, s->pos_x);
    w_i16(p +  2, s->pos_y);
    p[4] = s->category;
    p[5] = s->variant;
    w_u16(p +  6, s->respawn_ms);
    w_u16(p +  8, s->flags);
    w_u16(p + 10, s->reserved);
}
static void encode_deco(uint8_t *p, const LvlDeco *d) {
    w_i16(p +  0, d->pos_x);
    w_i16(p +  2, d->pos_y);
    w_i16(p +  4, d->scale_q);
    w_i16(p +  6, d->rot_q);
    w_u16(p +  8, d->sprite_str_idx);
    p[10] = d->layer;
    p[11] = d->flags;
}
static void encode_ambi(uint8_t *p, const LvlAmbi *a) {
    w_i16(p +  0, a->rect_x);
    w_i16(p +  2, a->rect_y);
    w_i16(p +  4, a->rect_w);
    w_i16(p +  6, a->rect_h);
    w_u16(p +  8, a->kind);
    w_i16(p + 10, a->strength_q);
    w_i16(p + 12, a->dir_x_q);
    w_i16(p + 14, a->dir_y_q);
}
static void encode_flag(uint8_t *p, const LvlFlag *f) {
    w_i16(p + 0, f->pos_x);
    w_i16(p + 2, f->pos_y);
    p[4] = f->team;
    p[5] = f->reserved[0];
    p[6] = f->reserved[1];
    p[7] = f->reserved[2];
}
static void encode_meta(uint8_t *p, const LvlMeta *m) {
    w_u16(p +  0, m->name_str_idx);
    w_u16(p +  2, m->blurb_str_idx);
    w_u16(p +  4, m->background_str_idx);
    w_u16(p +  6, m->music_str_idx);
    w_u16(p +  8, m->ambient_loop_str_idx);
    w_u16(p + 10, m->reverb_amount_q);
    w_u16(p + 12, m->mode_mask);
    /* M6 P09 — atmosphere block (9 × u16 = 18 B). Fills exactly the
     * 18 reserved bytes from the M5 P01 schema; LvlMeta stays 32 B. */
    w_u16(p + 14, m->theme_id);
    w_u16(p + 16, m->sky_top_rgb565);
    w_u16(p + 18, m->sky_bot_rgb565);
    w_u16(p + 20, m->fog_density_q);
    w_u16(p + 22, m->fog_color_rgb565);
    w_u16(p + 24, m->vignette_q);
    w_u16(p + 26, m->sun_angle_q);
    w_u16(p + 28, m->weather_kind);
    w_u16(p + 30, m->weather_density_q);
}

/* ---- Polygon broadphase grid (P02) -------------------------------- */
/*
 * For each tile, store the indices of polygons whose AABB overlaps it.
 * At collision time, particle->tile gives O(1) access to the small
 * candidate list (typically <=4 polys per tile in playtest maps).
 *
 * Two passes over the polygon list: first counts per-cell entries to
 * size the flat poly_grid, then writes them. Both arrays live in the
 * level arena (reset on level reload).
 */
LvlResult level_build_poly_broadphase(struct Level *level, struct Arena *arena) {
    Level *L = level;
    if (!L || L->poly_count == 0 || L->width <= 0 || L->height <= 0) return LVL_OK;
    int W = L->width, H = L->height;
    int ts = L->tile_size;
    int n_cells = W * H;

    int *off = (int *)arena_alloc(arena, sizeof(int) * (size_t)(n_cells + 1));
    if (!off) return LVL_ERR_OOM;
    memset(off, 0, sizeof(int) * (size_t)(n_cells + 1));

    /* Pass 1: count. off[c+1] holds count of cell c. */
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *poly = &L->polys[i];
        int min_x = poly->v_x[0], max_x = poly->v_x[0];
        int min_y = poly->v_y[0], max_y = poly->v_y[0];
        for (int k = 1; k < 3; ++k) {
            if (poly->v_x[k] < min_x) min_x = poly->v_x[k];
            if (poly->v_x[k] > max_x) max_x = poly->v_x[k];
            if (poly->v_y[k] < min_y) min_y = poly->v_y[k];
            if (poly->v_y[k] > max_y) max_y = poly->v_y[k];
        }
        int tx0 = min_x / ts, tx1 = max_x / ts;
        int ty0 = min_y / ts, ty1 = max_y / ts;
        if (tx0 < 0) tx0 = 0;
        if (tx1 >= W) tx1 = W - 1;
        if (ty0 < 0) ty0 = 0;
        if (ty1 >= H) ty1 = H - 1;
        if (tx0 > tx1 || ty0 > ty1) continue;
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                off[ty * W + tx + 1]++;
            }
        }
    }

    /* Prefix sum: off[c] = total entries in cells [0, c). */
    for (int c = 1; c <= n_cells; ++c) off[c] += off[c - 1];
    int total = off[n_cells];

    int grid_alloc = total > 0 ? total : 1;
    int *grid = (int *)arena_alloc(arena, sizeof(int) * (size_t)grid_alloc);
    if (!grid) return LVL_ERR_OOM;

    /* Cursor copy so pass 2 doesn't trash off[]. */
    int *cursor = (int *)arena_alloc(arena, sizeof(int) * (size_t)n_cells);
    if (!cursor) return LVL_ERR_OOM;
    memcpy(cursor, off, sizeof(int) * (size_t)n_cells);

    /* Pass 2: write polygon ids into the buckets. */
    for (int i = 0; i < L->poly_count; ++i) {
        const LvlPoly *poly = &L->polys[i];
        int min_x = poly->v_x[0], max_x = poly->v_x[0];
        int min_y = poly->v_y[0], max_y = poly->v_y[0];
        for (int k = 1; k < 3; ++k) {
            if (poly->v_x[k] < min_x) min_x = poly->v_x[k];
            if (poly->v_x[k] > max_x) max_x = poly->v_x[k];
            if (poly->v_y[k] < min_y) min_y = poly->v_y[k];
            if (poly->v_y[k] > max_y) max_y = poly->v_y[k];
        }
        int tx0 = min_x / ts, tx1 = max_x / ts;
        int ty0 = min_y / ts, ty1 = max_y / ts;
        if (tx0 < 0) tx0 = 0;
        if (tx1 >= W) tx1 = W - 1;
        if (ty0 < 0) ty0 = 0;
        if (ty1 >= H) ty1 = H - 1;
        if (tx0 > tx1 || ty0 > ty1) continue;
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                grid[cursor[ty * W + tx]++] = i;
            }
        }
    }

    L->poly_grid     = grid;
    L->poly_grid_off = off;
    LOG_I("level: built poly broadphase (%d polys, %d entries across %d cells)",
          L->poly_count, total, n_cells);
    return LVL_OK;
}

/* ---- Lump lookup -------------------------------------------------- */

typedef struct {
    uint32_t offset;
    uint32_t size;
    int      present;
} LumpRef;

static int find_lump(const uint8_t *file_buf, int file_size,
                     int dir_off, int section_count,
                     const char *name, LumpRef *out) {
    out->present = 0;
    for (int i = 0; i < section_count; ++i) {
        int e = dir_off + i * LVL_DIR_ENTRY_BYTES;
        if (e + LVL_DIR_ENTRY_BYTES > file_size) return 0;
        if (name_eq(file_buf + e + DIR_OFF_NAME, name)) {
            out->offset  = r_u32(file_buf + e + DIR_OFF_OFFSET);
            out->size    = r_u32(file_buf + e + DIR_OFF_SIZE);
            out->present = 1;
            return 1;
        }
    }
    return 0;
}

/* ---- Loader ------------------------------------------------------- */

LvlResult level_load(struct World *world, struct Arena *arena,
                     const char *path) {
    if (!world || !arena || !path) return LVL_ERR_IO;

    FILE *f = fopen(path, "rb");
    if (!f) return LVL_ERR_FILE_NOT_FOUND;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return LVL_ERR_IO; }
    long flen = ftell(f);
    if (flen < 0) { fclose(f); return LVL_ERR_IO; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return LVL_ERR_IO; }

    if ((size_t)flen > LVL_FILE_MAX_BYTES) {
        fclose(f);
        return LVL_ERR_TOO_LARGE;
    }
    if (flen < LVL_HEADER_BYTES) {
        fclose(f);
        return LVL_ERR_BAD_DIRECTORY;
    }

    uint8_t *buf = (uint8_t *)arena_alloc(arena, (size_t)flen);
    if (!buf) { fclose(f); return LVL_ERR_OOM; }

    size_t got = fread(buf, 1, (size_t)flen, f);
    fclose(f);
    if (got != (size_t)flen) return LVL_ERR_IO;

    int file_size = (int)flen;

    /* 1. Magic */
    if (buf[HDR_OFF_MAGIC + 0] != LVL_MAGIC0 ||
        buf[HDR_OFF_MAGIC + 1] != LVL_MAGIC1 ||
        buf[HDR_OFF_MAGIC + 2] != LVL_MAGIC2 ||
        buf[HDR_OFF_MAGIC + 3] != LVL_MAGIC3) {
        return LVL_ERR_BAD_MAGIC;
    }

    /* 2. Version */
    uint32_t version = r_u32(buf + HDR_OFF_VERSION);
    if (version == 0 || version > LVL_VERSION_CURRENT) return LVL_ERR_BAD_VERSION;

    /* 3. CRC */
    uint32_t stored_crc = r_u32(buf + HDR_OFF_CRC32);
    uint32_t actual_crc = crc32_with_zeroed_window(buf, file_size, HDR_OFF_CRC32);
    if (stored_crc != actual_crc) return LVL_ERR_BAD_CRC;

    /* Header values now trusted enough to drive structural reads. */
    uint32_t section_count = r_u32(buf + HDR_OFF_SECTION_COUNT);
    uint32_t world_w       = r_u32(buf + HDR_OFF_WORLD_W);
    uint32_t world_h       = r_u32(buf + HDR_OFF_WORLD_H);
    uint32_t tile_size     = r_u32(buf + HDR_OFF_TILE_SIZE);
    uint32_t strt_off      = r_u32(buf + HDR_OFF_STRT_OFF);
    uint32_t strt_size     = r_u32(buf + HDR_OFF_STRT_SIZE);

    /* Sanity bounds. */
    if (world_w == 0 || world_w > 512 || world_h == 0 || world_h > 512) {
        return LVL_ERR_BAD_SECTION;
    }
    if (tile_size != 32) return LVL_ERR_BAD_SECTION;

    /* 4. Lump directory bounds — the directory itself + each entry. */
    int dir_off = LVL_HEADER_BYTES;
    if ((uint64_t)dir_off + (uint64_t)section_count * LVL_DIR_ENTRY_BYTES
        > (uint64_t)file_size) {
        return LVL_ERR_BAD_DIRECTORY;
    }
    for (uint32_t i = 0; i < section_count; ++i) {
        int e = dir_off + (int)i * LVL_DIR_ENTRY_BYTES;
        uint32_t lo = r_u32(buf + e + DIR_OFF_OFFSET);
        uint32_t ls = r_u32(buf + e + DIR_OFF_SIZE);
        if ((uint64_t)lo + (uint64_t)ls > (uint64_t)file_size) {
            return LVL_ERR_BAD_DIRECTORY;
        }
    }

    /* String table — referenced by other sections. */
    if (strt_off > 0 || strt_size > 0) {
        if ((uint64_t)strt_off + (uint64_t)strt_size > (uint64_t)file_size) {
            return LVL_ERR_BAD_DIRECTORY;
        }
    }

    /* Reset Level fields — about to repopulate from the file. The
     * caller is expected to have arena-reset before calling us, so
     * stale pointers in `world->level` aren't pointing into freed
     * memory. */
    Level *L = &world->level;
    memset(L, 0, sizeof(*L));
    L->width     = (int)world_w;
    L->height    = (int)world_h;
    L->tile_size = (int)tile_size;
    L->gravity   = (Vec2){ 0.0f, 1080.0f };
    L->string_table      = strt_size > 0 ? (const char *)(buf + strt_off) : "";
    L->string_table_size = (int)strt_size;

    /* 5. Per-section decode. Required: TILE, SPWN, META. */

    /* TILE — required. */
    {
        LumpRef ref;
        if (!find_lump(buf, file_size, dir_off, (int)section_count, "TILE", &ref)) {
            return LVL_ERR_BAD_SECTION;
        }
        if (ref.size != world_w * world_h * (uint32_t)LVL_TILE_REC_BYTES) {
            return LVL_ERR_BAD_SECTION;
        }
        int n = (int)(world_w * world_h);
        L->tiles = (LvlTile *)arena_alloc(arena, sizeof(LvlTile) * (size_t)n);
        if (!L->tiles) return LVL_ERR_OOM;
        for (int i = 0; i < n; ++i) {
            decode_tile(buf + ref.offset + i * LVL_TILE_REC_BYTES, &L->tiles[i]);
        }
    }

    /* SPWN — required. */
    {
        LumpRef ref;
        if (!find_lump(buf, file_size, dir_off, (int)section_count, "SPWN", &ref)) {
            return LVL_ERR_BAD_SECTION;
        }
        if (ref.size % LVL_SPAWN_REC_BYTES != 0) return LVL_ERR_BAD_SECTION;
        int n = (int)ref.size / LVL_SPAWN_REC_BYTES;
        L->spawn_count = n;
        if (n > 0) {
            L->spawns = (LvlSpawn *)arena_alloc(arena, sizeof(LvlSpawn) * (size_t)n);
            if (!L->spawns) return LVL_ERR_OOM;
            for (int i = 0; i < n; ++i) {
                decode_spawn(buf + ref.offset + i * LVL_SPAWN_REC_BYTES, &L->spawns[i]);
            }
        }
    }

    /* META — required. */
    {
        LumpRef ref;
        if (!find_lump(buf, file_size, dir_off, (int)section_count, "META", &ref)) {
            return LVL_ERR_BAD_SECTION;
        }
        if (ref.size != LVL_META_REC_BYTES) return LVL_ERR_BAD_SECTION;
        decode_meta(buf + ref.offset, &L->meta);
    }

    /* POLY — optional. */
    {
        LumpRef ref;
        if (find_lump(buf, file_size, dir_off, (int)section_count, "POLY", &ref)) {
            if (ref.size % LVL_POLY_REC_BYTES != 0) return LVL_ERR_BAD_SECTION;
            int n = (int)ref.size / LVL_POLY_REC_BYTES;
            L->poly_count = n;
            if (n > 0) {
                L->polys = (LvlPoly *)arena_alloc(arena, sizeof(LvlPoly) * (size_t)n);
                if (!L->polys) return LVL_ERR_OOM;
                for (int i = 0; i < n; ++i) {
                    decode_poly(buf + ref.offset + i * LVL_POLY_REC_BYTES, &L->polys[i]);
                }
            }
        }
    }

    /* P02: build the polygon broadphase grid. Skipped when poly_count
     * is zero. */
    {
        LvlResult r = level_build_poly_broadphase(L, arena);
        if (r != LVL_OK) return r;
    }

    /* PICK — optional. */
    {
        LumpRef ref;
        if (find_lump(buf, file_size, dir_off, (int)section_count, "PICK", &ref)) {
            if (ref.size % LVL_PICKUP_REC_BYTES != 0) return LVL_ERR_BAD_SECTION;
            int n = (int)ref.size / LVL_PICKUP_REC_BYTES;
            L->pickup_count = n;
            if (n > 0) {
                L->pickups = (LvlPickup *)arena_alloc(arena, sizeof(LvlPickup) * (size_t)n);
                if (!L->pickups) return LVL_ERR_OOM;
                for (int i = 0; i < n; ++i) {
                    decode_pickup(buf + ref.offset + i * LVL_PICKUP_REC_BYTES, &L->pickups[i]);
                }
            }
        }
    }

    /* DECO — optional. */
    {
        LumpRef ref;
        if (find_lump(buf, file_size, dir_off, (int)section_count, "DECO", &ref)) {
            if (ref.size % LVL_DECO_REC_BYTES != 0) return LVL_ERR_BAD_SECTION;
            int n = (int)ref.size / LVL_DECO_REC_BYTES;
            L->deco_count = n;
            if (n > 0) {
                L->decos = (LvlDeco *)arena_alloc(arena, sizeof(LvlDeco) * (size_t)n);
                if (!L->decos) return LVL_ERR_OOM;
                for (int i = 0; i < n; ++i) {
                    decode_deco(buf + ref.offset + i * LVL_DECO_REC_BYTES, &L->decos[i]);
                }
            }
        }
    }

    /* AMBI — optional. */
    {
        LumpRef ref;
        if (find_lump(buf, file_size, dir_off, (int)section_count, "AMBI", &ref)) {
            if (ref.size % LVL_AMBI_REC_BYTES != 0) return LVL_ERR_BAD_SECTION;
            int n = (int)ref.size / LVL_AMBI_REC_BYTES;
            L->ambi_count = n;
            if (n > 0) {
                L->ambis = (LvlAmbi *)arena_alloc(arena, sizeof(LvlAmbi) * (size_t)n);
                if (!L->ambis) return LVL_ERR_OOM;
                for (int i = 0; i < n; ++i) {
                    decode_ambi(buf + ref.offset + i * LVL_AMBI_REC_BYTES, &L->ambis[i]);
                }
            }
        }
    }

    /* FLAG — optional. */
    {
        LumpRef ref;
        if (find_lump(buf, file_size, dir_off, (int)section_count, "FLAG", &ref)) {
            if (ref.size % LVL_FLAG_REC_BYTES != 0) return LVL_ERR_BAD_SECTION;
            int n = (int)ref.size / LVL_FLAG_REC_BYTES;
            L->flag_count = n;
            if (n > 0) {
                L->flags = (LvlFlag *)arena_alloc(arena, sizeof(LvlFlag) * (size_t)n);
                if (!L->flags) return LVL_ERR_OOM;
                for (int i = 0; i < n; ++i) {
                    decode_flag(buf + ref.offset + i * LVL_FLAG_REC_BYTES, &L->flags[i]);
                }
            }
        }
    }

    /* THMB — optional. Raw PNG bytes for the lobby preview thumbnail.
     * We don't decode here — just hand a pointer into the arena-owned
     * file buffer back to callers that want to decode (lobby_ui uses
     * raylib's LoadImageFromMemory). */
    {
        LumpRef ref;
        if (find_lump(buf, file_size, dir_off, (int)section_count, "THMB", &ref)) {
            if (ref.size > 0) {
                L->thumb_png_data = buf + ref.offset;
                L->thumb_png_size = (int)ref.size;
            }
        }
    }

    LOG_I("level: loaded %s (%dx%d, %d polys, %d spawns, %d pickups)",
          path, L->width, L->height, L->poly_count, L->spawn_count, L->pickup_count);
    return LVL_OK;
}

/* ---- Saver -------------------------------------------------------- */

LvlResult level_save(const struct World *world, struct Arena *scratch,
                     const char *path) {
    if (!world || !scratch || !path) return LVL_ERR_IO;
    const Level *L = &world->level;

    if (L->width  <= 0 || L->width  > 512) {
        LOG_E("level_save: width %d out of range", L->width);
        return LVL_ERR_BAD_SECTION;
    }
    if (L->height <= 0 || L->height > 512) {
        LOG_E("level_save: height %d out of range", L->height);
        return LVL_ERR_BAD_SECTION;
    }
    if (L->tile_size != 32) {
        LOG_E("level_save: tile_size %d (expected 32)", L->tile_size);
        return LVL_ERR_BAD_SECTION;
    }

    /* String table source. If the level's string_table is NULL/empty
     * we still emit a single zero byte (offset 0 = empty string). */
    const char *strt_src  = (L->string_table && L->string_table_size > 0)
                                ? L->string_table : "\0";
    int         strt_size = (L->string_table && L->string_table_size > 0)
                                ? L->string_table_size : 1;

    /* Section byte sizes. */
    int tile_n   = L->width * L->height;
    int tile_sz  = tile_n           * LVL_TILE_REC_BYTES;
    int poly_sz  = L->poly_count    * LVL_POLY_REC_BYTES;
    int spwn_sz  = L->spawn_count   * LVL_SPAWN_REC_BYTES;
    int pick_sz  = L->pickup_count  * LVL_PICKUP_REC_BYTES;
    int deco_sz  = L->deco_count    * LVL_DECO_REC_BYTES;
    int ambi_sz  = L->ambi_count    * LVL_AMBI_REC_BYTES;
    int flag_sz  = L->flag_count    * LVL_FLAG_REC_BYTES;
    int meta_sz  = LVL_META_REC_BYTES;
    int strt_sz  = strt_size;
    int thmb_sz  = (L->thumb_png_data && L->thumb_png_size > 0)
                     ? L->thumb_png_size : 0;

    /* Always emit TILE / SPWN / META / STRT (required + STRT). Optional
     * sections only get a directory entry if they have records. */
    int section_count = 4; /* TILE, SPWN, META, STRT */
    if (poly_sz > 0) ++section_count;
    if (pick_sz > 0) ++section_count;
    if (deco_sz > 0) ++section_count;
    if (ambi_sz > 0) ++section_count;
    if (flag_sz > 0) ++section_count;
    if (thmb_sz > 0) ++section_count;

    int dir_size = section_count * LVL_DIR_ENTRY_BYTES;
    int dir_off  = LVL_HEADER_BYTES;
    int payload_off = dir_off + dir_size;

    /* Lay out each section's offset right after the directory. */
    int tile_off = payload_off;             payload_off += tile_sz;
    int poly_off = (poly_sz > 0) ? payload_off : 0; payload_off += poly_sz;
    int spwn_off = payload_off;             payload_off += spwn_sz;
    int pick_off = (pick_sz > 0) ? payload_off : 0; payload_off += pick_sz;
    int deco_off = (deco_sz > 0) ? payload_off : 0; payload_off += deco_sz;
    int ambi_off = (ambi_sz > 0) ? payload_off : 0; payload_off += ambi_sz;
    int flag_off = (flag_sz > 0) ? payload_off : 0; payload_off += flag_sz;
    int meta_off = payload_off;             payload_off += meta_sz;
    int strt_off = payload_off;             payload_off += strt_sz;
    int thmb_off = (thmb_sz > 0) ? payload_off : 0; payload_off += thmb_sz;

    int total = payload_off;
    if ((size_t)total > LVL_FILE_MAX_BYTES) return LVL_ERR_TOO_LARGE;

    uint8_t *out = (uint8_t *)arena_alloc(scratch, (size_t)total);
    if (!out) return LVL_ERR_OOM;
    memset(out, 0, (size_t)total);

    /* Header. */
    out[HDR_OFF_MAGIC + 0] = LVL_MAGIC0;
    out[HDR_OFF_MAGIC + 1] = LVL_MAGIC1;
    out[HDR_OFF_MAGIC + 2] = LVL_MAGIC2;
    out[HDR_OFF_MAGIC + 3] = LVL_MAGIC3;
    w_u32(out + HDR_OFF_VERSION,        LVL_VERSION_CURRENT);
    w_u32(out + HDR_OFF_SECTION_COUNT,  (uint32_t)section_count);
    w_u32(out + HDR_OFF_FLAGS,          0);
    w_u32(out + HDR_OFF_WORLD_W,        (uint32_t)L->width);
    w_u32(out + HDR_OFF_WORLD_H,        (uint32_t)L->height);
    w_u32(out + HDR_OFF_TILE_SIZE,      (uint32_t)L->tile_size);
    w_u32(out + HDR_OFF_STRT_OFF,       (uint32_t)strt_off);
    w_u32(out + HDR_OFF_STRT_SIZE,      (uint32_t)strt_sz);
    w_u32(out + HDR_OFF_CRC32,          0);   /* zero now; patched after CRC */
    /* reserved[24] left at 0 by memset */

    /* Lump directory. Order: TILE, POLY?, SPWN, PICK?, DECO?, AMBI?, FLAG?, META, STRT. */
    int e = dir_off;
    name_pack(out + e + DIR_OFF_NAME, "TILE");
    w_u32(out + e + DIR_OFF_OFFSET, (uint32_t)tile_off);
    w_u32(out + e + DIR_OFF_SIZE,   (uint32_t)tile_sz);
    e += LVL_DIR_ENTRY_BYTES;

    if (poly_sz > 0) {
        name_pack(out + e + DIR_OFF_NAME, "POLY");
        w_u32(out + e + DIR_OFF_OFFSET, (uint32_t)poly_off);
        w_u32(out + e + DIR_OFF_SIZE,   (uint32_t)poly_sz);
        e += LVL_DIR_ENTRY_BYTES;
    }
    name_pack(out + e + DIR_OFF_NAME, "SPWN");
    w_u32(out + e + DIR_OFF_OFFSET, (uint32_t)spwn_off);
    w_u32(out + e + DIR_OFF_SIZE,   (uint32_t)spwn_sz);
    e += LVL_DIR_ENTRY_BYTES;

    if (pick_sz > 0) {
        name_pack(out + e + DIR_OFF_NAME, "PICK");
        w_u32(out + e + DIR_OFF_OFFSET, (uint32_t)pick_off);
        w_u32(out + e + DIR_OFF_SIZE,   (uint32_t)pick_sz);
        e += LVL_DIR_ENTRY_BYTES;
    }
    if (deco_sz > 0) {
        name_pack(out + e + DIR_OFF_NAME, "DECO");
        w_u32(out + e + DIR_OFF_OFFSET, (uint32_t)deco_off);
        w_u32(out + e + DIR_OFF_SIZE,   (uint32_t)deco_sz);
        e += LVL_DIR_ENTRY_BYTES;
    }
    if (ambi_sz > 0) {
        name_pack(out + e + DIR_OFF_NAME, "AMBI");
        w_u32(out + e + DIR_OFF_OFFSET, (uint32_t)ambi_off);
        w_u32(out + e + DIR_OFF_SIZE,   (uint32_t)ambi_sz);
        e += LVL_DIR_ENTRY_BYTES;
    }
    if (flag_sz > 0) {
        name_pack(out + e + DIR_OFF_NAME, "FLAG");
        w_u32(out + e + DIR_OFF_OFFSET, (uint32_t)flag_off);
        w_u32(out + e + DIR_OFF_SIZE,   (uint32_t)flag_sz);
        e += LVL_DIR_ENTRY_BYTES;
    }
    name_pack(out + e + DIR_OFF_NAME, "META");
    w_u32(out + e + DIR_OFF_OFFSET, (uint32_t)meta_off);
    w_u32(out + e + DIR_OFF_SIZE,   (uint32_t)meta_sz);
    e += LVL_DIR_ENTRY_BYTES;

    name_pack(out + e + DIR_OFF_NAME, "STRT");
    w_u32(out + e + DIR_OFF_OFFSET, (uint32_t)strt_off);
    w_u32(out + e + DIR_OFF_SIZE,   (uint32_t)strt_sz);
    e += LVL_DIR_ENTRY_BYTES;

    if (thmb_sz > 0) {
        name_pack(out + e + DIR_OFF_NAME, "THMB");
        w_u32(out + e + DIR_OFF_OFFSET, (uint32_t)thmb_off);
        w_u32(out + e + DIR_OFF_SIZE,   (uint32_t)thmb_sz);
        e += LVL_DIR_ENTRY_BYTES;
    }

    /* Payloads. */
    for (int i = 0; i < tile_n; ++i) {
        encode_tile(out + tile_off + i * LVL_TILE_REC_BYTES, &L->tiles[i]);
    }
    for (int i = 0; i < L->poly_count; ++i) {
        encode_poly(out + poly_off + i * LVL_POLY_REC_BYTES, &L->polys[i]);
    }
    for (int i = 0; i < L->spawn_count; ++i) {
        encode_spawn(out + spwn_off + i * LVL_SPAWN_REC_BYTES, &L->spawns[i]);
    }
    for (int i = 0; i < L->pickup_count; ++i) {
        encode_pickup(out + pick_off + i * LVL_PICKUP_REC_BYTES, &L->pickups[i]);
    }
    for (int i = 0; i < L->deco_count; ++i) {
        encode_deco(out + deco_off + i * LVL_DECO_REC_BYTES, &L->decos[i]);
    }
    for (int i = 0; i < L->ambi_count; ++i) {
        encode_ambi(out + ambi_off + i * LVL_AMBI_REC_BYTES, &L->ambis[i]);
    }
    for (int i = 0; i < L->flag_count; ++i) {
        encode_flag(out + flag_off + i * LVL_FLAG_REC_BYTES, &L->flags[i]);
    }
    encode_meta(out + meta_off, &L->meta);
    memcpy(out + strt_off, strt_src, (size_t)strt_sz);
    if (thmb_sz > 0) {
        memcpy(out + thmb_off, L->thumb_png_data, (size_t)thmb_sz);
    }

    /* CRC over the whole file with the CRC field as zeros (it already
     * is — we wrote 0 above). */
    uint32_t crc = level_crc32(out, total);
    w_u32(out + HDR_OFF_CRC32, crc);

    /* Write to disk. */
    FILE *f = fopen(path, "wb");
    if (!f) return LVL_ERR_IO;
    size_t wrote = fwrite(out, 1, (size_t)total, f);
    fclose(f);
    if (wrote != (size_t)total) return LVL_ERR_IO;

    LOG_I("level: saved %s (%d bytes, crc=%08x)", path, total, crc);
    return LVL_OK;
}

/* ---- Light THMB-only reader --------------------------------------- */

uint8_t *level_io_read_thumb(const char *path, int *out_size) {
    if (out_size) *out_size = 0;
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long flen = ftell(f);
    if (flen < LVL_HEADER_BYTES || (size_t)flen > LVL_FILE_MAX_BYTES) {
        fclose(f); return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint8_t *buf = (uint8_t *)malloc((size_t)flen);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)flen, f);
    fclose(f);
    if (got != (size_t)flen) { free(buf); return NULL; }

    int file_size = (int)flen;
    if (buf[HDR_OFF_MAGIC + 0] != LVL_MAGIC0 ||
        buf[HDR_OFF_MAGIC + 1] != LVL_MAGIC1 ||
        buf[HDR_OFF_MAGIC + 2] != LVL_MAGIC2 ||
        buf[HDR_OFF_MAGIC + 3] != LVL_MAGIC3) {
        free(buf); return NULL;
    }

    int section_count = (int)r_u32(buf + HDR_OFF_SECTION_COUNT);
    int dir_off = LVL_HEADER_BYTES;
    if ((int64_t)dir_off + (int64_t)section_count * LVL_DIR_ENTRY_BYTES
        > (int64_t)file_size) {
        free(buf); return NULL;
    }

    LumpRef ref;
    if (!find_lump(buf, file_size, dir_off, section_count, "THMB", &ref) ||
        ref.size == 0) {
        free(buf); return NULL;
    }
    if ((int64_t)ref.offset + (int64_t)ref.size > (int64_t)file_size) {
        free(buf); return NULL;
    }

    /* Copy out of the file buffer so the caller doesn't have to keep
     * the whole .lvl in memory. */
    uint8_t *thumb = (uint8_t *)malloc(ref.size);
    if (!thumb) { free(buf); return NULL; }
    memcpy(thumb, buf + ref.offset, ref.size);
    free(buf);

    if (out_size) *out_size = (int)ref.size;
    return thumb;
}
