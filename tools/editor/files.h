#pragma once

/*
 * File picker shim. We don't vendor tinyfiledialogs (3 kLOC of
 * cross-platform shell-out) — instead we draw a small raygui modal
 * with a text-input field and an OK / Cancel pair, and accept either
 * an absolute or working-directory-relative path.
 *
 * Trade-off entry: "Editor file picker is a raygui textbox, not a
 * native file dialog." Logged in TRADE_OFFS.md when P04 ships.
 */

#include <stdbool.h>

typedef enum {
    FILES_MODE_NONE = 0,
    FILES_MODE_OPEN,
    FILES_MODE_SAVE,
    FILES_MODE_MESSAGE,        /* read-only message box for validation errors */
} FilesMode;

typedef struct FilesDialog {
    FilesMode mode;
    char      input[512];
    char      title[64];
    char      message[1024];   /* multi-line; '\n'-separated */
    bool      result_pending;  /* true → set when user dismissed */
    bool      result_ok;       /* OK pressed (or Save chosen); else cancel */
} FilesDialog;

void files_open_dialog (FilesDialog *fd, const char *initial_path);
void files_save_dialog (FilesDialog *fd, const char *initial_path);
void files_message     (FilesDialog *fd, const char *title, const char *message);

/* Per-frame draw + input pump. Sets fd->result_pending when the user
 * clicks OK or Cancel. Caller checks result_ok / fd->input. */
void files_dialog_draw (FilesDialog *fd, int screen_w, int screen_h);

/* True if a dialog is currently active and consuming input. */
static inline bool files_dialog_active(const FilesDialog *fd) {
    return fd->mode != FILES_MODE_NONE;
}

void files_dialog_close(FilesDialog *fd);
