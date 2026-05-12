/* _POSIX_C_SOURCE 200809L for posix_spawn / kill / nanosleep /
 * waitpid prototypes under -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "proc_spawn.h"

#include "log.h"

#include <string.h>

#if defined(_WIN32)
  /* Trim the Windows API surface so it plays nice with raylib's
   * own globals (Rectangle / CloseWindow / etc.). proc_spawn.c
   * only needs the process / synchronization APIs. */
  #define WIN32_LEAN_AND_MEAN
  #define NOGDI
  #define NOUSER
  #include <windows.h>
#else
  #include <errno.h>
  #include <signal.h>
  #include <stdio.h>
  #include <stdlib.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <time.h>
  #include <unistd.h>
  /* posix_spawn is in <spawn.h> but glibc / musl / macOS all expose
   * it. We use it instead of fork+exec because (a) it works inside a
   * raylib-init'd process without inheriting the GL context surprises
   * fork() brings, and (b) it's a single syscall on Linux's vfork
   * path so spawning is fast. */
  #include <spawn.h>
  extern char **environ;
#endif

/* ---- POSIX implementation ----------------------------------------- */

#if !defined(_WIN32)

ProcHandle proc_spawn(const char *const *argv) {
    if (!argv || !argv[0]) return PROC_HANDLE_NULL;

    pid_t pid = 0;
    /* posix_spawn wants a non-const argv. Cast away const — the
     * standard allows it because the spawned child gets its own copy. */
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL,
                          (char *const *)argv, environ);
    if (rc != 0) {
        LOG_E("proc_spawn: posix_spawnp(%s) failed: %s", argv[0], strerror(rc));
        return PROC_HANDLE_NULL;
    }
    LOG_I("proc_spawn: launched '%s' as pid %d", argv[0], (int)pid);
    return (ProcHandle){ .native = (int64_t)pid, .pid = (int)pid };
}

ProcHandle proc_spawn_self(const char *argv0_self, const char *const *extra_argv) {
    if (!argv0_self) return PROC_HANDLE_NULL;
    enum { MAX_ARGS = 16 };
    const char *argv[MAX_ARGS + 2];
    int n = 0;
    argv[n++] = argv0_self;
    if (extra_argv) {
        while (extra_argv[0] && n < MAX_ARGS) {
            argv[n++] = *extra_argv++;
        }
    }
    argv[n] = NULL;
    return proc_spawn(argv);
}

ProcHandle proc_spawn_via_launcher(const char *argv0_self,
                                    const char *const *extra_argv,
                                    int timeout_ms) {
    /* POSIX path: no launcher pattern needed. Direct spawn works. */
    (void)timeout_ms;
    return proc_spawn_self(argv0_self, extra_argv);
}

bool proc_alive(ProcHandle h) {
    if (!proc_handle_valid(h)) return false;
    pid_t pid = (pid_t)h.native;
    int status = 0;
    pid_t rc = waitpid(pid, &status, WNOHANG);
    if (rc == 0) return true;             /* still running */
    if (rc == pid) return false;           /* reaped */
    if (rc < 0 && errno == ECHILD) return false;
    return true;                           /* be conservative on other errors */
}

void proc_terminate(ProcHandle h) {
    if (!proc_handle_valid(h)) return;
    pid_t pid = (pid_t)h.native;
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
        LOG_W("proc_terminate(pid=%d): kill(SIGTERM) failed: %s",
              (int)pid, strerror(errno));
    }
}

void proc_kill(ProcHandle h) {
    if (!proc_handle_valid(h)) return;
    pid_t pid = (pid_t)h.native;
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        LOG_W("proc_kill(pid=%d): kill(SIGKILL) failed: %s",
              (int)pid, strerror(errno));
    }
}

bool proc_wait(ProcHandle h, int timeout_ms) {
    if (!proc_handle_valid(h)) return true;
    pid_t pid = (pid_t)h.native;
    /* Poll-based; finer granularity than nanosleep-and-waitpid loops
     * but still simple. ~10 ms granularity. */
    int waited = 0;
    while (waited <= timeout_ms) {
        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) return true;
        if (rc < 0 && errno == ECHILD) return true;
        if (timeout_ms == 0) return false;
        struct timespec ts = { 0, 10 * 1000 * 1000L };  /* 10 ms */
        nanosleep(&ts, NULL);
        waited += 10;
    }
    return false;
}

