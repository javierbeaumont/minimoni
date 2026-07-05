/*
 * minimoni - zero-dependency system monitoring
 * Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MINIMONI_JSON_H
#define MINIMONI_JSON_H

#include "config.h"
#include "db.h"

/* --- JSON output writer --- */

/* A bounded JSON-object string builder: fields are appended into a fixed,
 * caller-provided buffer and silently truncated if it fills. Output only;
 * there is no parser here. */
typedef struct {
    char  *buf;
    size_t pos;
    size_t cap;
    int    comma; /* 1 after the first field; prepend ',' to next */
} jbuf_t;

void jbuf_init(jbuf_t *j, char *buf, size_t cap);
void jbuf_raw(jbuf_t *j, const char *s);
void jbuf_sep(jbuf_t *j);
void jbuf_begin(jbuf_t *j);
void jbuf_end(jbuf_t *j);
void jbuf_str(jbuf_t *j, const char *key, const char *val);
void jbuf_real(jbuf_t *j, const char *key, double val);
void jbuf_long(jbuf_t *j, const char *key, long val);
void jbuf_null(jbuf_t *j, const char *key);
void jbuf_pair(jbuf_t *j, const char *key, double warn, double crit);

/* --- Serializers --- */

/* Serialize the latest snapshot (for /api/current and /stream) into `j`,
 * applying the configured display units and emitting per-card status
 * thresholds. The hardware context (core count, sysfs critical trip point) is
 * passed in explicitly so this layer needs no dependency on the HTTP server. */
void json_serialize_current(jbuf_t *j, const db_row_t *r, const config_t *cfg, int num_cores,
                            int temp_critical_valid, double temp_critical);

/* Serialize one history point (for /api/metrics) into `j` using the configured
 * chart units and short keys. Only the temperature series is gated server-side
 * (it may lack a sensor); other charts are shown or hidden by the dashboard. */
void json_serialize_point(jbuf_t *j, const db_row_t *r, const config_t *cfg, int num_cores,
                          int temp_critical_valid, double temp_critical);

#endif /* MINIMONI_JSON_H */
