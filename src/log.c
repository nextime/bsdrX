/*
 * bsdrX — Bigscreen Remote Desktop agent.
 * Copyright (C) 2026 Stefy Lanza <stefy@nexlab.net>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "bsdr/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#ifdef __ANDROID__
#  include <android/log.h>   /* app stderr is discarded on Android -> mirror to logcat */
#endif

static bsdr_log_level g_level = BSDR_LOG_INFO;

static const char *level_name(bsdr_log_level l) {
    switch (l) {
        case BSDR_LOG_DEBUG: return "DEBUG";
        case BSDR_LOG_INFO:  return "INFO";
        case BSDR_LOG_WARN:  return "WARN";
        case BSDR_LOG_ERROR: return "ERROR";
        default:             return "?";
    }
}

void bsdr_log_set_level(bsdr_log_level level) { g_level = level; }

void bsdr_log(bsdr_log_level level, const char *tag, const char *fmt, ...) {
    if (level < g_level) return;

    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);

    fprintf(stderr, "%s %-14s %-5s ", ts, tag ? tag : "", level_name(level));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);

#ifdef __ANDROID__
    int prio = level == BSDR_LOG_DEBUG ? ANDROID_LOG_DEBUG :
               level == BSDR_LOG_WARN  ? ANDROID_LOG_WARN  :
               level == BSDR_LOG_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO;
    char msg[1024];
    va_list ap2;
    va_start(ap2, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap2);
    va_end(ap2);
    __android_log_print(prio, tag && *tag ? tag : "bsdr", "%s", msg);
#endif
}
