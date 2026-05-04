#include "editor_ui.h"

#include "palette.h"

#include "raylib.h"
#include "raygui.h"

#include "stb_ds.h"

#include <stdio.h>
#include <string.h>

const Color COL_BG          = { 12,  16,  24, 255};
const Color COL_PANEL       = { 28,  34,  44, 255};
const Color COL_PANEL_2     = { 36,  44,  56, 255};
const Color COL_TEXT        = {240, 244, 250, 255};
const Color COL_TEXT_DIM    = {190, 200, 215, 255};
const Color COL_TEXT_HIGH   = {255, 240, 180, 255};
const Color COL_ACCENT      = { 80, 130, 220, 255};
const Color COL_ACCENT_LO   = { 50,  90, 160, 255};
const Color COL_BORDER      = { 60,  70,  85, 255};

/* ---- Scale ---------------------------------------------------------- */

float ui_scale(int screen_h) {
    if (screen_h <= 0) return 1.0f;
    float raw = (float)screen_h / 720.0f;
    if (raw < 1.0f) raw = 1.0f;
    if (raw > 3.0f) raw = 3.0f;
    float snap = (float)((int)((raw * 4.0f) + 0.5f)) / 4.0f;
    if (snap < 1.0f) snap = 1.0f;
    return snap;
}

int ui_scl(int v, float s) { return (int)((float)v * s + 0.5f); }

UIDims ui_compute_dims(int screen_h) {
    UIDims d;
    d.scale         = ui_scale(screen_h);
    d.left_w        = ui_scl(160, d.scale);
    d.right_w       = ui_scl(240, d.scale);
    d.top_h         = ui_scl( 40, d.scale);
    d.bottom_h      = ui_scl( 30, d.scale);
    d.pad           = ui_scl(  8, d.scale);
    d.row_h         = ui_scl( 32, d.scale);
    d.palette_row_h = ui_scl( 24, d.scale);
    d.font_base     = ui_scl( 14, d.scale);
    d.font_lg       = ui_scl( 16, d.scale);
    d.font_sm       = ui_scl( 13, d.scale);
    return d;
}

/* ---- Text helpers --------------------------------------------------- */

void ui_draw_text(const char *s, int x, int y, int sz, Color col) {
    if (!s || !*s) return;
    DrawTextEx(GetFontDefault(), s, (Vector2){(float)x, (float)y},
               (float)sz, (float)sz / 10.0f, col);
}

int ui_measure_text(const char *s, int sz) {
    if (!s || !*s) return 0;
    Vector2 v = MeasureTextEx(GetFontDefault(), s,
                              (float)sz, (float)sz / 10.0f);
    return (int)(v.x + 0.5f);
}

/* ---- Top / status bars + tool buttons ------------------------------ */

void ui_draw_top_bar(const EditorDoc *d, const UIDims *D) {
    int sw = GetScreenWidth();
    DrawRectangle(0, 0, sw, D->top_h, COL_PANEL);
    DrawRectangle(0, D->top_h - 1, sw, 1, COL_BORDER);
    const char *full = d->source_path[0] ? d->source_path : "<untitled>";
    const char *base = strrchr(full, '/');
    base = base ? base + 1 : full;
#ifdef _WIN32
    const char *bb = strrchr(full, '\\');
    if (bb && bb > base) base = bb + 1;
#endif
    char buf[200];
    snprintf(buf, sizeof buf, "soldut_editor   %.96s%s",
             base, d->dirty ? " *" : "");
    ui_draw_text(buf, D->pad * 2, D->pad, D->font_lg, COL_TEXT);

    const char *hint = "press H for keyboard shortcuts";
    int w = ui_measure_text(hint, D->font_sm);
    ui_draw_text(hint, sw - w - D->pad * 2,
                 D->pad + (D->font_lg - D->font_sm) / 2,
                 D->font_sm, COL_TEXT_DIM);
}

