/* strtok_r is POSIX 2001 — feature-test macro tells glibc to expose it
 * under the strict -std=c11 we compile with. */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "shotmode.h"

#include "doc.h"
#include "editor_ui.h"
#include "palette.h"
#include "poly.h"
#include "render.h"
#include "tool.h"
#include "undo.h"
#include "validate.h"
#include "view.h"

#include "raylib.h"
/* raygui's IMPLEMENTATION is in main.c — we just need the declarations
 * here to call GuiSetStyle and friends. */
#include "raygui.h"

#include "stb_ds.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* COL_* lives in editor_ui.{c,h}. Shot mode uses the same palette so
 * the shots match what the user sees in the interactive editor. */

/* ---- Event encoding ----------------------------------------------- */

typedef enum {
    EV_NONE = 0,
    /* Tool / palette state. */
    EV_TOOL,
    EV_TILE_FLAGS,
    EV_POLY_KIND,
    EV_PRESET,
    EV_PICKUP_VARIANT,
    EV_AMBI_KIND,
    EV_SPAWN_TEAM,
    /* Direct doc mutations. */
    EV_TILE_PAINT,
    EV_TILE_ERASE,
    EV_TILE_FILL_RECT,
    EV_APPLY_PRESET,
    EV_POLY_BEGIN,
    EV_POLY_VERTEX,
    EV_POLY_CLOSE,
    EV_SPAWN_ADD,
    EV_PICKUP_ADD,
    EV_AMBI_ADD,
    EV_DECO_ADD,
    EV_FLAG_ADD,
    EV_SAVE,
    EV_LOAD_FILE,
    EV_NEW_DOC,
    EV_UNDO,
    EV_REDO,
    /* Modal / cursor / camera. */
    EV_OPEN_HELP,
    EV_CLOSE_HELP,
    EV_TOGGLE_HELP,
    EV_OPEN_META,
    EV_CLOSE_META,
    EV_OPEN_LOADOUT,
    EV_CLOSE_LOADOUT,
    EV_LOADOUT_APPLY,        /* writes modal state into ShotState.test_lo */
    EV_LOADOUT_SET,          /* set one slot's idx — i1=slot, str1=name (or numeric idx) */
    EV_LOADOUT_OPEN_DROPDOWN,/* programmatically expand a slot's dropdown — i1=slot */
    EV_LOADOUT_CLOSE_DROPDOWNS, /* collapse all dropdowns (modal stays open) */
    EV_CLICK_TOOL_BUTTON,    /* mirrors main.c's "user clicked tool btn" handler */
    EV_MOUSE,
    EV_CAM_TARGET,
    EV_CAM_ZOOM,
    /* Output / verification. */
    EV_SHOT,
    EV_DUMP,
    EV_VALIDATE,
    EV_ASSERT,
    EV_END,
} EvKind;

typedef enum {
    OP_EQ, OP_NE, OP_GT, OP_GE, OP_LT, OP_LE,
} CmpOp;

typedef struct ShotEvent {
    int      tick;
    EvKind   kind;
    char     str1[64];
    char     str2[128];           /* paths can be long-ish */
    int      i1, i2, i3, i4;
    float    f1;
    CmpOp    op;
} ShotEvent;

typedef struct ScriptHeader {
    int  window_w, window_h;
    char out_dir[256];
    int  max_ticks;
    char initial_load[256];
    int  new_w, new_h;
    char contact_sheet[64];
    int  contact_cols;
    int  contact_cell_w;
    int  contact_cell_h;
    bool draw_panels;            /* render the editor's UI chrome in shots */
} ScriptHeader;

/* ---- Parsing helpers ---------------------------------------------- */

