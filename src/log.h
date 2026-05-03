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