void ui_draw_status_bar(const EditorDoc *d, const EditorView *v,
                        const UIDims *D, ToolKind active_tool) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int y = sh - D->bottom_h;
    DrawRectangle(0, y, sw, D->bottom_h, COL_PANEL);
    DrawRectangle(0, y, sw, 1, COL_BORDER);
    char buf[256], camtxt[64];
    view_status_text(v, camtxt, sizeof camtxt);
    Vector2 mw = view_screen_to_world(v, GetMousePosition());
    snprintf(buf, sizeof buf,
             "%s   %s   cursor (%4d, %4d)   %td polys / %td spawns / %td pickups",
             tool_name(active_tool), camtxt, (int)mw.x, (int)mw.y,
             arrlen(d->polys), arrlen(d->spawns), arrlen(d->pickups));
    ui_draw_text(buf, D->pad * 2, y + (D->bottom_h - D->font_sm) / 2,
                 D->font_sm, COL_TEXT);
}

ToolKind ui_draw_tool_buttons(ToolKind active, const UIDims *D) {
    int sh = GetScreenHeight();
    DrawRectangle(0, D->top_h, D->left_w, sh - D->top_h - D->bottom_h, COL_PANEL);
    DrawRectangle(D->left_w - 1, D->top_h, 1, sh - D->top_h - D->bottom_h,
                  COL_BORDER);
    static const struct { ToolKind k; const char *label; const char *hk; } btns[] = {
        { TOOL_TILE,   "Tile",    "T" },
        { TOOL_POLY,   "Polygon", "P" },
        { TOOL_SPAWN,  "Spawn",   "S" },
        { TOOL_PICKUP, "Pickup",  "I" },
        { TOOL_AMBI,   "Ambient", "A" },
        { TOOL_DECO,   "Deco",    "D" },
        { TOOL_META,   "Meta",    "M" },
    };
    int n = (int)(sizeof btns / sizeof btns[0]);
    for (int i = 0; i < n; ++i) {
        char label[40];
        snprintf(label, sizeof label, "%s  [%s]", btns[i].label, btns[i].hk);
        Rectangle r = { (float)D->pad,
                        (float)(D->top_h + D->pad + i * (D->row_h + D->pad/2)),
                        (float)(D->left_w - D->pad * 2),
                        (float)D->row_h };
        bool was_active = (btns[i].k == active);
        if (was_active) {
            DrawRectangleRec(r, COL_ACCENT_LO);
            DrawRectangleLinesEx(r, 2.0f, COL_ACCENT);
        }
        if (GuiButton(r, label)) active = btns[i].k;
    }
    return active;
}

static void draw_panel_title(const char *text, int x, int y,
                             int w, int font_sz, Color text_col) {
    (void)w;
    ui_draw_text(text, x, y, font_sz, text_col);
}

/* ---- Palettes ------------------------------------------------------- */

void ui_draw_tile_palette(ToolCtx *c, const UIDims *D) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int x = sw - D->right_w;
    DrawRectangle(x, D->top_h, D->right_w, sh - D->top_h - D->bottom_h, COL_PANEL);
    DrawRectangle(x, D->top_h, 1, sh - D->top_h - D->bottom_h, COL_BORDER);
    draw_panel_title("Tile flags",
                     x + D->pad, D->top_h + D->pad,
                     D->right_w - D->pad * 2, D->font_lg, COL_TEXT_HIGH);
    int n;
    const TileFlagEntry *flags = palette_tile_flags(&n);
    int row_top = D->top_h + D->pad + D->font_lg + D->pad;
    int row_h   = D->palette_row_h + D->pad / 2;
    for (int i = 0; i < n; ++i) {
        Rectangle r = { (float)(x + D->pad), (float)(row_top + i * row_h),
                        (float)(D->right_w - D->pad * 2), (float)D->palette_row_h };
        bool on = (c->tile_flags & flags[i].flag) != 0;
        GuiCheckBox(r, flags[i].name, &on);
        if (on) c->tile_flags |=  flags[i].flag;
        else    c->tile_flags &= ~flags[i].flag;
    }
}