static int strieq(const char *a, const char *b) {
    return a && b && strcasecmp(a, b) == 0;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* Split the line in place into up to `cap` whitespace-separated tokens.
 * Returns the number of tokens; tokens point into the buffer.
 * Hand-rolled (rather than strtok_r) so we don't have to wrestle with
 * glibc's feature-test macros under -std=c11. */
static int split_tokens(char *line, char **out, int cap) {
    int n = 0;
    char *p = line;
    while (*p && n < cap) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        out[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        if (*p) *p++ = 0;
    }
    return n;
}

static int parse_tool(const char *s) {
    if (strieq(s, "tile"))   return TOOL_TILE;
    if (strieq(s, "poly"))   return TOOL_POLY;
    if (strieq(s, "spawn"))  return TOOL_SPAWN;
    if (strieq(s, "pickup")) return TOOL_PICKUP;
    if (strieq(s, "ambi") || strieq(s, "ambient")) return TOOL_AMBI;
    if (strieq(s, "deco"))   return TOOL_DECO;
    if (strieq(s, "meta"))   return TOOL_META;
    return -1;
}

static const char *tool_kind_str(int k) {
    switch (k) {
        case TOOL_TILE:   return "tile";
        case TOOL_POLY:   return "poly";
        case TOOL_SPAWN:  return "spawn";
        case TOOL_PICKUP: return "pickup";
        case TOOL_AMBI:   return "ambi";
        case TOOL_DECO:   return "deco";
        case TOOL_META:   return "meta";
    }
    return "?";
}

static int parse_poly_kind(const char *s) {
    if (strieq(s, "solid"))      return POLY_KIND_SOLID;
    if (strieq(s, "ice"))        return POLY_KIND_ICE;
    if (strieq(s, "deadly"))     return POLY_KIND_DEADLY;
    if (strieq(s, "one_way") || strieq(s, "oneway")) return POLY_KIND_ONE_WAY;
    if (strieq(s, "bg") || strieq(s, "background"))  return POLY_KIND_BACKGROUND;
    return -1;
}

static uint16_t parse_tile_flags(const char *list) {
    /* Comma-separated, case-insensitive. */
    uint16_t f = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", list);
    char *p = buf;
    while (*p) {
        char *t = p;
        while (*p && *p != ',') ++p;
        if (*p) *p++ = 0;
        t = trim(t);
        if      (strieq(t, "solid"))      f |= TILE_F_SOLID;
        else if (strieq(t, "ice"))        f |= TILE_F_ICE;
        else if (strieq(t, "deadly"))     f |= TILE_F_DEADLY;
        else if (strieq(t, "one_way") || strieq(t, "oneway")) f |= TILE_F_ONE_WAY;
        else if (strieq(t, "bg") || strieq(t, "background"))  f |= TILE_F_BACKGROUND;
    }
    return f;
}

static int parse_preset(const char *s) {
    if (strieq(s, "none")) return PRESET_COUNT;
    if (strieq(s, "ramp_up_30"))     return PRESET_RAMP_UP_30;
    if (strieq(s, "ramp_up_45"))     return PRESET_RAMP_UP_45;
    if (strieq(s, "ramp_up_60"))     return PRESET_RAMP_UP_60;
    if (strieq(s, "ramp_dn_30"))     return PRESET_RAMP_DN_30;
    if (strieq(s, "ramp_dn_45"))     return PRESET_RAMP_DN_45;
    if (strieq(s, "ramp_dn_60"))     return PRESET_RAMP_DN_60;
    if (strieq(s, "bowl_30"))        return PRESET_BOWL_30;
    if (strieq(s, "bowl_45"))        return PRESET_BOWL_45;
    if (strieq(s, "overhang_30"))    return PRESET_OVERHANG_30;
    if (strieq(s, "overhang_45"))    return PRESET_OVERHANG_45;
    if (strieq(s, "overhang_60"))    return PRESET_OVERHANG_60;
    if (strieq(s, "alcove_edge"))    return PRESET_ALCOVE_EDGE;
    if (strieq(s, "alcove_jetpack")) return PRESET_ALCOVE_JETPACK;
    if (strieq(s, "alcove_slope_roof") || strieq(s, "slope_roof_alcove"))
        return PRESET_ALCOVE_SLOPE_ROOF;
    if (strieq(s, "cave_block"))     return PRESET_CAVE_BLOCK;
    return -1;
}

static int parse_pickup_variant(const char *s) {
    int n;
    const PickupEntry *list = palette_pickups(&n);
    for (int i = 0; i < n; ++i) {
        const PickupEntry *e = &list[i];
        if (strieq(s, e->name)) return i;
        /* Also accept the enum-style aliases. */
        char alias[40];
        snprintf(alias, sizeof alias, "%s_%s",
                 e->category, e->name);
        if (strieq(s, alias)) return i;
    }
    /* Convenience aliases. */
    if (strieq(s, "health_s")) return PICK_HEALTH_S;
    if (strieq(s, "health_m")) return PICK_HEALTH_M;
    if (strieq(s, "health_l")) return PICK_HEALTH_L;
    if (strieq(s, "ammo_p"))   return PICK_AMMO_PRIMARY;
    if (strieq(s, "ammo_s"))   return PICK_AMMO_SECONDARY;
    if (strieq(s, "armor_l"))  return PICK_ARMOR_LIGHT;
    if (strieq(s, "armor_h"))  return PICK_ARMOR_HEAVY;
    if (strieq(s, "armor_r"))  return PICK_ARMOR_REACTIVE;
    if (strieq(s, "rail"))     return PICK_WEAPON_RAIL;
    if (strieq(s, "mass") || strieq(s, "massdriver"))   return PICK_WEAPON_MASSDRIVER;
    if (strieq(s, "plasma"))   return PICK_WEAPON_PLASMA;
    if (strieq(s, "berserk"))  return PICK_POWERUP_BERSERK;
    if (strieq(s, "invis"))    return PICK_POWERUP_INVIS;
    if (strieq(s, "jet_fuel") || strieq(s, "fuel"))     return PICK_JET_FUEL;
    if (strieq(s, "dummy"))    return PICK_PRACTICE_DUMMY;
    return -1;
}

static int parse_ambi_kind(const char *s) {
    if (strieq(s, "wind"))   return AMBI_WIND;
    if (strieq(s, "zero_g") || strieq(s, "zerog")) return AMBI_ZERO_G;
    if (strieq(s, "acid"))   return AMBI_ACID;
    if (strieq(s, "fog"))    return AMBI_FOG;
    return -1;
}

static int parse_op(const char *s, CmpOp *out) {
    if (strcmp(s, "==") == 0) { *out = OP_EQ; return 0; }
    if (strcmp(s, "!=") == 0) { *out = OP_NE; return 0; }
    if (strcmp(s, ">")  == 0) { *out = OP_GT; return 0; }
    if (strcmp(s, ">=") == 0) { *out = OP_GE; return 0; }
    if (strcmp(s, "<")  == 0) { *out = OP_LT; return 0; }
    if (strcmp(s, "<=") == 0) { *out = OP_LE; return 0; }
    return -1;
}

static const char *op_str(CmpOp op) {
    switch (op) {
        case OP_EQ: return "==";
        case OP_NE: return "!=";
        case OP_GT: return ">";
        case OP_GE: return ">=";
        case OP_LT: return "<";
        case OP_LE: return "<=";
    }
    return "?";
}

static int cmp_apply(int actual, CmpOp op, int expected) {
    switch (op) {
        case OP_EQ: return actual == expected;
        case OP_NE: return actual != expected;
        case OP_GT: return actual >  expected;
        case OP_GE: return actual >= expected;
        case OP_LT: return actual <  expected;
        case OP_LE: return actual <= expected;
    }
    return 0;
}

/* ---- Script parser ------------------------------------------------ */

static const char *script_basename(const char *path) {
    const char *b = strrchr(path, '/');
#ifdef _WIN32
    const char *bb = strrchr(path, '\\');
    if (bb && (!b || bb > b)) b = bb;
#endif
    return b ? b + 1 : path;
}

static int parse_script(const char *path, ScriptHeader *hdr,
                        ShotEvent **out_events) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "shotmode: cannot open script: %s\n", path);
        return -1;
    }
    /* Defaults. */
    memset(hdr, 0, sizeof(*hdr));
    hdr->window_w = 1280;
    hdr->window_h = 800;
    hdr->max_ticks = 600;
    hdr->draw_panels = true;
    /* Default output: build/shots/editor/<scriptname-without-ext>/ */
    {
        const char *bn = script_basename(path);
        char base[128];
        snprintf(base, sizeof base, "%s", bn);
        char *dot = strrchr(base, '.');
        if (dot) *dot = 0;
        snprintf(hdr->out_dir, sizeof hdr->out_dir,
                 "build/shots/editor/%s", base);
    }

    char line[1024];
    int  line_no = 0;
    int  errors  = 0;

    while (fgets(line, sizeof line, f)) {
        ++line_no;
        char *l = trim(line);
        if (!*l || *l == '#') continue;

        /* Strip trailing comments. We don't have quoted-string support
         * in directives, so any `#` after the directive is a comment.
         * Handles `at 5 tool tile  # comment` correctly. */
        char *hash = strchr(l, '#');
        if (hash) { *hash = 0; l = trim(l); if (!*l) continue; }

        char copy[1024];
        snprintf(copy, sizeof copy, "%s", l);
        char *toks[16] = {0};
        int  nt = split_tokens(copy, toks, 16);
        if (nt < 1) continue;

        if (strieq(toks[0], "window") && nt == 3) {
            hdr->window_w = atoi(toks[1]);
            hdr->window_h = atoi(toks[2]);
            continue;
        }
        if (strieq(toks[0], "out") && nt == 2) {
            snprintf(hdr->out_dir, sizeof hdr->out_dir, "%s", toks[1]);
            continue;
        }
        if (strieq(toks[0], "ticks") && nt == 2) {
            hdr->max_ticks = atoi(toks[1]);
            continue;
        }
        if (strieq(toks[0], "load") && nt == 2) {
            snprintf(hdr->initial_load, sizeof hdr->initial_load, "%s", toks[1]);
            continue;
        }
        if (strieq(toks[0], "new") && nt == 3) {
            hdr->new_w = atoi(toks[1]);
            hdr->new_h = atoi(toks[2]);
            continue;
        }
        if (strieq(toks[0], "panels") && nt == 2) {
            hdr->draw_panels = strieq(toks[1], "on") || strieq(toks[1], "true") ||
                               strieq(toks[1], "1");
            continue;
        }
        if (strieq(toks[0], "contact_sheet") && nt >= 2) {
            snprintf(hdr->contact_sheet, sizeof hdr->contact_sheet, "%s", toks[1]);
            /* Optional `cols <C>` and `cell <W> <H>` after the name. */
            for (int j = 2; j < nt; ) {
                if (strieq(toks[j], "cols") && j + 1 < nt) {
                    hdr->contact_cols = atoi(toks[j + 1]); j += 2;
                } else if (strieq(toks[j], "cell") && j + 2 < nt) {
                    hdr->contact_cell_w = atoi(toks[j + 1]);
                    hdr->contact_cell_h = atoi(toks[j + 2]); j += 3;
                } else {
                    ++j;
                }
            }
            continue;
        }

        /* Per-tick directives. */
        if (!strieq(toks[0], "at") || nt < 3) {
            fprintf(stderr, "shotmode: %s:%d: unrecognized line: %s\n",
                    path, line_no, l);
            ++errors;
            continue;
        }

        ShotEvent ev = {0};
        ev.tick = atoi(toks[1]);
        const char *cmd = toks[2];

        if (strieq(cmd, "tool") && nt == 4) {
            ev.kind = EV_TOOL;
            int t = parse_tool(toks[3]);
            if (t < 0) { fprintf(stderr, "shotmode: bad tool: %s\n", toks[3]); ++errors; continue; }
            ev.i1 = t;
        }
        else if (strieq(cmd, "tile_flags") && nt == 4) {
            ev.kind = EV_TILE_FLAGS;
            ev.i1 = parse_tile_flags(toks[3]);
        }
        else if (strieq(cmd, "poly_kind") && nt == 4) {
            ev.kind = EV_POLY_KIND;
            int k = parse_poly_kind(toks[3]);
            if (k < 0) { fprintf(stderr, "shotmode: bad poly_kind: %s\n", toks[3]); ++errors; continue; }
            ev.i1 = k;
        }
        else if (strieq(cmd, "preset") && nt == 4) {
            ev.kind = EV_PRESET;
            int p = parse_preset(toks[3]);
            if (p < 0) { fprintf(stderr, "shotmode: bad preset: %s\n", toks[3]); ++errors; continue; }
            ev.i1 = p;
        }
        else if (strieq(cmd, "pickup_variant") && nt == 4) {
            ev.kind = EV_PICKUP_VARIANT;
            int v = parse_pickup_variant(toks[3]);
            if (v < 0) { fprintf(stderr, "shotmode: bad pickup_variant: %s\n", toks[3]); ++errors; continue; }
            ev.i1 = v;
        }
        else if (strieq(cmd, "ambi_kind") && nt == 4) {
            ev.kind = EV_AMBI_KIND;
            int k = parse_ambi_kind(toks[3]);
            if (k < 0) { fprintf(stderr, "shotmode: bad ambi_kind: %s\n", toks[3]); ++errors; continue; }
            ev.i1 = k;
        }
        else if (strieq(cmd, "spawn_team") && nt == 4) {
            ev.kind = EV_SPAWN_TEAM;
            ev.i1 = atoi(toks[3]);
        }
        else if (strieq(cmd, "tile_paint") && nt == 5) {
            ev.kind = EV_TILE_PAINT;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]);
        }
        else if (strieq(cmd, "tile_erase") && nt == 5) {
            ev.kind = EV_TILE_ERASE;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]);
        }
        else if (strieq(cmd, "tile_fill_rect") && nt == 7) {
            ev.kind = EV_TILE_FILL_RECT;
            ev.i1 = atoi(toks[3]);   /* x0 in world pixels */
            ev.i2 = atoi(toks[4]);   /* y0 */
            ev.i3 = atoi(toks[5]);   /* x1 (exclusive) */
            ev.i4 = atoi(toks[6]);   /* y1 (exclusive) */
        }
        else if (strieq(cmd, "apply_preset") && nt == 6) {
            ev.kind = EV_APPLY_PRESET;
            int p = parse_preset(toks[3]);
            if (p < 0) { fprintf(stderr, "shotmode: bad preset: %s\n", toks[3]); ++errors; continue; }
            ev.i1 = p; ev.i2 = atoi(toks[4]); ev.i3 = atoi(toks[5]);
        }
        else if (strieq(cmd, "poly_begin"))  { ev.kind = EV_POLY_BEGIN; }
        else if (strieq(cmd, "poly_vertex") && nt == 5) {
            ev.kind = EV_POLY_VERTEX;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]);
        }
        else if (strieq(cmd, "poly_close")) { ev.kind = EV_POLY_CLOSE; }
        else if (strieq(cmd, "spawn_add") && nt == 6) {
            ev.kind = EV_SPAWN_ADD;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]); ev.i3 = atoi(toks[5]);
        }
        else if (strieq(cmd, "pickup_add") && nt == 6) {
            ev.kind = EV_PICKUP_ADD;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]);
            int v = parse_pickup_variant(toks[5]);
            if (v < 0) { fprintf(stderr, "shotmode: bad variant: %s\n", toks[5]); ++errors; continue; }
            ev.i3 = v;
        }
        else if (strieq(cmd, "ambi_add") && nt == 8) {
            ev.kind = EV_AMBI_ADD;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]);
            ev.i3 = atoi(toks[5]); ev.i4 = atoi(toks[6]);
            int k = parse_ambi_kind(toks[7]);
            if (k < 0) { fprintf(stderr, "shotmode: bad ambi_kind: %s\n", toks[7]); ++errors; continue; }
            ev.f1 = (float)k;
        }
        else if (strieq(cmd, "deco_add") && nt == 5) {
            ev.kind = EV_DECO_ADD;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]);
        }
        else if (strieq(cmd, "flag_add") && nt == 6) {
            ev.kind = EV_FLAG_ADD;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]); ev.i3 = atoi(toks[5]);
        }
        else if (strieq(cmd, "save") && nt == 4) {
            ev.kind = EV_SAVE;
            snprintf(ev.str2, sizeof ev.str2, "%s", toks[3]);
        }
        else if (strieq(cmd, "load_file") && nt == 4) {
            ev.kind = EV_LOAD_FILE;
            snprintf(ev.str2, sizeof ev.str2, "%s", toks[3]);
        }
        else if (strieq(cmd, "new_doc") && nt == 5) {
            ev.kind = EV_NEW_DOC;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]);
        }
        else if (strieq(cmd, "undo")) { ev.kind = EV_UNDO; }
        else if (strieq(cmd, "redo")) { ev.kind = EV_REDO; }
        else if (strieq(cmd, "open_help"))   { ev.kind = EV_OPEN_HELP;   }
        else if (strieq(cmd, "close_help"))  { ev.kind = EV_CLOSE_HELP;  }
        else if (strieq(cmd, "toggle_help")) { ev.kind = EV_TOGGLE_HELP; }
        else if (strieq(cmd, "open_meta"))   { ev.kind = EV_OPEN_META;   }
        else if (strieq(cmd, "close_meta"))  { ev.kind = EV_CLOSE_META;  }
        else if (strieq(cmd, "open_loadout"))    { ev.kind = EV_OPEN_LOADOUT;    }
        else if (strieq(cmd, "close_loadout"))   { ev.kind = EV_CLOSE_LOADOUT;   }
        else if (strieq(cmd, "loadout_apply"))   { ev.kind = EV_LOADOUT_APPLY;   }
        else if (strieq(cmd, "loadout_open_dropdown") && nt == 4) {
            /* loadout_open_dropdown <slot> — programmatically expands
             * one slot's dropdown so a screenshot can capture the
             * open-list state. The modal must already be open. */
            int slot = ui_loadout_slot_from_name(toks[3]);
            if (slot < 0) {
                fprintf(stderr, "shotmode: %s:%d: bad loadout slot: %s\n",
                        path, line_no, toks[3]);
                ++errors; continue;
            }
            ev.kind = EV_LOADOUT_OPEN_DROPDOWN;
            ev.i1   = slot;
        }
        else if (strieq(cmd, "loadout_close_dropdowns")) {
            ev.kind = EV_LOADOUT_CLOSE_DROPDOWNS;
        }
        else if (strieq(cmd, "loadout_set") && nt >= 5) {
            /* loadout_set <slot> <name_or_idx>
             * <slot>: chassis|primary|secondary|armor|jetpack
             * <name_or_idx>: an integer (direct dropdown idx) OR an
             *   option name with underscores in place of spaces (so
             *   the line tokenizer doesn't split mid-name). E.g.,
             *   `loadout_set secondary Grappling_Hook` → maps to the
             *   "Grappling Hook" entry. Index 0 always means
             *   "(default)" → empty string in test_lo. */
            int slot = ui_loadout_slot_from_name(toks[3]);
            if (slot < 0) {
                fprintf(stderr, "shotmode: %s:%d: bad loadout slot: %s\n",
                        path, line_no, toks[3]);
                ++errors; continue;
            }
            ev.kind = EV_LOADOUT_SET;
            ev.i1   = slot;
            snprintf(ev.str1, sizeof ev.str1, "%s", toks[4]);
        }
        else if (strieq(cmd, "click_tool_button") && nt == 4) {
            ev.kind = EV_CLICK_TOOL_BUTTON;
            int t = parse_tool(toks[3]);
            if (t < 0) { fprintf(stderr, "shotmode: bad tool: %s\n", toks[3]); ++errors; continue; }
            ev.i1 = t;
        }
        else if (strieq(cmd, "mouse") && nt == 5) {
            ev.kind = EV_MOUSE;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]);
        }
        else if (strieq(cmd, "camera_target") && nt == 5) {
            ev.kind = EV_CAM_TARGET;
            ev.i1 = atoi(toks[3]); ev.i2 = atoi(toks[4]);
        }
        else if (strieq(cmd, "camera_zoom") && nt == 4) {
            ev.kind = EV_CAM_ZOOM;
            ev.f1 = (float)atof(toks[3]);
        }
        else if (strieq(cmd, "shot") && nt == 4) {
            ev.kind = EV_SHOT;
            snprintf(ev.str1, sizeof ev.str1, "%s", toks[3]);
        }
        else if (strieq(cmd, "dump"))     { ev.kind = EV_DUMP;     }
        else if (strieq(cmd, "validate")) { ev.kind = EV_VALIDATE; }
        else if (strieq(cmd, "assert") && nt >= 6) {
            /* at <tick> assert <field> <op> <value> */
            ev.kind = EV_ASSERT;
            snprintf(ev.str1, sizeof ev.str1, "%s", toks[3]);
            CmpOp op;
            if (parse_op(toks[4], &op) != 0) {
                fprintf(stderr, "shotmode: %s:%d: bad assert op: %s\n",
                        path, line_no, toks[4]);
                ++errors; continue;
            }
            ev.op = op;
            /* Value is either an int or (for active_tool) a tool name. */
            if (strieq(ev.str1, "active_tool")) {
                int t = parse_tool(toks[5]);
                if (t < 0) {
                    fprintf(stderr, "shotmode: %s:%d: bad assert value: %s\n",
                            path, line_no, toks[5]);
                    ++errors; continue;
                }
                ev.i1 = t;
            } else {
                ev.i1 = atoi(toks[5]);
            }
        }
        else if (strieq(cmd, "end")) { ev.kind = EV_END; }
        else {
            fprintf(stderr, "shotmode: %s:%d: unknown directive: %s\n",
                    path, line_no, cmd);
            ++errors;
            continue;
        }

        arrput(*out_events, ev);
    }

    fclose(f);
    return errors == 0 ? 0 : -1;
}

