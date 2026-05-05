#include "map_download.h"

#include "arena.h"
#include "log.h"

#include <assert.h>
#include <string.h>

/* Bit array helpers. */
static inline bool bit_get(const uint8_t *bits, uint32_t i) {
    return (bits[i >> 3] >> (i & 7)) & 1u;
}
static inline void bit_set(uint8_t *bits, uint32_t i) {
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

/* Compile-time sanity: the bit array MUST cover the worst-case chunk
 * count for a max-size file. If the chunk payload shrinks in a future
 * pass, this trips. */
#if (((NET_MAP_MAX_FILE_BYTES + NET_MAP_CHUNK_PAYLOAD - 1) / NET_MAP_CHUNK_PAYLOAD) > MAP_DOWNLOAD_MAX_CHUNKS)
#  error "MAP_DOWNLOAD_MAX_CHUNKS too small for NET_MAP_MAX_FILE_BYTES"
#endif

bool map_download_init(MapDownload *d, struct Arena *permanent) {
    if (!d || !permanent) return false;
    memset(d, 0, sizeof(*d));
    d->buffer        = (uint8_t *)arena_alloc(permanent, NET_MAP_MAX_FILE_BYTES);
    d->received_bits = (uint8_t *)arena_alloc(permanent, MAP_DOWNLOAD_MAX_CHUNKS / 8);
    if (!d->buffer || !d->received_bits) {
        LOG_E("map_download_init: arena out of memory");
        return false;
    }
    return true;
}

bool map_download_begin(MapDownload *d, const MapDescriptor *desc, double now_seconds) {
    if (!d || !desc || !d->buffer || !d->received_bits) return false;
    if (desc->size_bytes == 0)        return false;       /* code-built marker */
    if (desc->size_bytes > NET_MAP_MAX_FILE_BYTES) {
        LOG_E("map_download_begin: descriptor size %u > cap %u",
              (unsigned)desc->size_bytes, (unsigned)NET_MAP_MAX_FILE_BYTES);
        return false;
    }
    d->active                = true;
    d->complete              = false;
    d->crc32                 = desc->crc32;
    d->total_size            = desc->size_bytes;
    d->bytes_received        = 0;
    d->chunk_count           = (desc->size_bytes + NET_MAP_CHUNK_PAYLOAD - 1)
                               / NET_MAP_CHUNK_PAYLOAD;
    d->last_progress_seconds = now_seconds;
    d->desc                  = *desc;
    /* Wipe the bitmap covering the chunks we'll receive (cheap; 224 B
     * worst case). Don't memset the whole buffer — fwrite at finalize
     * size_bytes will be authoritative. */
    memset(d->received_bits, 0, (size_t)((d->chunk_count + 7) / 8));
    return true;
}

bool map_download_apply_chunk(MapDownload *d,
                              uint32_t chunk_offset,
                              const uint8_t *data,
                              uint32_t chunk_len,
                              double now_seconds)
{
    if (!d || !d->active || !data) return false;
    if (chunk_len == 0)                                   return false;
    if (chunk_len > NET_MAP_CHUNK_PAYLOAD)                return false;
    if ((uint64_t)chunk_offset + (uint64_t)chunk_len
            > (uint64_t)d->total_size)                    return false;

    /* Chunk index = offset / payload size. The server's loop steps in
     * fixed-size strides (see net.c::server_handle_map_request) so an
     * offset that's not a multiple of payload size means the server
     * sent something we don't expect — reject defensively. */
    if (chunk_offset % NET_MAP_CHUNK_PAYLOAD != 0)        return false;
    uint32_t idx = chunk_offset / NET_MAP_CHUNK_PAYLOAD;
    if (idx >= d->chunk_count)                            return false;

    /* Last chunk may be short; all others must be full size. */
    bool is_last_chunk = (idx + 1 == d->chunk_count);
    uint32_t expect    = is_last_chunk
                       ? (d->total_size - chunk_offset)
                       : (uint32_t)NET_MAP_CHUNK_PAYLOAD;
    if (chunk_len != expect)                              return false;

    if (bit_get(d->received_bits, idx)) {
        /* Duplicate — ignore. Don't bump bytes_received. */
        return false;
    }
    memcpy(d->buffer + chunk_offset, data, chunk_len);
    bit_set(d->received_bits, idx);
    d->bytes_received        += chunk_len;
    d->last_progress_seconds = now_seconds;

    if (d->bytes_received == d->total_size) {
        d->complete = true;
        return true;
    }
    return false;
}

void map_download_cancel(MapDownload *d) {
    if (!d) return;
    d->active   = false;
    d->complete = false;
    /* Leave buffer + received_bits intact; map_download_begin clears
     * them again on next start, and the permanent-arena alloc must be
     * preserved for reuse. */
}

int map_download_progress_pct(const MapDownload *d) {
    if (!d || !d->active || d->total_size == 0) return 0;
    uint64_t pct = (uint64_t)d->bytes_received * 100u / d->total_size;
    if (pct > 100) pct = 100;
    return (int)pct;
}

bool map_download_is_stalled(const MapDownload *d,
                             double now_seconds,
                             double timeout_seconds)
{
    if (!d || !d->active || d->complete) return false;
    return (now_seconds - d->last_progress_seconds) > timeout_seconds;
}