void proc_close(ProcHandle h) {
    /* POSIX: the OS reaps via waitpid (above). Nothing else to
     * release here. */
    (void)h;
}

double time_now_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

void time_sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts = { ms / 1000, (long)((ms % 1000) * 1000000L) };
    nanosleep(&ts, NULL);
}

#else  /* _WIN32 */

/* ---- Win32 implementation ----------------------------------------- */

ProcHandle proc_spawn(const char *const *argv) {
    if (!argv || !argv[0]) return PROC_HANDLE_NULL;

    /* Build a single command line. Quote each argv element so paths /
     * args with spaces survive Windows' parsing rules. This is the
     * "good enough" subset that handles our --dedicated PORT usage;
     * for richer escaping we'd need to model CommandLineToArgvW
     * exactly. */
    char cmd[1024];
    size_t off = 0;
    for (int i = 0; argv[i]; ++i) {
        const char *a = argv[i];
        if (i > 0 && off < sizeof(cmd) - 1) cmd[off++] = ' ';
        if (off >= sizeof(cmd) - 1) break;
        cmd[off++] = '"';
        for (const char *p = a; *p && off < sizeof(cmd) - 2; ++p) {
            if (*p == '"' && off < sizeof(cmd) - 3) cmd[off++] = '\\';
            cmd[off++] = *p;
        }
        if (off < sizeof(cmd) - 1) cmd[off++] = '"';
    }
    cmd[off] = '\0';

    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    /* wan-fixes-16 — DO NOT inherit handles from the parent, AND
     * break away from the parent's Job Object.
     *
     * Background: the M2-era Host Server flow ran a listen-server
     * (server + client in the same process) and worked fine on
     * Windows. wan-fixes-5 split that into a parent UI + a spawned
     * `--dedicated PORT` child of the same binary; on Windows the
     * parent's UDP CONNECT packets to 127.0.0.1:<port> never reach
     * the spawned child. Diagnostics confirmed:
     *
     *   - getsockname shows the dedi bound 0.0.0.0:23073
     *   - FIONREAD on the dedi's socket stays at 0 across the
     *     parent's full 5 s connect window
     *   - A separate raw UDP probe sent from the parent to
     *     127.0.0.1:23080 with the dedi listening on a fresh
     *     non-blocking socket bound to the same port — sendto
     *     reports 38 bytes sent, recvfrom on the dedi returns
     *     nothing.
     *   - Two INDEPENDENT Soldut.exe processes (no parent-child
     *     relationship) talk over UDP loopback fine.
     *   - A separate machine (the user's friend's) reproduces the
     *     same behavior.
     *
     * Pattern: Windows silently drops UDP packets between a parent
     * process and its CreateProcess-spawned child when both are
     * descendants of an Explorer-launched ancestor. The most likely
     * mechanism is a Job Object that newer Explorer puts launched
     * processes into; the child inherits the job; UDP between
     * processes in the same restricted job is dropped.
     *
     * CREATE_BREAKAWAY_FROM_JOB tells CreateProcess to put the new
     * process OUTSIDE any job the parent is in. This requires the
     * parent's job to have JOB_OBJECT_LIMIT_BREAKAWAY_OK set —
     * Explorer's default job does, so this works in practice. If a
     * future parent is in a stricter job, CreateProcess returns
     * ERROR_ACCESS_DENIED (5) and we retry without the flag below.
     *
     * Also keeping handle inheritance off (no STARTF_USESTDHANDLES,
     * bInheritHandles=FALSE) so the dedi's I/O is fully its own —
     * already established in the previous wan-fixes-16 commit; the
     * dedi writes everything to soldut-server.log.
     *
     * CREATE_NO_WINDOW keeps the child headless (no visible console
     * flash) — still a console-subsystem binary, just hidden. */
    DWORD base_flags = CREATE_NO_WINDOW;
    DWORD flags = base_flags | CREATE_BREAKAWAY_FROM_JOB;

    BOOL ok = CreateProcessA(
        /*lpApplicationName*/ NULL,
        /*lpCommandLine*/     cmd,
        NULL, NULL,
        /*bInheritHandles*/   FALSE,
        /*dwCreationFlags*/   flags,
        NULL, NULL,
        &si, &pi);
    if (!ok) {
        DWORD err = GetLastError();
        /* If the parent's job doesn't allow breakaway, ERROR_ACCESS_DENIED
         * comes back. Retry without the flag — at least the spawn itself
         * succeeds, even if the parent↔child UDP path will still be
         * blocked by the job. We log the fallback so it's visible. */
        if (err == ERROR_ACCESS_DENIED) {
            LOG_W("proc_spawn: CREATE_BREAKAWAY_FROM_JOB denied "
                  "(parent job doesn't allow breakaway) — retrying "
                  "without it; UDP loopback to child may not work");
            flags = base_flags;
            ok = CreateProcessA(
                /*lpApplicationName*/ NULL,
                /*lpCommandLine*/     cmd,
                NULL, NULL,
                /*bInheritHandles*/   FALSE,
                /*dwCreationFlags*/   flags,
                NULL, NULL,
                &si, &pi);
            if (ok) {
                LOG_I("proc_spawn: fallback CreateProcess (no breakaway) "
                      "succeeded");
            }
        }
    } else {
        LOG_I("proc_spawn: CreateProcess with CREATE_BREAKAWAY_FROM_JOB "
              "succeeded");
    }
    if (!ok) {
        LOG_E("proc_spawn: CreateProcess(%s) failed: code=%lu",
              argv[0], (unsigned long)GetLastError());
        return PROC_HANDLE_NULL;
    }
    CloseHandle(pi.hThread);
    LOG_I("proc_spawn: launched '%s' as pid %lu",
          argv[0], (unsigned long)pi.dwProcessId);
    return (ProcHandle){
        .native = (int64_t)(uintptr_t)pi.hProcess,
        .pid    = (int)pi.dwProcessId,
    };
}