/* ---- Output helpers ----------------------------------------------- */

static void mkdirs_p(const char *path) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
#if defined(_WIN32)
            mkdir(buf);
#else
            mkdir(buf, 0755);
#endif
            *p = '/';
        }
    }
#if defined(_WIN32)
    mkdir(buf);
#else
    mkdir(buf, 0755);
#endif
}

static FILE *g_log_fp = NULL;

static void slog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log_fp) { va_list ap2; va_copy(ap2, ap); vfprintf(g_log_fp, fmt, ap2); va_end(ap2); fputc('\n', g_log_fp); }
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
}

/* Lightweight contact-sheet builder. Composes a grid of the captured
 * shots into one image so we can review a run as a single PNG. */
static void compose_contact_sheet(const char *out_dir, const char *name,
                                  char **shot_paths, int n_shots,
                                  int cell_w, int cell_h, int cols) {
    if (n_shots <= 0) return;
    if (cell_w <= 0)  cell_w = 320;
    if (cell_h <= 0)  cell_h = 200;
    if (cols   <= 0)  cols   = 4;
    int rows = (n_shots + cols - 1) / cols;
    int W = cell_w * cols;
    int H = cell_h * rows;
    Image grid = GenImageColor(W, H, BLACK);
    for (int i = 0; i < n_shots; ++i) {
        Image src = LoadImage(shot_paths[i]);
        if (src.data == NULL) continue;
        ImageResize(&src, cell_w, cell_h);
        int x = (i % cols) * cell_w;
        int y = (i / cols) * cell_h;
        ImageDraw(&grid, src,
                  (Rectangle){0, 0, (float)src.width, (float)src.height},
                  (Rectangle){(float)x, (float)y, (float)cell_w, (float)cell_h},
                  WHITE);
        UnloadImage(src);
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", out_dir, name);
    ExportImage(grid, path);
    UnloadImage(grid);
    slog("contact_sheet: wrote %s (%d shots in %dx%d cells)",
         path, n_shots, cols, rows);
}

/* ---- Render one frame --------------------------------------------- */

/* A small strip in the top of the canvas summarizing the doc state.
 * Only used when panels are off — when panels are on, the editor's
 * own top bar + status bar cover this. */
static void render_summary_overlay(const EditorDoc *d, int active_tool,
                                   const UIDims *D) {
    char buf[200];
    snprintf(buf, sizeof buf, "tool=%s polys=%td spawns=%td pickups=%td",
             tool_kind_str(active_tool),
             arrlen(d->polys), arrlen(d->spawns), arrlen(d->pickups));
    int sw = GetScreenWidth();
    int tw = ui_measure_text(buf, D->font_sm);
    DrawRectangle(sw - tw - D->pad * 2, D->pad / 2,
                  tw + D->pad * 2, D->font_sm + D->pad / 2,
                  (Color){0, 0, 0, 180});
    ui_draw_text(buf, sw - tw - D->pad, D->pad / 2 + D->pad / 4,
                 D->font_sm, COL_TEXT_HIGH);
}

/* ---- Apply event -------------------------------------------------- */

typedef struct ShotState {
    EditorDoc   doc;
    EditorView  view;
    UndoStack   undo;
    ToolCtx     ctx;
    int         active_tool;
    HelpModal   help;
    MetaModal   meta;
    LoadoutModal    loadout;        /* test-play loadout modal (L key) */
    TestPlayLoadout test_lo;        /* what F5 would forward to the game */
    Vector2     cursor_screen;
    int         assert_failures;
    bool        draw_panels;        /* render the actual UI chrome in shots */
} ShotState;

static int doc_field_value(const ShotState *s, const char *field) {
    if (strieq(field, "polys"))      return (int)arrlen(s->doc.polys);
    if (strieq(field, "spawns"))     return (int)arrlen(s->doc.spawns);
    if (strieq(field, "pickups"))    return (int)arrlen(s->doc.pickups);
    if (strieq(field, "ambis"))      return (int)arrlen(s->doc.ambis);
    if (strieq(field, "decos"))      return (int)arrlen(s->doc.decos);
    if (strieq(field, "flags"))      return (int)arrlen(s->doc.flags);
    if (strieq(field, "active_tool"))return s->active_tool;
    if (strieq(field, "dirty"))      return s->doc.dirty ? 1 : 0;
    if (strieq(field, "help_open"))  return s->help.open ? 1 : 0;
    if (strieq(field, "meta_open"))  return s->meta.open ? 1 : 0;
    if (strieq(field, "loadout_open")) return s->loadout.open ? 1 : 0;
    /* Which slot's dropdown is currently expanded (-1 if none). */
    if (strieq(field, "loadout_dropdown_open")) {
        for (int i = 0; i < LOADOUT_SLOT_COUNT; ++i)
            if (s->loadout.edit[i]) return i;
        return -1;
    }
    /* Per-slot dropdown index — what's currently picked in the modal,
     * regardless of whether Apply has been pressed. 0 means "(default)". */
    if (strieq(field, "loadout_chassis_idx"))   return s->loadout.idx[LOADOUT_SLOT_CHASSIS];
    if (strieq(field, "loadout_primary_idx"))   return s->loadout.idx[LOADOUT_SLOT_PRIMARY];
    if (strieq(field, "loadout_secondary_idx")) return s->loadout.idx[LOADOUT_SLOT_SECONDARY];
    if (strieq(field, "loadout_armor_idx"))     return s->loadout.idx[LOADOUT_SLOT_ARMOR];
    if (strieq(field, "loadout_jetpack_idx"))   return s->loadout.idx[LOADOUT_SLOT_JETPACK];
    if (strieq(field, "loadout_mode_idx"))      return s->loadout.idx[LOADOUT_SLOT_MODE];
    /* Per-slot APPLIED idx — what's stored in test_lo and would be
     * forwarded to the game on F5. Resolves the test_lo string back to
     * the dropdown idx so an integer assert can verify the apply flow.
     * Returns -1 if the test_lo string is set but doesn't match any
     * known option (catches typos in the apply path). */
    if (strieq(field, "test_lo_chassis_idx"))   return ui_loadout_slot_idx(LOADOUT_SLOT_CHASSIS,   s->test_lo.chassis);
    if (strieq(field, "test_lo_primary_idx"))   return ui_loadout_slot_idx(LOADOUT_SLOT_PRIMARY,   s->test_lo.primary);
    if (strieq(field, "test_lo_secondary_idx")) return ui_loadout_slot_idx(LOADOUT_SLOT_SECONDARY, s->test_lo.secondary);
    if (strieq(field, "test_lo_armor_idx"))     return ui_loadout_slot_idx(LOADOUT_SLOT_ARMOR,     s->test_lo.armor);
    if (strieq(field, "test_lo_jetpack_idx"))   return ui_loadout_slot_idx(LOADOUT_SLOT_JETPACK,   s->test_lo.jetpack);
    if (strieq(field, "test_lo_mode_idx"))      return ui_loadout_slot_idx(LOADOUT_SLOT_MODE,      s->test_lo.mode);
    if (strieq(field, "tiles_solid")) {
        int n = 0;
        int total = s->doc.width * s->doc.height;
        for (int i = 0; i < total; ++i)
            if (s->doc.tiles[i].flags & TILE_F_SOLID) ++n;
        return n;
    }
    if (strieq(field, "validate_problems")) {
        char buf[4096];
        return validate_doc(&s->doc, buf, sizeof buf);
    }
    return INT_MIN;       /* signal unknown field */
}

static void apply_event(ShotState *s, const ShotEvent *e) {
    EditorDoc *d  = &s->doc;
    UndoStack *u  = &s->undo;
    ToolCtx   *c  = &s->ctx;
    EditorView *v = &s->view;

    switch (e->kind) {
        case EV_TOOL:           s->active_tool = e->i1; c->kind = (ToolKind)e->i1; break;
        case EV_TILE_FLAGS:     c->tile_flags = (uint16_t)e->i1; break;
        case EV_POLY_KIND:      c->poly_kind  = (uint16_t)e->i1; break;
        case EV_PRESET:         c->poly_preset = (PresetKind)e->i1; break;
        case EV_PICKUP_VARIANT: c->pickup_variant = (PickupVariant)e->i1; break;
        case EV_AMBI_KIND:      c->ambi_kind = e->i1; break;
        case EV_SPAWN_TEAM:     c->spawn_team = (uint8_t)e->i1; break;

        case EV_TILE_PAINT: {
            int tx = e->i1 / d->tile_size;
            int ty = e->i2 / d->tile_size;
            LvlTile t = { .id = c->tile_id, .flags = c->tile_flags };
            undo_begin_tile_stroke(u);
            if (tx >= 0 && tx < d->width && ty >= 0 && ty < d->height) {
                LvlTile before = d->tiles[ty * d->width + tx];
                d->tiles[ty * d->width + tx] = t;
                undo_record_tile(u, tx, ty, before, t);
                d->dirty = true;
            }
            undo_end_tile_stroke(u, d);
            break;
        }
        case EV_TILE_ERASE: {
            int tx = e->i1 / d->tile_size;
            int ty = e->i2 / d->tile_size;
            LvlTile empty = (LvlTile){0};
            undo_begin_tile_stroke(u);
            if (tx >= 0 && tx < d->width && ty >= 0 && ty < d->height) {
                LvlTile before = d->tiles[ty * d->width + tx];
                d->tiles[ty * d->width + tx] = empty;
                undo_record_tile(u, tx, ty, before, empty);
                d->dirty = true;
            }
            undo_end_tile_stroke(u, d);
            break;
        }
        case EV_TILE_FILL_RECT: {
            /* Stamp every tile inside [x0,y0)..[x1,y1) world-pixel
             * rect with the current tile_flags. Saves writing 100+
             * tile_paint lines for floors and platforms. */
            int x0 = e->i1 / d->tile_size;
            int y0 = e->i2 / d->tile_size;
            int x1 = (e->i3 + d->tile_size - 1) / d->tile_size;
            int y1 = (e->i4 + d->tile_size - 1) / d->tile_size;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > d->width)  x1 = d->width;
            if (y1 > d->height) y1 = d->height;
            LvlTile t = { .id = c->tile_id, .flags = c->tile_flags };
            undo_begin_tile_stroke(u);
            for (int ty = y0; ty < y1; ++ty) {
                for (int tx = x0; tx < x1; ++tx) {
                    LvlTile before = d->tiles[ty * d->width + tx];
                    if (before.id == t.id && before.flags == t.flags) continue;
                    d->tiles[ty * d->width + tx] = t;
                    undo_record_tile(u, tx, ty, before, t);
                    d->dirty = true;
                }
            }
            undo_end_tile_stroke(u, d);
            break;
        }
        case EV_APPLY_PRESET: {
            int before = (int)arrlen(d->polys);
            int tris = palette_apply_preset(d, (PresetKind)e->i1, e->i2, e->i3);
            int after = (int)arrlen(d->polys);
            for (int i = before; i < after; ++i) {
                undo_record_obj_add(u, UC_POLY_ADD, i, &d->polys[i]);
            }
            slog("apply_preset %d at (%d,%d): %d triangles", e->i1, e->i2, e->i3, tris);
            break;
        }
        case EV_POLY_BEGIN:
            c->poly_vertex_count = 0;
            break;
        case EV_POLY_VERTEX:
            if (c->poly_vertex_count < POLY_MAX_VERTS) {
                c->poly_in_progress[c->poly_vertex_count++] =
                    (EditorPolyVert){ e->i1, e->i2 };
            }
            break;
        case EV_POLY_CLOSE: {
            int n = c->poly_vertex_count;
            PolyValidate r = poly_validate(c->poly_in_progress, n);
            if (r != POLY_VALID) {
                slog("poly_close: invalid (%d), %d vertices", (int)r, n);
                ++s->assert_failures;
                c->poly_vertex_count = 0;
                break;
            }
            LvlPoly tris[POLY_MAX_VERTS];
            int t = poly_triangulate(c->poly_in_progress, n,
                                     c->poly_kind, tris, POLY_MAX_VERTS);
            if (t <= 0) {
                slog("poly_close: triangulation failed (%d verts)", n);
                ++s->assert_failures;
                c->poly_vertex_count = 0;
                break;
            }
            for (int i = 0; i < t; ++i) {
                arrput(d->polys, tris[i]);
                undo_record_obj_add(u, UC_POLY_ADD,
                                    (int)arrlen(d->polys) - 1,
                                    &d->polys[arrlen(d->polys) - 1]);
            }
            slog("poly_close: %d verts → %d triangles", n, t);
            c->poly_vertex_count = 0;
            d->dirty = true;
            break;
        }
        case EV_SPAWN_ADD: {
            LvlSpawn sp = { .pos_x = (int16_t)e->i1, .pos_y = (int16_t)e->i2,
                            .team = (uint8_t)e->i3, .flags = 1, .lane_hint = 0 };
            arrput(d->spawns, sp);
            undo_record_obj_add(u, UC_SPAWN_ADD, (int)arrlen(d->spawns) - 1, &sp);
            d->dirty = true;
            break;
        }
        case EV_PICKUP_ADD: {
            int n;
            const PickupEntry *list = palette_pickups(&n);
            if (e->i3 < 0 || e->i3 >= n) break;
            const PickupEntry *pe = &list[e->i3];
            LvlPickup pk = {0};
            pk.pos_x = (int16_t)e->i1; pk.pos_y = (int16_t)e->i2;
            pk.category = pe->category_id; pk.variant = pe->variant_id;
            pk.respawn_ms = pe->respawn_ms;
            arrput(d->pickups, pk);
            undo_record_obj_add(u, UC_PICKUP_ADD, (int)arrlen(d->pickups) - 1, &pk);
            d->dirty = true;
            break;
        }
        case EV_AMBI_ADD: {
            LvlAmbi a = {0};
            a.rect_x = (int16_t)e->i1; a.rect_y = (int16_t)e->i2;
            a.rect_w = (int16_t)e->i3; a.rect_h = (int16_t)e->i4;
            a.kind = (uint16_t)(int)e->f1; a.strength_q = 16384;
            a.dir_x_q = 32767; a.dir_y_q = 0;
            arrput(d->ambis, a);
            undo_record_obj_add(u, UC_AMBI_ADD, (int)arrlen(d->ambis) - 1, &a);
            d->dirty = true;
            break;
        }
        case EV_DECO_ADD: {
            LvlDeco de = {0};
            de.pos_x = (int16_t)e->i1; de.pos_y = (int16_t)e->i2;
            de.scale_q = 32767; de.layer = 1;
            arrput(d->decos, de);
            undo_record_obj_add(u, UC_DECO_ADD, (int)arrlen(d->decos) - 1, &de);
            d->dirty = true;
            break;
        }
        case EV_FLAG_ADD: {
            LvlFlag f = { .pos_x = (int16_t)e->i1, .pos_y = (int16_t)e->i2,
                          .team = (uint8_t)e->i3 };
            arrput(d->flags, f);
            undo_record_obj_add(u, UC_FLAG_ADD, (int)arrlen(d->flags) - 1, &f);
            /* Mirror tool_flag's behavior: when both team-1 and team-2
             * flags are present, set the CTF bit on META.mode_mask so
             * the loader recognises this as a CTF map (and the runtime
             * --test-play auto-detection picks CTF mode). */
            int red_n = 0, blue_n = 0;
            int fn = (int)arrlen(d->flags);
            for (int k = 0; k < fn; ++k) {
                if (d->flags[k].team == 1) red_n++;
                if (d->flags[k].team == 2) blue_n++;
            }
            if (red_n > 0 && blue_n > 0) {
                d->meta.mode_mask |= (uint16_t)(1u << 2);   /* MATCH_MODE_CTF = 2 */
            }
            d->dirty = true;
            break;
        }
        case EV_SAVE: {
            bool ok = doc_save(d, e->str2);
            slog("save '%s': %s", e->str2, ok ? "ok" : "FAIL");
            if (!ok) ++s->assert_failures;
            break;
        }
        case EV_LOAD_FILE: {
            bool ok = doc_load(d, e->str2);
            slog("load '%s': %s", e->str2, ok ? "ok" : "FAIL");
            if (!ok) ++s->assert_failures;
            break;
        }
        case EV_NEW_DOC: {
            doc_new(d, e->i1, e->i2);
            view_center(v, d);
            undo_clear(u);
            break;
        }
        case EV_UNDO: undo_pop (u, d); break;
        case EV_REDO: undo_redo(u, d); break;

        case EV_OPEN_HELP:   ui_help_open  (&s->help); break;
        case EV_CLOSE_HELP:  ui_help_close (&s->help); break;
        case EV_TOGGLE_HELP: ui_help_toggle(&s->help); break;
        case EV_OPEN_META:   ui_meta_open  (&s->meta, &s->doc); break;
        case EV_CLOSE_META:  s->meta.open = false; break;
        case EV_OPEN_LOADOUT:  ui_loadout_open (&s->loadout, &s->test_lo); break;
        case EV_CLOSE_LOADOUT: ui_loadout_close(&s->loadout); break;
        case EV_LOADOUT_APPLY: {
            ui_loadout_apply(&s->loadout, &s->test_lo);
            ui_loadout_close(&s->loadout);
            slog("loadout applied: chassis='%s' primary='%s' secondary='%s' armor='%s' jet='%s' mode='%s'",
                 s->test_lo.chassis,   s->test_lo.primary,
                 s->test_lo.secondary, s->test_lo.armor,
                 s->test_lo.jetpack,   s->test_lo.mode);
            break;
        }
        case EV_LOADOUT_OPEN_DROPDOWN: {
            int slot = e->i1;
            if (slot < 0 || slot >= LOADOUT_SLOT_COUNT) break;
            for (int k = 0; k < LOADOUT_SLOT_COUNT; ++k) s->loadout.edit[k] = false;
            s->loadout.edit[slot] = true;
            slog("loadout_open_dropdown: slot=%d", slot);
            break;
        }
        case EV_LOADOUT_CLOSE_DROPDOWNS: {
            for (int k = 0; k < LOADOUT_SLOT_COUNT; ++k) s->loadout.edit[k] = false;
            slog("loadout_close_dropdowns");
            break;
        }
        case EV_LOADOUT_SET: {
            int slot = e->i1;
            if (slot < 0 || slot >= LOADOUT_SLOT_COUNT) break;
            int idx = -1;
            if (e->str1[0] >= '0' && e->str1[0] <= '9') {
                idx = atoi(e->str1);
            } else {
                /* Convert underscores back to spaces and look up. */
                char buf[64];
                snprintf(buf, sizeof buf, "%s", e->str1);
                for (char *p = buf; *p; ++p) if (*p == '_') *p = ' ';
                idx = ui_loadout_slot_idx(slot, buf);
            }
            if (idx < 0 || idx >= ui_loadout_slot_count(slot)) {
                slog("loadout_set: slot=%d value='%s' → unknown option   FAIL",
                     slot, e->str1);
                ++s->assert_failures;
                break;
            }
            s->loadout.idx[slot] = idx;
            slog("loadout_set: slot=%d → idx=%d (%s)", slot, idx,
                 ui_loadout_slot_name(slot, idx)[0]
                     ? ui_loadout_slot_name(slot, idx) : "(default)");
            break;
        }
        case EV_CLICK_TOOL_BUTTON: {
            /* Mirrors main.c's "user clicked a toolbar button" logic.
             * Always opens meta on click — that's bug 4's fix. */
            ToolKind tk = (ToolKind)e->i1;
            s->active_tool = tk;
            s->ctx.kind = tk;
            if (tk == TOOL_META) ui_meta_open(&s->meta, &s->doc);
            break;
        }

        case EV_MOUSE:
            s->cursor_screen = (Vector2){(float)e->i1, (float)e->i2};
            SetMousePosition(e->i1, e->i2);
            break;
        case EV_CAM_TARGET:
            v->cam.target = (Vector2){(float)e->i1, (float)e->i2};
            break;
        case EV_CAM_ZOOM:
            v->cam.zoom = e->f1;
            break;

        case EV_DUMP:
            slog("dump: doc %dx%d, %td polys, %td spawns, %td pickups, "
                 "%td ambis, %td decos, %td flags, dirty=%d",
                 d->width, d->height,
                 arrlen(d->polys), arrlen(d->spawns),
                 arrlen(d->pickups), arrlen(d->ambis),
                 arrlen(d->decos), arrlen(d->flags), d->dirty);
            break;

        case EV_VALIDATE: {
            char buf[4096];
            int n = validate_doc(d, buf, sizeof buf);
            slog("validate: %d problem(s)%s%s", n,
                 n > 0 ? "\n" : "",
                 n > 0 ? buf  : "");
            break;
        }

        case EV_ASSERT: {
            int actual = doc_field_value(s, e->str1);
            int ok = (actual != INT_MIN) && cmp_apply(actual, e->op, e->i1);
            if (e->str1[0] && strieq(e->str1, "active_tool")) {
                slog("assert: active_tool %s %s   (actual=%s)%s",
                     op_str(e->op), tool_kind_str(e->i1),
                     tool_kind_str(actual), ok ? "" : "   FAIL");
            } else {
                slog("assert: %s %s %d   (actual=%d)%s",
                     e->str1, op_str(e->op), e->i1, actual, ok ? "" : "   FAIL");
            }
            if (!ok) ++s->assert_failures;
            break;
        }

        case EV_SHOT:
        case EV_END:
        case EV_NONE:
            break;       /* handled elsewhere */
    }
}

