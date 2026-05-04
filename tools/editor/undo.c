#include "undo.h"

#include "log.h"

#include "stb_ds.h"

#include <stdlib.h>
#include <string.h>

void undo_init(UndoStack *u) {
    memset(u, 0, sizeof(*u));
}

static void cmd_drop(UndoCmd *c) {
    if (c->deltas)   arrfree(c->deltas);
    if (c->snapshot) free(c->snapshot);
    memset(c, 0, sizeof(*c));
}

static void clear_stack(UndoCmd *s, int *n) {
    for (int i = 0; i < *n; ++i) cmd_drop(&s[i]);
    *n = 0;
}

void undo_clear(UndoStack *u) {
    clear_stack(u->undo, &u->undo_count);
    clear_stack(u->redo, &u->redo_count);
    if (u->stroke_open) cmd_drop(&u->pending);
    u->stroke_open = false;
}

/* Push to undo stack; drop oldest if full. Always clears redo. */
static void push_undo(UndoStack *u, const UndoCmd *c) {
    /* Any new action invalidates the redo stack. */
    clear_stack(u->redo, &u->redo_count);

    if (u->undo_count >= UNDO_RING) {
        cmd_drop(&u->undo[0]);
        memmove(&u->undo[0], &u->undo[1],
                sizeof(UndoCmd) * (size_t)(UNDO_RING - 1));
        u->undo_count = UNDO_RING - 1;
    }
    u->undo[u->undo_count++] = *c;
}

/* ---- Tile-paint stroke -------------------------------------------- */

void undo_begin_tile_stroke(UndoStack *u) {
    if (u->stroke_open) return;
    memset(&u->pending, 0, sizeof(u->pending));
    u->pending.kind = UC_TILE_DELTAS;
    u->stroke_open = true;
}

void undo_record_tile(UndoStack *u, int x, int y, LvlTile before, LvlTile after) {
    if (!u->stroke_open) undo_begin_tile_stroke(u);
    UndoTileDelta d = { (uint16_t)x, (uint16_t)y, before, after };
    arrput(u->pending.deltas, d);
}

void undo_end_tile_stroke(UndoStack *u, EditorDoc *d) {
    (void)d;
    if (!u->stroke_open) return;
    u->stroke_open = false;
    int n = (int)arrlen(u->pending.deltas);
    if (n == 0) {
        cmd_drop(&u->pending);
        return;
    }
    /* Big stroke → upgrade to a snapshot. We don't have the "before"
     * grid handy here (we only have deltas), so we leave the deltas
     * variant in place even for big strokes. The threshold is mainly
     * to prevent unbounded growth on bucket fill, which uses
     * undo_snapshot_tiles() directly. */
    push_undo(u, &u->pending);
    memset(&u->pending, 0, sizeof(u->pending));
}

/* ---- Big-stroke snapshot ------------------------------------------ */

void undo_snapshot_tiles(UndoStack *u, const EditorDoc *d) {
    UndoCmd c = {0};
    c.kind = UC_TILE_SNAPSHOT;
    c.snap_w = d->width; c.snap_h = d->height;
    int n = d->width * d->height;
    c.snapshot = (LvlTile *)malloc(sizeof(LvlTile) * (size_t)n);
    if (!c.snapshot) {
        LOG_E("undo_snapshot_tiles: malloc failed");
        return;
    }
    memcpy(c.snapshot, d->tiles, sizeof(LvlTile) * (size_t)n);
    push_undo(u, &c);
}

/* ---- Object commands ---------------------------------------------- */

void undo_record_obj_add(UndoStack *u, UndoCmdKind kind, int idx, const void *rec) {
    UndoCmd c = {0};
    c.kind = kind;
    c.idx  = idx;
    switch (kind) {
        case UC_POLY_ADD:    c.rec.poly   = *(const LvlPoly   *)rec; break;
        case UC_SPAWN_ADD:   c.rec.spawn  = *(const LvlSpawn  *)rec; break;
        case UC_PICKUP_ADD:  c.rec.pickup = *(const LvlPickup *)rec; break;
        case UC_DECO_ADD:    c.rec.deco   = *(const LvlDeco   *)rec; break;
        case UC_AMBI_ADD:    c.rec.ambi   = *(const LvlAmbi   *)rec; break;
        case UC_FLAG_ADD:    c.rec.flag   = *(const LvlFlag   *)rec; break;
        default: return;
    }
    push_undo(u, &c);
}

void undo_record_obj_del(UndoStack *u, UndoCmdKind kind, int idx, const void *rec) {
    UndoCmd c = {0};
    c.kind = kind;
    c.idx  = idx;
    switch (kind) {
        case UC_POLY_DEL:    c.rec.poly   = *(const LvlPoly   *)rec; break;
        case UC_SPAWN_DEL:   c.rec.spawn  = *(const LvlSpawn  *)rec; break;
        case UC_PICKUP_DEL:  c.rec.pickup = *(const LvlPickup *)rec; break;
        case UC_DECO_DEL:    c.rec.deco   = *(const LvlDeco   *)rec; break;
        case UC_AMBI_DEL:    c.rec.ambi   = *(const LvlAmbi   *)rec; break;
        case UC_FLAG_DEL:    c.rec.flag   = *(const LvlFlag   *)rec; break;
        default: return;
    }
    push_undo(u, &c);
}

/* ---- Apply (forward / inverse) ------------------------------------ */

/* swap-with-back delete used here so the order matches what record_obj_
 * add would emit. The doc array order is unstable across mutations,
 * which the editor tolerates. */

