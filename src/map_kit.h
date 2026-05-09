#pragma once

#include "../third_party/raylib/src/raylib.h"

#include <stdbool.h>

/*
 * map_kit — per-map art bundle loaded from `assets/maps/<short>/`.
 *
 * The registry-level `MapDef` describes which map exists; this module
 * owns the GL-side textures for the *currently loaded* map's parallax
 * + tile atlas. There is exactly one bundle live at a time — replacing
 * one releases the previous bundle's textures.
 *
 * Files probed (per `documents/m5/08-rendering.md` §"Per-map kit assets"):
 *   assets/maps/<short>/parallax_far.png    — sky / distant skyline
 *   assets/maps/<short>/parallax_mid.png    — buildings / hills
 *   assets/maps/<short>/parallax_near.png   — foreground silhouettes
 *   assets/maps/<short>/tiles.png           — 256x256 tile atlas (8x8 grid of 32x32)
 *
 * Each is independently optional. A missing parallax_*.png leaves that
 * layer's `id == 0` and `draw_parallax_*` skips it. A missing
 * `tiles.png` leaves `tiles.id == 0` and `draw_level` falls back to the
 * M4 flat 2-tone tile rendering.
 *
 * The kit is loaded from inside `map_build` / `map_build_from_path` /
 * `map_build_for_descriptor` once `world->level` has been populated;
 * unloaded on `map_kit_unload()` (called from the same sites before the
 * next `map_kit_load`).
 *
 * Asset content lands at P15/P16 (ComfyUI parallax pipeline + per-kit
 * tile atlases). Until then every map's `MapKit` entries stay empty and
 * the renderer paints the M4 flat-fill fallback.
 */
typedef struct MapKit {
    Texture2D parallax_far;
    Texture2D parallax_mid;
    Texture2D parallax_near;
    Texture2D tiles;
    char      short_name[24];   /* loaded kit identifier; "" when empty */
} MapKit;

/* The single live kit. Other modules read directly. */
extern MapKit g_map_kit;

/* Load the per-map texture bundle from `assets/maps/<short_name>/`. If
 * any file is missing the corresponding texture stays at `id == 0` and
 * the renderer falls back gracefully. Idempotent — passing the same
 * short_name twice is a cheap no-op. Pass NULL/"" to release the
 * current kit (same as map_kit_unload). */
void map_kit_load(const char *short_name);

/* Drop every loaded texture and reset to "no kit". Called on shutdown
 * + before every map_kit_load that switches kits. */
void map_kit_unload(void);
