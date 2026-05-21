/*
 * minimoni — zero-dependency system monitoring
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

#ifndef MINIMONI_HTTP_H
#define MINIMONI_HTTP_H

#include "config.h"
#include "db.h"

struct mg_context; /* forward declaration — avoid pulling in civetweb.h here */

typedef struct {
    struct mg_context *mg;
    const config_t    *cfg;
    db_t              *db;
    int                num_cores;     /* from /sys/devices/system/cpu/online */
    double             temp_critical; /* critical trip point in °C, 0 if absent */
    int                temp_critical_valid;
    volatile int       stopping;
} http_ctx_t;

/*
 * Bind to cfg->listen, register all route handlers, and start the civetweb
 * thread pool.  ctx must remain valid until http_stop() returns.
 * Returns 0 on success, -1 on failure (message written to stderr).
 */
int http_start(http_ctx_t *ctx, const config_t *cfg, db_t *db);

/* Signal all SSE connections to close, stop the HTTP server, and block
 * until the civetweb thread pool has exited. */
void http_stop(http_ctx_t *ctx);

#endif /* MINIMONI_HTTP_H */
