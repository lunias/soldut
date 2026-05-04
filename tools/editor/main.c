/*
 * soldut_editor — M5 P04 level editor.
 *
 * Same engine code (level_io / arena / log / hash / ds), different main().
 * raygui for the toolbar / palette / modals; raylib for the canvas.
 *
 * Usage:
 *   ./build/soldut_editor                        # blank document
 *   ./build/soldut_editor assets/maps/foundry.lvl # open existing
 */

#include "doc.h"
#include "files.h"
#include "log.h"
#include "palette.h"
#include "play.h"
#include "tool.h"
#include "undo.h"
#include "validate.h"
#include "view.h"

#include "raylib.h"

/* raygui needs roundf / floorf at expansion time. Pull math.h in
 * before the implementation include so it sees the prototypes. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "stb_ds.h"

/* ---- UI scale ----------------------------------------------------- */

/* Mirrors src/ui.c::ui_compute_scale: 1.0× at 720p, snapping in 0.25
 * steps up to 3.0× at 4K. Computed each frame from current screen
 * height so a window resize updates the scale on the fly. */
static float editor_scale(int screen_h) {
    if (screen_h <= 0) return 1.0f;
    float raw = (float)screen_h / 720.0f;
    if (raw < 1.0f) raw = 1.0f;
    if (raw > 3.0f) raw = 3.0f;
    float snap = (float)((int)((raw * 4.0f) + 0.5f)) / 4.0f;
    if (snap < 1.0f) snap = 1.0f;
    return snap;
}

static int S(int v, float s) { return (int)((float)v * s + 0.5f); }

/* All UI dimensions in one struct. Computed once per frame. The base
 * values are sized for a 1280×720 design baseline; everything else
 * comes out of the `scale` factor. */
typedef struct UIDims {
    float scale;
    int   left_w, right_w, top_h, bottom_h;
    int   pad;
    int   row_h;            /* tool button row height in the left rail */
    int   palette_row_h;    /* right-rail item row height */
    int   font_base;        /* raygui TEXT_SIZE */
    int   font_lg;          /* labels in the top bar */
    int   font_sm;          /* status bar text */
} UIDims;

static UIDims compute_dims(int screen_h) {
    UIDims d;
    d.scale         = editor_scale(screen_h);
    d.left_w        = S(160, d.scale);
    d.right_w       = S(240, d.scale);
    d.top_h         = S( 40, d.scale);
    d.bottom_h      = S( 30, d.scale);
    d.pad           = S(  8, d.scale);
    d.row_h         = S( 32, d.scale);
    d.palette_row_h = S( 24, d.scale);
    d.font_base     = S( 14, d.scale);
    d.font_lg       = S( 16, d.scale);
    d.font_sm       = S( 13, d.scale);
    return d;
}

/* ---- Render ------------------------------------------------------- */

static Color color_from_rgba(uint32_t rgba) {
    return (Color){
        (unsigned char)((rgba >> 24) & 0xff),
        (unsigned char)((rgba >> 16) & 0xff),
        (unsigned char)((rgba >>  8) & 0xff),
        (unsigned char)(rgba & 0xff),
    };
}

static Color tile_color(LvlTile t) {
    if (t.flags & TILE_F_DEADLY)     return (Color){180,  60,  60, 255};
    if (t.flags & TILE_F_ICE)        return (Color){120, 200, 240, 255};
    if (t.flags & TILE_F_ONE_WAY)    return (Color){120, 200, 120, 200};
    if (t.flags & TILE_F_BACKGROUND) return (Color){ 80,  80,  90, 130};
    if (t.flags & TILE_F_SOLID)      return (Color){160, 160, 170, 255};
    return (Color){0, 0, 0, 0};
}

static Color poly_color(uint16_t kind) {
    int n;
    const PolyKindEntry *kinds = palette_poly_kinds(&n);
    for (int i = 0; i < n; ++i) {
        if (kinds[i].kind == kind) return color_from_rgba(kinds[i].rgba);
    }
    return (Color){200, 200, 200, 200};
}

