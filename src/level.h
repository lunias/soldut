#pragma once

#include "arena.h"
#include "world.h"

/*
 * Level — tile-grid map. Free-floating polygons land at the next
 * milestone; M1 only needs SOLID/EMPTY tiles plus a flat floor and a
 * couple of walls so the player has somewhere to test the physics.
 *
 * The .lvl binary loader specified in [07-level-design.md] is also
 * deferred. This module knows how to build the M1 hard-coded test
 * level from code.
 */

struct Game;

/* Build the M1 tutorial map directly into `level` using the supplied
 * arena for the tile storage. */
void level_build_tutorial(Level *level, Arena *level_arena);

/* Tile lookup. Out-of-bounds tiles read as TILE_SOLID so that off-map
 * particles don't escape forever — we treat the map's edges as walls. */
TileKind level_tile_at(const Level *level, int tx, int ty);

/* Read a tile's full TILE_F_* bitmask. Out-of-bounds tiles read as
 * TILE_F_SOLID. Used by the slope-aware contact resolver to decide
 * friction (ICE) and persist the kind on the per-particle contact
 * record. */
uint16_t level_flags_at(const Level *level, int tx, int ty);

/* World-space helpers. */
int  level_world_to_tile(const Level *level, float x);
bool level_point_solid(const Level *level, Vec2 p);

/* Test a swept ray against the tile grid AND the free-polygon set;
 * returns the closest hit parameter t in (0, 1]. Used for the hitscan
 * path that doesn't cross a mech. Returns false if the ray is
 * unobstructed. BACKGROUND polygons are skipped. */
bool level_ray_hits(const Level *level, Vec2 a, Vec2 b, float *out_t);

/* World-space bounds. */
float level_width_px(const Level *level);
float level_height_px(const Level *level);
