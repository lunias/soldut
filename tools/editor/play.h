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

/* Loadout to pass through on F5 — populated by main.c from the editor's
 * --test-chassis / --test-primary / --test-secondary / --test-armor /
 * --test-jetpack CLI args. Empty strings mean "fall back to the game's
 * default" (the same default the game uses when its own flags aren't
 * given). Names match the spelling the game's CLI parser expects
 * (case-insensitive for chassis/armor/jetpack; weapons by display
 * name e.g. "Grappling Hook"). */
typedef struct {
    char chassis  [32];
    char primary  [32];
    char secondary[32];
    char armor    [32];
    char jetpack  [32];
} TestPlayLoadout;

/* Save the doc to a temp .lvl, then fork the game binary with
 * --test-play <abs_path> + any non-empty loadout flags from `lo`.
 * Pass NULL for `lo` (or a zeroed struct) to spawn with the game's
 * default loadout. Returns true if the fork started; false on save
 * failure or fork failure (logged via LOG_E). */
bool play_test(EditorDoc *d, const TestPlayLoadout *lo);
