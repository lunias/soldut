#include "map_cache.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MC_MKDIR(path) _mkdir(path)
#define MC_PATHSEP '\\'
#else
#include <dirent.h>
#include <unistd.h>
#define MC_MKDIR(path) mkdir((path), 0755)
#define MC_PATHSEP '/'
#endif

/* ---- Path resolution ---------------------------------------------- */
/*
 * Resolved once on first call; subsequent calls return the cached
 * string. `g_cache_dir_ok` distinguishes "haven't tried" from
 * "tried and failed" so we don't re-mkdir on every probe. */

static char  g_cache_dir[512];
static int   g_cache_dir_ok    = 0;     /* tri-state: 0=unresolved, 1=ok, -1=failed */

#ifndef _WIN32
static const char *home_dir_fallback(void) {
    const char *h = getenv("HOME");
    return (h && *h) ? h : "/tmp";
}
#endif

static void mkdir_p_segment(const char *path) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    /* Skip leading slash on POSIX so we don't mkdir("") for absolute
     * paths. */
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = 0;
            MC_MKDIR(buf);
            *p = saved;
        }
    }
    MC_MKDIR(buf);
}

static bool resolve_cache_dir(void) {
    if (g_cache_dir_ok != 0) return g_cache_dir_ok > 0;

#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata) {
        LOG_W("map_cache: APPDATA not set; cache disabled");
        g_cache_dir_ok = -1;
        return false;
    }
    snprintf(g_cache_dir, sizeof(g_cache_dir),
             "%s\\Soldut\\maps", appdata);
#elif defined(__APPLE__)
    const char *home = home_dir_fallback();
    snprintf(g_cache_dir, sizeof(g_cache_dir),
             "%s/Library/Application Support/Soldut/maps", home);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        snprintf(g_cache_dir, sizeof(g_cache_dir),
                 "%s/soldut/maps", xdg);
    } else {
        snprintf(g_cache_dir, sizeof(g_cache_dir),
                 "%s/.local/share/soldut/maps", home_dir_fallback());
    }
#endif

    mkdir_p_segment(g_cache_dir);

    /* Verify the dir is actually writable by stat-ing it. */
    struct stat st;
    if (stat(g_cache_dir, &st) != 0) {
        LOG_W("map_cache: failed to create %s", g_cache_dir);
        g_cache_dir_ok = -1;
        return false;
    }
    g_cache_dir_ok = 1;
    return true;
}

bool map_cache_init(void) {
    return resolve_cache_dir();
}

/* ---- File path helpers --------------------------------------------- */

const char *map_cache_path(uint32_t crc32) {
    static char path[1024];
    if (!resolve_cache_dir()) {
        path[0] = '\0';
        return path;
    }
    snprintf(path, sizeof(path), "%s%c%08x.lvl",
             g_cache_dir, MC_PATHSEP, (unsigned)crc32);
    return path;
}

bool map_cache_has(uint32_t crc32) {
    if (crc32 == 0) return false;
    const char *p = map_cache_path(crc32);
    if (!p || !*p) return false;
    struct stat st;
    return (stat(p, &st) == 0);
}

bool map_cache_assets_path(const char *short_name, char *out, size_t out_cap) {
    if (!short_name || !out || out_cap == 0) return false;
    snprintf(out, out_cap, "assets/maps/%s.lvl", short_name);
    struct stat st;
    return (stat(out, &st) == 0);
}

/* ---- File CRC / size probes --------------------------------------- */

