#pragma once

/* M6 P06 — frame-time profiler.
 *
 * Always-on, zero-allocation, single-threaded (client thread). The
 * dedi server thread does not call any profile_* function.
 *
 * Cost per zone_begin/zone_end pair: two GetTime() calls + a subtract
 * + an add. ~30 ns total on the user's hardware. 14 zones × 30 ns =
 * ~420 ns/frame self-cost — three orders of magnitude under the
 * 16.6 ms frame budget.
 *
 * See documents/m6/06-perf-profiling-and-optimization.md §4. */

typedef enum {
    PROF_FRAME = 0,        /* total wall-clock for the frame */
    PROF_INPUT,            /* platform_sample_input + cursor read */
    PROF_SIM_STEPS,        /* sum of simulate_step calls this frame */
    PROF_BOT_STEPS,        /* host-side bot_step (auth only) */
    PROF_NET_POLL,         /* net_client_poll / net_server_poll */
    PROF_SNAP_INTERP,      /* snapshot_interp_remotes */
    PROF_RECONCILE,        /* reconcile_push_input + smoothing */
    PROF_DECAL_FLUSH,      /* decal_flush_pending */
    PROF_DRAW_WORLD,       /* BeginTextureMode(internal) ... End */
    PROF_DRAW_POST,        /* halftone shader pass into g_post_target */
    PROF_DRAW_BLIT,        /* DrawTexturePro upscale to backbuffer */
    PROF_DRAW_HUD,         /* hud_draw on backbuffer */
    PROF_DRAW_OVERLAY,     /* overlay callback (diag / summary) */
    PROF_PRESENT,          /* EndDrawing — SwapScreenBuffer + PollInputEvents */
    PROF_COUNT
} ProfSection;

void profile_init(void);              /* zero counters, latch start time */
void profile_frame_begin(void);       /* call once per frame before any zone */
void profile_frame_end(void);         /* call once per frame at the end */
void profile_zone_begin(ProfSection); /* GetTime() to start a zone */
void profile_zone_end  (ProfSection); /* GetTime() to end; accumulates ms */

/* Read accessors — used by the overlay + the CSV dump. */
float profile_zone_ms(ProfSection);    /* most recent frame's accumulated ms */
float profile_frame_ms(void);          /* PROF_FRAME's ms */
float profile_zone_p99_ms(ProfSection);/* over the rolling 256-frame window */
float profile_zone_p95_ms(ProfSection);
float profile_zone_median_ms(ProfSection);
int   profile_frames_logged(void);     /* count of completed frames */

const char *profile_zone_name(ProfSection s);

/* CSV dump support — header line written on open, one row per frame
 * on profile_frame_end. */
void  profile_csv_open(const char *path);
void  profile_csv_close(void);
int   profile_csv_is_open(void);

/* Build a one-line summary suitable for SHOT_LOG / the perf overlay
 * line. Caller-supplied buf, no allocation. */
void  profile_format_summary(char *buf, int buflen);

/* Dump a per-zone summary block to stdout — median/p95/p99/max ms
 * per zone, plus frame-wide median FPS and %-frames-over-16.6 / 33.3.
 * Called once at bench shutdown. */
void  profile_print_summary_to_stdout(void);

/* Returns 1 if every frame's PROF_FRAME ≤ 16.6 ms (p99 budget hold). */
int   profile_p99_under_budget(float budget_ms);
