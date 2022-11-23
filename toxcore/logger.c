/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */

/**
 * Text logging abstraction.
 */
#include "logger.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "ccompat.h"

struct Logger {
    logger_cb *callback;
    void *context;
    void *userdata;
};

static const char *logger_level_name(Logger_Level level)
{
    switch (level) {
        case LOGGER_LEVEL_TRACE:
            return "TRACE";

        case LOGGER_LEVEL_DEBUG:
            return "DEBUG";

        case LOGGER_LEVEL_INFO:
            return "INFO";

        case LOGGER_LEVEL_WARNING:
            return "WARNING";

        case LOGGER_LEVEL_ERROR:
            return "ERROR";
    }

    return "<unknown>";
}

non_null(1, 3, 5, 6) nullable(7)
static void logger_stderr_handler(void *context, Logger_Level level, const char *file, int line, const char *func,
                                  const char *message, void *userdata);

non_null(1, 3, 5, 6) nullable(7)
static void logger_stderr_handler(void *context, Logger_Level level, const char *file, int line, const char *func,
                                  const char *message, void *userdata)
{
#if (defined(MUTEXLOCKINGDEBUG) || defined(USE_STDERR_LOGGER))
    // GL stands for "global logger".
    fprintf(stderr, "[GL] %s %s:%d(%s): %s\n", logger_level_name(level), file, line, func, message);
#else
#ifndef NDEBUG
    // GL stands for "global logger".
    fprintf(stderr, "[GL] %s %s:%d(%s): %s\n", logger_level_name(level), file, line, func, message);
    fprintf(stderr, "Default stderr logger triggered; aborting program\n");
    abort();
#endif
#endif
}

static const Logger logger_stderr = {
    logger_stderr_handler,
    nullptr,
    nullptr,
};

/*
 * Public Functions
 */

Logger *logger_new(void)
{
    return (Logger *)calloc(1, sizeof(Logger));
}

void logger_kill(Logger *log)
{
    free(log);
}

void logger_callback_log(Logger *log, logger_cb *function, void *context, void *userdata)
{
    log->callback = function;
    log->context  = context;
    log->userdata = userdata;
}

void logger_write(const Logger *log, Logger_Level level, const char *file, int line, const char *func,
                  const char *format, ...)
{
    if (log == nullptr) {
        log = &logger_stderr;
    }

    if (log->callback == nullptr) {
        return;
    }

    // Only pass the file name, not the entire file path, for privacy reasons.
    // The full path may contain PII of the person compiling toxcore (their
    // username and directory layout).
    const char *filename = strrchr(file, '/');
    file = filename != nullptr ? filename + 1 : file;
#if defined(_WIN32) || defined(__CYGWIN__)
    // On Windows, the path separator *may* be a backslash, so we look for that
    // one too.
    const char *windows_filename = strrchr(file, '\\');
    file = windows_filename != nullptr ? windows_filename + 1 : file;
#endif

    // Format message
    char msg[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    log->callback(log->context, level, file, line, func, msg, log->userdata);
}


void logger_api_write(const Logger *log, Logger_Level level, const char *file, int line, const char *func,
                      const char *format, va_list args)
{
    if (!log) {
#ifdef USE_STDERR_LOGGER
        log = &logger_stderr;
#else
        fprintf(stderr, "NULL logger not permitted.\n");
        abort();
#endif
    }

    if (!log->callback) {
        return;
    }

    // Only pass the file name, not the entire file path, for privacy reasons.
    // The full path may contain PII of the person compiling toxcore (their
    // username and directory layout).
    const char *filename = strrchr(file, '/');
    file = filename ? filename + 1 : file;
#if defined(_WIN32) || defined(__CYGWIN__)
    // On Windows, the path separator *may* be a backslash, so we look for that
    // one too.
    const char *windows_filename = strrchr(file, '\\');
    file = windows_filename ? windows_filename + 1 : file;
#endif

    // Format message
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);

    log->callback(log->context, level, file, line, func, msg, log->userdata);
}

/*
 * hook mutex function so we can nicely log them (to the NULL logger!)
 */
int my_pthread_mutex_lock(pthread_mutex_t *mutex, const char *mutex_name, const char *file, int line, const char *func)
{
    pthread_t cur_pthread_tid = pthread_self();
#if !(defined(_WIN32) || defined(__WIN32__) || defined(WIN32))
    logger_write(NULL, LOGGER_LEVEL_DEBUG, file, line, func, "TID:%d:MTX_LOCK:S:%s:m=%p", (int)cur_pthread_tid, mutex_name, (void*)mutex);
#else
    logger_write(NULL, LOGGER_LEVEL_DEBUG, file, line, func, "MTX_LOCK:S:%s:m=%p",
                 mutex_name, (void *)mutex);
#endif
    int ret = (pthread_mutex_lock)(mutex);
#if !(defined(_WIN32) || defined(__WIN32__) || defined(WIN32))
    logger_write(NULL, LOGGER_LEVEL_DEBUG, file, line, func, "TID:%d:MTX_LOCK:E:%s:m=%p", (int)cur_pthread_tid, mutex_name, (void*)mutex);
#else
    logger_write(NULL, LOGGER_LEVEL_DEBUG, file, line, func, "MTX_LOCK:E:%s:m=%p",
                 mutex_name, (void *)mutex);
#endif
    return ret;
}

int my_pthread_mutex_unlock(pthread_mutex_t *mutex, const char *mutex_name, const char *file, int line, const char *func)
{
    pthread_t cur_pthread_tid = pthread_self();
#if !(defined(_WIN32) || defined(__WIN32__) || defined(WIN32))
    logger_write(NULL, LOGGER_LEVEL_DEBUG, file, line, func, "TID:%d:MTX_unLOCK:S:%s:m=%p", (int)cur_pthread_tid, mutex_name, (void*)mutex);
#else
    logger_write(NULL, LOGGER_LEVEL_DEBUG, file, line, func, "MTX_unLOCK:S:%s:m=%p",
                 mutex_name, (void *)mutex);
#endif
    int ret = (pthread_mutex_unlock)(mutex);
#if !(defined(_WIN32) || defined(__WIN32__) || defined(WIN32))
    logger_write(NULL, LOGGER_LEVEL_DEBUG, file, line, func, "TID:%d:MTX_unLOCK:E:%s:m=%p", (int)cur_pthread_tid, mutex_name, (void*)mutex);
#else
    logger_write(NULL, LOGGER_LEVEL_DEBUG, file, line, func, "MTX_unLOCK:E:%s:m=%p",
                 mutex_name, (void *)mutex);
#endif
    return ret;
}
