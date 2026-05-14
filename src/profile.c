/* M6 P06 — frame-time profiler implementation. See profile.h. */

#include "profile.h"

#include "raylib.h"          /* GetTime */
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROF_RING_FRAMES  256   /* rolling window for p95/p99 */

static const char *s_zone_names[PROF_COUNT] = {
    "frame", "input", "sim", "bots", "net",
    "snap_interp", "reconcile", "decal_flush",
    "draw_world", "draw_post", "draw_blit",
    "draw_hud", "draw_overlay", "present"
};

static double s_zone_start [PROF_COUNT];
static double s_zone_acc_s [PROF_COUNT];      /* this frame, seconds */
static float  s_zone_last_ms[PROF_COUNT];     /* most recent completed frame */
static float  s_ring_ms[PROF_RING_FRAMES][PROF_COUNT];
static int    s_ring_head;                    /* next write index */
static int    s_ring_count;                   /* # rows written (caps at ring size) */
static int    s_frames_logged;                /* monotonic frame count */

static FILE *s_csv;

void profile_init(void) {
    memset(s_zone_start,   0, sizeof s_zone_start);
    memset(s_zone_acc_s,   0, sizeof s_zone_acc_s);
    memset(s_zone_last_ms, 0, sizeof s_zone_last_ms);
    memset(s_ring_ms,      0, sizeof s_ring_ms);
    s_ring_head     = 0;
    s_ring_count    = 0;
    s_frames_logged = 0;
}

const char *profile_zone_name(ProfSection s) {
    if (s < 0 || s >= PROF_COUNT) return "?";
    return s_zone_names[s];
}

void profile_frame_begin(void) {
    /* Zero the accumulators for the new frame, then start the
     * frame-wide zone. Individual zones may begin/end multiple
     * times this frame; their times accumulate. */
    for (int i = 0; i < PROF_COUNT; ++i) s_zone_acc_s[i] = 0.0;
    s_zone_start[PROF_FRAME] = GetTime();
}

void profile_zone_begin(ProfSection s) {
    if (s < 0 || s >= PROF_COUNT) return;
    s_zone_start[s] = GetTime();
}

void profile_zone_end(ProfSection s) {
    if (s < 0 || s >= PROF_COUNT) return;
    double dt = GetTime() - s_zone_start[s];
    if (dt < 0.0) dt = 0.0;
    s_zone_acc_s[s] += dt;
}

void profile_frame_end(void) {
    /* Close PROF_FRAME. */
    double dt = GetTime() - s_zone_start[PROF_FRAME];
    if (dt < 0.0) dt = 0.0;
    s_zone_acc_s[PROF_FRAME] = dt;

    /* Snapshot the per-frame accumulators into the ring + last_ms. */
    for (int i = 0; i < PROF_COUNT; ++i) {
        float ms = (float)(s_zone_acc_s[i] * 1000.0);
        s_zone_last_ms[i]      = ms;
        s_ring_ms[s_ring_head][i] = ms;
    }
    s_ring_head = (s_ring_head + 1) % PROF_RING_FRAMES;
    if (s_ring_count < PROF_RING_FRAMES) s_ring_count++;
    s_frames_logged++;

    /* CSV row. */
    if (s_csv) {
        fprintf(s_csv, "%d", s_frames_logged);
        for (int i = 0; i < PROF_COUNT; ++i) {
            fprintf(s_csv, ",%.4f", (double)s_zone_last_ms[i]);
        }
        fputc('\n', s_csv);
    }
}

float profile_zone_ms(ProfSection s) {
    if (s < 0 || s >= PROF_COUNT) return 0.0f;
    return s_zone_last_ms[s];
}

float profile_frame_ms(void) {
    return s_zone_last_ms[PROF_FRAME];
}

int profile_frames_logged(void) { return s_frames_logged; }

/* Insertion sort over the ring's float column for zone `s`. The ring
 * is at most 256 entries — insertion sort is fine and avoids pulling
 * in qsort + a comparator callback. */
static int collect_zone_sorted(ProfSection s, float *out) {
    int n = s_ring_count;
    for (int i = 0; i < n; ++i) {
        float v = s_ring_ms[i][s];
        int j = i;
        while (j > 0 && out[j-1] > v) { out[j] = out[j-1]; --j; }
        out[j] = v;
    }
    return n;
}

static float percentile(const float *sorted, int n, float pct) {
    if (n <= 0) return 0.0f;
    if (pct <= 0.0f) return sorted[0];
    if (pct >= 1.0f) return sorted[n-1];
    float idx = pct * (float)(n - 1);
    int   lo  = (int)idx;
    float frac = idx - (float)lo;
    if (lo + 1 >= n) return sorted[lo];
    return sorted[lo] * (1.0f - frac) + sorted[lo+1] * frac;
}