static void draw_doc(const EditorDoc *d) {
    /* Tiles. */
    for (int y = 0; y < d->height; ++y) {
        for (int x = 0; x < d->width; ++x) {
            LvlTile t = d->tiles[y * d->width + x];
            Color c = tile_color(t);
            if (c.a == 0) continue;
            DrawRectangle(x * d->tile_size, y * d->tile_size,
                          d->tile_size, d->tile_size, c);
        }
    }
    /* Polygons (filled). */
    int pn = (int)arrlen(d->polys);
    for (int i = 0; i < pn; ++i) {
        const LvlPoly *p = &d->polys[i];
        Color c = poly_color(p->kind);
        Vector2 v0 = {(float)p->v_x[0], (float)p->v_y[0]};
        Vector2 v1 = {(float)p->v_x[1], (float)p->v_y[1]};
        Vector2 v2 = {(float)p->v_x[2], (float)p->v_y[2]};
        DrawTriangle(v0, v1, v2, c);
        DrawTriangleLines(v0, v1, v2, (Color){c.r/2u, c.g/2u, c.b/2u, 255});
    }
    /* Spawn points. */
    int sn = (int)arrlen(d->spawns);
    for (int i = 0; i < sn; ++i) {
        const LvlSpawn *s = &d->spawns[i];
        Color c =
            (s->team == 1) ? (Color){240, 100, 100, 255} :
            (s->team == 2) ? (Color){100, 160, 240, 255} :
                             (Color){200, 200, 200, 255};
        DrawCircle(s->pos_x, s->pos_y, 6.0f, c);
        DrawCircleLines(s->pos_x, s->pos_y, 16.0f, c);
    }
    /* Pickups. */
    int pkn = (int)arrlen(d->pickups);
    for (int i = 0; i < pkn; ++i) {
        const LvlPickup *p = &d->pickups[i];
        DrawCircle(p->pos_x, p->pos_y, 4.0f, (Color){255, 220, 100, 255});
        DrawCircleLines(p->pos_x, p->pos_y, 12.0f, (Color){255, 220, 100, 255});
    }
    /* Decorations. */
    int dn = (int)arrlen(d->decos);
    for (int i = 0; i < dn; ++i) {
        const LvlDeco *e = &d->decos[i];
        DrawRectangleLines(e->pos_x - 8, e->pos_y - 8, 16, 16,
                           (Color){255, 140, 240, 255});
    }
    /* Ambient zones. */
    int an = (int)arrlen(d->ambis);
    for (int i = 0; i < an; ++i) {
        const LvlAmbi *a = &d->ambis[i];
        Color c =
            (a->kind == AMBI_WIND)   ? (Color){ 80, 240, 120, 90} :
            (a->kind == AMBI_ZERO_G) ? (Color){180, 180, 240, 90} :
            (a->kind == AMBI_ACID)   ? (Color){240, 200,  80, 90} :
                                        (Color){180, 180, 180, 90};
        DrawRectangle(a->rect_x, a->rect_y, a->rect_w, a->rect_h, c);
        DrawRectangleLines(a->rect_x, a->rect_y, a->rect_w, a->rect_h,
                           (Color){c.r, c.g, c.b, 255});
    }
    /* Flag bases. */
    int fn = (int)arrlen(d->flags);
    for (int i = 0; i < fn; ++i) {
        const LvlFlag *f = &d->flags[i];
        Color c = (f->team == 1) ? (Color){240, 80, 80, 255}
                                 : (Color){100, 160, 240, 255};
        DrawRectangle(f->pos_x - 6, f->pos_y - 12, 12, 24, c);
        DrawCircle(f->pos_x, f->pos_y - 14, 6, c);
    }
}

/* ---- Color palette (high-contrast, pinned) ----------------------- */

#define COL_BG          (Color){ 12,  16,  24, 255}
#define COL_PANEL       (Color){ 28,  34,  44, 255}
#define COL_PANEL_2     (Color){ 36,  44,  56, 255}
#define COL_TEXT        (Color){240, 244, 250, 255}
#define COL_TEXT_DIM    (Color){190, 200, 215, 255}
#define COL_TEXT_HIGH   (Color){255, 240, 180, 255}
#define COL_ACCENT      (Color){ 80, 130, 220, 255}
#define COL_ACCENT_LO   (Color){ 50,  90, 160, 255}
#define COL_BORDER      (Color){ 60,  70,  85, 255}

/* Scale-aware text helper. Uses raylib's bilinear-filtered default
 * font, which we mark filtered at startup. */
static void draw_text(const char *s, int x, int y, int sz, Color col) {
    if (!s || !*s) return;
    DrawTextEx(GetFontDefault(), s, (Vector2){(float)x, (float)y},
               (float)sz, (float)sz / 10.0f, col);
}

