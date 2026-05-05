/*
 * tests/map_registry_test.c — unit test for src/maps.c::map_registry_init.
 *
 * Verifies the M5 P08b runtime map registry: builtins seed correctly,
 * arbitrary `assets/maps/<name>.lvl` files surface as registry entries,
 * filename-collision with a builtin overrides the builtin's CRC + size +
 * mode_mask, the registry cap is honored, and malformed `.lvl` files are
 * skipped without crashing.
 *
 * Returns 0 on all-pass, 1 on any assertion failure.
 *
 * The test writes real `.lvl` files via `level_save` into a per-test
 * tmp dir, then drives `map_registry_init_from(tmp_dir)` and inspects
 * `g_map_registry`. No raylib runtime — links the headless object set.
 */

/* mkdtemp lives behind _BSD_SOURCE / _DEFAULT_SOURCE in glibc; declare
 * the feature-test macro before any system header. */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE

#include "../src/arena.h"
#include "../src/level.h"
#include "../src/level_io.h"
#include "../src/log.h"
#include "../src/maps.h"
#include "../src/match.h"
#include "../src/world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int g_failed = 0;

#define ASSERT_EQ_INT(label, actual, expected) do {                          \
    long __a = (long)(actual);                                               \
    long __e = (long)(expected);                                             \
    if (__a != __e) {                                                        \
        fprintf(stderr, "FAIL: %s: actual=%ld expected=%ld\n",               \
                label, __a, __e);                                            \
        ++g_failed;                                                          \
    } else {                                                                 \
        fprintf(stdout, "ok:   %s: %ld\n", label, __a);                      \
    }                                                                        \
} while (0)

#define ASSERT_TRUE(label, cond) do {                                        \
    if (!(cond)) {                                                           \
        fprintf(stderr, "FAIL: %s\n", label);                                \
        ++g_failed;                                                          \
    } else {                                                                 \
        fprintf(stdout, "ok:   %s\n", label);                                \
    }                                                                        \
} while (0)

#define ASSERT_STREQ(label, actual, expected) do {                           \
    const char *__a = (actual);                                              \
    const char *__e = (expected);                                            \
    if (!__a || !__e || strcmp(__a, __e) != 0) {                             \
        fprintf(stderr, "FAIL: %s: actual='%s' expected='%s'\n",             \
                label, __a ? __a : "(null)", __e ? __e : "(null)");          \
        ++g_failed;                                                          \
    } else {                                                                 \
        fprintf(stdout, "ok:   %s: '%s'\n", label, __a);                     \
    }                                                                        \
} while (0)

/* Make a unique tmp dir for each test case. mkdtemp consumes the
 * trailing XXXXXX of its template; caller may treat the buffer as a
 * directory path on success. */
static bool make_tmpdir(char *out, size_t out_cap) {
    snprintf(out, out_cap, "/tmp/soldut_map_reg_XXXXXX");
    return mkdtemp(out) != NULL;
}

/* Write a small synthetic .lvl at `path` with the supplied mode_mask.
 * `seed` varies the floor tile id so distinct files yield distinct
 * CRCs. Returns true on success. */
static bool write_lvl(const char *path, uint16_t mode_mask, unsigned seed) {
    Arena scratch = {0};
    arena_init(&scratch, 1 * 1024 * 1024, "scratch");

    static World w;
    memset(&w, 0, sizeof(w));
    Level *L = &w.level;
    L->width     = 16;
    L->height    = 12;
    L->tile_size = 32;

    int n = L->width * L->height;
    L->tiles = (LvlTile *)arena_alloc(&scratch, sizeof(LvlTile) * (size_t)n);
    if (!L->tiles) { arena_destroy(&scratch); return false; }
    memset(L->tiles, 0, sizeof(LvlTile) * (size_t)n);
    for (int x = 0; x < L->width; ++x) {
        L->tiles[(L->height - 1) * L->width + x].flags = TILE_F_SOLID;
        L->tiles[(L->height - 1) * L->width + x].id    = (uint16_t)(seed & 0xff);
    }

    L->spawns = (LvlSpawn *)arena_alloc(&scratch, sizeof(LvlSpawn));
    L->spawn_count = 1;
    L->spawns[0] = (LvlSpawn){ .pos_x = 96, .pos_y = 320, .team = 1 };

    L->meta.mode_mask = mode_mask;

    LvlResult r = level_save(&w, &scratch, path);
    arena_destroy(&scratch);
    return r == LVL_OK;
}

/* Write a malformed file (random bytes, no SDLV magic) so the scan
 * must skip it gracefully. */
static bool write_garbage(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    static const char junk[] = "this is not a .lvl file";
    fwrite(junk, 1, sizeof junk - 1, f);
    fclose(f);
    return true;
}

/* unlink every file under `dir`, then rmdir. Best-effort cleanup so
 * test re-runs don't leave debris. */
static void cleanup_dir(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' 2>/dev/null", dir);
    int rc = system(cmd);
    (void)rc;
}

