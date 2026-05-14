#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * map_thumb — render a 256×144 PNG-encoded preview of a Level.
 *
 * Used by:
 *   - tools/cook_maps   (regenerates thumbs alongside the 8 builtin .lvls)
 *   - tools/cook_thumbs (build step that refreshes sidecar PNGs)
 *   - tools/editor      (writes a thumb whenever the user saves a .lvl)
 *   - src/level_io      (encodes thumb bytes into the THMB lump)
 *
 * The renderer reads the same Level fields runtime + editor agree on
 * (tiles + polygons + spawns + flags + pickups), so the preview always
 * matches what the player will load.
 *
 * raylib must be initialised on the calling thread (the implementation
 * uses raylib's Image API — software pixel writes, no GL context
 * needed, but raylib's allocator hooks are still consulted).
 */

struct Level;

#define MAP_THUMB_W 256
#define MAP_THUMB_H 144

/* Render `L` into a freshly-allocated PNG byte buffer. Returns NULL on
 * failure; on success, `*out_size` receives the byte count and the
 * caller MUST `MemFree` the buffer (raylib's allocator). */
unsigned char *map_thumb_encode_png(const struct Level *L, int *out_size);

/* Render + write to disk. Returns true on success. */
bool map_thumb_write_png(const struct Level *L, const char *out_path);
