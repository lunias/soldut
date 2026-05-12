#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * proc_spawn — cross-platform child-process management for the
 * dedicated-server flow.
 *
 * The "Host Server" UI button used to run a listen-server (server +
 * client in one process). That gave the host an asymmetric advantage:
 * the host's own input fed the simulation immediately (zero latency)
 * while the joining client ate one-way + interp delay. For a
 * competitive shooter we want every player to render identically.
 *
 * Post-fix flow: the host UI spawns a CHILD process of the same
 * Soldut binary in `--dedicated PORT` mode (no raylib, no audio, just
 * sim + net), then connects to it as a regular client. Both players
 * are now clients of the same server — same prediction path, same
 * interp delay, same lag-comp.
 *
 * This header is the minimum surface to start / stop / probe a child
 * process. POSIX (Linux/macOS) and Windows are both supported.
 */

typedef struct {
    /* Opaque platform handle:
     *   POSIX  — pid_t cast to int64 (>= 1 on success)
     *   Win32  — HANDLE cast to int64 (non-zero on success)
     * Use the helpers below instead of touching it directly. */
    int64_t native;
    int     pid;          /* numeric PID for logging; 0 if N/A */
} ProcHandle;

#define PROC_HANDLE_NULL  ((ProcHandle){0, 0})

static inline bool proc_handle_valid(ProcHandle h) {
    return h.native != 0;
}

/* Argv to pass to the spawned child. argv[0] is the executable; trailing
 * NULL is required. Search PATH if argv[0] has no slash. */
ProcHandle proc_spawn(const char *const *argv);

/* Convenience: launches a sibling of the current process. argv0_self is
 * argv[0] from main() — used to locate the executable. Args may
 * include port, mode flags, etc. */
ProcHandle proc_spawn_self(const char *argv0_self, const char *const *extra_argv);

/* wan-fixes-16 — "launcher pattern" indirect spawn for Windows.
 *
 * Windows silently drops UDP packets between a parent process and
 * its direct CreateProcess-spawned child, even with bInheritHandles=
 * FALSE + CREATE_NO_WINDOW + CREATE_BREAKAWAY_FROM_JOB. The filter
 * appears to be PPID-based at packet time. Workaround: spawn an
 * intermediate Soldut.exe in `--launch-dedicated PORT [args]` mode.
 * The launcher immediately CreateProcess's the real dedi with
 * `--dedicated PORT [args]` and exits with the dedi's PID as its
 * exit code. The dedi's PPID then points at a dead launcher process,
 * breaking the direct parent-child relationship between the UI and
 * the dedi.
 *
 * This function:
 *   1. Substitutes the first `--dedicated` token in extra_argv with
 *      `--launch-dedicated` and proc_spawn's the launcher.
 *   2. WaitForSingleObject's the launcher for up to `timeout_ms`.
 *   3. GetExitCodeProcess to read the dedi PID.
 *   4. OpenProcess on the dedi PID with TERMINATE | SYNCHRONIZE rights.
 *   5. Returns a ProcHandle wrapping the dedi handle for kill-on-exit.
 *
 * On POSIX this is just a passthrough to proc_spawn_self — the bug
 * doesn't exist there and direct spawn works fine. */
ProcHandle proc_spawn_via_launcher(const char *argv0_self,
                                    const char *const *extra_argv,
                                    int timeout_ms);

/* Returns true if the child is still running. Non-blocking. */
bool proc_alive(ProcHandle h);

/* Politely ask the child to exit (POSIX: SIGTERM; Win32: WM_CLOSE
 * fallback to TerminateProcess on console-style apps). */
void proc_terminate(ProcHandle h);

/* Last-resort kill (POSIX: SIGKILL; Win32: TerminateProcess). */
void proc_kill(ProcHandle h);

/* Wait up to `timeout_ms` for the child to exit; returns true if it
 * exited within the window. 0 = non-blocking poll. */
bool proc_wait(ProcHandle h, int timeout_ms);

/* Close the handle's OS-side resources. Idempotent. After this the
 * handle should be reset to PROC_HANDLE_NULL by the caller. */
void proc_close(ProcHandle h);

/* ---- Portable time + sleep ---------------------------------------- *
 *
 * Lives here because proc_spawn.c already has the Win32 vs POSIX
 * platform fork wired up for spawn/kill/wait, and main.c's dedicated
 * server loop needs monotonic time + sleep without dragging
 * <windows.h> into the same TU as raylib (which collides on Rectangle
 * / CloseWindow). Use these from any TU; the implementation is
 * isolated in proc_spawn.c.
 *
 *   time_now_ms()     — milliseconds since arbitrary epoch
 *                       (CLOCK_MONOTONIC on POSIX, QPC on Win32).
 *                       Suitable for delta-time / scheduling.
 *   time_sleep_ms(ms) — block for at least `ms` milliseconds. Negative
 *                       or zero is a no-op. */
double time_now_ms(void);
void   time_sleep_ms(int ms);
