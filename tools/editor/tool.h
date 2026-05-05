#pragma once

/*
 * Tool dispatch — a small vtable per tool. Tools are picked from the
 * left-side toolbar (see main.c) or via keyboard (T/P/S/I/A/D/M).
 *
 * One tool is "active" at any time; the universal verbs (Ctrl+Z/S/O/F5/
 * pan/zoom/grid) are handled in main.c BEFORE tool dispatch so a tool
 * never has to worry about them.
 */

#include "doc.h"
#include "palette.h"
#include "poly.h"
#include "undo.h"
#include "view.h"

#include <stdbool.h>

typedef enum {
    TOOL_TILE = 0,         /* T */
    TOOL_POLY,             /* P */
    TOOL_SPAWN,            /* S */
    TOOL_PICKUP,           /* I */
    TOOL_AMBI,             /* A */
    TOOL_DECO,             /* D */
    TOOL_FLAG,             /* F — CTF flag base (M5 P07) */
    TOOL_META,             /* M */
    TOOL_COUNT,
} ToolKind;

typedef struct ToolCtx {
    ToolKind kind;

    /* Tile tool state. */
    uint16_t tile_id;
    uint16_t tile_flags;       /* TILE_F_* — paints with this bitmask */

    /* Polygon tool state. */
    EditorPolyVert poly_in_progress[POLY_MAX_VERTS];
    int            poly_vertex_count;
    uint16_t       poly_kind;
    PresetKind     poly_preset;          /* PRESET_COUNT = none */

    /* Spawn tool state. */
    uint8_t  spawn_team;       /* 0 any / 1 red / 2 blue */
    uint8_t  spawn_lane_hint;  /* increments on each placement */

    /* Pickup tool state. */
    PickupVariant pickup_variant;

    /* Ambient zone state. */
    int      ambi_drag_start_x, ambi_drag_start_y;
    bool     ambi_dragging;
    int      ambi_kind;        /* AMBI_* */

    /* Decoration tool state. */
    uint8_t  deco_layer;
    uint16_t deco_sprite_str;  /* STRT offset; 0 = no sprite (empty placeholder) */

    /* Flag tool state. P07 — CTF flag bases. The team auto-toggles on
     * each placement so a designer can drop one of each side without
     * fiddling. 1 (Red) is placed first; 2 (Blue) second. Reverts to
     * 1 after the second is placed. */
    uint8_t  flag_team;
} ToolCtx;

typedef struct ToolVTable {
    void (*on_press)  (EditorDoc*, UndoStack*, ToolCtx*, int wx, int wy);
    void (*on_drag)   (EditorDoc*, UndoStack*, ToolCtx*, int wx, int wy);
    void (*on_release)(EditorDoc*, UndoStack*, ToolCtx*, int wx, int wy);
    void (*draw_overlay)(const EditorDoc*, const ToolCtx*, const EditorView*);
    bool (*on_key)    (EditorDoc*, UndoStack*, ToolCtx*, int key);
} ToolVTable;

/* Returns a static array indexed by ToolKind. */
const ToolVTable *tool_vtables(void);

void tool_ctx_init   (ToolCtx *c);
const char *tool_name(ToolKind k);

/* Right-click handlers (each tool owns its own erase / cancel verb). */
void tool_on_press_right  (EditorDoc *d, UndoStack *u, ToolCtx *c, int wx, int wy, ToolKind k);
