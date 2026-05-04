#pragma once

/*
 * editor shot mode — script-driven validation runner.
 *
 * Reads a `.shot` script (see grammar below), runs the editor's doc /
 * tool / undo APIs deterministically, captures PNG screenshots at
 * marked ticks, and emits assertions that can fail the run. Pairs
 * with the game's `src/shotmode.c` in spirit but this one drives the
 * editor's workflow rather than the game's input bitmask.
 *
 * Script grammar (line-oriented; '#' starts a comment; blank lines OK):
 *
 *   ## Header (anywhere before the first 'at'):
 *   window <w> <h>            initial window size, default 1280x800
 *   out <dir>                 output directory; default build/shots/editor/<scriptname>
 *   ticks <n>                 max ticks before forced end, default 600
 *   load <path>               initial .lvl to load
 *   new <w> <h>               blank doc dimensions in tiles
 *   contact_sheet <name>      composite all shots into <name>.png
 *      [cols <C>]              optional override for grid columns
 *      [cell <W> <H>]          optional override for cell pixel size
 *   panels on|off              render the editor's full UI chrome
 *                              in shots (default on); off shows only
 *                              the canvas + tool overlay + a small
 *                              top-right state summary.
 *
 *   ## Per-tick directives:
 *   at <tick> tool <name>             tile|poly|spawn|pickup|ambi|deco|meta
 *   at <tick> tile_flags <list>       comma-list: solid,ice,deadly,one_way,bg
 *   at <tick> poly_kind <name>        solid|ice|deadly|one_way|background
 *   at <tick> preset <name>           ramp_up_45 / ramp_up_30 / ... / none
 *
 *   at <tick> tile_paint <wx> <wy>    stamp the active tile-flags at world coords
 *   at <tick> tile_erase <wx> <wy>
 *   at <tick> tile_fill_rect <x0> <y0> <x1> <y1>   fill a world-pixel rectangle
 *
 *   at <tick> apply_preset <name> <wx> <wy>   drop a slope/alcove preset
 *
 *   at <tick> poly_begin
 *   at <tick> poly_vertex <wx> <wy>
 *   at <tick> poly_close
 *
 *   at <tick> spawn_add <wx> <wy> <team>
 *   at <tick> pickup_add <wx> <wy> <variant>
 *   at <tick> ambi_add <x> <y> <w> <h> <kind>
 *   at <tick> deco_add <wx> <wy>
 *   at <tick> flag_add <wx> <wy> <team>
 *
 *   at <tick> save <path>             direct doc_save (skips the modal)
 *   at <tick> load_file <path>        direct doc_load
 *   at <tick> new_doc <w> <h>         direct doc_new
 *
 *   at <tick> undo
 *   at <tick> redo
 *
 *   at <tick> open_help               toggle the help modal on
 *   at <tick> close_help
 *   at <tick> toggle_help             same as ui_help_toggle()
 *   at <tick> open_meta
 *   at <tick> close_meta
 *   at <tick> click_tool_button <name>  mirrors a real toolbar click
 *                                      (always opens meta when name=meta)
 *
 *   at <tick> mouse <sx> <sy>         set cursor for overlay rendering
 *   at <tick> camera_target <wx> <wy>
 *   at <tick> camera_zoom <z>
 *
 *   at <tick> shot <name>             write <out>/<name>.png after this frame
 *   at <tick> dump                    dump doc state to log
 *   at <tick> validate                run validate_doc; log results
 *
 *   at <tick> assert polys|spawns|pickups|ambis|decos|flags|tiles_solid|
 *                    validate_problems|active_tool|dirty|help_open|meta_open
 *                    <op> <value>
 *      <op> ∈ { ==, !=, >, >=, <, <= }
 *
 *   at <tick> end                     exit successfully
 */

int editor_shotmode_run(const char *script_path);
