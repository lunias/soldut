#pragma once

/*
 * editor render helpers — the canvas-side draws (doc, polygons, spawns,
 * pickups, decals, ambient zones, flags). Shared between the
 * interactive editor (main.c) and shot mode (shotmode.c) so the
 * scripted runner sees the same pixels the user sees.
 *
 * UI panels + modals stay in main.c — shot mode's primary subject is
 * the document, not the chrome.
 */

#include "doc.h"

#include "raylib.h"

#include <stdint.h>

/* Color the renderer assigns to a tile based on its flag mask. */
Color tile_color(LvlTile t);

/* Color for a polygon kind. Reads palette_poly_kinds() so the editor
 * and the level palette stay in sync. */
Color poly_color(uint16_t kind);

/* Convert an RGBA8 packed uint32 to a raylib Color. */
Color color_from_rgba(uint32_t rgba);

/* Draw the document's tiles + polygons + spawns + pickups + decos +
 * ambient zones + flag bases. Caller owns BeginDrawing / BeginMode2D. */
void draw_doc(const EditorDoc *d);