static int find_entry_by_short_name(const char *name) {
    for (int i = 0; i < g_map_registry.count; ++i) {
        if (strcasecmp(g_map_registry.entries[i].short_name, name) == 0) return i;
    }
    return -1;
}

int main(void) {
    log_init("/tmp/map_registry_test.log");

    /* ---- Test 1: empty directory yields exactly the 4 builtins ---- */
    {
        char dir[64];
        if (!make_tmpdir(dir, sizeof dir)) {
            fprintf(stderr, "FAIL: mkdtemp\n"); return 1;
        }
        map_registry_init_from(dir);
        ASSERT_EQ_INT("empty dir: count is 4 builtins",
                      g_map_registry.count, MAP_BUILTIN_COUNT);
        ASSERT_STREQ("empty dir: entry 0 short_name",
                     g_map_registry.entries[0].short_name, "foundry");
        ASSERT_STREQ("empty dir: entry 1 short_name",
                     g_map_registry.entries[1].short_name, "slipstream");
        ASSERT_STREQ("empty dir: entry 2 short_name",
                     g_map_registry.entries[2].short_name, "reactor");
        ASSERT_STREQ("empty dir: entry 3 short_name",
                     g_map_registry.entries[3].short_name, "crossfire");
        ASSERT_TRUE("empty dir: foundry has no .lvl on disk",
                    g_map_registry.entries[0].has_lvl_on_disk == false);
        ASSERT_TRUE("empty dir: foundry mode_mask is FFA|TDM",
                    g_map_registry.entries[0].mode_mask ==
                    ((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM)));
        ASSERT_TRUE("empty dir: crossfire mode_mask includes CTF",
                    (g_map_registry.entries[3].mode_mask & (1u << MATCH_MODE_CTF)) != 0);
        cleanup_dir(dir);
    }

    /* ---- Test 2: a custom .lvl appears as a 5th entry ---- */
    {
        char dir[64];
        if (!make_tmpdir(dir, sizeof dir)) {
            fprintf(stderr, "FAIL: mkdtemp\n"); return 1;
        }
        char path[256];
        snprintf(path, sizeof path, "%s/my_arena.lvl", dir);
        ASSERT_TRUE("test2: write my_arena.lvl",
                    write_lvl(path, (uint16_t)((1u << MATCH_MODE_FFA) |
                                                (1u << MATCH_MODE_TDM)), 7));

        map_registry_init_from(dir);
        ASSERT_EQ_INT("test2: count is 5",
                      g_map_registry.count, MAP_BUILTIN_COUNT + 1);
        int idx = find_entry_by_short_name("my_arena");
        ASSERT_TRUE("test2: my_arena present", idx == MAP_BUILTIN_COUNT);
        if (idx >= 0) {
            ASSERT_TRUE("test2: my_arena has .lvl on disk",
                        g_map_registry.entries[idx].has_lvl_on_disk == true);
            ASSERT_TRUE("test2: my_arena CRC non-zero",
                        g_map_registry.entries[idx].file_crc != 0);
            ASSERT_TRUE("test2: my_arena size non-zero",
                        g_map_registry.entries[idx].file_size != 0);
            ASSERT_TRUE("test2: my_arena mode_mask is FFA|TDM",
                        g_map_registry.entries[idx].mode_mask ==
                        ((1u << MATCH_MODE_FFA) | (1u << MATCH_MODE_TDM)));
            /* Display name with no META string falls back to titlecased
             * short_name → "My Arena" (underscore becomes space, first
             * letter of each word uppercased). */
            ASSERT_STREQ("test2: my_arena display name titlecased",
                         g_map_registry.entries[idx].display_name, "My Arena");
            ASSERT_TRUE("test2: my_arena id matches its slot",
                        g_map_registry.entries[idx].id == idx);
        }

        /* map_id_from_name finds it by short_name AND display_name. */
        ASSERT_EQ_INT("test2: map_id_from_name(\"my_arena\")",
                      map_id_from_name("my_arena"), MAP_BUILTIN_COUNT);
        ASSERT_EQ_INT("test2: map_id_from_name(\"My Arena\")",
                      map_id_from_name("My Arena"), MAP_BUILTIN_COUNT);
        ASSERT_EQ_INT("test2: map_id_from_name(\"foundry\")",
                      map_id_from_name("foundry"), MAP_FOUNDRY);
        ASSERT_EQ_INT("test2: map_id_from_name unknown",
                      map_id_from_name("definitely_not_a_real_map"), -1);
        cleanup_dir(dir);
    }

    /* ---- Test 3: foundry.lvl OVERRIDES the builtin ---- */
    {
        char dir[64];
        if (!make_tmpdir(dir, sizeof dir)) {
            fprintf(stderr, "FAIL: mkdtemp\n"); return 1;
        }
        char path[256];
        snprintf(path, sizeof path, "%s/foundry.lvl", dir);
        /* Write with CTF in the mode_mask so we can detect override. */
        uint16_t mm = (uint16_t)((1u << MATCH_MODE_FFA) |
                                  (1u << MATCH_MODE_TDM) |
                                  (1u << MATCH_MODE_CTF));
        ASSERT_TRUE("test3: write foundry.lvl",
                    write_lvl(path, mm, 42));

        map_registry_init_from(dir);
        ASSERT_EQ_INT("test3: count is still 4 (override, not append)",
                      g_map_registry.count, MAP_BUILTIN_COUNT);
        ASSERT_STREQ("test3: foundry slot still occupied",
                     g_map_registry.entries[MAP_FOUNDRY].short_name, "foundry");
        ASSERT_TRUE("test3: foundry now has .lvl on disk",
                    g_map_registry.entries[MAP_FOUNDRY].has_lvl_on_disk == true);
        ASSERT_TRUE("test3: foundry CRC non-zero",
                    g_map_registry.entries[MAP_FOUNDRY].file_crc != 0);
        ASSERT_TRUE("test3: foundry mode_mask now includes CTF",
                    (g_map_registry.entries[MAP_FOUNDRY].mode_mask &
                     (1u << MATCH_MODE_CTF)) != 0);
        ASSERT_TRUE("test3: foundry id is still 0",
                    g_map_registry.entries[MAP_FOUNDRY].id == MAP_FOUNDRY);
        cleanup_dir(dir);
    }

    /* ---- Test 4: malformed .lvl is skipped without crashing ---- */
    {
        char dir[64];
        if (!make_tmpdir(dir, sizeof dir)) {
            fprintf(stderr, "FAIL: mkdtemp\n"); return 1;
        }
        char path[256];
        snprintf(path, sizeof path, "%s/garbage.lvl", dir);
        ASSERT_TRUE("test4: write garbage", write_garbage(path));

        map_registry_init_from(dir);
        ASSERT_EQ_INT("test4: count is 4 (garbage skipped)",
                      g_map_registry.count, MAP_BUILTIN_COUNT);
        ASSERT_TRUE("test4: garbage not in registry",
                    find_entry_by_short_name("garbage") == -1);
        cleanup_dir(dir);
    }

    /* ---- Test 5: registry cap of 32 honored ---- */
    {
        char dir[64];
        if (!make_tmpdir(dir, sizeof dir)) {
            fprintf(stderr, "FAIL: mkdtemp\n"); return 1;
        }
        /* Write 35 distinct .lvl files. With 4 builtins, only 32 -
         * 4 = 28 custom entries fit, leaving 7 to be skipped with a
         * "cap reached" log line. */
        int wrote = 0;
        for (int i = 0; i < 35; ++i) {
            char path[256];
            snprintf(path, sizeof path, "%s/extra_%02d.lvl", dir, i);
            if (write_lvl(path, (uint16_t)(1u << MATCH_MODE_FFA),
                          (unsigned)(i + 100))) {
                ++wrote;
            }
        }
        ASSERT_EQ_INT("test5: wrote 35 files", wrote, 35);

        map_registry_init_from(dir);
        ASSERT_EQ_INT("test5: count clamped to MAP_REGISTRY_MAX",
                      g_map_registry.count, MAP_REGISTRY_MAX);
        cleanup_dir(dir);
    }

    /* ---- Test 6: NULL / non-existent dir → builtins-only ---- */
    {
        map_registry_init_from(NULL);
        ASSERT_EQ_INT("test6 (NULL dir): count is 4 builtins",
                      g_map_registry.count, MAP_BUILTIN_COUNT);

        map_registry_init_from("/this/path/should/not/exist/anywhere");
        ASSERT_EQ_INT("test6 (missing dir): count is 4 builtins",
                      g_map_registry.count, MAP_BUILTIN_COUNT);
    }

    /* ---- Test 7: idempotency — re-init wipes prior custom entries ---- */
    {
        /* First init with one custom; then re-init from empty dir should
         * shrink back to 4. */
        char dir1[64], dir2[64];
        if (!make_tmpdir(dir1, sizeof dir1) ||
            !make_tmpdir(dir2, sizeof dir2)) {
            fprintf(stderr, "FAIL: mkdtemp\n"); return 1;
        }
        char path[256];
        snprintf(path, sizeof path, "%s/extra.lvl", dir1);
        write_lvl(path, (uint16_t)(1u << MATCH_MODE_FFA), 99);
        map_registry_init_from(dir1);
        ASSERT_EQ_INT("test7: first init has 5 entries",
                      g_map_registry.count, MAP_BUILTIN_COUNT + 1);
        map_registry_init_from(dir2);   /* empty dir */
        ASSERT_EQ_INT("test7: re-init shrinks back to 4",
                      g_map_registry.count, MAP_BUILTIN_COUNT);
        cleanup_dir(dir1);
        cleanup_dir(dir2);
    }

    /* Final summary. */
    if (g_failed == 0) {
        fprintf(stdout, "\nmap_registry_test: ALL PASSED\n");
        return 0;
    }
    fprintf(stderr, "\nmap_registry_test: %d ASSERTION(S) FAILED\n", g_failed);
    return 1;
}
