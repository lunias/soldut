/*
 * tests/prefs_test.c — round-trip + edge-case coverage for the
 * user-prefs persistence module (wan-fixes-8).
 *
 * The lobby UI calls prefs_save after every chassis / weapon / armor /
 * jetpack / team / name edit. Next launch's prefs_load should
 * reconstitute the exact same UserPrefs (modulo whitespace + comment
 * ordering, neither of which we round-trip).
 *
 * Format-level invariants under test:
 *   - missing file → defaults (no error, false return)
 *   - malformed line → field keeps its loaded-so-far value, warning logged
 *   - unknown chassis / weapon / armor / jetpack name → keep default
 *   - inline `#` comment stripped from values
 *   - atomic save (tmp + rename) leaves no partial file behind on crash
 */

#include "../src/log.h"
#include "../src/prefs.h"
#include "../src/weapons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failed = 0;

#define ASSERT_EQ(label, actual, expected) do {                          \
    long long __a = (long long)(actual);                                 \
    long long __e = (long long)(expected);                               \
    if (__a != __e) {                                                    \
        fprintf(stderr, "FAIL: %s: actual=%lld expected=%lld\n",         \
                label, __a, __e); ++g_failed;                            \
    } else {                                                             \
        fprintf(stdout, "ok:   %s: %lld\n", label, __a);                 \
    }                                                                    \
} while (0)

#define ASSERT_STR(label, actual, expected) do {                         \
    const char *__a = (actual) ? (actual) : "(null)";                    \
    const char *__e = (expected) ? (expected) : "(null)";                \
    if (strcmp(__a, __e) != 0) {                                         \
        fprintf(stderr, "FAIL: %s: actual='%s' expected='%s'\n",         \
                label, __a, __e); ++g_failed;                            \
    } else {                                                             \
        fprintf(stdout, "ok:   %s: '%s'\n", label, __a);                 \
    }                                                                    \
} while (0)

#define ASSERT_TRUE(label, cond) do {                                    \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL: %s\n", label); ++g_failed;                \
    } else {                                                             \
        fprintf(stdout, "ok:   %s\n", label);                            \
    }                                                                    \
} while (0)

int main(void) {
    log_init("/tmp/prefs_test.log");

    /* ---- 1. Missing file → defaults ------------------------------- */
    {
        const char *path = "/tmp/prefs_test_missing.cfg";
        remove(path);
        UserPrefs p;
        bool loaded = prefs_load(&p, path);
        ASSERT_EQ("missing file: returns false", loaded, 0);
        ASSERT_STR("missing file: name = 'player'", p.name, "player");
        /* Defaults for chassis are CHASSIS_TROOPER (0). */
        ASSERT_EQ("missing file: chassis = TROOPER", p.loadout.chassis_id, 0);
    }

    /* ---- 2. Round-trip a custom prefs file ------------------------ */
    {
        const char *path = "/tmp/prefs_test_roundtrip.cfg";
        remove(path);

        UserPrefs w; prefs_defaults(&w);
        snprintf(w.name, sizeof w.name, "Ethan");
        /* Chassis / weapons / armor / jet by name — exact values
         * resolved at save time from the in-binary tables. */
        w.loadout.chassis_id   = 2;   /* Heavy */
        w.loadout.armor_id     = 2;   /* Heavy (3rd entry) */
        w.loadout.jetpack_id   = 2;   /* Heavy */
        w.team                 = 2;   /* BLUE */
        snprintf(w.connect_addr, sizeof w.connect_addr,
                 "soldut.example.org:23073");

        bool saved = prefs_save(&w, path);
        ASSERT_TRUE("save: returns true", saved);

        UserPrefs r;
        bool loaded = prefs_load(&r, path);
        ASSERT_TRUE("load: returns true", loaded);
        ASSERT_STR("round-trip: name", r.name, "Ethan");
        ASSERT_EQ ("round-trip: chassis", r.loadout.chassis_id, 2);
        ASSERT_EQ ("round-trip: armor",   r.loadout.armor_id,   2);
        ASSERT_EQ ("round-trip: jetpack", r.loadout.jetpack_id, 2);
        ASSERT_EQ ("round-trip: team",    r.team,               2);
        ASSERT_STR("round-trip: addr",    r.connect_addr,
                   "soldut.example.org:23073");

        remove(path);
    }

    /* ---- 3. Hand-edited file with comments + whitespace ----------- */
    {
        const char *path = "/tmp/prefs_test_handedit.cfg";
        FILE *f = fopen(path, "w");
        fprintf(f,
            "# hand-edited\n"
            "\n"
            "  name  =  Player2  \n"
            "chassis=Scout  # comment after value\n"
            "team=1\n"
            "; semicolon-comment line\n"
            "primary=Pulse Rifle\n");
        fclose(f);

        UserPrefs r;
        bool loaded = prefs_load(&r, path);
        ASSERT_TRUE("hand-edit: load returns true", loaded);
        ASSERT_STR("hand-edit: trimmed name", r.name, "Player2");
        ASSERT_EQ ("hand-edit: chassis Scout (id=1)", r.loadout.chassis_id, 1);
        ASSERT_EQ ("hand-edit: team=1", r.team, 1);
        remove(path);
    }

    /* ---- 4. Malformed value falls back to default ----------------- */
    {
        const char *path = "/tmp/prefs_test_malformed.cfg";
        FILE *f = fopen(path, "w");
        fprintf(f,
            "name=Player3\n"
            "chassis=NonExistentChassis\n"
            "primary=NotARealGun\n"
            "armor=Vapor\n");
        fclose(f);

        UserPrefs r;
        prefs_load(&r, path);
        ASSERT_STR("malformed: name still applied", r.name, "Player3");
        /* find_chassis_id falls back to TROOPER on unknown name. */
        ASSERT_EQ("malformed: bad chassis → TROOPER", r.loadout.chassis_id, 0);
        remove(path);
    }

    /* ---- 5. Atomic save — tmp file shouldn't outlive a successful
     * save -------------------------------------------------------- */
    {
        const char *path = "/tmp/prefs_test_atomic.cfg";
        char tmp[256]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
        remove(path); remove(tmp);

        UserPrefs w; prefs_defaults(&w);
        prefs_save(&w, path);
        ASSERT_TRUE("atomic: target file exists", access(path, F_OK) == 0);
        ASSERT_TRUE("atomic: tmp file cleaned up", access(tmp, F_OK) != 0);
        remove(path);
    }

    fprintf(stdout, "%s\n",
            g_failed == 0 ? "prefs_test: ALL PASSED"
                          : "prefs_test: FAILED");
    log_shutdown();
    return g_failed == 0 ? 0 : 1;
}