static int measure_text(const char *s, int sz) {
    if (!s || !*s) return 0;
    Vector2 v = MeasureTextEx(GetFontDefault(), s,
                              (float)sz, (float)sz / 10.0f);
    return (int)(v.x + 0.5f);
}

/* ---- UI panels ---------------------------------------------------- */

static void draw_top_bar(const EditorDoc *d, const UIDims *D) {
    int sw = GetScreenWidth();
    DrawRectangle(0, 0, sw, D->top_h, COL_PANEL);
    DrawRectangle(0, D->top_h - 1, sw, 1, COL_BORDER);
    /* Strip the source path to its basename so it fits comfortably. */
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
    draw_text(buf, D->pad * 2, D->pad, D->font_lg, COL_TEXT);

    /* Right side: hint about the help key. */
    const char *hint = "press H for keyboard shortcuts";
    int w = measure_text(hint, D->font_sm);
    draw_text(hint, sw - w - D->pad * 2, D->pad + (D->font_lg - D->font_sm) / 2,
              D->font_sm, COL_TEXT_DIM);
}

static void draw_status_bar(const EditorDoc *d, const EditorView *v,
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
    draw_text(buf, D->pad * 2, y + (D->bottom_h - D->font_sm) / 2,
              D->font_sm, COL_TEXT);
}

static ToolKind draw_tool_buttons(ToolKind active, const UIDims *D) {
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
    /* A small panel-section label with a brighter color than body
     * text — this is the contrast bump per the user's feedback. */
    (void)w;
    draw_text(text, x, y, font_sz, text_col);
}

