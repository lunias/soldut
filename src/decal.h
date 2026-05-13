#pragma once

#include "math.h"

/*
 * The splat layer — a render-target-sized texture that dying blood
 * particles bake into. Drawing the layer back is one textured quad per
 * frame; thousands of decals at zero per-frame cost.
 *
 * Kept simple at M1: one layer the size of the level (no chunking). For
 * the M1 tutorial map (3200×1280) that's ~16 MB, well under the budget.
 */

void decal_init(int level_width_px, int level_height_px);
void decal_shutdown(void);

/* Paint a single blood splat at world-space `pos`. Safe to call from
 * anywhere in the simulate-step (we batch into one BeginTextureMode pair
 * inside decal_flush_pending, which the renderer calls once per frame). */
void decal_paint_blood(Vec2 pos, float radius);

/* M6 P02 — Paint a single scorch mark at world-space `pos`. Same queue
 * + flush shape as decal_paint_blood; different stamp (dark
 * brown/grey discs). Used by mech_jet_fx for grounded-jet impingement
 * scorch marks and by ignition-flash takeoff scars. Scorch decals are
 * permanent for the round (TRADE_OFFS.md). */
void decal_paint_scorch(Vec2 pos, float radius);

/* Called by the renderer once per frame, *outside* its main BeginMode2D
 * pair, to flush any queued paints into the layer. (raylib forbids
 * nesting BeginTextureMode inside BeginMode2D.) */
void decal_flush_pending(void);

/* Draw the layer back into the scene. Caller is inside BeginMode2D. */
void decal_draw_layer(void);

void decal_clear(void);