ProcHandle proc_spawn_self(const char *argv0_self, const char *const *extra_argv) {
    if (!argv0_self) return PROC_HANDLE_NULL;
    enum { MAX_ARGS = 16 };
    const char *argv[MAX_ARGS + 2];
    int n = 0;
    argv[n++] = argv0_self;
    if (extra_argv) {
        while (extra_argv[0] && n < MAX_ARGS) {
            argv[n++] = *extra_argv++;
        }
    }
    argv[n] = NULL;
    return proc_spawn(argv);
}

ProcHandle proc_spawn_via_launcher(const char *argv0_self,
                                    const char *const *extra_argv,
                                    int timeout_ms) {
    /* Windows: substitute the first "--dedicated" token with
     * "--launch-dedicated" and spawn that. The launcher (Soldut.exe
     * in --launch-dedicated mode) re-spawns with "--dedicated", gets
     * the dedi's PID, exits with that PID as exit code. We then read
     * the exit code and OpenProcess() the dedi by PID. */
    if (!argv0_self) return PROC_HANDLE_NULL;

    enum { MAX_ARGS = 16 };
    const char *argv[MAX_ARGS + 2];
    int n = 0;
    argv[n++] = argv0_self;

    bool replaced = false;
    if (extra_argv) {
        while (extra_argv[0] && n < MAX_ARGS) {
            if (!replaced && strcmp(extra_argv[0], "--dedicated") == 0) {
                argv[n++] = "--launch-dedicated";
                replaced = true;
            } else {
                argv[n++] = extra_argv[0];
            }
            ++extra_argv;
        }
    }
    argv[n] = NULL;

    if (!replaced) {
        LOG_W("proc_spawn_via_launcher: no --dedicated token to "
              "rewrite — falling back to direct spawn");
        return proc_spawn(argv);
    }

    LOG_I("proc_spawn_via_launcher: starting launcher mode");
    ProcHandle launcher = proc_spawn(argv);
    if (!proc_handle_valid(launcher)) {
        LOG_E("proc_spawn_via_launcher: launcher spawn failed");
        return PROC_HANDLE_NULL;
    }

    int wait_ms = (timeout_ms > 0) ? timeout_ms : 2000;
    if (!proc_wait(launcher, wait_ms)) {
        LOG_E("proc_spawn_via_launcher: launcher did not exit within %d ms",
              wait_ms);
        proc_kill(launcher);
        proc_close(launcher);
        return PROC_HANDLE_NULL;
    }

    HANDLE launcher_h = (HANDLE)(uintptr_t)launcher.native;
    DWORD launcher_exit = 0;
    BOOL got_exit = GetExitCodeProcess(launcher_h, &launcher_exit);
    proc_close(launcher);
    if (!got_exit) {
        LOG_E("proc_spawn_via_launcher: GetExitCodeProcess failed: code=%lu",
              (unsigned long)GetLastError());
        return PROC_HANDLE_NULL;
    }

    /* Sentinel: launcher uses 0xFFFFFFFF when its own CreateProcess
     * fails; 0 if it never wrote a PID. Anything else is a real PID. */
    if (launcher_exit == 0 || launcher_exit == 0xFFFFFFFF) {
        LOG_E("proc_spawn_via_launcher: launcher reported invalid "
              "dedi PID (exit=%lu)", (unsigned long)launcher_exit);
        return PROC_HANDLE_NULL;
    }

    /* Open the dedi process by PID. We need TERMINATE + SYNCHRONIZE
     * + QUERY for proc_alive/wait/kill on the returned handle. */
    HANDLE dedi = OpenProcess(
        PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_INFORMATION,
        FALSE, launcher_exit);
    if (!dedi) {
        LOG_E("proc_spawn_via_launcher: OpenProcess(pid=%lu) failed: "
              "code=%lu (dedi exited already?)",
              (unsigned long)launcher_exit,
              (unsigned long)GetLastError());
        return PROC_HANDLE_NULL;
    }

    LOG_I("proc_spawn_via_launcher: dedi pid=%lu opened OK "
          "(launcher exited)", (unsigned long)launcher_exit);

    return (ProcHandle){
        .native = (int64_t)(uintptr_t)dedi,
        .pid    = (int)launcher_exit,
    };
}

