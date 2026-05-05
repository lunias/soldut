#pragma once

#include "net.h"          /* MapDescriptor, NET_MAP_* limits */

#include <stdbool.h>
#include <stdint.h>

/*
 * map_download — client-side reassembly of a streamed .lvl file.
 *
 * One-at-a-time: a client downloads at most one map per process. The
 * buffer + chunk bitmap are allocated up front on the permanent arena
 * (sized to NET_MAP_MAX_FILE_BYTES so it can be reused across rounds
 * without reallocation, per Rule 3 in 01-philosophy.md).
 *
 * Lifecycle:
 *   map_download_init(d, permanent_arena)         — once at process init
 *   map_download_begin(d, desc)                   — start receiving for descriptor
 *   map_download_apply_chunk(d, off, data, len)   — handle a NET_MSG_MAP_CHUNK
 *   map_download_is_complete(d)                   — buffer fully populated?
 *   map_download_cancel(d)                        — clear active flag, keep buffer for reuse
 */

struct Arena;

/* Bit-per-chunk array. NET_MAP_MAX_FILE_BYTES / NET_MAP_CHUNK_PAYLOAD =
 * 2*1024*1024 / 1180 = ~1778 chunks; round up to 1792 (224 bytes). The
 * static-assert below ensures we don't undercount if the constants
 * shift. */
#define MAP_DOWNLOAD_MAX_CHUNKS  1792

typedef struct MapDownload {
    bool          active;
    bool          complete;
    uint32_t      crc32;
    uint32_t      total_size;
    uint32_t      bytes_received;     /* unique bytes only; duplicates ignored */
    uint32_t      chunk_count;        /* total chunks expected */
    double        last_progress_seconds;   /* wall-clock; for stall watchdog */

    uint8_t      *buffer;             /* permanent_arena alloc, NET_MAP_MAX_FILE_BYTES */
    uint8_t      *received_bits;      /* permanent_arena alloc, MAP_DOWNLOAD_MAX_CHUNKS/8 */

    /* Snapshotted descriptor for the active download (so net.c can
     * rebuild MAP_REQUEST messages without re-reading the in-flight
     * INITIAL_STATE). */
    MapDescriptor desc;
} MapDownload;

/* One-time setup at process init. Allocates buffer + bitmap on the
 * permanent arena. Returns false if the arena is too small to satisfy. */
bool map_download_init(MapDownload *d, struct Arena *permanent);

/* Begin reassembling a new download. Resets buffer + bitmap; copies the
 * descriptor. Pre-condition: map_download_init has been called.
 * Returns false on bad descriptor (size > cap, etc). */
bool map_download_begin(MapDownload *d, const MapDescriptor *desc, double now_seconds);

/* Apply a chunk (already validated for size + offset bounds by caller).
 * Updates bytes_received iff this chunk wasn't previously seen.
 * Returns true if this is the chunk that completes the download
 * (caller should run finalize). */
bool map_download_apply_chunk(MapDownload *d,
                              uint32_t chunk_offset,
                              const uint8_t *data,
                              uint32_t chunk_len,
                              double now_seconds);

void map_download_cancel(MapDownload *d);

/* Returns 0..100 — for the lobby UI progress bar. */
int  map_download_progress_pct(const MapDownload *d);

/* Stall watchdog: returns true if active and (now - last_progress) > timeout.
 * Caller (per-frame in net poll) decides what to do (cancel + drop). */
bool map_download_is_stalled(const MapDownload *d,
                             double now_seconds,
                             double timeout_seconds);