void ui_draw_poly_palette(ToolCtx *c, const UIDims *D) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int x = sw - D->right_w;
    DrawRectangle(x, D->top_h, D->right_w, sh - D->top_h - D->bottom_h, COL_PANEL);
    DrawRectangle(x, D->top_h, 1, sh - D->top_h - D->bottom_h, COL_BORDER);
    int y = D->top_h + D->pad;
    draw_panel_title("Polygon kind", x + D->pad, y, D->right_w - D->pad * 2,
                     D->font_lg, COL_TEXT_HIGH);
    y += D->font_lg + D->pad;
    int kn;
    const PolyKindEntry *kinds = palette_poly_kinds(&kn);
    int row_h = D->palette_row_h + D->pad / 2;
    for (int i = 0; i < kn; ++i) {
        Rectangle r = { (float)(x + D->pad), (float)y,
                        (float)(D->right_w - D->pad * 2),
                        (float)D->palette_row_h };
        bool on = (c->poly_kind == kinds[i].kind);
        if (on) {
            DrawRectangleRec(r, COL_ACCENT_LO);
            DrawRectangleLinesEx(r, 2.0f, COL_ACCENT);
        }
        if (GuiButton(r, kinds[i].name)) c->poly_kind = (uint16_t)kinds[i].kind;
        y += row_h;
    }
    y += D->pad;
    draw_panel_title("Presets", x + D->pad, y, D->right_w - D->pad * 2,
                     D->font_lg, COL_TEXT_HIGH);
    y += D->font_lg + D->pad / 2;
    int pn;
    const PresetEntry *presets = palette_presets(&pn);
    const char *cat = "";
    int cat_h = D->font_sm + D->pad / 4;
    int btm   = sh - D->bottom_h - D->pad;
    for (int i = 0; i < pn; ++i) {
        if (strcmp(presets[i].category, cat) != 0) {
            cat = presets[i].category;
            if (y + cat_h > btm) break;
            ui_draw_text(cat, x + D->pad + D->pad / 2, y, D->font_sm, COL_TEXT_DIM);
            y += cat_h;
        }
        if (y + D->palette_row_h > btm) break;
        Rectangle r = { (float)(x + D->pad), (float)y,
                        (float)(D->right_w - D->pad * 2),
                        (float)D->palette_row_h };
        bool on = (c->poly_preset == presets[i].kind);
        if (on) {
            DrawRectangleRec(r, (Color){80, 130, 50, 255});
            DrawRectangleLinesEx(r, 2.0f, (Color){140, 220, 90, 255});
        }
        if (GuiButton(r, presets[i].name)) {
            c->poly_preset = on ? PRESET_COUNT : presets[i].kind;
        }
        y += D->palette_row_h + D->pad / 4;
    }
}

void ui_draw_pickup_palette(ToolCtx *c, const UIDims *D) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int x = sw - D->right_w;
    DrawRectangle(x, D->top_h, D->right_w, sh - D->top_h - D->bottom_h, COL_PANEL);
    DrawRectangle(x, D->top_h, 1, sh - D->top_h - D->bottom_h, COL_BORDER);
    int y = D->top_h + D->pad;
    draw_panel_title("Pickups", x + D->pad, y, D->right_w - D->pad * 2,
                     D->font_lg, COL_TEXT_HIGH);
    y += D->font_lg + D->pad;
    int n;
    const PickupEntry *pickups = palette_pickups(&n);
    int row_h = D->palette_row_h + D->pad / 4;
    int btm   = sh - D->bottom_h - D->pad;
    for (int i = 0; i < n; ++i) {
        if (y + D->palette_row_h > btm) break;
        Rectangle r = { (float)(x + D->pad), (float)y,
                        (float)(D->right_w - D->pad * 2),
                        (float)D->palette_row_h };
        bool on = ((int)c->pickup_variant == i);
        if (on) {
            DrawRectangleRec(r, (Color){120, 90, 30, 255});
            DrawRectangleLinesEx(r, 2.0f, (Color){240, 200, 90, 255});
        }
        if (GuiButton(r, pickups[i].name)) c->pickup_variant = (PickupVariant)i;
        y += row_h;
    }
}

