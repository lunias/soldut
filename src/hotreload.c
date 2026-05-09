#include "hotreload.h"
#include "log.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef DEV_BUILD
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

#define HOTRELOAD_MAX        64
#define HOTRELOAD_POLL_SEC   0.25

#ifdef DEV_BUILD

typedef struct {
    char              path[256];
    int64_t           mtime;          /* st_mtime; 0 = file not present at register-time */
    HotReloadCallback cb;
    bool              enabled;
} Entry;

static Entry  g_entries[HOTRELOAD_MAX];
static int    g_entry_count = 0;
static double g_last_poll_s = -1.0;

static int64_t stat_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (int64_t)st.st_mtime;
}

void hotreload_register(const char *path, HotReloadCallback cb) {
    if (!path || !*path || !cb) return;
    /* Idempotent on duplicates. */
    for (int i = 0; i < g_entry_count; ++i) {
        if (strncmp(g_entries[i].path, path, sizeof g_entries[i].path) == 0) {
            g_entries[i].cb      = cb;
            g_entries[i].enabled = true;
            return;
        }
    }
    if (g_entry_count >= HOTRELOAD_MAX) {
        LOG_W("hotreload: registry full (%d entries) — dropping %s",
              HOTRELOAD_MAX, path);
        return;
    }
    Entry *e = &g_entries[g_entry_count++];
    snprintf(e->path, sizeof e->path, "%s", path);
    e->mtime   = stat_mtime(path);
    e->cb      = cb;
    e->enabled = true;
    LOG_I("hotreload: watching %s (mtime=%lld)", path, (long long)e->mtime);
}

void hotreload_clear(void) {
    g_entry_count = 0;
    g_last_poll_s = -1.0;
}

void hotreload_poll(void) {
    double now = GetTime();
    if (g_last_poll_s < 0.0) g_last_poll_s = now;
    if (now - g_last_poll_s < HOTRELOAD_POLL_SEC) return;
    g_last_poll_s = now;

    for (int i = 0; i < g_entry_count; ++i) {
        Entry *e = &g_entries[i];
        if (!e->enabled) continue;
        int64_t cur = stat_mtime(e->path);
        if (cur == 0) continue;            /* file gone — wait for it to come back */
        if (cur == e->mtime) continue;     /* unchanged */
        int64_t prev = e->mtime;
        e->mtime = cur;
        if (prev == 0) continue;           /* file appeared after register; first sighting */
        LOG_I("hotreload: %s changed, firing callback", e->path);
        if (e->cb) e->cb(e->path);
    }
}

#else  /* !DEV_BUILD — every entry point is a no-op. */

void hotreload_register(const char *path, HotReloadCallback cb) {
    (void)path; (void)cb;
}
void hotreload_clear(void) {}
void hotreload_poll(void)  {}

#endif