float profile_zone_p99_ms(ProfSection s) {
    if (s < 0 || s >= PROF_COUNT) return 0.0f;
    float buf[PROF_RING_FRAMES];
    int n = collect_zone_sorted(s, buf);
    return percentile(buf, n, 0.99f);
}

float profile_zone_p95_ms(ProfSection s) {
    if (s < 0 || s >= PROF_COUNT) return 0.0f;
    float buf[PROF_RING_FRAMES];
    int n = collect_zone_sorted(s, buf);
    return percentile(buf, n, 0.95f);
}

float profile_zone_median_ms(ProfSection s) {
    if (s < 0 || s >= PROF_COUNT) return 0.0f;
    float buf[PROF_RING_FRAMES];
    int n = collect_zone_sorted(s, buf);
    return percentile(buf, n, 0.50f);
}

void profile_csv_open(const char *path) {
    if (s_csv) return;
    if (!path || !*path) return;
    s_csv = fopen(path, "w");
    if (!s_csv) {
        LOG_W("profile_csv_open: failed to open '%s'", path);
        return;
    }
    fputs("frame", s_csv);
    for (int i = 0; i < PROF_COUNT; ++i) {
        fprintf(s_csv, ",%s_ms", s_zone_names[i]);
    }
    fputc('\n', s_csv);
    fflush(s_csv);
    LOG_I("profile: csv -> %s", path);
}

void profile_csv_close(void) {
    if (!s_csv) return;
    fflush(s_csv);
    fclose(s_csv);
    s_csv = NULL;
}

int profile_csv_is_open(void) { return s_csv != NULL; }

void profile_format_summary(char *buf, int buflen) {
    if (!buf || buflen <= 0) return;
    int n = snprintf(buf, (size_t)buflen,
        "frame=%.2fms sim=%.2f draw_w=%.2f post=%.2f blit=%.2f hud=%.2f present=%.2f",
        (double)profile_zone_ms(PROF_FRAME),
        (double)profile_zone_ms(PROF_SIM_STEPS),
        (double)profile_zone_ms(PROF_DRAW_WORLD),
        (double)profile_zone_ms(PROF_DRAW_POST),
        (double)profile_zone_ms(PROF_DRAW_BLIT),
        (double)profile_zone_ms(PROF_DRAW_HUD),
        (double)profile_zone_ms(PROF_PRESENT));
    (void)n;
}

int profile_p99_under_budget(float budget_ms) {
    return profile_zone_p99_ms(PROF_FRAME) <= budget_ms ? 1 : 0;
}

void profile_print_summary_to_stdout(void) {
    if (s_ring_count <= 0) {
        fprintf(stdout, "profile: no frames recorded\n");
        return;
    }
    /* Per-zone table. */
    fprintf(stdout, "\n=== profile summary (%d frames in rolling window, %d total) ===\n",
            s_ring_count, s_frames_logged);
    fprintf(stdout, "%-14s  %8s  %8s  %8s  %8s\n",
            "zone", "median", "p95", "p99", "max");
    float buf[PROF_RING_FRAMES];
    for (int z = 0; z < PROF_COUNT; ++z) {
        int n = collect_zone_sorted((ProfSection)z, buf);
        float med = percentile(buf, n, 0.50f);
        float p95 = percentile(buf, n, 0.95f);
        float p99 = percentile(buf, n, 0.99f);
        float mx  = n > 0 ? buf[n-1] : 0.0f;
        fprintf(stdout, "%-14s  %8.3f  %8.3f  %8.3f  %8.3f\n",
                s_zone_names[z], (double)med, (double)p95,
                (double)p99, (double)mx);
    }

    /* Frame-wide derived stats. */
    int n_frame = collect_zone_sorted(PROF_FRAME, buf);
    float med   = percentile(buf, n_frame, 0.50f);
    int over16 = 0, over33 = 0;
    for (int i = 0; i < n_frame; ++i) {
        if (buf[i] > 16.6f) over16++;
        if (buf[i] > 33.3f) over33++;
    }
    float fps_med = (med > 0.0f) ? (1000.0f / med) : 0.0f;
    fprintf(stdout, "\nmedian FPS:        %.1f\n", (double)fps_med);
    fprintf(stdout, "%%frames > 16.6ms:  %.1f%%\n",
            (double)(100.0f * (float)over16 / (float)n_frame));
    fprintf(stdout, "%%frames > 33.3ms:  %.1f%%\n",
            (double)(100.0f * (float)over33 / (float)n_frame));
    fflush(stdout);
}