void ui_draw_spawn_palette(ToolCtx *c, const UIDims *D) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int x = sw - D->right_w;
    DrawRectangle(x, D->top_h, D->right_w, sh - D->top_h - D->bottom_h, COL_PANEL);
    DrawRectangle(x, D->top_h, 1, sh - D->top_h - D->bottom_h, COL_BORDER);
    int y = D->top_h + D->pad;
    draw_panel_title("Spawn team", x + D->pad, y, D->right_w - D->pad * 2,
                     D->font_lg, COL_TEXT_HIGH);
    y += D->font_lg + D->pad;
    static const struct { uint8_t t; const char *name; } teams[] = {
        { 0, "Any   [0]" }, { 1, "Red   [1]" }, { 2, "Blue  [2]" },
    };
    int row_h = D->palette_row_h + D->pad / 2;
    for (int i = 0; i < 3; ++i) {
        Rectangle r = { (float)(x + D->pad), (float)y,
                        (float)(D->right_w - D->pad * 2),
                        (float)D->palette_row_h };
        bool on = (c->spawn_team == teams[i].t);
        if (on) {
            DrawRectangleRec(r, COL_ACCENT_LO);
            DrawRectangleLinesEx(r, 2.0f, COL_ACCENT);
        }
        if (GuiButton(r, teams[i].name)) c->spawn_team = teams[i].t;
        y += row_h;
    }
}

void ui_draw_ambi_palette(ToolCtx *c, const UIDims *D) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int x = sw - D->right_w;
    DrawRectangle(x, D->top_h, D->right_w, sh - D->top_h - D->bottom_h, COL_PANEL);
    DrawRectangle(x, D->top_h, 1, sh - D->top_h - D->bottom_h, COL_BORDER);
    int y = D->top_h + D->pad;
    draw_panel_title("Ambient kind", x + D->pad, y, D->right_w - D->pad * 2,
                     D->font_lg, COL_TEXT_HIGH);
    y += D->font_lg + D->pad;
    int n;
    const AmbiKindEntry *ambis = palette_ambi_kinds(&n);
    int row_h = D->palette_row_h + D->pad / 2;
    for (int i = 0; i < n; ++i) {
        Rectangle r = { (float)(x + D->pad), (float)y,
                        (float)(D->right_w - D->pad * 2),
                        (float)D->palette_row_h };
        bool on = (c->ambi_kind == ambis[i].kind);
        if (on) {
            DrawRectangleRec(r, COL_ACCENT_LO);
            DrawRectangleLinesEx(r, 2.0f, COL_ACCENT);
        }
        if (GuiButton(r, ambis[i].name)) c->ambi_kind = ambis[i].kind;
        y += row_h;
    }
}

void ui_draw_empty_palette(const UIDims *D) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int x = sw - D->right_w;
    DrawRectangle(x, D->top_h, D->right_w, sh - D->top_h - D->bottom_h, COL_PANEL);
    DrawRectangle(x, D->top_h, 1, sh - D->top_h - D->bottom_h, COL_BORDER);
}

/* ---- Meta modal ----------------------------------------------------- */

void ui_meta_open(MetaModal *m, const EditorDoc *d) {
    m->open = true;
    snprintf(m->name,  sizeof m->name,  "%s", doc_str_lookup(d, d->meta.name_str_idx));
    snprintf(m->blurb, sizeof m->blurb, "%s", doc_str_lookup(d, d->meta.blurb_str_idx));
    m->mode_ffa = (d->meta.mode_mask & 1u) != 0;
    m->mode_tdm = (d->meta.mode_mask & 2u) != 0;
    m->mode_ctf = (d->meta.mode_mask & 4u) != 0;
    m->name_edit = true;
    m->blurb_edit = false;
}

void ui_meta_apply(MetaModal *m, EditorDoc *d) {
    d->meta.name_str_idx  = doc_str_intern(d, m->name);
    d->meta.blurb_str_idx = doc_str_intern(d, m->blurb);
    d->meta.mode_mask =
        (m->mode_ffa ? 1u : 0u) |
        (m->mode_tdm ? 2u : 0u) |
        (m->mode_ctf ? 4u : 0u);
    d->dirty = true;
}

