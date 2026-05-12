/*
 * tools/audio_inventory/run_inventory.c — P19 audio asset inventory.
 *
 * Walks the SFX manifest in src/audio.c (exposed via the
 * audio_manifest_*() accessors), the servo loop path, and the
 * derived per-map music + ambient paths produced by cook_maps'
 * set_meta() (`assets/music/<kit>.ogg`, `assets/sfx/ambient_<kit>.ogg`).
 *
 * Prints a CSV row per expected file: path,kind,size_bytes,status.
 *   status = PRESENT (file exists, non-zero, codec OK at runtime
 *                     -- format-check is delegated to audio_normalize),
 *            MISSING (no file on disk),
 *            EMPTY   (file exists but size_bytes == 0).
 *
 * Also writes one path per line of any MISSING/EMPTY entry to
 * tools/audio_inventory/missing.txt so a sourcing script can iterate.
 *
 * Exit code: 0 even when entries are missing (the brief: inventory
 * is informational; gating is `make test-audio-smoke`). A non-zero
 * exit is reserved for I/O failures inside the tool itself.
 */
#include "audio.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* The unique per-map kit short_names used by tools/cook_maps/cook_maps.c
 * (set_meta calls). Crossfire shares Foundry's kit ("foundry") so it
 * doesn't add a unique entry. Keep in sync with cook_maps.c. */
static const char *const g_kits[] = {
    "foundry",       /* Foundry + Crossfire */
    "maintenance",   /* Slipstream */
    "reactor",       /* Reactor */
    "atrium",        /* Concourse */
    "exterior",      /* Catwalk */
    "aurora",        /* Aurora */
    "citadel",       /* Citadel */
};
#define G_KITS_COUNT (int)(sizeof g_kits / sizeof g_kits[0])

/* Status of a path on disk. */
typedef enum {
    INV_PRESENT,
    INV_MISSING,
    INV_EMPTY,
} InvStatus;

static InvStatus stat_path(const char *path, long *size_out) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (size_out) *size_out = 0;
        return INV_MISSING;
    }
    if (size_out) *size_out = (long)st.st_size;
    if (st.st_size == 0) return INV_EMPTY;
    return INV_PRESENT;
}

static const char *status_str(InvStatus s) {
    switch (s) {
        case INV_PRESENT: return "PRESENT";
        case INV_MISSING: return "MISSING";
        case INV_EMPTY:   return "EMPTY";
    }
    return "?";
}

int main(void) {
    FILE *miss = fopen("tools/audio_inventory/missing.txt", "w");
    if (!miss) {
        fprintf(stderr, "audio_inventory: cannot open missing.txt for write\n");
        return 1;
    }

    printf("path,kind,size_bytes,status\n");

    int total = 0;
    int missing = 0;

    /* SFX manifest */
    int sfx_n = audio_manifest_count();
    for (int i = 0; i < sfx_n; ++i) {
        const char *path = audio_manifest_path(i);
        const char *kind = audio_manifest_kind(i);
        if (!path || !kind) continue;
        long size = 0;
        InvStatus st = stat_path(path, &size);
        printf("%s,%s,%ld,%s\n", path, kind, size, status_str(st));
        ++total;
        if (st != INV_PRESENT) {
            fprintf(miss, "%s\n", path);
            ++missing;
        }
    }

    /* Servo loop */
    {
        const char *path = audio_servo_path();
        long size = 0;
        InvStatus st = stat_path(path, &size);
        printf("%s,%s,%ld,%s\n", path, "SERVO", size, status_str(st));
        ++total;
        if (st != INV_PRESENT) {
            fprintf(miss, "%s\n", path);
            ++missing;
        }
    }

    /* Per-map music + ambient */
    for (int k = 0; k < G_KITS_COUNT; ++k) {
        char music_path[128], amb_path[128];
        snprintf(music_path, sizeof music_path, "assets/music/%s.ogg", g_kits[k]);
        snprintf(amb_path,   sizeof amb_path,   "assets/sfx/ambient_%s.ogg", g_kits[k]);

        long size = 0;
        InvStatus st = stat_path(music_path, &size);
        printf("%s,%s,%ld,%s\n", music_path, "MUSIC", size, status_str(st));
        ++total;
        if (st != INV_PRESENT) { fprintf(miss, "%s\n", music_path); ++missing; }

        size = 0;
        st = stat_path(amb_path, &size);
        printf("%s,%s,%ld,%s\n", amb_path, "AMBIENT", size, status_str(st));
        ++total;
        if (st != INV_PRESENT) { fprintf(miss, "%s\n", amb_path); ++missing; }
    }

    fclose(miss);

    fprintf(stderr, "audio_inventory: %d/%d entries missing (see tools/audio_inventory/missing.txt)\n",
            missing, total);
    return 0;
}
