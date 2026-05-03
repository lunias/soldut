#pragma once

#include <stdarg.h>

/* Note the SLOG_ prefix: raylib defines LOG_DEBUG / LOG_INFO / LOG_ERROR
 * as TraceLogLevel enum values, so we cannot reuse those names in any
 * TU that pulls in both raylib.h and log.h. The user-facing macros
 * (LOG_D / LOG_I / LOG_W / LOG_E) are unaffected. */
typedef enum {
    SLOG_DEBUG = 0,
    SLOG_INFO,
    SLOG_WARN,
    SLOG_ERROR,
} LogLevel;

void log_init(const char *file_path);
void log_shutdown(void);

void log_msg(LogLevel level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void log_vmsg(LogLevel level, const char *fmt, va_list ap);

#define LOG_D(...) log_msg(SLOG_DEBUG, __VA_ARGS__)
#define LOG_I(...) log_msg(SLOG_INFO,  __VA_ARGS__)
#define LOG_W(...) log_msg(SLOG_WARN,  __VA_ARGS__)
#define LOG_E(...) log_msg(SLOG_ERROR, __VA_ARGS__)

/* Shot-mode diagnostic logging.
 *
 * Set to 1 by shotmode_run before world build, cleared on exit. The
 * SHOT_LOG macro is a no-op (just a load + branch) when off, so
 * sprinkling these into hot paths costs nothing in normal play.
 *
 * Output goes to the log file only (not stdout), so SHOT_LOG can be
 * verbose without drowning the terminal. shotmode_run points the log
 * file at build/shots/<scriptname>.log so each run produces a single
 * text file paired with its PNGs — the intended audience is an LLM
 * reviewing a shot run by reading the log + the contact sheet. */
extern int g_shot_mode;

void log_shot(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define SHOT_LOG(...) do { if (g_shot_mode) log_shot(__VA_ARGS__); } while (0)
