#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * map_cache — client-side content-addressed cache of downloaded .lvl files.
 *
 * Files in the cache live at <cache_dir>/<crc32_hex>.lvl. Two servers
 * shipping different content with the same short_name don't collide
 * because the lookup is by CRC.
 *
 * Cache directory by platform (per documents/m5/10-map-sharing.md):
 *   Linux : $XDG_DATA_HOME/soldut/maps/   (default ~/.local/share/soldut/maps/)
 *   macOS : ~/Library/Application Support/Soldut/maps/
 *   Win   : %APPDATA%/Soldut/maps/
 *
 * Created lazily on first call to map_cache_init() and on first write.
 * Module is intentionally tiny — no dynamic indexing, no manifest. The
 * filesystem IS the index (one syscall to stat + read).
 */

bool        map_cache_init(void);                      /* idempotent */
bool        map_cache_has(uint32_t crc32);             /* file exists at <cache>/<crc>.lvl */
const char *map_cache_path(uint32_t crc32);            /* shared static buffer; not thread-safe */
bool        map_cache_write(uint32_t crc32,
                            const uint8_t *data,
                            uint32_t size);            /* atomic via .tmp + rename */
void        map_cache_evict_lru(uint64_t cap_bytes);   /* delete oldest .lvl files until total <= cap */

/* Read raw bytes of a cached map into `dst`. Returns the number of bytes
 * read (== file size on success), or -1 on error. Caller must size `dst`
 * to >= the expected file size. */
int         map_cache_read(uint32_t crc32, uint8_t *dst, int dst_cap);

/* Lookup the absolute path for a shipped (`assets/maps/<short>.lvl`) map
 * regardless of cwd. Used by the client's resolve path before the
 * download cache. Returns true if the file exists. `out` receives the
 * resolved path (>= 256 bytes recommended). */
bool        map_cache_assets_path(const char *short_name,
                                  char *out, size_t out_cap);

/* Read a .lvl file's CRC32 directly from its header (offset 36, 4 bytes
 * little-endian). Returns 0 if the file is missing / too short / can't
 * be read. Used by the client's "do I already have this map?" probe. */
uint32_t    map_cache_file_crc(const char *path);

/* Read a .lvl file's size in bytes. Returns 0 on error. */
uint32_t    map_cache_file_size(const char *path);