void ui_meta_modal_draw(MetaModal *m, EditorDoc *d, const UIDims *D) {
    if (!m->open) return;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 160});
    int dlg_w = ui_scl(560, D->scale), dlg_h = ui_scl(320, D->scale);
    int dlg_x = (sw - dlg_w) / 2, dlg_y = (sh - dlg_h) / 2;
    GuiPanel((Rectangle){(float)dlg_x, (float)dlg_y, (float)dlg_w, (float)dlg_h},
             "Map metadata");
    int label_x = dlg_x + D->pad * 2;
    int field_x = label_x + ui_scl(120, D->scale);
    int field_w = dlg_w - (field_x - dlg_x) - D->pad * 2;
    int y = dlg_y + ui_scl(40, D->scale);
    int line_h = D->row_h + D->pad / 2;

    ui_draw_text("Display name", label_x, y + D->pad / 2, D->font_base, COL_TEXT);
    Rectangle nb = {(float)field_x, (float)y, (float)field_w, (float)D->row_h};
    if (GuiTextBox(nb, m->name, (int)sizeof m->name, m->name_edit)) {
        m->name_edit = !m->name_edit; m->blurb_edit = false;
    }
    y += line_h;

    ui_draw_text("Blurb", label_x, y + D->pad / 2, D->font_base, COL_TEXT);
    Rectangle bb = {(float)field_x, (float)y, (float)field_w, (float)D->row_h};
    if (GuiTextBox(bb, m->blurb, (int)sizeof m->blurb, m->blurb_edit)) {
        m->blurb_edit = !m->blurb_edit; m->name_edit = false;
    }
    y += line_h;

    ui_draw_text("Modes", label_x, y + D->pad / 2, D->font_base, COL_TEXT);
    int chk = ui_scl(16, D->scale);
    int gap = ui_scl(80, D->scale);
    GuiCheckBox((Rectangle){(float)field_x,           (float)y + (D->row_h - chk)/2.0f,
                            (float)chk, (float)chk}, "FFA", &m->mode_ffa);
    GuiCheckBox((Rectangle){(float)(field_x + gap),   (float)y + (D->row_h - chk)/2.0f,
                            (float)chk, (float)chk}, "TDM", &m->mode_tdm);
    GuiCheckBox((Rectangle){(float)(field_x + 2*gap), (float)y + (D->row_h - chk)/2.0f,
                            (float)chk, (float)chk}, "CTF", &m->mode_ctf);
    y += line_h;

    int btn_w = ui_scl(110, D->scale);
    Rectangle ok = {(float)(dlg_x + dlg_w - btn_w - D->pad * 2),
                    (float)(dlg_y + dlg_h - D->row_h - D->pad),
                    (float)btn_w, (float)D->row_h};
    Rectangle cn = {(float)(dlg_x + dlg_w - btn_w * 2 - D->pad * 3),
                    (float)(dlg_y + dlg_h - D->row_h - D->pad),
                    (float)btn_w, (float)D->row_h};
    if (GuiButton(ok, "Apply")) { ui_meta_apply(m, d); m->open = false; }
    if (GuiButton(cn, "Cancel")) { m->open = false; }
    if (IsKeyPressed(KEY_ESCAPE)) { m->open = false; }
}

/* ---- Help modal ----------------------------------------------------- */

typedef struct { const char *key; const char *desc; } HelpRow;
typedef struct { const char *title; const HelpRow *rows; int n; } HelpSection;

