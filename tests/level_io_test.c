/*
 * level_io_test — round-trip + corruption tests for the .lvl format.
 *
 * Build: make test-level-io
 * Run:   ./build/level_io_test
 *
 * Failure mode: prints FAIL lines, exits non-zero. Unlike headless_sim
 * (which dumps text for humans), this test asserts and returns 1 if any
 * check fails so CI can flag regressions.
 */

#define _POSIX_C_SOURCE 200809L

#include "../src/arena.h"
#include "../src/level_io.h"
#include "../src/log.h"
#include "../src/world.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { int passed, failed; } TestStats;

#define TEST(stats, cond, msg) do {                                   \
    if (cond) { ++(stats)->passed; }                                  \
    else { ++(stats)->failed;                                         \
        fprintf(stderr, "FAIL: %s\n", msg); }                         \
} while (0)

#define TESTF(stats, cond, fmt, ...) do {                             \
    if (cond) { ++(stats)->passed; }                                  \
    else { ++(stats)->failed;                                         \
        fprintf(stderr, "FAIL: " fmt "\n", __VA_ARGS__); }            \
} while (0)

/* Build a synthetic World with a representative sample of every
 * section. Fields are non-trivial values so bit-flips are detectable.
 * The level uses the supplied arena for all storage. */
static void build_synthetic(World *w, Arena *arena) {
    memset(w, 0, sizeof(*w));
    Level *L = &w->level;
    L->width     = 8;
    L->height    = 6;
    L->tile_size = 32;
    L->gravity   = (Vec2){ 0.0f, 1080.0f };

    int n = L->width * L->height;
    L->tiles = (LvlTile *)arena_alloc(arena, sizeof(LvlTile) * (size_t)n);
    memset(L->tiles, 0, sizeof(LvlTile) * (size_t)n);
    /* Floor row (last row) — solid. Mid block at (3,3) — solid + ice. */
    for (int x = 0; x < L->width; ++x) {
        L->tiles[(L->height - 1) * L->width + x] = (LvlTile){ .id = 1, .flags = TILE_F_SOLID };
    }
    L->tiles[3 * L->width + 3] = (LvlTile){ .id = 2, .flags = TILE_F_SOLID | TILE_F_ICE };

    /* 3 polygons. */
    L->poly_count = 3;
    L->polys = (LvlPoly *)arena_alloc(arena, sizeof(LvlPoly) * 3);
    L->polys[0] = (LvlPoly){
        .v_x = { 100, 200, 150 }, .v_y = { 100, 100, 50 },
        .normal_x = { 0, 16384, -16384 }, .normal_y = { -32768, 16384, 16384 },
        .kind = TILE_F_SOLID, .group_id = 0, .bounce_q = 32767, .reserved = 0,
    };
    L->polys[1] = (LvlPoly){
        .v_x = { -32, 0, 0 }, .v_y = { 0, 0, 32 },
        .normal_x = { -32768, 0, 32767 }, .normal_y = { 0, 32767, 0 },
        .kind = TILE_F_DEADLY, .group_id = 7, .bounce_q = 0, .reserved = 0,
    };
    L->polys[2] = (LvlPoly){
        .v_x = { 1000, 1100, 1050 }, .v_y = { 800, 800, 750 },
        .normal_x = { 1, 2, 3 }, .normal_y = { 4, 5, 6 },
        .kind = TILE_F_BACKGROUND, .group_id = 9, .bounce_q = 16384, .reserved = 42,
    };

    /* 2 spawns. */
    L->spawn_count = 2;
    L->spawns = (LvlSpawn *)arena_alloc(arena, sizeof(LvlSpawn) * 2);
    L->spawns[0] = (LvlSpawn){ .pos_x = 256, .pos_y = 160, .team = 1, .flags = 1, .lane_hint = 5, .reserved = 0 };
    L->spawns[1] = (LvlSpawn){ .pos_x = -100, .pos_y = 200, .team = 2, .flags = 2, .lane_hint = 200, .reserved = 0 };

    /* 1 pickup. */
    L->pickup_count = 1;
    L->pickups = (LvlPickup *)arena_alloc(arena, sizeof(LvlPickup) * 1);
    L->pickups[0] = (LvlPickup){
        .pos_x = 480, .pos_y = 64, .category = 3, .variant = 7,
        .respawn_ms = 30000, .flags = 0x0001, .reserved = 0,
    };

    /* 2 decorations. */
    L->deco_count = 2;
    L->decos = (LvlDeco *)arena_alloc(arena, sizeof(LvlDeco) * 2);
    L->decos[0] = (LvlDeco){
        .pos_x = 50, .pos_y = 80, .scale_q = 32767, .rot_q = 64,
        .sprite_str_idx = 1, .layer = 2, .flags = 0,
    };
    L->decos[1] = (LvlDeco){
        .pos_x = 1024, .pos_y = -16, .scale_q = 16384, .rot_q = -32,
        .sprite_str_idx = 6, .layer = 0, .flags = 1,
    };

    /* 1 ambient zone. */
    L->ambi_count = 1;
    L->ambis = (LvlAmbi *)arena_alloc(arena, sizeof(LvlAmbi) * 1);
    L->ambis[0] = (LvlAmbi){
        .rect_x = 200, .rect_y = 100, .rect_w = 300, .rect_h = 80,
        .kind = 1 /* WIND */, .strength_q = 16384,
        .dir_x_q = 32767, .dir_y_q = 0,
    };

    /* 2 flag bases. */
    L->flag_count = 2;
    L->flags = (LvlFlag *)arena_alloc(arena, sizeof(LvlFlag) * 2);
    L->flags[0] = (LvlFlag){ .pos_x = 100, .pos_y = 150, .team = 1, .reserved = {0,0,0} };
    L->flags[1] = (LvlFlag){ .pos_x = 700, .pos_y = 150, .team = 2, .reserved = {0,0,0} };

    /* META.
     *
     * M6 P09 — exercises every promoted atmosphere field with
     * distinct non-zero values so the encode/decode round-trip
     * regresses if any one ever drifts. */
    L->meta = (LvlMeta){
        .name_str_idx = 1, .blurb_str_idx = 6,
        .background_str_idx = 14, .music_str_idx = 0,
        .ambient_loop_str_idx = 0, .reverb_amount_q = 16384,
        .mode_mask = 0x07, /* FFA | TDM | CTF */
        .theme_id          = 3,       /* NEON */
        .sky_top_rgb565    = 0x4188,
        .sky_bot_rgb565    = 0x10A4,
        .fog_density_q     = 26214,   /* ~0.40 */
        .fog_color_rgb565  = 0x8410,
        .vignette_q        = 13107,   /* ~0.20 */
        .sun_angle_q       = 16384,   /* 0.25 → π/2 */
        .weather_kind      = 4,       /* EMBERS */
        .weather_density_q = 32767,   /* ~0.50 */
    };

    /* String table. Offset 0 reserved as empty. */
    static const char strt[] =
        "\0"             /* 0 — empty */
        "Test\0"         /* 1 — */
        "blurby\0"       /* 6 — */
        "bg.png\0";      /* 14 — */
    L->string_table      = strt;
    L->string_table_size = (int)sizeof(strt) - 1; /* drop final NUL we'd otherwise store twice */
    /* Leave the original last NUL: every embedded string already has its
     * own. The size equals the literal's storage size minus the implicit
     * \0 the C compiler appends; the trailing entry's own NUL closes
     * the blob. */
}

