#pragma once

#include "match.h"
#include "world.h"

#include <stdbool.h>

/*
 * ctf — Capture-the-Flag rules.
 *
 * Server-authoritative. The host runs `ctf_step` per tick to advance
 * timers, detect touches, and fire pickup / capture / return transitions.
 * Each transition pushes the new state onto the wire via
 * `net_server_broadcast_flag_state` (see net.h). Clients mirror the
 * state locally — they never run ctf_step beyond seeding the flags at
 * round start. Spec: `documents/m5/06-ctf.md`.
 *
 * The both-flags-home rule: scoring requires (a) the carrier touching
 * THEIR OWN flag at base, and (b) that flag being in HOME state — i.e.
 * the enemy isn't carrying it.
 *
 * carrier penalties (half jet, secondary disabled) live in mech.c via
 * the helpers below — `ctf` doesn't reach into the mech struct directly.
 */

/* Initialize World.flags[] at round start from level.flags (LvlFlag
 * records). When match.mode != CTF or the level has != 2 flag records,
 * leaves world->flag_count = 0 and world->flags[] zeroed. Idempotent;
 * safe to call from both the host's start_round and the client's
 * round-start handler. */
struct Game;
void ctf_init_round(World *w, MatchModeId mode);

/* Per-tick update — server-only. Decrements auto-return timers, scans
 * mechs for touch transitions, fires pickup / return / capture. Caller
 * must check `w->authoritative && match->phase == ACTIVE` upstream;
 * ctf_step also re-checks defensively. */
void ctf_step(struct Game *g, float dt);

/* True if `mech_id` is currently carrying ANY flag. Used by the carrier
 * penalty hooks (jet halve, secondary disable). O(2) — checks both
 * world->flags[]. */
bool ctf_is_carrier(const World *w, int mech_id);

/* World-space position to use for rendering / touch tests. HOME →
 * home_pos, DROPPED → dropped_pos, CARRIED → carrier's chest. */
Vec2 ctf_flag_position(const World *w, int flag_idx);

/* Drop any flag the dying mech is carrying. Called from mech_kill (no
 * Game* there). The flag transitions to DROPPED at the supplied position
 * with a 30-s auto-return timer; sets `world.flag_state_dirty` so
 * main.c broadcasts on the next tick. No-op when mode != CTF or the
 * mech isn't a carrier. */
void ctf_drop_on_death(World *w, MatchModeId mode, int mech_id, Vec2 death_pos);
