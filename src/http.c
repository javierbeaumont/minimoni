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

#define _POSIX_C_SOURCE 200809L

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
#include "embed.h"
#include "http.h"

/* =========================================================================
 * JSON buffer helpers
 * ======================================================================= */

typedef struct {
    char  *buf;
    size_t pos;
    size_t cap;
    int    comma; /* 1 after the first field — prepend ',' to next */
} jbuf_t;

static void jbuf_init(jbuf_t *j, char *buf, size_t cap)
{
    j->buf = buf;
    j->pos = 0;
    j->cap = cap;
    j->comma = 0;
    if (cap > 0)
        buf[0] = '\0';
}

static void jbuf_raw(jbuf_t *j, const char *s)
{
    size_t n = strlen(s);
    if (j->pos + n < j->cap) {
        memcpy(j->buf + j->pos, s, n);
        j->pos += n;
        j->buf[j->pos] = '\0';
    }
}

static void jbuf_sep(jbuf_t *j)
{
    if (j->comma)
        jbuf_raw(j, ",");
    j->comma = 1;
}

static void jbuf_begin(jbuf_t *j)
{
    jbuf_raw(j, "{");
    j->comma = 0;
}

static void jbuf_end(jbuf_t *j) { jbuf_raw(j, "}"); }

static void jbuf_str(jbuf_t *j, const char *key, const char *val)
{
    jbuf_sep(j);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "\"%s\":\"%s\"", key, val);
    jbuf_raw(j, tmp);
}

static int cfg_has(const char list[][16], int count, const char *name)
{
    if (count == 0)
        return 1; /* default: show all */
    if (count < 0)
        return 0; /* explicit empty list: hide all */
    for (int i = 0; i < count; i++)
        if (strcmp(list[i], name) == 0)
            return 1;
    return 0;
}

static void jbuf_real(jbuf_t *j, const char *key, double val)
{
    jbuf_sep(j);
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "\"%s\":%.4g", key, val);
    jbuf_raw(j, tmp);
}

static void jbuf_long(jbuf_t *j, const char *key, long val)
{
    jbuf_sep(j);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "\"%s\":%ld", key, val);
    jbuf_raw(j, tmp);
}

static void jbuf_null(jbuf_t *j, const char *key)
{
    jbuf_sep(j);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "\"%s\":null", key);
    jbuf_raw(j, tmp);
}

/* =========================================================================
 * Unit conversions  (raw → configured unit)
 * ======================================================================= */

static double net_convert(double bps, const char *unit)
{
    if (!unit || unit[0] == 'm') {
        if (unit && unit[1] == 'b' && unit[2] == 'p') /* mbps */
            return bps * 8.0 / 1e6;
        return bps / 1048576.0; /* mb */
    }
    if (unit[0] == 'g') {
        if (unit[1] == 'b' && unit[2] == 'p') /* gbps */
            return bps * 8.0 / 1e9;
        return bps / 1073741824.0; /* gb */
    }
    if (unit[0] == 'k') {
        if (unit[1] == 'b' && unit[2] == 'p') /* kbps */
            return bps * 8.0 / 1000.0;
        return bps / 1024.0; /* kb */
    }
    return bps / 1048576.0;
}

static double mem_convert(double mb, const char *unit)
{
    if (unit && unit[0] == 'g')
        return mb / 1024.0;
    return mb; /* mb (or % — caller uses mem_percent directly) */
}

static double disk_convert(double gb, const char *unit)
{
    if (unit && unit[0] == 't')
        return gb / 1024.0;
    return gb; /* gb (or % — caller uses disk_percent directly) */
}

static double temp_convert(double celsius, const char *unit, float temp_max)
{
    if (!unit)
        return celsius;
    if (unit[0] == 'f')
        return celsius * 9.0 / 5.0 + 32.0;
    if (unit[0] == '%')
        return (temp_max > 0) ? celsius * 100.0 / temp_max : celsius;
    return celsius;
}

static double load_convert(double load, int cores, const char *unit)
{
    if (unit && unit[0] == '%' && cores > 0)
        return load * 100.0 / (double)cores;
    return load;
}

