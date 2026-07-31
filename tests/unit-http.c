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

/* Unit tests for src/http.c: the handlers are called directly with the civetweb
 * entry points stubbed below, no live server. tests/cli.sh stays the contract
 * that real civetweb drives them the way these stubs assume. */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runner.h"

/* --- civetweb stubs (defined before http.c so its calls land here) --- */

static char      g_resp[16384];
static size_t    g_resp_len;
static char      g_mime[64];
static long long g_declared_len;

static void resp_reset(void)
{
    g_resp[0] = '\0';
    g_resp_len = 0;
    g_mime[0] = '\0';
    g_declared_len = -1;
}

static void resp_append(const void *buf, size_t len)
{
    if (g_resp_len + len >= sizeof(g_resp))
        len = sizeof(g_resp) - g_resp_len - 1;
    memcpy(g_resp + g_resp_len, buf, len);
    g_resp_len += len;
    g_resp[g_resp_len] = '\0';
}

static struct mg_request_info g_ri;

#include "civetweb.h"

int mg_write(struct mg_connection *conn, const void *buf, size_t len)
{
    (void)conn;
    resp_append(buf, len);
    return (int)len;
}

/* -1 = never fail; otherwise the 0-based mg_printf call that returns -1, which
 * is how a test acts out a client hanging up mid-stream. */
static int g_printf_fails_at = -1;
static int g_printf_calls;

int mg_printf(struct mg_connection *conn, const char *fmt, ...)
{
    (void)conn;
    if (g_printf_fails_at >= 0 && g_printf_calls++ >= g_printf_fails_at)
        return -1;
    char    b[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n > 0)
        resp_append(b, (size_t)n < sizeof(b) ? (size_t)n : sizeof(b) - 1);
    return n;
}

int mg_send_http_ok(struct mg_connection *conn, const char *mime_type, long long content_length)
{
    (void)conn;
    snprintf(g_mime, sizeof(g_mime), "%s", mime_type ? mime_type : "");
    g_declared_len = content_length;
    return 0;
}

const struct mg_request_info *mg_get_request_info(const struct mg_connection *conn)
{
    (void)conn;
    return &g_ri;
}

/* Minimal query-string lookup: enough for "a=1&b=2" style requests. */
int mg_get_var(const char *data, size_t data_len, const char *name, char *dst, size_t dst_len)
{
    dst[0] = '\0';
    if (!data)
        return -1;
    size_t nlen = strlen(name);
    for (const char *p = data; p < data + data_len;) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : data + data_len;
        if ((size_t)(end - p) > nlen && strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            size_t vlen = (size_t)(end - p) - nlen - 1;
            if (vlen >= dst_len)
                return -2;
            memcpy(dst, p + nlen + 1, vlen);
            dst[vlen] = '\0';
            return (int)vlen;
        }
        if (!amp)
            break;
        p = amp + 1;
    }
    return -1;
}

static int g_handlers_registered;

struct mg_context *mg_start(const struct mg_callbacks *cb, void *user_data, const char **opts)
{
    (void)cb;
    (void)user_data;
    (void)opts;
    static int dummy;
    return (struct mg_context *)&dummy;
}

void mg_stop(struct mg_context *ctx) { (void)ctx; }

void mg_set_request_handler(struct mg_context *ctx, const char *uri, mg_request_handler handler,
                            void *cbdata)
{
    (void)ctx;
    (void)uri;
    (void)handler;
    (void)cbdata;
    g_handlers_registered++;
}

/* --- Module under test --- */

/* Defined here rather than passed as -D: threading it through make and sh -c
 * mangles the quoting. */
#define MINIMONI_VERSION "unit-test"

#include "../src/http.c"

/* --- Fixtures --- */

static char     g_dbpath[256];
static db_t     g_db;
static config_t g_cfg;
static int      g_fixture_failed;

