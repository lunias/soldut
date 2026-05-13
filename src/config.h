#pragma once

#include "match.h"
#include "version.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * config — soldut.cfg parsing.
 *
 * Hosts can drop a `soldut.cfg` next to the executable to set match
 * defaults. CLI flags override the file (the file is the "saved
 * defaults", not the source of truth at runtime).
 *
 * Format: key=value, one per line, '#' comments. Values are trimmed.
 *
 *     # ports + capacity
 *     port=23073
 *     max_players=16
 *
 *     # match rules
 *     mode=ffa            # ffa | tdm | ctf
 *     score_limit=25
 *     time_limit=600      # seconds
 *     friendly_fire=0
 *     auto_start_seconds=60
 *
 *     # rotation (comma-separated short names from maps.c / match modes)
 *     map_rotation=foundry,slipstream,reactor
 *     mode_rotation=ffa,ffa,tdm
 *
 * Anything missing keeps its built-in default. Anything malformed logs
 * a warning and falls back to the default.
 */

#define CONFIG_ROTATION_MAX 8

typedef struct ServerConfig {
    /* Connection. */
    uint16_t  port;
    int       max_players;

    /* Snapshot broadcast rate in Hz. Default 60 for crisp remote-player
     * motion. 30 (the M2 default) is still valid for tight upstream
     * bandwidth on 16+ player public hosts. The interp delay is derived
     * as `3 * (1000 / snapshot_hz)` floored at NET_INTERP_DELAY_MS
     * (100 ms) for WAN jitter tolerance — see net.h. Capped at 60
     * because the sim is fixed at 60 Hz; broadcasting faster than the
     * sim has nothing new to say. */
    int       snapshot_hz;

    /* Optional render-interp-delay override (ms). 0 = derive from
     * snapshot_hz via the standard formula (the default).
     * Non-zero = use this value verbatim, applied both server-side
     * (lag-comp offset) and client-side (remote-mech render time)
     * after the same 40..200 ms clamp. Knob added in wan-fixes-2 so
     * hosts on jittery WAN can bump the buffer past the formula's
     * defaults; LAN-only hosts can dial lower to claw back latency. */
    int       interp_delay_ms;

    /* Pre-round countdown in seconds (wan-fixes-5). The lobby auto-
     * start fires → match enters COUNTDOWN → start_round spawns
     * mechs. Default 5 s gives players time to read the map / mode
     * line. Shot tests want this much shorter so PNG timing can land
     * inside MATCH_PHASE_ACTIVE. 0 keeps the in-code default. */
    float     countdown_default;

    /* Match rules. */
    MatchModeId mode;
    int         score_limit;
    float       time_limit;
    bool        friendly_fire;
    float       auto_start_seconds;

    /* Number of rounds played per "match." Players ready up once at
     * the start of a match; rounds within a match transition
     * seamlessly (no return to the lobby UI). After the last round
     * the game returns to the lobby for the next match. */
    int         rounds_per_match;

    /* M6 P03 — internal-render-resolution cap (line count). The world
     * + post-process pass run at this height; the result is bilinear-
     * upscaled to the window. Honours the original v1 commitment in
     * documents/10-performance-budget.md:208. 0 means "match window
     * height" (no cap), useful for shot tests that want pixel-byte-
     * identical output across runs. Anything between 360 and 4320 is
     * accepted; anything outside that band logs a warning and falls
     * back to the default. */
    int       internal_res_h;

    /* Rotations. */
    int       map_rotation [CONFIG_ROTATION_MAX];
    int       map_rotation_count;
    MatchModeId mode_rotation[CONFIG_ROTATION_MAX];
    int       mode_rotation_count;

    /* Bookkeeping. */
    bool      loaded_from_file;
    char      source_path[256];
} ServerConfig;

/* Populate cfg with built-in defaults (see config.c). Always succeeds. */
void config_defaults(ServerConfig *cfg);

/* Load + parse `path` if it exists. Returns true if a file was read
 * (regardless of warnings). Missing file is not an error — defaults
 * just stay in place and `loaded_from_file` remains false. */
bool config_load(ServerConfig *cfg, const char *path);

/* Pull the next map / mode from the rotation. Caller passes the round
 * counter; we round-robin through the rotation table. Falls back to
 * the static `mode`/`map_rotation[0]` if rotation is empty. */
int         config_pick_map (const ServerConfig *cfg, int round_index);
MatchModeId config_pick_mode(const ServerConfig *cfg, int round_index);
