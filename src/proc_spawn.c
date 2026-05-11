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
    /* Inherit stdout/stderr/stdin so console launches show the
     * dedicated server's log in the same terminal. */
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    BOOL ok = CreateProcessA(
        /*lpApplicationName*/ NULL,
        /*lpCommandLine*/     cmd,
        NULL, NULL,
        /*bInheritHandles*/   TRUE,
        /*dwCreationFlags*/   0,
        NULL, NULL,
        &si, &pi);
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