static void apply_inverse(UndoCmd *c, EditorDoc *d) {
    switch (c->kind) {
        case UC_TILE_DELTAS: {
            int n = (int)arrlen(c->deltas);
            /* Walk in reverse so overlapping painted tiles roll back
             * cleanly. */
            for (int i = n - 1; i >= 0; --i) {
                UndoTileDelta *t = &c->deltas[i];
                if (t->x < d->width && t->y < d->height) {
                    d->tiles[(int)t->y * d->width + (int)t->x] = t->before;
                }
            }
            break;
        }
        case UC_TILE_SNAPSHOT: {
            /* Swap snapshot with current grid → snapshot now holds
             * forward state for redo. */
            int n = d->width * d->height;
            if (c->snap_w == d->width && c->snap_h == d->height) {
                LvlTile *tmp = (LvlTile *)malloc(sizeof(LvlTile) * (size_t)n);
                memcpy(tmp, d->tiles, sizeof(LvlTile) * (size_t)n);
                memcpy(d->tiles, c->snapshot, sizeof(LvlTile) * (size_t)n);
                free(c->snapshot);
                c->snapshot = tmp;
            } else {
                LOG_W("undo: tile snapshot size mismatch (skipping)");
            }
            break;
        }
        case UC_POLY_ADD:    arrdel(d->polys,   arrlen(d->polys)   - 1); break;
        case UC_SPAWN_ADD:   arrdel(d->spawns,  arrlen(d->spawns)  - 1); break;
        case UC_PICKUP_ADD:  arrdel(d->pickups, arrlen(d->pickups) - 1); break;
        case UC_DECO_ADD:    arrdel(d->decos,   arrlen(d->decos)   - 1); break;
        case UC_AMBI_ADD:    arrdel(d->ambis,   arrlen(d->ambis)   - 1); break;
        case UC_FLAG_ADD:    arrdel(d->flags,   arrlen(d->flags)   - 1); break;
        case UC_POLY_DEL:    arrput(d->polys,   c->rec.poly);   break;
        case UC_SPAWN_DEL:   arrput(d->spawns,  c->rec.spawn);  break;
        case UC_PICKUP_DEL:  arrput(d->pickups, c->rec.pickup); break;
        case UC_DECO_DEL:    arrput(d->decos,   c->rec.deco);   break;
        case UC_AMBI_DEL:    arrput(d->ambis,   c->rec.ambi);   break;
        case UC_FLAG_DEL:    arrput(d->flags,   c->rec.flag);   break;
        default: break;
    }
    d->dirty = true;
}

static void apply_forward(UndoCmd *c, EditorDoc *d) {
    switch (c->kind) {
        case UC_TILE_DELTAS: {
            int n = (int)arrlen(c->deltas);
            for (int i = 0; i < n; ++i) {
                UndoTileDelta *t = &c->deltas[i];
                if (t->x < d->width && t->y < d->height) {
                    d->tiles[(int)t->y * d->width + (int)t->x] = t->after;
                }
            }
            break;
        }
        case UC_TILE_SNAPSHOT: {
            int n = d->width * d->height;
            if (c->snap_w == d->width && c->snap_h == d->height) {
                LvlTile *tmp = (LvlTile *)malloc(sizeof(LvlTile) * (size_t)n);
                memcpy(tmp, d->tiles, sizeof(LvlTile) * (size_t)n);
                memcpy(d->tiles, c->snapshot, sizeof(LvlTile) * (size_t)n);
                free(c->snapshot);
                c->snapshot = tmp;
            }
            break;
        }
        case UC_POLY_ADD:    arrput(d->polys,   c->rec.poly);   break;
        case UC_SPAWN_ADD:   arrput(d->spawns,  c->rec.spawn);  break;
        case UC_PICKUP_ADD:  arrput(d->pickups, c->rec.pickup); break;
        case UC_DECO_ADD:    arrput(d->decos,   c->rec.deco);   break;
        case UC_AMBI_ADD:    arrput(d->ambis,   c->rec.ambi);   break;
        case UC_FLAG_ADD:    arrput(d->flags,   c->rec.flag);   break;
        case UC_POLY_DEL:    arrdel(d->polys,   arrlen(d->polys)   - 1); break;
        case UC_SPAWN_DEL:   arrdel(d->spawns,  arrlen(d->spawns)  - 1); break;
        case UC_PICKUP_DEL:  arrdel(d->pickups, arrlen(d->pickups) - 1); break;
        case UC_DECO_DEL:    arrdel(d->decos,   arrlen(d->decos)   - 1); break;
        case UC_AMBI_DEL:    arrdel(d->ambis,   arrlen(d->ambis)   - 1); break;
        case UC_FLAG_DEL:    arrdel(d->flags,   arrlen(d->flags)   - 1); break;
        default: break;
    }
    d->dirty = true;
}

bool undo_pop(UndoStack *u, EditorDoc *d) {
    if (u->stroke_open) undo_end_tile_stroke(u, d);
    if (u->undo_count == 0) return false;
    UndoCmd c = u->undo[--u->undo_count];
    apply_inverse(&c, d);
    /* Move to redo. */
    if (u->redo_count < UNDO_RING) {
        u->redo[u->redo_count++] = c;
    } else {
        cmd_drop(&c);
    }
    return true;
}

bool undo_redo(UndoStack *u, EditorDoc *d) {
    if (u->redo_count == 0) return false;
    UndoCmd c = u->redo[--u->redo_count];
    apply_forward(&c, d);
    if (u->undo_count < UNDO_RING) {
        u->undo[u->undo_count++] = c;
    } else {
        cmd_drop(&c);
    }
    return true;
}
