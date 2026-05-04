#include "files.h"

#include "raylib.h"
#include "raygui.h"

#include <stdio.h>
#include <string.h>

void files_open_dialog(FilesDialog *fd, const char *initial_path) {
    fd->mode = FILES_MODE_OPEN;
    snprintf(fd->title, sizeof fd->title, "Open .lvl");
    if (initial_path && initial_path[0]) {
        snprintf(fd->input, sizeof fd->input, "%s", initial_path);
    } else {
        snprintf(fd->input, sizeof fd->input, "assets/maps/scratch.lvl");
    }
    fd->message[0] = 0;
    fd->result_pending = false;
    fd->result_ok = false;
}

void files_save_dialog(FilesDialog *fd, const char *initial_path) {
    fd->mode = FILES_MODE_SAVE;
    snprintf(fd->title, sizeof fd->title, "Save .lvl as...");
    if (initial_path && initial_path[0]) {
        snprintf(fd->input, sizeof fd->input, "%s", initial_path);
    } else {
        snprintf(fd->input, sizeof fd->input, "assets/maps/scratch.lvl");
    }
    fd->message[0] = 0;
    fd->result_pending = false;
    fd->result_ok = false;
}

void files_message(FilesDialog *fd, const char *title, const char *message) {
    fd->mode = FILES_MODE_MESSAGE;
    snprintf(fd->title,   sizeof fd->title,   "%s", title ? title : "");
    snprintf(fd->message, sizeof fd->message, "%s", message ? message : "");
    fd->input[0] = 0;
    fd->result_pending = false;
    fd->result_ok = false;
}

void files_dialog_close(FilesDialog *fd) {
    fd->mode = FILES_MODE_NONE;
    fd->result_pending = false;
}

/* Mirror of main.c::editor_scale so files.c can size itself
 * consistently without taking a shared header dependency. */
static float scale_for(int sh) {
    if (sh <= 0) return 1.0f;
    float raw = (float)sh / 720.0f;
    if (raw < 1.0f) raw = 1.0f;
    if (raw > 3.0f) raw = 3.0f;
    float snap = (float)((int)((raw * 4.0f) + 0.5f)) / 4.0f;
    if (snap < 1.0f) snap = 1.0f;
    return snap;
}
static int scl(int v, float s) { return (int)((float)v * s + 0.5f); }

void files_dialog_draw(FilesDialog *fd, int sw, int sh) {
    if (fd->mode == FILES_MODE_NONE) return;

    float s = scale_for(sh);
    int pad      = scl(8, s);
    int row_h    = scl(32, s);
    int font_sz  = scl(14, s);
    int title_h  = scl(16, s) + pad;
    int btn_w    = scl(110, s);
    int line_h   = font_sz + pad / 2;

    /* Dim the background. */
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 160});

    int dlg_w = (fd->mode == FILES_MODE_MESSAGE) ? scl(680, s) : scl(560, s);
    int dlg_h = (fd->mode == FILES_MODE_MESSAGE) ? scl(380, s) : scl(220, s);
    if (dlg_w > sw - pad * 4) dlg_w = sw - pad * 4;
    if (dlg_h > sh - pad * 4) dlg_h = sh - pad * 4;
    int dlg_x = (sw - dlg_w) / 2;
    int dlg_y = (sh - dlg_h) / 2;

    /* Custom panel + title bar. raygui's GuiPanel title strip is too
     * cramped at 4K. */
    Color col_panel    = (Color){ 28,  34,  44, 255};
    Color col_panel_2  = (Color){ 36,  44,  56, 255};
    Color col_text     = (Color){240, 244, 250, 255};
    Color col_text_dim = (Color){190, 200, 215, 255};
    Color col_text_hi  = (Color){255, 240, 180, 255};
    Color col_border   = (Color){ 60,  70,  85, 255};

    DrawRectangle(dlg_x, dlg_y, dlg_w, dlg_h, col_panel);
    DrawRectangleLinesEx((Rectangle){(float)dlg_x, (float)dlg_y,
                                     (float)dlg_w, (float)dlg_h}, 2.0f, col_border);
    DrawRectangle(dlg_x, dlg_y, dlg_w, title_h, col_panel_2);
    DrawRectangle(dlg_x, dlg_y + title_h, dlg_w, 1, col_border);
    DrawTextEx(GetFontDefault(), fd->title,
               (Vector2){(float)(dlg_x + pad * 2), (float)(dlg_y + pad / 2)},
               (float)font_sz, (float)font_sz / 10.0f, col_text_hi);

    if (fd->mode == FILES_MODE_MESSAGE) {
        /* Wrap the message into the panel: split on '\n', draw per-line.
         * Caps at the dialog's body height. */
        int body_y0 = dlg_y + title_h + pad;
        int body_y1 = dlg_y + dlg_h - row_h - pad * 2;
        const char *p = fd->message;
        int y = body_y0;
        while (*p && y < body_y1) {
            const char *eol = strchr(p, '\n');
            int len = eol ? (int)(eol - p) : (int)strlen(p);
            char buf[1024];
            int n = (len < (int)sizeof buf - 1) ? len : (int)sizeof buf - 1;
            memcpy(buf, p, (size_t)n); buf[n] = 0;
            DrawTextEx(GetFontDefault(), buf,
                       (Vector2){(float)(dlg_x + pad * 2), (float)y},
                       (float)font_sz, (float)font_sz / 10.0f,
                       (buf[0]) ? col_text : col_text_dim);
            y += line_h;
            p = eol ? eol + 1 : p + len;
        }

        Rectangle ok = {(float)(dlg_x + dlg_w - btn_w - pad * 2),
                        (float)(dlg_y + dlg_h - row_h - pad),
                        (float)btn_w, (float)row_h};
        if (GuiButton(ok, "OK") || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
            fd->result_pending = true;
            fd->result_ok = true;
        }
        return;
    }

    /* Open / save: a path text field + OK / Cancel. */
    int body_y = dlg_y + title_h + pad * 2;
    DrawTextEx(GetFontDefault(), "Path:",
               (Vector2){(float)(dlg_x + pad * 2), (float)body_y + (row_h - font_sz) / 2.0f},
               (float)font_sz, (float)font_sz / 10.0f, col_text);
    int label_w = scl(60, s);
    Rectangle tb = {(float)(dlg_x + pad * 2 + label_w),
                    (float)body_y,
                    (float)(dlg_w - pad * 4 - label_w),
                    (float)row_h};
    static bool edit_mode = true;
    if (GuiTextBox(tb, fd->input, (int)sizeof fd->input, edit_mode)) {
        edit_mode = !edit_mode;
    }

    Rectangle btn_ok = {(float)(dlg_x + dlg_w - btn_w - pad * 2),
                        (float)(dlg_y + dlg_h - row_h - pad),
                        (float)btn_w, (float)row_h};
    Rectangle btn_cn = {(float)(dlg_x + dlg_w - btn_w * 2 - pad * 3),
                        (float)(dlg_y + dlg_h - row_h - pad),
                        (float)btn_w, (float)row_h};
    bool ok_clicked = GuiButton(btn_ok, fd->mode == FILES_MODE_SAVE ? "Save" : "Open");
    bool cn_clicked = GuiButton(btn_cn, "Cancel");
    if (IsKeyPressed(KEY_ENTER))  ok_clicked = true;
    if (IsKeyPressed(KEY_ESCAPE)) cn_clicked = true;

    if (ok_clicked) {
        fd->result_pending = true;
        fd->result_ok = true;
    } else if (cn_clicked) {
        fd->result_pending = true;
        fd->result_ok = false;
    }
}