/* =========================================================================
 * Bucket-snapping algorithm (DESIGN.md § Downsampling and retention)
 * ======================================================================= */

static const int BUCKETS[] = {60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400};
#define NBUCKETS ((int)(sizeof(BUCKETS) / sizeof(BUCKETS[0])))

/* Return the best bucket size in seconds, or 0 for raw (no aggregation).
 * Iterates ascending so ties naturally resolve to the smaller bucket. */
static int pick_bucket(long range_sec, int interval_sec, int points, int actual_count)
{
    /* Default 240 = enough to eyeball a trend without wasting work for
     * clients that didn't pass `points` (curl, custom dashboards). The
     * bundled dashboard computes its own value from canvas width × dpr
     * and almost always passes it. */
    if (points <= 0)
        points = 240;
    if (actual_count >= 0 && actual_count <= points)
        return 0; /* fewer rows than target — show raw for progressive resolution */
    long ideal = range_sec / (long)points;
    if (ideal <= interval_sec)
        return 0; /* raw */

    int  best = -1;
    long best_diff = LONG_MAX;
    for (int i = 0; i < NBUCKETS; i++) {
        int b = BUCKETS[i];
        if (b % interval_sec != 0)
            continue;
        long diff = labs(range_sec / b - (long)points);
        if (diff < best_diff) {
            best = i;
            best_diff = diff;
        }
    }
    return (best >= 0) ? BUCKETS[best] : interval_sec;
}

/* =========================================================================
 * CPU count  (for load normalisation when cpu_load_unit = "%")
 * ======================================================================= */

static int read_num_cores(void)
{
    FILE *f = fopen("/sys/devices/system/cpu/online", "r");
    if (!f)
        return 1;
    char     buf[64];
    unsigned lo = 0, hi = 0;
    int      n = 1;
    if (fgets(buf, sizeof(buf), f)) {
        if (sscanf(buf, "%u-%u", &lo, &hi) == 2)
            n = (int)(hi - lo + 1);
        else if (sscanf(buf, "%u", &lo) == 1)
            n = 1;
    }
    fclose(f);
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
        type[strcspn(type, "\n")] = '\0';
        if (strcmp(type, "critical") != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone0/trip_point_%d_temp", i);
        f = fopen(path, "r");
        if (!f)
            break;
        long md = 0;
        int  ok = (fscanf(f, "%ld", &md) == 1);
        fclose(f);
        if (ok && md > 0) {
            ctx->temp_critical = (double)md / 1000.0;
            ctx->temp_critical_valid = 1;
        }
        break;
    }
}

/* =========================================================================
 * Serialize one db_row_t to a jbuf using the configured units.
 * Used by /api/current and /stream (SSE).
 * ======================================================================= */