uint32_t map_cache_file_crc(const char *path) {
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 36, SEEK_SET) != 0) {   /* HDR_OFF_CRC32 = 36 */
        fclose(f);
        return 0;
    }
    uint8_t b[4];
    size_t got = fread(b, 1, 4, f);
    fclose(f);
    if (got != 4) return 0;
    return (uint32_t)b[0]        |
           ((uint32_t)b[1] <<  8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

uint32_t map_cache_file_size(const char *path) {
    if (!path || !*path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (st.st_size <= 0) return 0;
    /* Caller should validate vs NET_MAP_MAX_FILE_BYTES; we return raw size. */
    return (uint32_t)st.st_size;
}

/* ---- Read whole file ---------------------------------------------- */

int map_cache_read(uint32_t crc32, uint8_t *dst, int dst_cap) {
    if (!dst || dst_cap <= 0) return -1;
    const char *p = map_cache_path(crc32);
    if (!p || !*p) return -1;
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    if (flen < 0 || flen > dst_cap) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    size_t got = fread(dst, 1, (size_t)flen, f);
    fclose(f);
    if (got != (size_t)flen) return -1;
    return (int)flen;
}

/* ---- Atomic write -------------------------------------------------- */
/*
 * tmp file then rename — POSIX rename is atomic on the same filesystem;
 * on Windows MoveFileEx with REPLACE_EXISTING is the moral equivalent
 * (rename(2) on Windows is NOT atomic if dest exists, hence the dance).
 */

bool map_cache_write(uint32_t crc32, const uint8_t *data, uint32_t size) {
    if (!data || size == 0) return false;
    if (!resolve_cache_dir()) return false;

    char final_path[1024];
    snprintf(final_path, sizeof(final_path), "%s%c%08x.lvl",
             g_cache_dir, MC_PATHSEP, (unsigned)crc32);
    char tmp_path[1100];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        LOG_W("map_cache: cannot open %s for write", tmp_path);
        return false;
    }
    size_t put = fwrite(data, 1, size, f);
    int    cls = fclose(f);
    if (put != size || cls != 0) {
        remove(tmp_path);
        LOG_W("map_cache: short write to %s (%zu/%u)", tmp_path, put, (unsigned)size);
        return false;
    }

#ifdef _WIN32
    /* Windows rename(2) fails if dest exists. Remove first, then rename
     * — small race window but the cache is single-writer per process. */
    remove(final_path);
    if (rename(tmp_path, final_path) != 0) {
        remove(tmp_path);
        LOG_W("map_cache: rename %s → %s failed", tmp_path, final_path);
        return false;
    }
#else
    if (rename(tmp_path, final_path) != 0) {
        remove(tmp_path);
        LOG_W("map_cache: rename %s → %s failed", tmp_path, final_path);
        return false;
    }
#endif
    LOG_I("map_cache: wrote %u bytes for crc=%08x → %s",
          (unsigned)size, (unsigned)crc32, final_path);
    return true;
}

/* ---- LRU eviction -------------------------------------------------- */
/*
 * Scan cache dir, sort by mtime ascending, delete oldest until total
 * size <= cap_bytes. Crude (one-shot scan, no in-memory index) but
 * correct: the cache is small (≤128 maps at 500 KB each) and eviction
 * runs only on write.
 */

typedef struct {
    char     name[64];
    time_t   mtime;
    uint64_t size;
} CacheEntry;

#define MC_MAX_SCAN 256

static int cmp_mtime_asc(const void *a, const void *b) {
    const CacheEntry *ea = (const CacheEntry *)a;
    const CacheEntry *eb = (const CacheEntry *)b;
    if (ea->mtime < eb->mtime) return -1;
    if (ea->mtime > eb->mtime) return  1;
    return 0;
}

void map_cache_evict_lru(uint64_t cap_bytes) {
    if (!resolve_cache_dir()) return;

    CacheEntry entries[MC_MAX_SCAN];
    int        nentries = 0;
    uint64_t   total = 0;

#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*.lvl", g_cache_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (nentries >= MC_MAX_SCAN) break;
        size_t nL = strlen(fd.cFileName);
        if (nL >= sizeof(entries[nentries].name)) continue;
        char full[1100];
        snprintf(full, sizeof(full), "%s\\%s", g_cache_dir, fd.cFileName);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        memcpy(entries[nentries].name, fd.cFileName, nL + 1);
        entries[nentries].mtime = st.st_mtime;
        entries[nentries].size  = (uint64_t)st.st_size;
        total += entries[nentries].size;
        nentries++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(g_cache_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *n = de->d_name;
        size_t nL = strlen(n);
        if (nL < 5 || strcmp(n + nL - 4, ".lvl") != 0) continue;
        if (nL >= sizeof(entries[nentries].name)) continue;
        if (nentries >= MC_MAX_SCAN) break;
        char full[1100];
        snprintf(full, sizeof(full), "%s/%s", g_cache_dir, n);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        memcpy(entries[nentries].name, n, nL + 1);
        entries[nentries].mtime = st.st_mtime;
        entries[nentries].size  = (uint64_t)st.st_size;
        total += entries[nentries].size;
        nentries++;
    }
    closedir(d);
#endif

    if (total <= cap_bytes) return;

    qsort(entries, (size_t)nentries, sizeof(entries[0]), cmp_mtime_asc);

    for (int i = 0; i < nentries && total > cap_bytes; ++i) {
        char full[1100];
        /* %.*s with the entries[].name's bounded size tells the
         * compiler the substitution can't blow the buffer. */
        const int nmax = (int)(sizeof(entries[0].name) - 1);
#ifdef _WIN32
        snprintf(full, sizeof(full), "%s\\%.*s", g_cache_dir,
                 nmax, entries[i].name);
#else
        snprintf(full, sizeof(full), "%s/%.*s", g_cache_dir,
                 nmax, entries[i].name);
#endif
        if (remove(full) == 0) {
            total -= entries[i].size;
            LOG_I("map_cache: LRU evicted %s (%llu bytes)", entries[i].name,
                  (unsigned long long)entries[i].size);
        }
    }
}