/* Read a whole file into a malloc'd buffer. *out_size receives the size
 * on success; returns NULL on error (test caller fails). */
static uint8_t *slurp(const char *path, int *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *out_size = (int)n;
    return buf;
}

static int dump_to_disk(const char *path, const uint8_t *buf, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t wrote = fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    return wrote == (size_t)n;
}

int main(void) {
    log_init(NULL);
    TestStats S = {0};
    int rc = 0;

    /* Use distinct paths so multiple test runs don't collide. */
    char path1[256], path2[256];
    snprintf(path1, sizeof path1, "/tmp/soldut_lvl_test_%d_a.lvl", (int)getpid());
    snprintf(path2, sizeof path2, "/tmp/soldut_lvl_test_%d_b.lvl", (int)getpid());

    /* Two arenas: one for the source world's allocations + the save
     * scratch buffer, one for loading the round-tripped world into. */
    Arena src_arena, load_arena;
    arena_init(&src_arena,  2u * 1024u * 1024u, "lvl_test_src");
    arena_init(&load_arena, 2u * 1024u * 1024u, "lvl_test_load");

    /* ---- TEST 1 — round-trip via disk ---- */
    World w_src;
    build_synthetic(&w_src, &src_arena);

    LvlResult r;
    r = level_save(&w_src, &src_arena, path1);
    TESTF(&S, r == LVL_OK, "level_save: %s", level_io_result_str(r));

    /* Reload into a fresh World. */
    World w_loaded;
    memset(&w_loaded, 0, sizeof(w_loaded));
    r = level_load(&w_loaded, &load_arena, path1);
    TESTF(&S, r == LVL_OK, "level_load: %s", level_io_result_str(r));

    /* Re-save from the loaded world to a second path and compare files
     * byte-for-byte. If round-trip is byte-exact, the two .lvl files
     * are identical. (We use a fresh arena for the second save scratch
     * because the loaded world's strings live in load_arena.) */
    Arena resave_arena;
    arena_init(&resave_arena, 2u * 1024u * 1024u, "lvl_test_resave");
    r = level_save(&w_loaded, &resave_arena, path2);
    TESTF(&S, r == LVL_OK, "level_save (resave): %s", level_io_result_str(r));

    int n1 = 0, n2 = 0;
    uint8_t *buf1 = slurp(path1, &n1);
    uint8_t *buf2 = slurp(path2, &n2);
    TEST(&S, buf1 && buf2, "slurp produced both buffers");
    TESTF(&S, n1 == n2, "round-trip byte size match (%d vs %d)", n1, n2);
    if (buf1 && buf2 && n1 == n2) {
        int diff_off = -1;
        for (int i = 0; i < n1; ++i) {
            if (buf1[i] != buf2[i]) { diff_off = i; break; }
        }
        TESTF(&S, diff_off < 0, "round-trip byte-exact (first diff at %d)", diff_off);
    }

    /* Section-level sanity checks on the loaded world. */
    Level *Ls = &w_src.level;
    Level *Ll = &w_loaded.level;
    TEST(&S, Ll->width  == Ls->width,  "width");
    TEST(&S, Ll->height == Ls->height, "height");
    TEST(&S, Ll->poly_count   == Ls->poly_count,   "poly_count");
    TEST(&S, Ll->spawn_count  == Ls->spawn_count,  "spawn_count");
    TEST(&S, Ll->pickup_count == Ls->pickup_count, "pickup_count");
    TEST(&S, Ll->deco_count   == Ls->deco_count,   "deco_count");
    TEST(&S, Ll->ambi_count   == Ls->ambi_count,   "ambi_count");
    TEST(&S, Ll->flag_count   == Ls->flag_count,   "flag_count");

    if (Ll->poly_count == Ls->poly_count) {
        for (int i = 0; i < Ll->poly_count; ++i) {
            int eq = memcmp(&Ll->polys[i], &Ls->polys[i], sizeof(LvlPoly)) == 0;
            TESTF(&S, eq, "poly[%d] mem-equal", i);
        }
    }
    if (Ll->spawn_count == Ls->spawn_count) {
        for (int i = 0; i < Ll->spawn_count; ++i) {
            int eq = memcmp(&Ll->spawns[i], &Ls->spawns[i], sizeof(LvlSpawn)) == 0;
            TESTF(&S, eq, "spawn[%d] mem-equal", i);
        }
    }
    int n_tiles = Ll->width * Ll->height;
    if (Ll->tiles && Ls->tiles && Ll->width == Ls->width && Ll->height == Ls->height) {
        int eq = memcmp(Ll->tiles, Ls->tiles, sizeof(LvlTile) * (size_t)n_tiles) == 0;
        TEST(&S, eq, "tile grid mem-equal");
    }
    if (Ll->meta.name_str_idx == Ls->meta.name_str_idx) {
        TEST(&S, memcmp(&Ll->meta, &Ls->meta, sizeof(LvlMeta)) == 0, "meta mem-equal");
    }
    /* M6 P09 — explicit per-field assertions on the promoted
     * atmosphere block. Catches a future schema drift even if the
     * memcmp above is masked by padding changes. */
    TEST(&S, Ll->meta.theme_id          == Ls->meta.theme_id,
         "meta.theme_id round-trip");
    TEST(&S, Ll->meta.sky_top_rgb565    == Ls->meta.sky_top_rgb565,
         "meta.sky_top_rgb565 round-trip");
    TEST(&S, Ll->meta.sky_bot_rgb565    == Ls->meta.sky_bot_rgb565,
         "meta.sky_bot_rgb565 round-trip");
    TEST(&S, Ll->meta.fog_density_q     == Ls->meta.fog_density_q,
         "meta.fog_density_q round-trip");
    TEST(&S, Ll->meta.fog_color_rgb565  == Ls->meta.fog_color_rgb565,
         "meta.fog_color_rgb565 round-trip");
    TEST(&S, Ll->meta.vignette_q        == Ls->meta.vignette_q,
         "meta.vignette_q round-trip");
    TEST(&S, Ll->meta.sun_angle_q       == Ls->meta.sun_angle_q,
         "meta.sun_angle_q round-trip");
    TEST(&S, Ll->meta.weather_kind      == Ls->meta.weather_kind,
         "meta.weather_kind round-trip");
    TEST(&S, Ll->meta.weather_density_q == Ls->meta.weather_density_q,
         "meta.weather_density_q round-trip");

    /* ---- TEST 2 — bit-flipped file rejects with BAD_CRC ----
     * Flip the very last byte of the saved file (well past header /
     * directory / required-section regions) — guaranteed to alter CRC
     * without breaking magic or directory bounds. */
    if (buf1) {
        uint8_t *flipped = (uint8_t *)malloc((size_t)n1);
        memcpy(flipped, buf1, (size_t)n1);
        flipped[n1 - 1] ^= 0x01;
        char path3[256];
        snprintf(path3, sizeof path3, "/tmp/soldut_lvl_test_%d_flip.lvl", (int)getpid());
        TEST(&S, dump_to_disk(path3, flipped, n1), "wrote flipped file");
        Arena tmp_arena;
        arena_init(&tmp_arena, 2u * 1024u * 1024u, "lvl_test_flip");
        World w_flip; memset(&w_flip, 0, sizeof(w_flip));
        LvlResult rf = level_load(&w_flip, &tmp_arena, path3);
        TESTF(&S, rf == LVL_ERR_BAD_CRC,
              "bit-flipped file: expected BAD_CRC, got %d (%s)",
              (int)rf, level_io_result_str(rf));
        arena_destroy(&tmp_arena);
        unlink(path3);
        free(flipped);
    }

    /* ---- TEST 3 — truncated file rejects with BAD_DIRECTORY/BAD_SECTION ----
     * Truncate to half size. Where the cut lands depends on file shape
     * but it will always violate directory or per-section bounds. */
    if (buf1 && n1 > 64) {
        int trunc_n = n1 / 2;
        char path4[256];
        snprintf(path4, sizeof path4, "/tmp/soldut_lvl_test_%d_trunc.lvl", (int)getpid());
        TEST(&S, dump_to_disk(path4, buf1, trunc_n), "wrote truncated file");
        Arena tmp_arena;
        arena_init(&tmp_arena, 2u * 1024u * 1024u, "lvl_test_trunc");
        World w_trunc; memset(&w_trunc, 0, sizeof(w_trunc));
        LvlResult rt = level_load(&w_trunc, &tmp_arena, path4);
        TESTF(&S, rt == LVL_ERR_BAD_DIRECTORY || rt == LVL_ERR_BAD_SECTION
                  || rt == LVL_ERR_BAD_CRC,
              "truncated file: expected BAD_DIRECTORY/BAD_SECTION/BAD_CRC, got %d (%s)",
              (int)rt, level_io_result_str(rt));
        arena_destroy(&tmp_arena);
        unlink(path4);
    }

    /* Cleanup. */
    unlink(path1);
    unlink(path2);
    free(buf1);
    free(buf2);
    arena_destroy(&src_arena);
    arena_destroy(&load_arena);
    arena_destroy(&resave_arena);

    printf("\nlevel_io_test: %d passed, %d failed\n", S.passed, S.failed);
    if (S.failed > 0) rc = 1;
    return rc;
}