static void draw_tile_palette(ToolCtx *c, const UIDims *D) {
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

static void draw_poly_palette(ToolCtx *c, const UIDims *D) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int x = sw - D->right_w;
    int panel_x = x;
    DrawRectangle(panel_x, D->top_h, D->right_w, sh - D->top_h - D->bottom_h, COL_PANEL);
    DrawRectangle(panel_x, D->top_h, 1, sh - D->top_h - D->bottom_h, COL_BORDER);
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
            draw_text(cat, x + D->pad + D->pad / 2, y, D->font_sm, COL_TEXT_DIM);
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

static void draw_pickup_palette(ToolCtx *c, const UIDims *D) {
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

static void draw_spawn_palette(ToolCtx *c, const UIDims *D) {
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

static void draw_ambi_palette(ToolCtx *c, const UIDims *D) {
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

static void draw_empty_palette(const UIDims *D) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int x = sw - D->right_w;
    DrawRectangle(x, D->top_h, D->right_w, sh - D->top_h - D->bottom_h, COL_PANEL);
    DrawRectangle(x, D->top_h, 1, sh - D->top_h - D->bottom_h, COL_BORDER);
}

/* ---- Meta modal --------------------------------------------------- */

typedef struct MetaModal {
    bool   open;
    char   name[64];
    char   blurb[128];
    bool   mode_ffa;
    bool   mode_tdm;
    bool   mode_ctf;
    bool   name_edit;
    bool   blurb_edit;
} MetaModal;

static void meta_open(MetaModal *m, const EditorDoc *d) {
    m->open = true;
    snprintf(m->name,  sizeof m->name,  "%s", doc_str_lookup(d, d->meta.name_str_idx));
    snprintf(m->blurb, sizeof m->blurb, "%s", doc_str_lookup(d, d->meta.blurb_str_idx));
    m->mode_ffa = (d->meta.mode_mask & 1u) != 0;
    m->mode_tdm = (d->meta.mode_mask & 2u) != 0;
    m->mode_ctf = (d->meta.mode_mask & 4u) != 0;
    m->name_edit = true;
    m->blurb_edit = false;
}

static void meta_apply(MetaModal *m, EditorDoc *d) {
    d->meta.name_str_idx  = doc_str_intern(d, m->name);
    d->meta.blurb_str_idx = doc_str_intern(d, m->blurb);
    d->meta.mode_mask =
        (m->mode_ffa ? 1u : 0u) |
        (m->mode_tdm ? 2u : 0u) |
        (m->mode_ctf ? 4u : 0u);
    d->dirty = true;
}

static void meta_modal_draw(MetaModal *m, EditorDoc *d, const UIDims *D) {
    if (!m->open) return;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 160});
    int dlg_w = S(560, D->scale), dlg_h = S(320, D->scale);
    int dlg_x = (sw - dlg_w) / 2, dlg_y = (sh - dlg_h) / 2;
    GuiPanel((Rectangle){(float)dlg_x, (float)dlg_y, (float)dlg_w, (float)dlg_h},
             "Map metadata");
    int label_x = dlg_x + D->pad * 2;
    int field_x = label_x + S(120, D->scale);
    int field_w = dlg_w - (field_x - dlg_x) - D->pad * 2;
    int y = dlg_y + S(40, D->scale);
    int line_h = D->row_h + D->pad / 2;

    draw_text("Display name", label_x, y + D->pad / 2, D->font_base, COL_TEXT);
    Rectangle nb = {(float)field_x, (float)y, (float)field_w, (float)D->row_h};
    if (GuiTextBox(nb, m->name, (int)sizeof m->name, m->name_edit)) {
        m->name_edit = !m->name_edit; m->blurb_edit = false;
    }
    y += line_h;

    draw_text("Blurb", label_x, y + D->pad / 2, D->font_base, COL_TEXT);
    Rectangle bb = {(float)field_x, (float)y, (float)field_w, (float)D->row_h};
    if (GuiTextBox(bb, m->blurb, (int)sizeof m->blurb, m->blurb_edit)) {
        m->blurb_edit = !m->blurb_edit; m->name_edit = false;
    }
    y += line_h;

    draw_text("Modes", label_x, y + D->pad / 2, D->font_base, COL_TEXT);
    int chk = S(16, D->scale);
    int gap = S(80, D->scale);
    GuiCheckBox((Rectangle){(float)field_x,           (float)y + (D->row_h - chk)/2.0f,
                            (float)chk, (float)chk}, "FFA", &m->mode_ffa);
    GuiCheckBox((Rectangle){(float)(field_x + gap),   (float)y + (D->row_h - chk)/2.0f,
                            (float)chk, (float)chk}, "TDM", &m->mode_tdm);
    GuiCheckBox((Rectangle){(float)(field_x + 2*gap), (float)y + (D->row_h - chk)/2.0f,
                            (float)chk, (float)chk}, "CTF", &m->mode_ctf);
    y += line_h;

    int btn_w = S(110, D->scale);
    Rectangle ok = {(float)(dlg_x + dlg_w - btn_w - D->pad * 2),
                    (float)(dlg_y + dlg_h - D->row_h - D->pad),
                    (float)btn_w, (float)D->row_h};
    Rectangle cn = {(float)(dlg_x + dlg_w - btn_w * 2 - D->pad * 3),
                    (float)(dlg_y + dlg_h - D->row_h - D->pad),
                    (float)btn_w, (float)D->row_h};
    if (GuiButton(ok, "Apply")) { meta_apply(m, d); m->open = false; }
    if (GuiButton(cn, "Cancel")) { m->open = false; }
    if (IsKeyPressed(KEY_ESCAPE)) { m->open = false; }
}

/* ---- Help modal --------------------------------------------------- */

typedef struct HelpModal { bool open; float scroll; } HelpModal;

/* Each row is (group | binding | description). Empty group string
 * starts a fresh section; rows with empty key+desc become group titles. */
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
    { "Global",            g_help_global,  (int)(sizeof g_help_global  / sizeof g_help_global[0])  },
    { "View",              g_help_view,    (int)(sizeof g_help_view    / sizeof g_help_view[0])    },
    { "Tools",             g_help_tools,   (int)(sizeof g_help_tools   / sizeof g_help_tools[0])   },
    { "Tile tool",         g_help_tile,    (int)(sizeof g_help_tile    / sizeof g_help_tile[0])    },
    { "Polygon tool",      g_help_poly,    (int)(sizeof g_help_poly    / sizeof g_help_poly[0])    },
};

static void help_open(HelpModal *h)  { h->open = true;  h->scroll = 0.0f; }
static void help_close(HelpModal *h) { h->open = false; }

static int help_total_rows(int *out_section_count) {
    int rows = 0;
    int n = HELP_SECTIONS_N + 1;        /* +1 for the trailing object-tools block */
    for (int i = 0; i < HELP_SECTIONS_N; ++i) rows += g_help_sections[i].n + 1;
    rows += (int)(sizeof g_help_objects / sizeof g_help_objects[0]) + 1;
    if (out_section_count) *out_section_count = n;
    return rows;
}

