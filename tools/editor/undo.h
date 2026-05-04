#pragma once

/*
 * Undo / redo for the editor.
 *
 * Two command shapes:
 *  - Per-tile commands, batched into "strokes" between mouse-press and
 *    mouse-release. The whole stroke undoes atomically.
 *  - Per-object commands (poly/spawn/pickup/deco/ambi/flag add or
 *    remove), atomic per action.
 *
 * For very large tile operations (>UNDO_TILE_SNAPSHOT_THRESHOLD), we
 * snapshot the entire tile grid (malloc'd) and store that instead of
 * per-tile deltas. Cap keeps worst-case undo memory bounded for
 * bucket fill / map resize.
 *
 * Two stacks (undo + redo). New actions clear redo. Capped at
 * UNDO_RING; oldest entries silently drop on overflow.
 */

#include "doc.h"
#include "world.h"

#include <stdbool.h>
#include <stdint.h>

#define UNDO_RING                    64
#define UNDO_TILE_SNAPSHOT_THRESHOLD 256

typedef enum {
    UC_NONE = 0,
    UC_TILE_DELTAS,
    UC_TILE_SNAPSHOT,
    UC_POLY_ADD, UC_POLY_DEL,
    UC_SPAWN_ADD, UC_SPAWN_DEL,
    UC_PICKUP_ADD, UC_PICKUP_DEL,
    UC_DECO_ADD, UC_DECO_DEL,
    UC_AMBI_ADD, UC_AMBI_DEL,
    UC_FLAG_ADD, UC_FLAG_DEL,
} UndoCmdKind;

typedef struct UndoTileDelta {
    uint16_t x, y;
    LvlTile  before, after;
} UndoTileDelta;

typedef struct UndoCmd {
    UndoCmdKind kind;
    /* Tile-deltas variant. arrput-allocated; freed in undo_drop. */
    UndoTileDelta *deltas;
    /* Snapshot variant — full tile grid. malloc'd; freed in undo_drop. */
    LvlTile *snapshot;
    int      snap_w, snap_h;
    /* Object variants — payload + insertion index. */
    union {
        LvlPoly   poly;
        LvlSpawn  spawn;
        LvlPickup pickup;
        LvlDeco   deco;
        LvlAmbi   ambi;
        LvlFlag   flag;
    } rec;
    int idx;
} UndoCmd;

typedef struct UndoStack {
    UndoCmd undo[UNDO_RING];   int undo_count;
    UndoCmd redo[UNDO_RING];   int redo_count;

    bool    stroke_open;
    UndoCmd pending;            /* accumulating tile-paint stroke */
} UndoStack;

void undo_init   (UndoStack *u);
void undo_clear  (UndoStack *u);

/* Tile-paint stroke API. */
void undo_begin_tile_stroke(UndoStack *u);
void undo_record_tile      (UndoStack *u, int x, int y, LvlTile before, LvlTile after);
void undo_end_tile_stroke  (UndoStack *u, EditorDoc *d);

/* Snapshot the full tile grid (use this BEFORE bucket-fill / resize). */
void undo_snapshot_tiles   (UndoStack *u, const EditorDoc *d);

/* Object commands. The doc index is informational; on undo we restore
 * the record at the end of its array (same shape ADD/DEL produces). */
void undo_record_obj_add   (UndoStack *u, UndoCmdKind kind, int idx, const void *rec);
void undo_record_obj_del   (UndoStack *u, UndoCmdKind kind, int idx, const void *rec);

bool undo_pop  (UndoStack *u, EditorDoc *d);
bool undo_redo (UndoStack *u, EditorDoc *d);
