#pragma once

#include "input.h"
#include "match.h"
#include "mech.h"

#include <stdbool.h>

/*
 * prefs — persisted player preferences.
 *
 * The lobby UI carries a draft of the user's loadout / team / display
 * name on `LobbyUIState`. Pre-wan-fixes-8 that draft was in-memory
 * only: closing the client lost the user's picks. This module persists
 * it to `soldut-prefs.cfg` next to the executable, using the same
 * key=value text format as `soldut.cfg` (see src/config.c). Hand-
 * editable; the game rewrites it after every loadout / team / name
 * change in the lobby.
 *
 * File location: `soldut-prefs.cfg` resolved relative to the current
 * working directory — same convention as `soldut.cfg` and `bans.txt`.
 * On both macOS and Windows that resolves to the directory containing
 * the executable when the user launches by double-clicking the binary
 * out of the artifact zip. Power users running from a checkout get
 * the repo root.
 *
 * Format:
 *
 *     # soldut-prefs.cfg
 *     name=Player1
 *     chassis=Heavy
 *     primary=Mass Driver
 *     secondary=Frag Grenades
 *     armor=Heavy
 *     jetpack=Heavy
 *     team=1
 *     connect_addr=127.0.0.1:23073
 *     master_volume=0.80
 *
 * Names are matched case-insensitively against the chassis / weapon /
 * armor / jetpack tables. Missing keys keep their compile-time
 * defaults; malformed values log a warning and fall back to the
 * default for that field.
 */

#define PREFS_PATH         "soldut-prefs.cfg"
#define PREFS_NAME_BYTES    24
#define PREFS_ADDR_BYTES    64
#define PREFS_DEFAULT_VOLUME 0.80f

typedef struct {
    char        name[PREFS_NAME_BYTES];
    MechLoadout loadout;            /* chassis_id / primary_id / secondary_id /
                                       armor_id / jetpack_id */
    int         team;               /* MATCH_TEAM_*: 0=spectator, 1=ffa/red, 2=blue */
    char        connect_addr[PREFS_ADDR_BYTES];
    float       master_volume;      /* [0.0, 1.0] linear gain; +/- keys at
                                       runtime nudge by 5% and persist. */
    int         internal_res_h;     /* M6 P03 — line count for the internal
                                       world+post render target. 0 = match
                                       window. Defaults to 1080. Falls back to
                                       the same value in ServerConfig when this
                                       prefs file is missing. */
    float       shake_scale;        /* M6 P10 — scalar multiplier on screen
                                       shake amplitude + rotation. [0.0, 2.0].
                                       0 disables shake entirely; 1.0 is the
                                       baseline; CLI `--shake-scale F`
                                       overrides. Defaults to 1.0. */
} UserPrefs;

/* Fill `out` with the compile-time defaults. Always succeeds. */
void prefs_defaults(UserPrefs *out);

/* Load `path` into `out`. Returns true if a file was read (regardless
 * of warnings on individual fields). Missing file is NOT an error —
 * `out` keeps the defaults from prefs_defaults. Pass `PREFS_PATH` as
 * the default path. */
bool prefs_load(UserPrefs *out, const char *path);

/* Write `p` to `path` atomically (write to `<path>.tmp`, rename to
 * `<path>` on success). Returns true on success; logs a warning +
 * returns false if the file isn't writable. Idempotent — safe to call
 * after every UI cycle-button click. */
bool prefs_save(const UserPrefs *p, const char *path);