static http_ctx_t *fixture_ctx(void)
{
    static http_ctx_t ctx;
    snprintf(g_dbpath, sizeof(g_dbpath), "/tmp/minimoni-http-%d.db", (int)getpid());
    unlink(g_dbpath);
    config_defaults(&g_cfg);
    g_fixture_failed = db_open(&g_db, g_dbpath, 60) != 0;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = &g_cfg;
    ctx.db = &g_db;
    ctx.num_cores = 4;
    return &ctx;
}

/* Tears the fixture down and folds a failed db_open into the verdict. */
static int fixture_done(int ok)
{
    db_close(&g_db);
    unlink(g_dbpath);
    return (ok && !g_fixture_failed) ? 0 : 1;
}

static void insert_sample(void)
{
    metrics_t m;
    memset(&m, 0, sizeof(m));
    m.load_1m = 1.0;
    m.mem_total_mb = 1000.0;
    m.mem_used_mb = 500.0;
    m.mem_percent = 50.0;
    m.disk_total_gb = 100.0;
    m.disk_percent = 40.0;
    m.uptime_seconds = 3600.0;
    db_insert(&g_db, &m);
}

static void request(const char *query)
{
    memset(&g_ri, 0, sizeof(g_ri));
    g_ri.query_string = query;
    resp_reset();
    g_printf_fails_at = -1;
    g_printf_calls = 0;
}

static int has(const char *needle) { return strstr(g_resp, needle) != NULL; }

/* --- Static endpoints --- */

static int test_health_reports_ok_and_version(void)
{
    request(NULL);
    int rc = handler_health(NULL, NULL);
    return (rc == 200 && has("\"status\":\"ok\"") && has("\"version\"") &&
            strcmp(g_mime, "application/json") == 0)
               ? 0
               : 1;
}

static int test_root_serves_the_embedded_dashboard(void)
{
    request(NULL);
    int rc = handler_root(NULL, NULL);
    return (rc == 200 && g_resp_len > 0 && strstr(g_mime, "text/html") &&
            g_declared_len == (long long)dashboard_index_html_len)
               ? 0
               : 1;
}

/* --- /api/current --- */

static int test_current_503_without_data(void)
{
    http_ctx_t *ctx = fixture_ctx();
    request(NULL);
    int rc = handler_current(NULL, ctx);
    return fixture_done(rc == 503 && has("no data collected yet"));
}

static int test_current_serializes_the_latest_row(void)
{
    http_ctx_t *ctx = fixture_ctx();
    insert_sample();
    request(NULL);
    int rc = handler_current(NULL, ctx);
    return fixture_done(rc == 200 && has("\"mem_percent\":50") && has("\"thresh_cpu\"") &&
                        strcmp(g_mime, "application/json") == 0);
}

/* --- /api/metrics: range and points handling --- */

static int test_metrics_serializes_a_configured_range(void)
{
    http_ctx_t *ctx = fixture_ctx();
    insert_sample();
    request("range=7d");
    int rc = handler_metrics(NULL, ctx);
    return fixture_done(rc == 200 && has("\"range\":\"7d\"") && has("\"points\":[") &&
                        has("\"t\":") && strstr(g_resp, "]}") != NULL);
}

/* An unlisted range and no query string at all both fall back to the first
 * configured range, never 400. */
static int test_metrics_falls_back_to_the_first_range(void)
{
    http_ctx_t *ctx = fixture_ctx();
    int         ok = 1;
    const char *qs[] = {"range=bogus", NULL};
    for (size_t i = 0; i < sizeof(qs) / sizeof(qs[0]); i++) {
        request(qs[i]);
        if (handler_metrics(NULL, ctx) != 200 || !has("\"range\":\"1d\""))
            ok = 0;
    }
    return fixture_done(ok);
}

