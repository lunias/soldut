#pragma once

/*
 * Shot mode — deterministic, scriptable headless-style runner that
 * boots the real World + renderer, drives a tick-stamped input script,
 * and writes PNG screenshots at chosen ticks.
 *
 * The point is "what would I see if I played for 3 seconds and held D",
 * answered by a file an LLM (or human) can read.
 *
 * A .shot script is line-based, '#' starts a comment. Directives:
 *
 *     window <w> <h>           default 1280 720
 *     seed   <hi> <lo>         override World RNG; default game_init's seed
 *     out    <dir>             default build/shots
 *     spawn_at <wx> <wy>       move the player mech to (wx,wy) after the
 *                              level is built, before any tick runs.
 *                              Used to focus tests on a specific location
 *                              (e.g. next to the dummy platform) without
 *                              having to script the traversal.
 *     aim    <wx> <wy>         persistent aim, anchored in world space
 *     mouse  <sx> <sy>         persistent cursor in screen space; converted
 *                              to world each tick via the live camera —
 *                              this is what the player actually does.
 *
 *     at <tick> press   <btn>      hold a button down from this tick on
 *     at <tick> release <btn>      stop holding it
 *     at <tick> tap     <btn>      press for exactly one tick
 *     at <tick> aim     <wx> <wy>  switch to world-space aim from now on
 *     at <tick> mouse   <sx> <sy>  switch to screen-space cursor from now on
 *     at <tick> shot    <name>     write <out>/<name>.png after tick
 *     at <tick> end                exit
 *
 *     burst <prefix> from <t0> to <t1> every <k>
 *                              expands to one shot per k ticks in [t0,t1].
 *                              Names are formed as "<prefix>_t<NNN>".
 *                              Useful for ragdoll / fall sequences.
 *
 *     mouse_lerp <sx> <sy> from <t0> to <t1>
 *                              sweep the cursor linearly across screen
 *                              from its current position at t0 to (sx,sy)
 *                              at t1, one mouse event per tick.
 *
 *     loadout <chassis> <primary> <secondary> <armor> <jetpack>
 *                              configure the local mech's loadout BEFORE
 *                              any tick runs. Use '-' for any slot to
 *                              keep the default. Names match
 *                              chassis_id_from_name / weapon name table /
 *                              armor_def / jetpack_def (case-insensitive
 *                              prefix match for weapons).
 *                              Examples:
 *                                loadout Heavy "Mass Driver" - Heavy Standard
 *                                loadout Scout "Plasma SMG" Knife - Burst
 *
 *     contact_sheet <name> [cols <C>] [cell <W> <H>]
 *                              after the script completes, compose all
 *                              shot PNGs into a single grid image at
 *                              <out>/<name>.png. Defaults: 4 cols,
 *                              320×180 cells. A 16-shot 4×4 sheet at
 *                              defaults is 1280×720 — same pixel cost
 *                              as one full-size shot, so an LLM
 *                              reviewing the run pays for one image
 *                              instead of sixteen.
 *
 * `aim` and `mouse` are mutually exclusive — whichever directive (or
 * 'at' event) fired most recently wins. Use mouse for accurate gameplay
 * reproduction (camera follow changes the world target as you run);
 * use aim for tests pinned to a fixed world point.
 *
 * Buttons: left right jump jet crouch prone fire reload melee use swap dash.
 *
 * Returns process exit code (0 on success).
 */
int shotmode_run(const char *script_path);