static const HelpRow g_help_global[] = {
    { "Ctrl+S",            "Save (uses current source path)" },
    { "Ctrl+Shift+S",      "Save As… (opens a path modal)" },
    { "Ctrl+O",            "Open .lvl…" },
    { "Ctrl+N",            "New blank document" },
    { "Ctrl+Z",            "Undo" },
    { "Ctrl+Y / Ctrl+Shift+Z", "Redo" },
    { "F5",                "Test-play (saves to a temp .lvl, forks the game)" },
    { "H",                 "Show / hide this help" },
    { "Esc",               "Close any modal / abandon polygon in progress" },
};
static const HelpRow g_help_view[] = {
    { "Space + drag",      "Pan camera" },
    { "Middle-mouse drag", "Pan camera" },
    { "Ctrl + scroll",     "Zoom in / out (anchored at cursor)" },
    { "G",                 "Toggle 32-px tile grid" },
    { "Shift + G",         "Toggle 4-px sub-tile snap grid" },
};
static const HelpRow g_help_tools[] = {
    { "T", "Tile-paint tool" },
    { "P", "Polygon tool (click verts; click vert 0 or Enter to close)" },
    { "S", "Spawn-point tool" },
    { "I", "Pickup-spawner tool" },
    { "A", "Ambient-zone tool (drag a rectangle)" },
    { "D", "Decoration tool" },
    { "M", "Map metadata modal (name, blurb, modes)" },
};
static const HelpRow g_help_tile[] = {
    { "Left mouse",  "Paint with the active tile flags" },
    { "Right mouse", "Erase tile" },
    { "Hold + drag", "Continuous paint (one undo stroke)" },
    { "S / I / D / O / B", "Toggle SOLID / ICE / DEADLY / ONE_WAY / BACKGROUND flag" },
};
static const HelpRow g_help_poly[] = {
    { "Left mouse",       "Drop a polygon vertex (snapped to 4 px)" },
    { "Click vertex 0",   "Close the polygon (or Enter)" },
    { "Backspace",        "Drop last vertex" },
    { "Esc / Right mouse","Abandon polygon in progress" },
    { "Click while preset armed", "Drop the slope/alcove preset at cursor" },
};
static const HelpRow g_help_objects[] = {
    { "Spawn 0 / 1 / 2",  "Set team to Any / Red / Blue" },
    { "Pickup [ / ]",     "Cycle pickup variant down / up" },
    { "Right mouse",      "Delete nearest object within 24 px (Spawn/Pickup/Deco)" },
    { "Right mouse (Ambi)", "Delete the ambient zone the cursor is inside" },
};

#define HELP_SECTIONS_N 5
static const HelpSection g_help_sections[HELP_SECTIONS_N] = {
    { "Global",       g_help_global,  (int)(sizeof g_help_global  / sizeof g_help_global[0])  },
    { "View",         g_help_view,    (int)(sizeof g_help_view    / sizeof g_help_view[0])    },
    { "Tools",        g_help_tools,   (int)(sizeof g_help_tools   / sizeof g_help_tools[0])   },
    { "Tile tool",    g_help_tile,    (int)(sizeof g_help_tile    / sizeof g_help_tile[0])    },
    { "Polygon tool", g_help_poly,    (int)(sizeof g_help_poly    / sizeof g_help_poly[0])    },
};

void ui_help_open (HelpModal *h) { h->open = true;  h->scroll = 0.0f; }
void ui_help_close(HelpModal *h) { h->open = false; }

static int help_total_rows(void) {
    int rows = 0;
    for (int i = 0; i < HELP_SECTIONS_N; ++i) rows += g_help_sections[i].n + 1;
    rows += (int)(sizeof g_help_objects / sizeof g_help_objects[0]) + 1;
    return rows;
}

void ui_help_modal_draw(HelpModal *h, const UIDims *D) {
    if (!h->open) return;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 170});

    int dlg_w = ui_scl(720, D->scale);
    if (dlg_w > sw - D->pad * 4) dlg_w = sw - D->pad * 4;
    int dlg_h = sh - D->pad * 6;
    if (dlg_h > ui_scl(720, D->scale)) dlg_h = ui_scl(720, D->scale);
    int dlg_x = (sw - dlg_w) / 2, dlg_y = (sh - dlg_h) / 2;

    DrawRectangle(dlg_x, dlg_y, dlg_w, dlg_h, COL_PANEL);
    DrawRectangleLinesEx((Rectangle){(float)dlg_x, (float)dlg_y,
                                     (float)dlg_w, (float)dlg_h}, 2.0f, COL_BORDER);
    int title_h = D->font_lg + D->pad;
    DrawRectangle(dlg_x, dlg_y, dlg_w, title_h, COL_PANEL_2);
    DrawRectangle(dlg_x, dlg_y + title_h, dlg_w, 1, COL_BORDER);
    ui_draw_text("Keyboard shortcuts", dlg_x + D->pad * 2, dlg_y + D->pad / 2,
                 D->font_lg, COL_TEXT_HIGH);

    int btn_w = ui_scl(80, D->scale);
    Rectangle close_btn = {
        (float)(dlg_x + dlg_w - btn_w - D->pad),
        (float)(dlg_y + (title_h - D->row_h) / 2.0f),
        (float)btn_w, (float)D->row_h
    };
    if (GuiButton(close_btn, "Close") || IsKeyPressed(KEY_ESCAPE)) {
        ui_help_close(h);
    }

    int body_y0 = dlg_y + title_h + D->pad;
    int body_y1 = dlg_y + dlg_h - D->pad;
    int key_x   = dlg_x + D->pad * 2;
    int key_col_w = ui_scl(220, D->scale);
    int desc_x  = key_x + key_col_w + D->pad;
    int line_h  = D->font_base + D->pad / 2;

    int total_rows = help_total_rows();
    int rows_visible = (body_y1 - body_y0) / line_h - 1;
    if (rows_visible < 1) rows_visible = 1;
    int max_scroll = total_rows - rows_visible;
    if (max_scroll < 0) max_scroll = 0;
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) h->scroll -= wheel * 2.0f;
    if (h->scroll < 0) h->scroll = 0;
    if (h->scroll > (float)max_scroll) h->scroll = (float)max_scroll;

    int row_idx = 0;
    int y = body_y0 - (int)(h->scroll * (float)line_h);

