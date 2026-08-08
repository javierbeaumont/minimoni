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

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "civetweb.h"
#include "config.h"
#include "db.h"
#include "downsample.h"
#include "embed.h"
#include "http.h"
#include "json.h"
#include "metrics.h"

/* =========================================================================
 * CPU count  (for load normalisation when cpu_load_unit = "%")
 * ======================================================================= */

static int read_num_cores(void)
{
    FILE *f = fopen("/sys/devices/system/cpu/online", "r");
    if (!f)
        return 1;
    char  buf[64];
    char *got = fgets(buf, sizeof(buf), f);
    fclose(f);
    if (!got)
        return 1;

    char         *end;
    unsigned long lo = strtoul(buf, &end, 10);
    if (end == buf)
        return 1;

    int n = 1;
    if (*end == '-') {
        char         *end2;
        unsigned long hi = strtoul(end + 1, &end2, 10);
        if (end2 != end + 1 && hi >= lo)
            n = (int)(hi - lo + 1);
    }
    return (n > 0) ? n : 1;
}

static void read_temp_critical(http_ctx_t *ctx)
{
    ctx->temp_critical_valid = 0;
    for (int i = 0; i < 16; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone0/trip_point_%d_type", i);
        FILE *f = fopen(path, "r");
        if (!f)
            break;
        char type[32] = {0};
        fgets(type, sizeof(type), f);
        fclose(f);
        /* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) strcspn stays within type[32] */
        type[strcspn(type, "\n")] = '\0';
        if (strcmp(type, "critical") != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone0/trip_point_%d_temp", i);
        f = fopen(path, "r");
        if (!f)
            break;
        char  buf[32];
        char *end = buf;
        long  md = fgets(buf, sizeof(buf), f) ? strtol(buf, &end, 10) : 0;
        fclose(f);
        if (end != buf && md > 0) {
            ctx->temp_critical = (double)md / 1000.0;
            ctx->temp_critical_valid = 1;
        }
        break;
    }
}

/* =========================================================================
 * Request handlers
 * ======================================================================= */

static int handler_root(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    mg_send_http_ok(conn, "text/html; charset=utf-8", (long long)dashboard_index_html_len);
    mg_write(conn, dashboard_index_html, dashboard_index_html_len);
    return 200;
}

static int handler_health(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    static const char body[] = "{\"status\":\"ok\",\"version\":\"" MINIMONI_VERSION "\"}";
    mg_send_http_ok(conn, "application/json", sizeof(body) - 1);
    mg_write(conn, body, sizeof(body) - 1);
    return 200;
}

/* /api/current: latest snapshot with unit conversions applied */
static int handler_current(struct mg_connection *conn, void *cbdata)
{
    const http_ctx_t *ctx = (const http_ctx_t *)cbdata;
    db_row_t          row;
    int               ret = db_current(ctx->db, &row);
    if (ret == 1) {
        static const char e[] = "{\"error\":\"no data collected yet\"}";
        mg_printf(conn,
                  "HTTP/1.1 503 Service Unavailable\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %zu\r\n\r\n%s",
                  sizeof(e) - 1, e);
        return 503;
    }
    if (ret < 0) {
        static const char e[] = "{\"error\":\"database error\"}";
        mg_printf(conn,
                  "HTTP/1.1 500 Internal Server Error\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %zu\r\n\r\n%s",
                  sizeof(e) - 1, e);
        return 500;
    }

    char   buf[4096];
    jbuf_t j;
    jbuf_init(&j, buf, sizeof(buf));
    jbuf_begin(&j);
    json_serialize_current(&j, &row, ctx->cfg, ctx->num_cores, ctx->temp_critical_valid,
                           ctx->temp_critical, ctx->net_speed_mbit);
    jbuf_end(&j);

    mg_send_http_ok(conn, "application/json", (long long)j.pos);
    mg_write(conn, buf, j.pos);
    return 200;
}

/* /api/metrics?range=<range>: time-series with short keys */
static int handler_metrics(struct mg_connection *conn, void *cbdata)
{
    const http_ctx_t             *ctx = (const http_ctx_t *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    char                          range_str[16] = "";
    if (ri->query_string)
        mg_get_var(ri->query_string, strlen(ri->query_string), "range", range_str,
                   sizeof(range_str));

    /* Validate range against configured ranges */
    int range_ok = 0;
    for (int i = 0; i < ctx->cfg->range_count; i++) {
        if (strcmp(range_str, ctx->cfg->ranges[i]) == 0) {
            range_ok = 1;
            break;
        }
    }
    if (!range_ok) {
        /* default to first configured range */
        if (ctx->cfg->range_count > 0)
            snprintf(range_str, sizeof(range_str), "%s", ctx->cfg->ranges[0]);
        else
            snprintf(range_str, sizeof(range_str), "%s", "1d");
    }

    /* Convert range string to seconds */
    char *end;
    long  n = strtol(range_str, &end, 10);
    long  rsec = 0;
    if (end && n > 0) {
        if (*end == 'h')
            rsec = n * 3600L;
        else if (*end == 'd')
            rsec = n * 86400L;
        else if (*end == 'm')
            rsec = n * 60L;
    }
    if (rsec <= 0)
        rsec = 86400L;

    /* Optional points hint from the client. The dashboard JS chooses how many
     * data points it can render and passes it here. The server caps at 1440
     * (one point per minute over a 24h window) to bound the response size.
     * Values <=0 or missing fall back to the pick_bucket default. */
    int  points = 0;
    char points_str[16] = "";
    if (ri->query_string)
        mg_get_var(ri->query_string, strlen(ri->query_string), "points", points_str,
                   sizeof(points_str));
    if (points_str[0]) {
        long p = strtol(points_str, NULL, 10);
        if (p > 1440)
            p = 1440;
        if (p > 0)
            points = (int)p;
    }

    int actual_count = db_count_range(ctx->db, rsec);
    int bucket = pick_bucket(rsec, (int)ctx->cfg->interval_seconds, points, actual_count);

    db_row_t *rows = NULL;
    int       cnt = db_query_range(ctx->db, rsec, bucket, &rows);
    if (cnt < 0) {
        static const char e[] = "{\"error\":\"database error\"}";
        mg_printf(conn,
                  "HTTP/1.1 500 Internal Server Error\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %zu\r\n\r\n%s",
                  sizeof(e) - 1, e);
        return 500;
    }

    /* Stream response without buffering the full body. */
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/json\r\n"
              "Connection: close\r\n\r\n"
              "{\"range\":\"%s\",\"points\":[",
              range_str);

    char pt[512];
    for (int i = 0; i < cnt; i++) {
        jbuf_t j;
        jbuf_init(&j, pt, sizeof(pt));
        jbuf_begin(&j);
        json_serialize_point(&j, &rows[i], ctx->cfg, ctx->num_cores, ctx->temp_critical_valid,
                             ctx->temp_critical, ctx->net_speed_mbit);
        jbuf_end(&j);

        if (i > 0)
            mg_write(conn, ",", 1);
        mg_write(conn, pt, j.pos);
    }

    char   tail[256];
    jbuf_t u;
    jbuf_init(&u, tail, sizeof(tail));
    u.comma = 0;
    json_serialize_units(&u, rows, cnt, ctx->cfg);

    free(rows);
    db_release_memory(ctx->db);
    mg_write(conn, "],", 2);
    mg_write(conn, tail, u.pos);
    mg_write(conn, "}", 1);
    return 200;
}

/* /stream: SSE endpoint; blocks until the client disconnects or the server
 * is stopping.  Pushes a current snapshot every cfg->refresh_seconds. */
static int handler_stream(struct mg_connection *conn, void *cbdata)
{
    const http_ctx_t *ctx = (const http_ctx_t *)cbdata;
    mg_printf(conn, "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: keep-alive\r\n\r\n");

    for (;;) {
        db_row_t row;
        if (db_current(ctx->db, &row) == 0) {
            char   buf[4096];
            jbuf_t j;
            jbuf_init(&j, buf, sizeof(buf));
            jbuf_begin(&j);
            json_serialize_current(&j, &row, ctx->cfg, ctx->num_cores, ctx->temp_critical_valid,
                                   ctx->temp_critical, ctx->net_speed_mbit);
            jbuf_end(&j);

            int w = mg_printf(conn, "data: %.*s\n\n", (int)j.pos, buf);
            if (w <= 0)
                break; /* client disconnected */
        }

        /* Wait refresh_seconds in 1-second ticks. Send SSE keepalive comments
         * at the configured interval to detect client disconnection early.
         * Keepalive is inactive when sse_keepalive_seconds >= refresh_seconds. */
        struct timespec ts = {1, 0};
        int             ka = ctx->cfg->sse_keepalive_seconds;
        int             ka_active = ka > 0 && ka < ctx->cfg->refresh_seconds;
        for (int i = 1; i <= ctx->cfg->refresh_seconds && !ctx->stopping; i++) {
            nanosleep(&ts, NULL);
            if (ka_active && i % ka == 0) {
                if (mg_printf(conn, ": keepalive\n\n") <= 0)
                    return 200;
            }
        }

        if (ctx->stopping)
            break;
    }
    return 200;
}

/* =========================================================================
 * http_start / http_stop
 * ======================================================================= */

int http_start(http_ctx_t *ctx, const config_t *cfg, db_t *db)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = cfg;
    ctx->db = db;
    ctx->num_cores = read_num_cores();
    read_temp_critical(ctx);
    /* Host properties, read once at startup like the trip point above: a link
     * renegotiation (or a new cable) needs a restart to be picked up. */
    ctx->net_speed_mbit = metrics_link_speed_mbit();

    char threads_str[8];
    snprintf(threads_str, sizeof(threads_str), "%d", cfg->threads);
    const char *options[] = {"listening_ports",    cfg->listen, "num_threads", threads_str,
                             "request_timeout_ms", "30000",     NULL};

    struct mg_callbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    ctx->mg = mg_start(&cbs, NULL, options);
    if (!ctx->mg) {
        fprintf(stderr, "http: failed to bind on %s\n", cfg->listen);
        return -1;
    }

    mg_set_request_handler(ctx->mg, "/$", handler_root, ctx);
    mg_set_request_handler(ctx->mg, "/stream$", handler_stream, ctx);
    mg_set_request_handler(ctx->mg, "/api/current$", handler_current, ctx);
    mg_set_request_handler(ctx->mg, "/api/metrics$", handler_metrics, ctx);
    mg_set_request_handler(ctx->mg, "/api/health$", handler_health, ctx);

    return 0;
}

void http_stop(http_ctx_t *ctx)
{
    ctx->stopping = 1;
    if (ctx->mg) {
        mg_stop(ctx->mg);
        ctx->mg = NULL;
    }
}
