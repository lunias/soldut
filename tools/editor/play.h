#pragma once

/*
 * F5 test-play. Fork the game binary with --test-play <path> and let
 * the editor stay open. posix_spawn on Linux/macOS, CreateProcess on
 * Windows; Linux is the v1 path.
 *
 * The editor doesn't wait on the child — clicking back to the editor
 * window resumes editing. The child cleans up its own window when the
 * user closes it.
 */

#include "doc.h"

#include <stdbool.h>

/* Save the doc to a temp .lvl, then fork the game binary with
 * --test-play <abs_path>. Returns true if the fork started; false on
 * save failure or fork failure (logged via LOG_E). */
bool play_test(EditorDoc *d);
