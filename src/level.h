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

/* Same as level_ray_hits, but ALSO writes the unit-vector surface
 * normal at the hit point (pointing AWAY from the surface, into the
 * open). Used by projectile.c::projectile_step to reflect bouncy
 * grenades off arbitrary surfaces (sloped polygons) instead of the
 * old axis-aligned approximation that nudged grenades INTO sloped
 * polys. Outputs are left untouched on no-hit. */
bool level_ray_hits_normal(const Level *level, Vec2 a, Vec2 b,
                           float *out_t, float *out_nx, float *out_ny);

/* M6 P09 — Kinematic-physics ray-cast. Treats TILE_F_ONE_WAY as
 * non-blocking when the ray is moving UPWARD (dy < 0), so an
 * integrate-time sweep doesn't pre-clamp a jet'ing particle just
 * below an editor-painted ONE_WAY platform. Used by physics_integrate
 * (the tunneling-prevention sweep). Non-physics callers — grapple
 * rope swept-test, line-of-sight checks — stick with the conservative
 * `level_ray_hits` above. */
bool level_ray_hits_kinematic(const Level *level, Vec2 a, Vec2 b, float *out_t);

/* World-space bounds. */
float level_width_px(const Level *level);
float level_height_px(const Level *level);