static int test_metrics_handles_every_range_unit(void)
{
    http_ctx_t *ctx = fixture_ctx();
    /* Minutes/hours/days each have their own seconds conversion. */
    snprintf(g_cfg.ranges[0], sizeof(g_cfg.ranges[0]), "%s", "30m");
    snprintf(g_cfg.ranges[1], sizeof(g_cfg.ranges[1]), "%s", "12h");
    snprintf(g_cfg.ranges[2], sizeof(g_cfg.ranges[2]), "%s", "2d");
    g_cfg.range_count = 3;
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        char q[32];
        snprintf(q, sizeof(q), "range=%s", g_cfg.ranges[i]);
        request(q);
        char want[32];
        snprintf(want, sizeof(want), "\"range\":\"%s\"", g_cfg.ranges[i]);
        if (handler_metrics(NULL, ctx) != 200 || !has(want))
            ok = 0;
    }
    return fixture_done(ok);
}

/* The hint only feeds pick_bucket, so nothing it can say makes the endpoint fail. */
static int test_metrics_clamps_the_points_hint(void)
{
    http_ctx_t *ctx = fixture_ctx();
    insert_sample();
    int         ok = 1;
    const char *qs[] = {"range=1d&points=99999", "range=1d&points=0", "range=1d&points=abc",
                        "range=1d&points=480"};
    for (size_t i = 0; i < sizeof(qs) / sizeof(qs[0]); i++) {
        request(qs[i]);
        if (handler_metrics(NULL, ctx) != 200 || !has("\"points\":["))
            ok = 0;
    }
    return fixture_done(ok);
}

/* --- /stream (SSE) --- */

/* `stopping` short-circuits the 1-second wait loop, so this one never sleeps. */
static int test_stream_sends_a_frame_then_stops(void)
{
    http_ctx_t *ctx = fixture_ctx();
    insert_sample();
    ctx->stopping = 1;
    request(NULL);
    int rc = handler_stream(NULL, ctx);
    return fixture_done(rc == 200 && has("text/event-stream") && has("data: {") &&
                        has("\"mem_percent\""));
}

static int test_stream_stops_when_the_client_hangs_up(void)
{
    http_ctx_t *ctx = fixture_ctx();
    insert_sample();
    request(NULL);
    g_printf_fails_at = 1; /* headers out, first frame fails: the only exit while running */
    int rc = handler_stream(NULL, ctx);
    return fixture_done(rc == 200);
}

static int test_stream_sends_keepalive_comments(void)
{
    http_ctx_t *ctx = fixture_ctx();
    insert_sample();
    /* Keepalive only runs when its interval is below refresh, and one has to go
     * out before a failed write can end the loop: two ticks, two seconds. */
    g_cfg.refresh_seconds = 2;
    g_cfg.sse_keepalive_seconds = 1;
    request(NULL);
    g_printf_fails_at = 3; /* headers, frame, keepalive, then the second keepalive fails */
    int rc = handler_stream(NULL, ctx);
    return fixture_done(rc == 200 && has(": keepalive"));
}

/* --- Start/stop plumbing --- */

static int test_http_start_registers_every_route(void)
{
    http_ctx_t *ctx = fixture_ctx();
    g_handlers_registered = 0;
    int rc = http_start(ctx, &g_cfg, &g_db);
    int ok = (rc == 0 && g_handlers_registered >= 5 && ctx->num_cores >= 1);
    http_stop(ctx);
    return fixture_done(ok);
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(health_reports_ok_and_version),
    T(root_serves_the_embedded_dashboard),
    T(current_503_without_data),
    T(current_serializes_the_latest_row),
    T(metrics_serializes_a_configured_range),
    T(metrics_falls_back_to_the_first_range),
    T(metrics_handles_every_range_unit),
    T(metrics_clamps_the_points_hint),
    T(stream_sends_a_frame_then_stops),
    T(stream_stops_when_the_client_hangs_up),
    T(stream_sends_keepalive_comments),
    T(http_start_registers_every_route),
};

int main(void)
{
    if (!freopen("/dev/null", "w", stderr))
        return 2;
    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
