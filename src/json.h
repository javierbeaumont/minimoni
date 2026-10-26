/*
 * minimoni: zero-dependency system monitoring
 *
 * Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
 *
 * This file is part of minimoni.
 *
 * minimoni is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * minimoni is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with minimoni. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MINIMONI_JSON_H
#define MINIMONI_JSON_H

#include "config.h"
#include "db.h"

/* --- JSON output writer --- */

/* A bounded JSON-object builder over a caller-provided buffer. What does not fit is dropped, a
 * field whole with its comma, and sets `overflow`: such a buffer must not be sent. Output only. */
typedef struct {
    char  *buf;
    size_t pos;
    size_t cap;
    int    comma; /* 1 after the first field; prepend ',' to next */
    int    overflow;
} jbuf_t;

/* /api/current and each /stream frame; the largest valid config must fit. */
#define JSON_CURRENT_CAP 4096

/* One /api/metrics point; the widest values of every chart must fit. */
#define JSON_POINT_CAP 512

/* The units at the end of /api/metrics; every magnitude at the top of its ladder must fit. */
#define JSON_UNITS_CAP 256

void jbuf_init(jbuf_t *j, char *buf, size_t cap);
void jbuf_raw(jbuf_t *j, const char *s);
void jbuf_sep(jbuf_t *j);
void jbuf_begin(jbuf_t *j);
void jbuf_end(jbuf_t *j);
void jbuf_str(jbuf_t *j, const char *key, const char *val);

/* Escape `src` as a JSON string body (no quotes) into `dst`, per RFC 8259.
 * Bytes >= 0x20 pass through, so valid UTF-8 in stays valid UTF-8 out. Returns
 * the length written, or -1 if it would not fit, leaving nothing usable in
 * `dst`: the caller drops the whole field. */
int json_escape(char *dst, size_t cap, const char *src);
/* "key":val with 4 significant digits. JSON has no NaN or Infinity, so a value that is not finite
 * is written as null. */
void jbuf_real(jbuf_t *j, const char *key, double val);
void jbuf_long(jbuf_t *j, const char *key, long val);
void jbuf_null(jbuf_t *j, const char *key);
void jbuf_pair(jbuf_t *j, const char *key, double warn, double crit);

/* Display units for one /api/metrics window, chosen from the largest value it
 * carries so a card and its chart always read in the same magnitude. */
void json_serialize_units(jbuf_t *j, const db_row_t *rows, int cnt, const config_t *cfg);

/* --- Serializers --- */

/* Serialize the latest snapshot (for /api/current and /stream) into `j`,
 * applying the configured display units and emitting per-card status
 * thresholds. The hardware context (core count, sysfs critical trip point) is
 * passed in explicitly so this layer needs no dependency on the HTTP server. */
void json_serialize_current(jbuf_t *j, const db_row_t *r, const config_t *cfg, int num_cores,
                            int temp_critical_valid, double temp_critical, int detected_speed_mbit);

/* Serialize one history point (for /api/metrics) into `j` using the configured chart units and
 * short keys; a chart that is not configured sends no fields. */
void json_serialize_point(jbuf_t *j, const db_row_t *r, const config_t *cfg, int num_cores,
                          int temp_critical_valid, double temp_critical, int detected_speed_mbit);

#endif /* MINIMONI_JSON_H */
