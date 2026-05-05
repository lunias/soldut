/*
 * tests/map_chunk_test.c — unit test for map_download chunk reassembly.
 *
 * Covers:
 *  - in-order chunk reassembly to total_size
 *  - duplicate chunk ignored (bytes_received doesn't double-count)
 *  - out-of-bounds chunk rejected
 *  - non-aligned offset rejected
 *  - bit-flip in reassembled buffer fails CRC verification
 *
 * No raylib, no ENet — pure data-path tests against the public API.
 */

#include "../src/arena.h"
#include "../src/level_io.h"
#include "../src/log.h"
#include "../src/map_download.h"
#include "../src/net.h"
#include "../src/world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* getpid */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); } \
} while (0)

/* Build a synthetic .lvl in-memory (3 KB) and return its CRC. The
 * level_save path requires an Arena + a World stub; we mirror the
 * synth_map.c shape but write to the arena buffer rather than disk. */
static uint32_t build_synth_buffer(Arena *scratch,
                                   uint8_t **out_buf,
                                   uint32_t *out_size)
{
    static World w;
    memset(&w, 0, sizeof(w));
    Level *L = &w.level;
    L->width = 16; L->height = 12; L->tile_size = 32;

    int n = L->width * L->height;
    L->tiles = (LvlTile *)arena_alloc(scratch, sizeof(LvlTile) * (size_t)n);
    memset(L->tiles, 0, sizeof(LvlTile) * (size_t)n);
    for (int x = 0; x < L->width; ++x) {
        L->tiles[(L->height - 1) * L->width + x].flags = TILE_F_SOLID;
    }
    L->spawns = (LvlSpawn *)arena_alloc(scratch, sizeof(LvlSpawn));
    L->spawn_count = 1;
    L->spawns[0] = (LvlSpawn){ .pos_x = 96, .pos_y = 320, .team = 1 };
    L->meta.mode_mask = 0x3;

    /* Save to a temp path then read it back into the arena. */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/soldut_chunk_test_%d.lvl", (int)getpid());
    LvlResult r = level_save(&w, scratch, tmp_path);
    if (r != LVL_OK) {
        fprintf(stderr, "build_synth_buffer: level_save failed: %s\n",
                level_io_result_str(r));
        return 0;
    }
    FILE *f = fopen(tmp_path, "rb");
    if (!f) { perror("fopen"); return 0; }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)arena_alloc(scratch, (size_t)flen);
    if (fread(buf, 1, (size_t)flen, f) != (size_t)flen) {
        fclose(f);
        return 0;
    }
    fclose(f);
    remove(tmp_path);

    /* CRC is at offset 36; computed with the field zeroed. */
    uint32_t stored_crc;
    memcpy(&stored_crc, buf + 36, 4);
    *out_buf  = buf;
    *out_size = (uint32_t)flen;
    return stored_crc;
}

