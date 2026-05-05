#pragma once

/*
 * editor UI — toolbar + palettes + modals (panels other than the
 * canvas itself). Extracted from main.c so shot mode (shotmode.c) can
 * render the same chrome and we can validate UI scaling / button
 * layout / text legibility without round-tripping through the user.
 *
 * The interactive editor (main.c) and the shot runner (shotmode.c)
 * both use these. raygui's IMPLEMENTATION lives in main.c only — the
 * functions here CALL raygui but don't define it.
 */

#include "doc.h"
#include "play.h"          /* for TestPlayLoadout */
#include "tool.h"
#include "view.h"

#include "raylib.h"

#include <stdbool.h>

/* ---- Per-frame UI dimensions. Computed from screen height; mirrors
 * the game's src/ui.c::ui_compute_scale (1.0× at 720p → 3.0× at 4K
 * snapping in 0.25 steps). */
typedef struct UIDims {
    float scale;
    int   left_w, right_w, top_h, bottom_h;
    int   pad;
    int   row_h;
    int   palette_row_h;
    int   font_base;
    int   font_lg;
    int   font_sm;
} UIDims;

UIDims ui_compute_dims(int screen_h);
float  ui_scale       (int screen_h);
int    ui_scl         (int v, float s);

/* Pinned high-contrast color set used by every panel and modal. */
extern const Color COL_BG;
extern const Color COL_PANEL;
extern const Color COL_PANEL_2;
extern const Color COL_TEXT;
extern const Color COL_TEXT_DIM;
extern const Color COL_TEXT_HIGH;
extern const Color COL_ACCENT;
extern const Color COL_ACCENT_LO;
extern const Color COL_BORDER;

/* Bilinear-default-font text helper. */
void ui_draw_text  (const char *s, int x, int y, int sz, Color col);
int  ui_measure_text(const char *s, int sz);

/* ---- Panels. Each draws its own background + border. */
void ui_draw_top_bar     (const EditorDoc *d, const UIDims *D);
void ui_draw_status_bar  (const EditorDoc *d, const EditorView *v,
                          const UIDims *D, ToolKind active_tool);

/* Draws the toolbar's seven tool buttons. If the user clicked a
 * button this frame, *active is updated and the function returns
 * the clicked tool (so the caller can fire side effects like opening
 * the META modal even when the active tool was already TOOL_META).
 * Returns -1 if no button was clicked this frame. */
int ui_draw_tool_buttons(ToolKind *active, const UIDims *D);

/* Draws a small "[L] Loadout" button in the top bar's right area.
 * Returns true if the user clicked it this frame. The "press H for
 * keyboard shortcuts" hint is moved leftward to make room. */
bool ui_draw_loadout_button(const UIDims *D);

void ui_draw_tile_palette  (ToolCtx *c, const UIDims *D);
void ui_draw_poly_palette  (ToolCtx *c, const UIDims *D);
void ui_draw_pickup_palette(ToolCtx *c, const UIDims *D);
void ui_draw_spawn_palette (ToolCtx *c, const UIDims *D);
void ui_draw_ambi_palette  (ToolCtx *c, const UIDims *D);
void ui_draw_empty_palette (const UIDims *D);

/* ---- Meta modal (Map metadata). */
typedef struct MetaModal {
    bool open;
    char name[64];
    char blurb[128];
    bool mode_ffa;
    bool mode_tdm;
    bool mode_ctf;
    bool name_edit;
    bool blurb_edit;
} MetaModal;

void ui_meta_open      (MetaModal *m, const EditorDoc *d);
void ui_meta_apply     (MetaModal *m, EditorDoc *d);
void ui_meta_modal_draw(MetaModal *m, EditorDoc *d, const UIDims *D);

/* ---- Help modal (keyboard shortcuts). */
typedef struct HelpModal { bool open; float scroll; } HelpModal;

void ui_help_open       (HelpModal *h);
void ui_help_close      (HelpModal *h);
void ui_help_toggle     (HelpModal *h);
void ui_help_modal_draw (HelpModal *h, const UIDims *D);

/* ---- Test-play loadout modal (L hotkey or top-bar button) ---------
 *
 * Configures the chassis / primary / secondary / armor / jetpack /
 * match-mode the F5 test-play child gets. Same content as the editor's
 * --test-* CLI flags, just exposed via dropdowns so a user can set the
 * loadout mid-session without restarting the editor. Apply writes back
 * into the caller's TestPlayLoadout (read by play_test on F5).
 *
 * Index 0 in each dropdown is "(default)" — empty string in the
 * TestPlayLoadout, which makes the spawned game fall back to its own
 * defaults. Names are the same display names the game's CLI parser
 * accepts (e.g. "Grappling Hook").
 *
 * The MODE slot is a special case: "(default)" keeps the runtime's
 * META auto-detect (the .lvl's mode_mask CTF bit picks CTF, otherwise
 * FFA). Picking FFA / TDM / CTF explicitly overrides that. */
enum {
    LOADOUT_SLOT_CHASSIS   = 0,
    LOADOUT_SLOT_PRIMARY   = 1,
    LOADOUT_SLOT_SECONDARY = 2,
    LOADOUT_SLOT_ARMOR     = 3,
    LOADOUT_SLOT_JETPACK   = 4,
    LOADOUT_SLOT_MODE      = 5,
    LOADOUT_SLOT_COUNT     = 6,
};

typedef struct LoadoutModal {
    bool open;
    int  idx[LOADOUT_SLOT_COUNT];      /* dropdown indices, 0 = default */
    bool edit[LOADOUT_SLOT_COUNT];     /* GuiDropdownBox open-state per slot */
} LoadoutModal;

void ui_loadout_open      (LoadoutModal *m, const TestPlayLoadout *cur);
void ui_loadout_close     (LoadoutModal *m);
void ui_loadout_apply     (const LoadoutModal *m, TestPlayLoadout *out);
void ui_loadout_modal_draw(LoadoutModal *m, TestPlayLoadout *out,
                           const UIDims *D);

/* Slot-name → LOADOUT_SLOT_* (case-insensitive: "chassis" / "primary"
 * / "secondary" / "armor" / "jetpack"). Returns -1 if unknown. */
int  ui_loadout_slot_from_name(const char *name);

/* Lookup the dropdown idx for a given option name in the given slot.
 * Pass an empty string to get 0 (the "(default)" entry). Returns -1
 * if the name doesn't match any option. Used by shotmode + the CLI-
 * arg path to seed the modal's selection from a string. */
int  ui_loadout_slot_idx(int slot, const char *name);

/* Inverse: dropdown idx → option name. Returns "" for idx 0. Returns
 * NULL for an out-of-range idx. */
const char *ui_loadout_slot_name(int slot, int idx);

/* Number of options in a slot's dropdown (including "(default)" at 0). */
int  ui_loadout_slot_count(int slot);
