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
#include "editor_ui.h"
#include "files.h"
#include "log.h"
#include "palette.h"
#include "play.h"
#include "render.h"
#include "shotmode.h"
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

/* UI scale, color set, panels, modals all live in editor_ui.{c,h}.
 * main.c just orchestrates the per-frame loop. */


/* ---- Universal verb dispatch ------------------------------------- */

static bool ctrl_down(void) {
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}
static bool shift_down(void) {
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
}

/* ---- Main --------------------------------------------------------- */

int main(int argc, char **argv) {
    /* Shot-mode early-exit: drives the editor through a scripted set
     * of doc / tool / undo operations, captures PNGs at marked ticks,
     * runs assertions, exits non-zero on any failure. The whole point
     * is to let me iterate on the editor without round-tripping
     * screenshots or asking the user to drive the GUI. */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            log_init("soldut_editor.log");
            int rc = editor_shotmode_run(argv[i + 1]);
            log_shutdown();
            return rc;
        }
    }

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
        UIDims D = ui_compute_dims(GetScreenHeight());
        /* raygui: scale the font + control padding to match the rest
         * of the UI. */
        GuiSetStyle(DEFAULT, TEXT_SIZE,    D.font_base);
        GuiSetStyle(DEFAULT, TEXT_SPACING, 1);

        /* ---- Modal toggles processed BEFORE rendering ---------------
         * EndDrawing() calls PollInputEvents at its tail, which shifts
         * IsKeyPressed's edge state forward. Checking IsKeyPressed
         * AFTER EndDrawing reads the state for the *next* frame's
         * inputs, so a "press H to close" check that lives after
         * EndDrawing always misses the press that was meant to close.
         *
         * Process H + Esc here, at the very top of the frame, so the
         * input state is the same one the universal verb section
         * reads (open-on-H worked because it was already up here). */
        if (!files_dialog_active(&files) && !ctrl_down()) {
            if (IsKeyPressed(KEY_H)) ui_help_toggle(&help);
            if (help.open && IsKeyPressed(KEY_ESCAPE)) ui_help_close(&help);
            if (meta.open && IsKeyPressed(KEY_ESCAPE)) meta.open = false;
        }

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
            ui_help_modal_draw(&help, &D);
            EndDrawing();
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
            ui_meta_modal_draw(&meta, &doc, &D);
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
                ui_meta_open(&meta, &doc);
            }
            /* H is handled at the top of the frame via ui_help_toggle
             * — see the modal-toggles block above. */
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

        ui_draw_top_bar(&doc, &D);
        /* Always open the meta modal when the user CLICKS the Meta
         * button, even if active_tool was already TOOL_META — the
         * earlier "if (picked != active_tool)" gate meant the second
         * click silently no-op'd. ui_draw_tool_buttons now reports
         * the actual click separately from the state update. */
        int clicked = ui_draw_tool_buttons(&active_tool, &D);
        if (clicked == TOOL_META) {
            ui_meta_open(&meta, &doc);
        }
        switch (active_tool) {
            case TOOL_TILE:   ui_draw_tile_palette(&ctx, &D); break;
            case TOOL_POLY:   ui_draw_poly_palette(&ctx, &D); break;
            case TOOL_PICKUP: ui_draw_pickup_palette(&ctx, &D); break;
            case TOOL_SPAWN:  ui_draw_spawn_palette(&ctx, &D); break;
            case TOOL_AMBI:   ui_draw_ambi_palette(&ctx, &D); break;
            case TOOL_DECO:
            case TOOL_META:
            default:          ui_draw_empty_palette(&D); break;
        }
        ui_draw_status_bar(&doc, &view, &D, active_tool);
        EndDrawing();
    }

    doc_free(&doc);
    undo_clear(&undo);
    log_shutdown();
    CloseWindow();
    return 0;
}
