/* Asks glibc to expose POSIX-2008 declarations (localtime_r, etc.). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *g_log_file;

static const char *level_tag(LogLevel l) {
    switch (l) {
        case SLOG_DEBUG: return "DEBUG";
        case SLOG_INFO:  return "INFO ";
        case SLOG_WARN:  return "WARN ";
        case SLOG_ERROR: return "ERROR";
    }
    return "?????";
}

void log_init(const char *file_path) {
    if (g_log_file) return;
    if (file_path && *file_path) {
        g_log_file = fopen(file_path, "w");
        /* If we can't open it, stderr will still work. */
    }
}

void log_shutdown(void) {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void log_vmsg(LogLevel level, const char *fmt, va_list ap) {
    char timestamp[32];
    time_t t = time(NULL);
    struct tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tm);

    /* Print twice: once to the appropriate stream, once to the log file. */
    FILE *stream = (level >= SLOG_WARN) ? stderr : stdout;
    va_list ap_copy;
    va_copy(ap_copy, ap);

    fprintf(stream, "[%s %s] ", timestamp, level_tag(level));
    vfprintf(stream, fmt, ap);
    fputc('\n', stream);

    if (g_log_file) {
        fprintf(g_log_file, "[%s %s] ", timestamp, level_tag(level));
        vfprintf(g_log_file, fmt, ap_copy);
        fputc('\n', g_log_file);
        fflush(g_log_file);
    }

    va_end(ap_copy);
}

void log_msg(LogLevel level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(level, fmt, ap);
    va_end(ap);
}