int main(void) {
    Arena scratch = {0};
    arena_init(&scratch, 8 * 1024 * 1024, "scratch");

    Arena permanent = {0};
    arena_init(&permanent, 4 * 1024 * 1024, "permanent");

    uint8_t *src = NULL;
    uint32_t total = 0;
    uint32_t expected_crc = build_synth_buffer(&scratch, &src, &total);
    CHECK(expected_crc != 0, "synth buffer has non-zero CRC");
    CHECK(total > 64, "synth buffer is at least header-sized");

    /* Verify our buffer-CRC helper agrees with the on-disk CRC. */
    uint32_t computed_crc = level_compute_buffer_crc(src, (int)total);
    CHECK(computed_crc == expected_crc, "level_compute_buffer_crc round-trips");

    MapDownload d;
    bool ok = map_download_init(&d, &permanent);
    CHECK(ok, "map_download_init succeeds");

    MapDescriptor desc = {0};
    desc.crc32      = expected_crc;
    desc.size_bytes = total;
    desc.short_name_len = 5;
    memcpy(desc.short_name, "synth", 6);

    ok = map_download_begin(&d, &desc, 0.0);
    CHECK(ok, "map_download_begin succeeds");
    CHECK(d.active && !d.complete, "active+!complete after begin");
    CHECK(d.total_size == total, "total_size matches descriptor");

    uint32_t expect_chunks = (total + NET_MAP_CHUNK_PAYLOAD - 1) / NET_MAP_CHUNK_PAYLOAD;
    CHECK(d.chunk_count == expect_chunks, "chunk_count derived correctly");

    /* Stream chunks in order. */
    uint32_t off = 0;
    int sent = 0;
    while (off < total) {
        uint32_t want = total - off;
        if (want > NET_MAP_CHUNK_PAYLOAD) want = NET_MAP_CHUNK_PAYLOAD;
        bool done = map_download_apply_chunk(&d, off, src + off,
                                             (uint32_t)want, 1.0);
        ++sent;
        if (off + want >= total) {
            CHECK(done, "last apply_chunk returns done=true");
        } else {
            CHECK(!done, "non-final apply_chunk returns done=false");
        }
        off += want;
    }
    CHECK((uint32_t)sent == expect_chunks, "all chunks delivered exactly once");
    CHECK(d.bytes_received == total, "bytes_received hits total_size");
    CHECK(d.complete, "complete flag set after final chunk");

    /* Reassembled buffer should match source. */
    CHECK(memcmp(d.buffer, src, total) == 0, "reassembled buffer matches source");

    /* Buffer CRC should match descriptor CRC. */
    uint32_t reassembled_crc = level_compute_buffer_crc(d.buffer, (int)total);
    CHECK(reassembled_crc == expected_crc, "reassembled CRC matches");

    /* ---- Duplicate chunk: bytes_received shouldn't move. */
    uint32_t before = d.bytes_received;
    bool again = map_download_apply_chunk(&d, 0, src, NET_MAP_CHUNK_PAYLOAD, 2.0);
    CHECK(!again, "duplicate apply_chunk returns false");
    CHECK(d.bytes_received == before, "duplicate doesn't bump bytes_received");

    /* ---- Out-of-bounds rejection. Reset and try a chunk that overflows. */
    map_download_cancel(&d);
    ok = map_download_begin(&d, &desc, 3.0);
    CHECK(ok, "begin again after cancel");
    /* Sneak an out-of-bounds offset (well past total). */
    bool oob = map_download_apply_chunk(&d, total + 100u, src,
                                        NET_MAP_CHUNK_PAYLOAD, 4.0);
    CHECK(!oob, "out-of-bounds chunk rejected");
    CHECK(d.bytes_received == 0, "OOB rejection doesn't bump bytes_received");

    /* ---- Non-aligned offset rejection. */
    bool unaligned = map_download_apply_chunk(&d, 17u, src,
                                              NET_MAP_CHUNK_PAYLOAD, 5.0);
    CHECK(!unaligned, "non-aligned offset rejected");

    /* ---- CRC mismatch on bit-flip. */
    map_download_cancel(&d);
    ok = map_download_begin(&d, &desc, 6.0);
    off = 0;
    while (off < total) {
        uint32_t want = total - off;
        if (want > NET_MAP_CHUNK_PAYLOAD) want = NET_MAP_CHUNK_PAYLOAD;
        map_download_apply_chunk(&d, off, src + off, (uint32_t)want, 7.0);
        off += want;
    }
    /* Flip a single bit in the reassembled buffer's payload area
     * (avoid the 4-byte CRC field at offset 36). */
    d.buffer[total / 2] ^= 0x01;
    uint32_t flipped_crc = level_compute_buffer_crc(d.buffer, (int)total);
    CHECK(flipped_crc != expected_crc, "bit-flipped buffer fails CRC check");

    fprintf(stdout, "\nmap_chunk_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
