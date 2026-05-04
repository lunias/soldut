#include "play.h"

#include "doc.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* raylib defines Rectangle / CloseWindow / ShowCursor as types and
 * functions that collide with wingdi.h + winuser.h. The lean defines
 * skip those Win32 headers; we only need kernel32 (CreateProcessA,
 * CloseHandle, STARTUPINFOA, PROCESS_INFORMATION). */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOGDI
#    define NOGDI
#  endif
#  ifndef NOUSER
#    define NOUSER
#  endif
#  include <windows.h>
#else
#  include <spawn.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char **environ;
#endif

#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

/* Resolve to an absolute path the child can find regardless of cwd. */
static int resolve_abs(const char *in, char *out, size_t out_cap) {
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (!_fullpath(buf, in, MAX_PATH)) return -1;
    snprintf(out, out_cap, "%s", buf);
    return 0;
#else
    char buf[PATH_MAX];
    if (!realpath(in, buf)) {
        /* realpath fails on non-existent paths. We only run after a
         * successful save, so the file exists — log + give up. */
        return -1;
    }
    snprintf(out, out_cap, "%s", buf);
    return 0;
#endif
}

/* Find the game binary. The editor sits at build/soldut_editor; the
 * game ships at ./soldut (or .exe on Windows). When the editor is run
 * from the project root via make, both share the cwd. */
static int find_game_binary(char *out, size_t out_cap) {
    static const char *candidates[] = {
        "./soldut", "./soldut.exe",
        "../soldut", "../soldut.exe",
        NULL,
    };
    for (int i = 0; candidates[i]; ++i) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            if (resolve_abs(candidates[i], out, out_cap) == 0) return 0;
        }
    }
    return -1;
}

bool play_test(EditorDoc *d) {
    /* Save to a temp .lvl. We pin a fixed name in the system tmpdir so
     * the child reads the most recent test-play state on each F5. */
    char tmp_path[512];
#if defined(_WIN32)
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = ".";
    snprintf(tmp_path, sizeof tmp_path, "%s/soldut-editor-test.lvl", tmp);
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(tmp_path, sizeof tmp_path, "%s/soldut-editor-test.lvl", tmp);
#endif

    if (!doc_save(d, tmp_path)) {
        LOG_E("editor: F5 — save to %s failed", tmp_path);
        return false;
    }

    char abs_lvl[512];
    if (resolve_abs(tmp_path, abs_lvl, sizeof abs_lvl) != 0) {
        snprintf(abs_lvl, sizeof abs_lvl, "%s", tmp_path);
    }

    char game_bin[512];
    if (find_game_binary(game_bin, sizeof game_bin) != 0) {
        LOG_E("editor: F5 — couldn't find ./soldut binary; build the game first");
        return false;
    }

#if defined(_WIN32)
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "\"%s\" --test-play \"%s\"", game_bin, abs_lvl);
    STARTUPINFOA si = { .cb = sizeof si };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        LOG_E("editor: F5 — CreateProcess failed: %lu", (unsigned long)GetLastError());
        return false;
    }
    /* We don't wait on the child — let the editor stay interactive.
     * The handles leak per-spawn but the editor's lifetime is short. */
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOG_I("editor: F5 — spawned %s (test-play %s)", game_bin, abs_lvl);
    return true;
#else
    char *argv[] = {
        game_bin,
        (char *)"--test-play",
        abs_lvl,
        NULL,
    };
    pid_t pid = 0;
    int rc = posix_spawn(&pid, game_bin, NULL, NULL, argv, environ);
    if (rc != 0) {
        LOG_E("editor: F5 — posix_spawn(%s): %s", game_bin, strerror(rc));
        return false;
    }
    /* Don't wait — the editor stays usable while the test play runs. */
    LOG_I("editor: F5 — spawned %s (pid %d, test-play %s)",
          game_bin, (int)pid, abs_lvl);
    return true;
#endif
}