/* ---- Driver ------------------------------------------------------- */

int editor_shotmode_run(const char *script_path) {
    ScriptHeader hdr;
    ShotEvent   *events = NULL;
    if (parse_script(script_path, &hdr, &events) != 0) {
        if (events) arrfree(events);
        return 2;
    }
    int n_events = (int)arrlen(events);

    mkdirs_p(hdr.out_dir);

    /* Open log next to the shots. */
    char log_path[512];
    {
        const char *bn = script_basename(script_path);
        char base[128];
        snprintf(base, sizeof base, "%s", bn);
        char *dot = strrchr(base, '.');
        if (dot) *dot = 0;
        snprintf(log_path, sizeof log_path, "%s/%s.log", hdr.out_dir, base);
    }
    g_log_fp = fopen(log_path, "w");
    if (!g_log_fp) {
        fprintf(stderr, "shotmode: cannot open log: %s\n", log_path);
    }
    slog("shotmode: %s -> %s (%dx%d, %d events, max=%d ticks)",
         script_path, hdr.out_dir, hdr.window_w, hdr.window_h,
         n_events, hdr.max_ticks);

    /* Window + GL context. We use a visible window (raylib's HIDDEN
     * flag is platform-dependent and we don't need it on a developer
     * machine). MSAA matches the interactive editor.
     *
     * M6 P03-hotfix — HIGHDPI dropped here too. See
     * tools/editor/main.c for the rationale; shotmode runs the editor
     * at fixed window sizes for paired-screenshot regression tests,
     * so HIGHDPI added nothing here and broke real users with
     * fractional-DPI Windows. */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(hdr.window_w, hdr.window_h, "soldut_editor — shot");
    if (!IsWindowReady()) {
        slog("shotmode: InitWindow failed");
        if (g_log_fp) fclose(g_log_fp);
        arrfree(events);
        return 3;
    }
    SetTargetFPS(0);                /* don't throttle the runner */
    Font def = GetFontDefault();
    if (def.texture.id != 0) {
        SetTextureFilter(def.texture, TEXTURE_FILTER_BILINEAR);
    }

    ShotState s = {0};
    doc_init(&s.doc);
    if (hdr.initial_load[0]) {
        if (!doc_load(&s.doc, hdr.initial_load)) {
            slog("shotmode: initial load failed: %s", hdr.initial_load);
            doc_new(&s.doc, hdr.new_w ? hdr.new_w : EDITOR_DEFAULT_W,
                            hdr.new_h ? hdr.new_h : EDITOR_DEFAULT_H);
        }
    } else {
        doc_new(&s.doc,
                hdr.new_w ? hdr.new_w : EDITOR_DEFAULT_W,
                hdr.new_h ? hdr.new_h : EDITOR_DEFAULT_H);
    }
    view_init(&s.view, &s.doc);
    undo_init(&s.undo);
    tool_ctx_init(&s.ctx);
    s.active_tool = TOOL_TILE;
    s.cursor_screen = (Vector2){0, 0};
    s.draw_panels = hdr.draw_panels;

    /* Track every emitted shot path so we can build a contact sheet. */
    char **shot_paths = NULL;

    bool ended = false;
    for (int t = 0; t <= hdr.max_ticks && !ended; ++t) {
        /* Apply non-shot events first; SHOT / END are processed after
         * the render pass so the shot captures the post-event frame. */
        const ShotEvent *shot_this_tick = NULL;
        bool end_this_tick = false;
        for (int i = 0; i < n_events; ++i) {
            const ShotEvent *e = &events[i];
            if (e->tick != t) continue;
            if (e->kind == EV_SHOT) { shot_this_tick = e; continue; }
            if (e->kind == EV_END)  { end_this_tick = true; continue; }
            apply_event(&s, e);
        }

        /* Anchor the cursor for overlay rendering. raylib's poll runs
         * at EndDrawing so we re-pin every frame. */
        SetMousePosition((int)s.cursor_screen.x, (int)s.cursor_screen.y);

        UIDims D = ui_compute_dims(GetScreenHeight());
        GuiSetStyle(DEFAULT, TEXT_SIZE,    D.font_base);
        GuiSetStyle(DEFAULT, TEXT_SPACING, 1);

        BeginDrawing();
        ClearBackground(COL_BG);
        BeginMode2D(s.view.cam);
        draw_doc(&s.doc);
        view_draw_grid(&s.view, &s.doc);
        const ToolVTable *vt = &tool_vtables()[s.active_tool];
        if (vt->draw_overlay) vt->draw_overlay(&s.doc, &s.ctx, &s.view);
        EndMode2D();

        if (s.draw_panels) {
            ui_draw_top_bar(&s.doc, &D);
            ToolKind tk = (ToolKind)s.active_tool;
            (void)ui_draw_tool_buttons(&tk, &D);
            switch (s.active_tool) {
                case TOOL_TILE:   ui_draw_tile_palette  (&s.ctx, &D); break;
                case TOOL_POLY:   ui_draw_poly_palette  (&s.ctx, &D); break;
                case TOOL_PICKUP: ui_draw_pickup_palette(&s.ctx, &D); break;
                case TOOL_SPAWN:  ui_draw_spawn_palette (&s.ctx, &D); break;
                case TOOL_AMBI:   ui_draw_ambi_palette  (&s.ctx, &D); break;
                case TOOL_DECO:
                case TOOL_META:
                default:          ui_draw_empty_palette (&D); break;
            }
            ui_draw_status_bar(&s.doc, &s.view, &D, (ToolKind)s.active_tool);
        } else {
            render_summary_overlay(&s.doc, s.active_tool, &D);
        }

        if (s.help.open)         ui_help_modal_draw   (&s.help, &D);
        else if (s.meta.open)    ui_meta_modal_draw   (&s.meta, &s.doc, &D);
        else if (s.loadout.open) ui_loadout_modal_draw(&s.loadout, &s.test_lo, &D);

        EndDrawing();

        if (shot_this_tick) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s.png",
                     hdr.out_dir, shot_this_tick->str1);
            Image img = LoadImageFromScreen();
            ExportImage(img, path);
            UnloadImage(img);
            char *dup = (char *)malloc(strlen(path) + 1);
            if (dup) { strcpy(dup, path); arrput(shot_paths, dup); }
            slog("shot t=%d: %s", t, path);
        }
        if (end_this_tick) ended = true;
    }

    if (hdr.contact_sheet[0] && arrlen(shot_paths) > 0) {
        int cw = hdr.contact_cell_w > 0 ? hdr.contact_cell_w : 480;
        int ch = hdr.contact_cell_h > 0 ? hdr.contact_cell_h : 300;
        int cc = hdr.contact_cols    > 0 ? hdr.contact_cols    : 3;
        compose_contact_sheet(hdr.out_dir, hdr.contact_sheet,
                              shot_paths, (int)arrlen(shot_paths),
                              cw, ch, cc);
    }

    slog("shotmode: done. assertion failures = %d", s.assert_failures);
    int rc = (s.assert_failures > 0) ? 1 : 0;

    /* Cleanup. */
    for (int i = 0; i < (int)arrlen(shot_paths); ++i) free(shot_paths[i]);
    arrfree(shot_paths);
    arrfree(events);
    doc_free(&s.doc);
    undo_clear(&s.undo);
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
    CloseWindow();
    return rc;
}