static void help_modal_draw(HelpModal *h, const UIDims *D) {
    if (!h->open) return;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 170});

    int dlg_w = S(720, D->scale);
    if (dlg_w > sw - D->pad * 4) dlg_w = sw - D->pad * 4;
    int dlg_h = sh - D->pad * 6;
    if (dlg_h > S(720, D->scale)) dlg_h = S(720, D->scale);
    int dlg_x = (sw - dlg_w) / 2, dlg_y = (sh - dlg_h) / 2;

    /* Custom panel — the GuiPanel title strip is too small at 4K. */
    DrawRectangle(dlg_x, dlg_y, dlg_w, dlg_h, COL_PANEL);
    DrawRectangleLinesEx((Rectangle){(float)dlg_x, (float)dlg_y,
                                     (float)dlg_w, (float)dlg_h}, 2.0f, COL_BORDER);
    int title_h = D->font_lg + D->pad;
    DrawRectangle(dlg_x, dlg_y, dlg_w, title_h, COL_PANEL_2);
    DrawRectangle(dlg_x, dlg_y + title_h, dlg_w, 1, COL_BORDER);
    draw_text("Keyboard shortcuts", dlg_x + D->pad * 2, dlg_y + D->pad / 2,
              D->font_lg, COL_TEXT_HIGH);

    int btn_w = S(80, D->scale);
    Rectangle close_btn = {
        (float)(dlg_x + dlg_w - btn_w - D->pad),
        (float)(dlg_y + (title_h - D->row_h) / 2.0f),
        (float)btn_w, (float)D->row_h
    };
    if (GuiButton(close_btn, "Close") || IsKeyPressed(KEY_ESCAPE)) {
        help_close(h);
    }

    /* Body area: two columns "key" + "description". */
    int body_y0 = dlg_y + title_h + D->pad;
    int body_y1 = dlg_y + dlg_h - D->pad;
    int key_x   = dlg_x + D->pad * 2;
    int key_col_w = S(220, D->scale);
    int desc_x  = key_x + key_col_w + D->pad;
    int line_h  = D->font_base + D->pad / 2;

    /* Scroll handling. */
    int total_rows = help_total_rows(NULL);
    int rows_visible = (body_y1 - body_y0) / line_h - 1;
    if (rows_visible < 1) rows_visible = 1;
    int max_scroll = total_rows - rows_visible;
    if (max_scroll < 0) max_scroll = 0;
    /* Mouse wheel scrolls when modal is open. */
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) h->scroll -= wheel * 2.0f;
    if (h->scroll < 0) h->scroll = 0;
    if (h->scroll > (float)max_scroll) h->scroll = (float)max_scroll;

    int row_idx = 0;
    int y = body_y0 - (int)(h->scroll * (float)line_h);

#define EMIT_SECTION(title_str, rows_arr, n_rows)                                  \
    do {                                                                            \
        if (y >= body_y0 - line_h && y < body_y1) {                                 \
            draw_text((title_str), key_x, y, D->font_lg, COL_TEXT_HIGH);            \
            DrawRectangle(key_x, y + D->font_lg + 2, dlg_w - D->pad * 4 - 1, 1,     \
                          COL_BORDER);                                              \
        }                                                                           \
        y += line_h; row_idx++;                                                     \
        for (int _i = 0; _i < (n_rows); ++_i) {                                     \
            if (y >= body_y0 - line_h && y < body_y1) {                             \
                Color zebra = (row_idx & 1) ? COL_PANEL : COL_PANEL_2;              \
                DrawRectangle(key_x - D->pad / 2, y - 2,                            \
                              dlg_w - D->pad * 3, line_h, zebra);                   \
                draw_text((rows_arr)[_i].key,  key_x,  y, D->font_base, COL_TEXT_HIGH); \
                draw_text((rows_arr)[_i].desc, desc_x, y, D->font_base, COL_TEXT);  \
            }                                                                       \
            y += line_h; row_idx++;                                                 \
        }                                                                           \
        y += D->pad / 2;                                                            \
    } while (0)

    EMIT_SECTION("Global",        g_help_global,  (int)(sizeof g_help_global  / sizeof g_help_global[0]));
    EMIT_SECTION("View",          g_help_view,    (int)(sizeof g_help_view    / sizeof g_help_view[0]));
    EMIT_SECTION("Tools",         g_help_tools,   (int)(sizeof g_help_tools   / sizeof g_help_tools[0]));
    EMIT_SECTION("Tile tool",     g_help_tile,    (int)(sizeof g_help_tile    / sizeof g_help_tile[0]));
    EMIT_SECTION("Polygon tool",  g_help_poly,    (int)(sizeof g_help_poly    / sizeof g_help_poly[0]));
    EMIT_SECTION("Objects",       g_help_objects, (int)(sizeof g_help_objects / sizeof g_help_objects[0]));
