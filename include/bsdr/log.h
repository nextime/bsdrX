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
/* Tiny leveled logger to stderr. */
#ifndef BSDR_LOG_H
#define BSDR_LOG_H

typedef enum {
    BSDR_LOG_DEBUG = 0,
    BSDR_LOG_INFO,
    BSDR_LOG_WARN,
    BSDR_LOG_ERROR
} bsdr_log_level;

void bsdr_log_set_level(bsdr_log_level level);
void bsdr_log(bsdr_log_level level, const char *tag, const char *fmt, ...);

#define BSDR_DEBUG(tag, ...) bsdr_log(BSDR_LOG_DEBUG, tag, __VA_ARGS__)
#define BSDR_INFO(tag, ...)  bsdr_log(BSDR_LOG_INFO,  tag, __VA_ARGS__)
#define BSDR_WARN(tag, ...)  bsdr_log(BSDR_LOG_WARN,  tag, __VA_ARGS__)
#define BSDR_ERROR(tag, ...) bsdr_log(BSDR_LOG_ERROR, tag, __VA_ARGS__)

#endif /* BSDR_LOG_H */