bool proc_alive(ProcHandle h) {
    if (!proc_handle_valid(h)) return false;
    HANDLE p = (HANDLE)(uintptr_t)h.native;
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(p, &exit_code)) return false;
    return exit_code == STILL_ACTIVE;
}

void proc_terminate(ProcHandle h) {
    /* No graceful "please exit" path for headless console children on
     * Windows without a console attached; fall back to TerminateProcess
     * with exit-code 0 (which the dedicated child treats as a normal
     * shutdown — `g_dedicated_should_quit` doesn't get to flush, but
     * net_close + log_shutdown still run via OS process teardown). */
    proc_kill(h);
}

void proc_kill(ProcHandle h) {
    if (!proc_handle_valid(h)) return;
    HANDLE p = (HANDLE)(uintptr_t)h.native;
    /* If the child has already exited, TerminateProcess returns
     * ERROR_ACCESS_DENIED (5) on Windows — which we'd otherwise log
     * as a WARN and confuse the user into thinking something is
     * broken. Check the exit code first so an already-exited child
     * is a silent no-op. */
    DWORD exit_code = 0;
    if (GetExitCodeProcess(p, &exit_code) && exit_code != STILL_ACTIVE) {
        LOG_I("proc_kill(pid=%d): already exited (code=%lu) — no-op",
              h.pid, (unsigned long)exit_code);
        return;
    }
    if (!TerminateProcess(p, 0)) {
        LOG_W("proc_kill(pid=%d): TerminateProcess failed: code=%lu",
              h.pid, (unsigned long)GetLastError());
    }
}

bool proc_wait(ProcHandle h, int timeout_ms) {
    if (!proc_handle_valid(h)) return true;
    HANDLE p = (HANDLE)(uintptr_t)h.native;
    DWORD wait = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD rc = WaitForSingleObject(p, wait);
    return rc == WAIT_OBJECT_0;
}

void proc_close(ProcHandle h) {
    if (!proc_handle_valid(h)) return;
    HANDLE p = (HANDLE)(uintptr_t)h.native;
    CloseHandle(p);
}

double time_now_ms(void) {
    static LARGE_INTEGER s_freq = {0};
    if (s_freq.QuadPart == 0) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)s_freq.QuadPart;
}

void time_sleep_ms(int ms) {
    if (ms <= 0) return;
    Sleep((DWORD)ms);
}

#endif