#define EMIT_SECTION(title_str, rows_arr, n_rows)                                \
    do {                                                                          \
        if (y >= body_y0 - line_h && y < body_y1) {                               \
            ui_draw_text((title_str), key_x, y, D->font_lg, COL_TEXT_HIGH);       \
            DrawRectangle(key_x, y + D->font_lg + 2, dlg_w - D->pad * 4 - 1, 1,   \
                          COL_BORDER);                                            \
        }                                                                         \
        y += line_h; row_idx++;                                                   \
        for (int _i = 0; _i < (n_rows); ++_i) {                                   \
            if (y >= body_y0 - line_h && y < body_y1) {                           \
                Color zebra = (row_idx & 1) ? COL_PANEL : COL_PANEL_2;            \
                DrawRectangle(key_x - D->pad / 2, y - 2,                          \
                              dlg_w - D->pad * 3, line_h, zebra);                 \
                ui_draw_text((rows_arr)[_i].key,  key_x,  y, D->font_base, COL_TEXT_HIGH); \
                ui_draw_text((rows_arr)[_i].desc, desc_x, y, D->font_base, COL_TEXT); \
            }                                                                     \
            y += line_h; row_idx++;                                               \
        }                                                                         \
        y += D->pad / 2;                                                          \
    } while (0)

    EMIT_SECTION("Global",        g_help_global,  (int)(sizeof g_help_global  / sizeof g_help_global[0]));
    EMIT_SECTION("View",          g_help_view,    (int)(sizeof g_help_view    / sizeof g_help_view[0]));
    EMIT_SECTION("Tools",         g_help_tools,   (int)(sizeof g_help_tools   / sizeof g_help_tools[0]));
    EMIT_SECTION("Tile tool",     g_help_tile,    (int)(sizeof g_help_tile    / sizeof g_help_tile[0]));
    EMIT_SECTION("Polygon tool",  g_help_poly,    (int)(sizeof g_help_poly    / sizeof g_help_poly[0]));
    EMIT_SECTION("Objects",       g_help_objects, (int)(sizeof g_help_objects / sizeof g_help_objects[0]));
#undef EMIT_SECTION

    if (max_scroll > 0) {
        int bar_x = dlg_x + dlg_w - D->pad;
        int bar_y = body_y0;
        int bar_h = body_y1 - body_y0;
        DrawRectangle(bar_x, bar_y, 4, bar_h, COL_PANEL_2);
        int knob_h = (int)((float)bar_h * (float)rows_visible / (float)total_rows);
        if (knob_h < 16) knob_h = 16;
        int knob_y = bar_y + (int)((float)(bar_h - knob_h) * h->scroll / (float)max_scroll);
        DrawRectangle(bar_x, knob_y, 4, knob_h, COL_ACCENT);
    }

    const char *hint = "scroll wheel to scroll · H or Esc to close";
    int hint_w = ui_measure_text(hint, D->font_sm);
    ui_draw_text(hint, dlg_x + (dlg_w - hint_w) / 2,
                 dlg_y + dlg_h - D->font_sm - D->pad / 2,
                 D->font_sm, COL_TEXT_DIM);
}