static void serialize_current(jbuf_t *j, const db_row_t *r, const http_ctx_t *ctx)
{
    const config_t *cfg = ctx->cfg;
    const char     *lu = cfg->cpu_load_card_unit;
    const char     *mu = cfg->memory_card_unit;
    const char     *du = cfg->disk_card_unit;
    const char     *nu = cfg->net_card_unit;

    jbuf_str(j, "timestamp", r->timestamp);

    jbuf_real(j, "load_1m", load_convert(r->load_1m, ctx->num_cores, lu));
    jbuf_real(j, "load_5m", load_convert(r->load_5m, ctx->num_cores, lu));
    jbuf_real(j, "load_15m", load_convert(r->load_15m, ctx->num_cores, lu));

    if (r->cpu_valid) {
        jbuf_real(j, "cpu_user_percent", r->cpu_user_percent);
        jbuf_real(j, "cpu_system_percent", r->cpu_system_percent);
        jbuf_real(j, "cpu_idle_percent", r->cpu_idle_percent);
    } else {
        jbuf_null(j, "cpu_user_percent");
        jbuf_null(j, "cpu_system_percent");
        jbuf_null(j, "cpu_idle_percent");
    }

    if (mu[0] != '%') {
        jbuf_real(j, "mem_used", mem_convert(r->mem_used_mb, mu));
        jbuf_real(j, "mem_available", mem_convert(r->mem_available_mb, mu));
        jbuf_real(j, "mem_total", mem_convert(r->mem_total_mb, mu));
    }
    jbuf_real(j, "mem_percent", r->mem_percent);

    if (du[0] != '%') {
        jbuf_real(j, "disk_used", disk_convert(r->disk_used_gb, du));
        jbuf_real(j, "disk_total", disk_convert(r->disk_total_gb, du));
        jbuf_real(j, "disk_free", disk_convert(r->disk_free_gb, du));
    }
    jbuf_real(j, "disk_percent", r->disk_percent);

    if (cfg_has(cfg->cards, cfg->card_count, "temp")) {
        if (r->temp_valid)
            jbuf_real(j, "temp", temp_convert(r->temp_celsius, cfg->temp_card_unit, cfg->temp_max));
        else
            jbuf_null(j, "temp");
        if (ctx->temp_critical_valid)
            jbuf_real(j, "temp_critical",
                      temp_convert(ctx->temp_critical, cfg->temp_card_unit, cfg->temp_max));
        else
            jbuf_null(j, "temp_critical");
    }

    if (r->net_valid) {
        jbuf_real(j, "net_rx", net_convert(r->net_rx_bps, nu));
        jbuf_real(j, "net_tx", net_convert(r->net_tx_bps, nu));
    } else {
        jbuf_null(j, "net_rx");
        jbuf_null(j, "net_tx");
    }

    jbuf_real(j, "uptime_seconds", r->uptime_seconds);

    jbuf_str(j, "mem_card_unit", cfg->memory_card_unit);
    jbuf_str(j, "mem_chart_unit", cfg->memory_chart_unit);
    jbuf_str(j, "disk_card_unit", cfg->disk_card_unit);
    jbuf_str(j, "disk_chart_unit", cfg->disk_chart_unit);
    jbuf_str(j, "temp_card_unit", cfg->temp_card_unit);
    jbuf_str(j, "temp_chart_unit", cfg->temp_chart_unit);
    jbuf_str(j, "net_card_unit", cfg->net_card_unit);
    jbuf_str(j, "net_chart_unit", cfg->net_chart_unit);
    jbuf_str(j, "cpu_load_card_unit", cfg->cpu_load_card_unit);
    jbuf_str(j, "cpu_load_chart_unit", cfg->cpu_load_chart_unit);

    jbuf_str(j, "title", cfg->title);
    jbuf_str(j, "version", MINIMONI_VERSION);
    jbuf_str(j, "theme", cfg->theme);
    jbuf_sep(j);
    jbuf_raw(j, cfg->show_footer ? "\"show_footer\":true" : "\"show_footer\":false");
    jbuf_str(j, "uptime_unit", cfg->uptime_unit);

    jbuf_sep(j);
    jbuf_raw(j, "\"ranges\":[");
    for (int i = 0; i < cfg->range_count; i++) {
        char rs[16];
        snprintf(rs, sizeof(rs), "%s\"%s\"", i > 0 ? "," : "", cfg->ranges[i]);
        jbuf_raw(j, rs);
    }
    jbuf_raw(j, "]");

    if (cfg->chart_count == 0) {
        jbuf_sep(j);
        jbuf_raw(j, "\"charts\":null");
    } else if (cfg->chart_count < 0) {
        jbuf_sep(j);
        jbuf_raw(j, "\"charts\":[]");
    } else {
        jbuf_sep(j);
        jbuf_raw(j, "\"charts\":[");
        for (int i = 0; i < cfg->chart_count; i++) {
            char cs[24];
            snprintf(cs, sizeof(cs), "%s\"%s\"", i > 0 ? "," : "", cfg->charts[i]);
            jbuf_raw(j, cs);
        }
        jbuf_raw(j, "]");
    }

    if (cfg->card_count == 0) {
        jbuf_sep(j);
        jbuf_raw(j, "\"cards\":null");
    } else if (cfg->card_count < 0) {
        jbuf_sep(j);
        jbuf_raw(j, "\"cards\":[]");
    } else {
        jbuf_sep(j);
        jbuf_raw(j, "\"cards\":[");
        for (int i = 0; i < cfg->card_count; i++) {
            char ks[24];
            snprintf(ks, sizeof(ks), "%s\"%s\"", i > 0 ? "," : "", cfg->cards[i]);
            jbuf_raw(j, ks);
        }
        jbuf_raw(j, "]");
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

/* /api/current — latest snapshot with unit conversions applied */
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
    serialize_current(&j, &row, ctx);
    jbuf_end(&j);

    mg_send_http_ok(conn, "application/json", (long long)j.pos);
    mg_write(conn, buf, j.pos);
    return 200;
}

/* /api/metrics?range=<range> — time-series with short keys */
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
     * data points it can render and passes it here. The server caps at 1440,
     * which is the design point of the tier ladder: every tier transition is
     * sized so that `bucket × 1440 ≤ transition_age`, meaning the system can
     * deliver up to 1440 source rows in any range without gaps. Above 1440
     * the ladder cannot guarantee uniform coverage (see ADR-0005). Values
     * <=0 or missing fall back to the pick_bucket default. */
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

    const config_t *cfg = ctx->cfg;
    const char     *lu = cfg->cpu_load_chart_unit;
    const char     *mu = cfg->memory_chart_unit;
    const char     *du = cfg->disk_chart_unit;
    const char     *nu = cfg->net_chart_unit;
    int             pct_mu = (mu[0] == '%');
    int             pct_du = (du[0] == '%');

    /* Stream response without buffering the full body. */
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/json\r\n"
              "Connection: close\r\n\r\n"
              "{\"range\":\"%s\",\"points\":[",
              range_str);

    char pt[512];
    for (int i = 0; i < cnt; i++) {
        const db_row_t *r = &rows[i];
        jbuf_t          j;
        jbuf_init(&j, pt, sizeof(pt));
        jbuf_begin(&j);

        jbuf_long(&j, "t", r->unix_time);

        jbuf_real(&j, "l1", load_convert(r->load_1m, ctx->num_cores, lu));
        jbuf_real(&j, "l5", load_convert(r->load_5m, ctx->num_cores, lu));
        jbuf_real(&j, "l15", load_convert(r->load_15m, ctx->num_cores, lu));

        if (r->cpu_valid) {
            jbuf_real(&j, "cu", r->cpu_user_percent);
            jbuf_real(&j, "cs", r->cpu_system_percent);
            jbuf_real(&j, "ci", r->cpu_idle_percent);
        } else {
            jbuf_null(&j, "cu");
            jbuf_null(&j, "cs");
            jbuf_null(&j, "ci");
        }

        if (!pct_mu) {
            jbuf_real(&j, "mu", mem_convert(r->mem_used_mb, mu));
            jbuf_real(&j, "ma", mem_convert(r->mem_available_mb, mu));
            jbuf_real(&j, "mt", mem_convert(r->mem_total_mb, mu));
        }
        jbuf_real(&j, "mp", r->mem_percent);

        if (!pct_du) {
            jbuf_real(&j, "du", disk_convert(r->disk_used_gb, du));
            jbuf_real(&j, "dt", disk_convert(r->disk_total_gb, du));
            jbuf_real(&j, "df", disk_convert(r->disk_free_gb, du));
        }
        jbuf_real(&j, "dp", r->disk_percent);

        if (cfg_has(cfg->charts, cfg->chart_count, "temp")) {
            if (r->temp_valid)
                jbuf_real(&j, "tp",
                          temp_convert(r->temp_celsius, cfg->temp_chart_unit, cfg->temp_max));
            else
                jbuf_null(&j, "tp");
        }

        if (r->net_valid) {
            jbuf_real(&j, "nr", net_convert(r->net_rx_bps, nu));
            jbuf_real(&j, "nt", net_convert(r->net_tx_bps, nu));
        } else {
            jbuf_null(&j, "nr");
            jbuf_null(&j, "nt");
        }

        jbuf_real(&j, "up", r->uptime_seconds);
        jbuf_end(&j);

        if (i > 0)
            mg_write(conn, ",", 1);
        mg_write(conn, pt, j.pos);
    }

    free(rows);
    db_release_memory(ctx->db);
    mg_write(conn, "]}", 2);
    return 200;
}

/* /stream — SSE endpoint; blocks until the client disconnects or the server
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
            serialize_current(&j, &row, ctx);
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