#undef EMIT_SECTION

    /* Scrollbar indicator on the right edge. */
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

    /* Footer hint. */
    const char *hint = "scroll wheel to scroll · H or Esc to close";
    int hint_w = measure_text(hint, D->font_sm);
    draw_text(hint, dlg_x + (dlg_w - hint_w) / 2,
              dlg_y + dlg_h - D->font_sm - D->pad / 2,
              D->font_sm, COL_TEXT_DIM);
}

/* ---- Universal verb dispatch ------------------------------------- */

static bool ctrl_down(void) {
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}
static bool shift_down(void) {
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

/* ---- Main --------------------------------------------------------- */

int main(int argc, char **argv) {
    log_init("soldut_editor.log");
    LOG_I("editor: starting");

    /* HIGHDPI tells raylib to give us a backbuffer at the monitor's
     * physical pixel count on hi-DPI displays (Retina, 4K-scaled
     * Windows). We then use GetScreenHeight() to pick a UI scale
     * that keeps text legible at native pixel density. */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT |
                   FLAG_WINDOW_HIGHDPI);
    SetTraceLogLevel(LOG_WARNING);

    /* Initial window size: 80% of the primary monitor, clamped to a
     * usable range. On a 4K monitor that's ~3000×1700; on 1080p it's
     * ~1500×850. */
    int init_w = 1280, init_h = 800;
    {
        /* GetMonitor* needs a window; raylib's standalone calls work
         * after InitWindow only. Open at default + resize immediately. */
    }
    InitWindow(init_w, init_h, "soldut_editor — M5 P04");

    /* Now that GLFW is up, query the monitor and resize. */
    int mon = GetCurrentMonitor();
    int mw  = GetMonitorWidth(mon);
    int mh  = GetMonitorHeight(mon);
    if (mw > 0 && mh > 0) {
        int target_w = (int)(mw * 0.8f);
        int target_h = (int)(mh * 0.8f);
        if (target_w < 1280) target_w = 1280;
        if (target_h < 800)  target_h = 800;
        SetWindowSize(target_w, target_h);
        SetWindowPosition((mw - target_w) / 2, (mh - target_h) / 2);
    }
    SetTargetFPS(60);

    /* Bilinear-filter the default font so it stays sharp when raygui
     * draws it 2× / 3× the bitmap size. Same trick as platform.c. */
    Font def_font = GetFontDefault();
    if (def_font.texture.id != 0) {
        SetTextureFilter(def_font.texture, TEXTURE_FILTER_BILINEAR);
    }

    EditorDoc doc;       doc_init(&doc);
    doc_new(&doc, EDITOR_DEFAULT_W, EDITOR_DEFAULT_H);
    if (argc > 1) {
        if (!doc_load(&doc, argv[1])) {
            LOG_W("editor: could not open %s — starting blank", argv[1]);
        }
    }

    EditorView view;     view_init(&view, &doc);
    UndoStack  undo;     undo_init(&undo);
    ToolCtx    ctx;      tool_ctx_init(&ctx);
    FilesDialog files;   memset(&files, 0, sizeof files);
    MetaModal  meta;     memset(&meta, 0, sizeof meta);
    HelpModal  help;     memset(&help, 0, sizeof help);

    ToolKind active_tool = TOOL_TILE;

    while (!WindowShouldClose()) {
        UIDims D = compute_dims(GetScreenHeight());
        /* raygui: scale the font + control padding to match the rest
         * of the UI. */
        GuiSetStyle(DEFAULT, TEXT_SIZE,    D.font_base);
        GuiSetStyle(DEFAULT, TEXT_SPACING, 1);

        /* ---- File dialog has top priority — eat all input ---------- */
        if (files_dialog_active(&files)) {
            BeginDrawing();
            ClearBackground(COL_BG);
            BeginMode2D(view.cam);
            draw_doc(&doc);
            view_draw_grid(&view, &doc);
            EndMode2D();
            files_dialog_draw(&files, GetScreenWidth(), GetScreenHeight());
            EndDrawing();

            if (files.result_pending) {
                files.result_pending = false;
                FilesMode m = files.mode;
                bool ok = files.result_ok;
                char path[512];
                snprintf(path, sizeof path, "%s", files.input);
                files_dialog_close(&files);

                if (ok) {
                    if (m == FILES_MODE_OPEN) {
                        doc_load(&doc, path);
                        view_center(&view, &doc);
                        undo_clear(&undo);
                    } else if (m == FILES_MODE_SAVE) {
                        char errs[4096];
                        int problems = validate_doc(&doc, errs, sizeof errs);
                        if (problems > 0) {
                            files_message(&files, "Save blocked: validation",
                                          errs);
                        } else {
                            doc_save(&doc, path);
                        }
                    }
                }
            }
            continue;
        }

        /* ---- Help modal ------------------------------------------- */
        if (help.open) {
            BeginDrawing();
            ClearBackground(COL_BG);
            BeginMode2D(view.cam);
            draw_doc(&doc);
            view_draw_grid(&view, &doc);
            EndMode2D();
            help_modal_draw(&help, &D);
            EndDrawing();
            if (IsKeyPressed(KEY_H)) help_close(&help);
            continue;
        }

        /* ---- Meta modal ------------------------------------------- */
        if (meta.open) {
            BeginDrawing();
            ClearBackground(COL_BG);
            BeginMode2D(view.cam);
            draw_doc(&doc);
            view_draw_grid(&view, &doc);
            EndMode2D();
            meta_modal_draw(&meta, &doc, &D);
            EndDrawing();
            continue;
        }

        /* ---- Universal verbs --------------------------------------- */
        if (ctrl_down() && IsKeyPressed(KEY_S)) {
            if (shift_down() || !doc.source_path[0]) {
                files_save_dialog(&files, doc.source_path);
            } else {
                char errs[4096];
                int problems = validate_doc(&doc, errs, sizeof errs);
                if (problems > 0) {
                    files_message(&files, "Save blocked: validation", errs);
                } else {
                    doc_save(&doc, doc.source_path);
                }
            }
        }
        if (ctrl_down() && IsKeyPressed(KEY_O)) {
            files_open_dialog(&files, doc.source_path);
        }
        if (ctrl_down() && IsKeyPressed(KEY_N)) {
            doc_new(&doc, EDITOR_DEFAULT_W, EDITOR_DEFAULT_H);
            view_center(&view, &doc);
            undo_clear(&undo);
        }
        if (ctrl_down() && IsKeyPressed(KEY_Z)) {
            if (shift_down()) undo_redo(&undo, &doc);
            else              undo_pop (&undo, &doc);
        }
        if (ctrl_down() && IsKeyPressed(KEY_Y)) {
            undo_redo(&undo, &doc);
        }
        if (IsKeyPressed(KEY_F5)) {
            char errs[4096];
            int problems = validate_doc(&doc, errs, sizeof errs);
            if (problems > 0) {
                files_message(&files, "F5 blocked: validation", errs);
            } else {
                play_test(&doc);
            }
        }

        /* Tool hotkeys + help open. Skip if Ctrl is held. */
        if (!ctrl_down()) {
            if (IsKeyPressed(KEY_T)) active_tool = TOOL_TILE;
            if (IsKeyPressed(KEY_P)) active_tool = TOOL_POLY;
            if (IsKeyPressed(KEY_S)) active_tool = TOOL_SPAWN;
            if (IsKeyPressed(KEY_I)) active_tool = TOOL_PICKUP;
            if (IsKeyPressed(KEY_A)) active_tool = TOOL_AMBI;
            if (IsKeyPressed(KEY_D)) active_tool = TOOL_DECO;
            if (IsKeyPressed(KEY_M)) {
                active_tool = TOOL_META;
                meta_open(&meta, &doc);
            }
            if (IsKeyPressed(KEY_H)) help_open(&help);
        }

        /* Tool-specific key forwarding. */
        const ToolVTable *vt = &tool_vtables()[active_tool];
        ctx.kind = active_tool;
        for (int k = KEY_A; k <= KEY_Z; ++k) {
            if (IsKeyPressed(k) && vt->on_key && !ctrl_down()) {
                if (k != KEY_T && k != KEY_P && k != KEY_S && k != KEY_I &&
                    k != KEY_A && k != KEY_D && k != KEY_M && k != KEY_H) {
                    vt->on_key(&doc, &undo, &ctx, k);
                }
            }
        }
        if (vt->on_key) {
            if (IsKeyPressed(KEY_BACKSPACE)) vt->on_key(&doc, &undo, &ctx, KEY_BACKSPACE);
            if (IsKeyPressed(KEY_ENTER))     vt->on_key(&doc, &undo, &ctx, KEY_ENTER);
            if (IsKeyPressed(KEY_KP_ENTER))  vt->on_key(&doc, &undo, &ctx, KEY_KP_ENTER);
            if (IsKeyPressed(KEY_ESCAPE))    vt->on_key(&doc, &undo, &ctx, KEY_ESCAPE);
            if (IsKeyPressed(KEY_ZERO))      vt->on_key(&doc, &undo, &ctx, KEY_ZERO);
            if (IsKeyPressed(KEY_ONE))       vt->on_key(&doc, &undo, &ctx, KEY_ONE);
            if (IsKeyPressed(KEY_TWO))       vt->on_key(&doc, &undo, &ctx, KEY_TWO);
            if (IsKeyPressed(KEY_COMMA))     vt->on_key(&doc, &undo, &ctx, KEY_COMMA);
            if (IsKeyPressed(KEY_PERIOD))    vt->on_key(&doc, &undo, &ctx, KEY_PERIOD);
            if (IsKeyPressed(KEY_LEFT_BRACKET))  vt->on_key(&doc, &undo, &ctx, KEY_LEFT_BRACKET);
            if (IsKeyPressed(KEY_RIGHT_BRACKET)) vt->on_key(&doc, &undo, &ctx, KEY_RIGHT_BRACKET);
        }

        /* ---- Camera ------------------------------------------------- */
        bool cam_consumed = view_update(&view);

        /* ---- Tool dispatch (mouse) --------------------------------- */
        Vector2 m_screen = GetMousePosition();
        bool over_left  = m_screen.x < D.left_w;
        bool over_right = m_screen.x > GetScreenWidth() - D.right_w;
        bool over_top   = m_screen.y < D.top_h;
        bool over_bot   = m_screen.y > GetScreenHeight() - D.bottom_h;
        bool over_canvas = !cam_consumed && !over_left && !over_right
                                          && !over_top && !over_bot;

        if (over_canvas) {
            Vector2 mw_pos = view_screen_to_world(&view, m_screen);
            int snap_px = (active_tool == TOOL_TILE) ? 1 :
                          (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) ? 1 : 4;
            Vector2 mws = view_snap(mw_pos, snap_px);
            int wx = (int)mws.x, wy = (int)mws.y;
            if (active_tool == TOOL_TILE) {
                wx = (int)mw_pos.x; wy = (int)mw_pos.y;
            }

            if (vt->on_press && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                vt->on_press(&doc, &undo, &ctx, wx, wy);
            }
            if (vt->on_drag && IsMouseButtonDown(MOUSE_BUTTON_LEFT)
                            && !IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                vt->on_drag(&doc, &undo, &ctx, wx, wy);
            }
            if (vt->on_release && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                vt->on_release(&doc, &undo, &ctx, wx, wy);
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                tool_on_press_right(&doc, &undo, &ctx, wx, wy, active_tool);
            }
        }

        /* ---- Render ------------------------------------------------- */
        BeginDrawing();
        ClearBackground(COL_BG);
        BeginMode2D(view.cam);
        draw_doc(&doc);
        view_draw_grid(&view, &doc);
        if (vt->draw_overlay) vt->draw_overlay(&doc, &ctx, &view);
        EndMode2D();

        draw_top_bar(&doc, &D);
        ToolKind picked = draw_tool_buttons(active_tool, &D);
        if (picked != active_tool) {
            active_tool = picked;
            if (active_tool == TOOL_META) meta_open(&meta, &doc);
        }
        switch (active_tool) {
            case TOOL_TILE:   draw_tile_palette  (&ctx, &D); break;
            case TOOL_POLY:   draw_poly_palette  (&ctx, &D); break;
            case TOOL_PICKUP: draw_pickup_palette(&ctx, &D); break;
            case TOOL_SPAWN:  draw_spawn_palette (&ctx, &D); break;
            case TOOL_AMBI:   draw_ambi_palette  (&ctx, &D); break;
            case TOOL_DECO:
            case TOOL_META:
            default:          draw_empty_palette (&D); break;
        }
        draw_status_bar(&doc, &view, &D, active_tool);
        EndDrawing();
    }

    doc_free(&doc);
    undo_clear(&undo);
    log_shutdown();
    CloseWindow();
    return 0;
}
