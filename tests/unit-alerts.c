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

/* Unit tests for src/alerts.c (zero-dependency, no framework).
 * Build with: make test. The module is #included directly.
 *
 * BearSSL is replaced at the link seam, the way unit-http.c replaces civetweb: a fake vtable can
 * report any verdict, where the real engine only produces one per certificate it is handed. That
 * the X.509 policy plus real BearSSL delivers a webhook is asserted in tests/e2e-webhook.sh. */

#define _POSIX_C_SOURCE 200809L

#include "runner.h"

#include "alerts.h"
#include "bearssl.h"
#include "trust.h"

/* --- BearSSL link seam --- */

static unsigned    g_verdict;
static const char *g_seen_name;

static void fake_start_chain(const br_x509_class **ctx, const char *server_name)
{
    (void)ctx;
    g_seen_name = server_name;
}

static void fake_start_cert(const br_x509_class **ctx, uint32_t len)
{
    (void)ctx;
    (void)len;
}

static void fake_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
}

static void fake_end_cert(const br_x509_class **ctx) { (void)ctx; }

static unsigned fake_end_chain(const br_x509_class **ctx)
{
    (void)ctx;
    return g_verdict;
}

static const br_x509_pkey *fake_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    (void)ctx;
    (void)usages;
    return NULL;
}

const br_x509_class br_x509_minimal_vtable = {sizeof(br_x509_minimal_context),
                                              fake_start_chain,
                                              fake_start_cert,
                                              fake_append,
                                              fake_end_cert,
                                              fake_end_chain,
                                              fake_get_pkey};

/* Only referenced from post_webhook, which these tests do not reach; defined so the module links
 * without libbearssl.a. */
void br_ssl_client_init_full(br_ssl_client_context *cc, br_x509_minimal_context *xc,
                             const br_x509_trust_anchor *ta, size_t num)
{
    (void)cc;
    (void)xc;
    (void)ta;
    (void)num;
}

int br_ssl_client_reset(br_ssl_client_context *cc, const char *name, int resume)
{
    (void)cc;
    (void)name;
    (void)resume;
    return 0;
}

void br_ssl_engine_set_buffer(br_ssl_engine_context *cc, void *iobuf, size_t len, int bidi)
{
    (void)cc;
    (void)iobuf;
    (void)len;
    (void)bidi;
}

void br_sslio_init(br_sslio_context *ctx, br_ssl_engine_context           *eng,
                   int (*rf)(void *, unsigned char *, size_t), void       *rc,
                   int (*wf)(void *, const unsigned char *, size_t), void *wc)
{
    (void)ctx;
    (void)eng;
    (void)rf;
    (void)rc;
    (void)wf;
    (void)wc;
}

int br_sslio_write_all(br_sslio_context *ctx, const void *src, size_t len)
{
    (void)ctx;
    (void)src;
    (void)len;
    return -1;
}

int br_sslio_flush(br_sslio_context *ctx)
{
    (void)ctx;
    return -1;
}

int br_sslio_read(br_sslio_context *ctx, void *dst, size_t len)
{
    (void)ctx;
    (void)dst;
    (void)len;
    return -1;
}

void br_ssl_engine_close(br_ssl_engine_context *cc) { (void)cc; }

/* The trust anchor loader, also reached only from post_webhook; tests/integration-trust.c runs the
 * real one. */
int trust_load(trust_t *t, const char *path)
{
    (void)t;
    (void)path;
    return -1;
}

const char *trust_system_file(void) { return NULL; }

void trust_free(trust_t *t) { (void)t; }

/* json and db seams: src/json.c pulls units.c and config.c, and config.c pulls vendor/tomlc17.c, so
 * the real ones would drag vendored code into a unit build. This json_escape escapes `"` and `\`
 * as the real one does, and nothing else. */
int json_escape(char *dst, size_t cap, const char *src)
{
    size_t len = 0;

    for (; *src; src++) {
        int quoted = *src == '"' || *src == '\\';

        if (len + quoted + 1 >= cap)
            return -1;

        if (quoted)
            dst[len++] = '\\';
        dst[len++] = *src;
    }
    dst[len] = '\0';

    return (int)len;
}

/* What the fake alert log answers: whether an alert is firing, and whether a cooldown holds it. */
static int g_firing;
static int g_cooling = 1;

int db_alert_on_cooldown(db_t *db, const char *alert_name, long cooldown_seconds, time_t now)
{
    (void)db;
    (void)alert_name;
    (void)cooldown_seconds;
    (void)now;
    return g_cooling;
}

int db_alert_firing(db_t *db, const char *alert_name, char *fired_at, size_t size)
{
    (void)db;
    (void)alert_name;
    (void)fired_at;
    (void)size;
    return g_firing;
}

int db_alert_log_fire(db_t *db, const char *alert_name, time_t now)
{
    (void)db;
    (void)alert_name;
    (void)now;
    return 0;
}

int db_alert_log_resolve(db_t *db, const char *alert_name, time_t now)
{
    (void)db;
    (void)alert_name;
    (void)now;
    return 0;
}

int db_alert_resolve_others(db_t *db, const char *const names[], int count, time_t now)
{
    (void)db;
    (void)names;
    (void)count;
    (void)now;
    return 0;
}

#include "../src/alerts.c"

/* --- The policy: which verdicts the opt-out drops --- */

static unsigned verdict_for(unsigned reported)
{
    const br_x509_class *vt = &br_x509_minimal_vtable;
    g_verdict = reported;
    return xi_end_chain(&vt);
}

static int test_drops_not_trusted(void)
{
    return verdict_for(BR_ERR_X509_NOT_TRUSTED) == 0 ? 0 : 1;
}

/* br_x509_minimal reports a good chain as 0, never as BR_ERR_X509_OK. */
static int test_keeps_success(void) { return verdict_for(0) == 0 ? 0 : 1; }

/* An unverified endpoint is still not a free pass: everything else propagates. */
static int test_propagates_expired(void)
{
    return verdict_for(BR_ERR_X509_EXPIRED) == BR_ERR_X509_EXPIRED ? 0 : 1;
}

static int test_propagates_bad_server_name(void)
{
    return verdict_for(BR_ERR_X509_BAD_SERVER_NAME) == BR_ERR_X509_BAD_SERVER_NAME ? 0 : 1;
}

static int test_propagates_empty_chain(void)
{
    return verdict_for(BR_ERR_X509_EMPTY_CHAIN) == BR_ERR_X509_EMPTY_CHAIN ? 0 : 1;
}

static int test_drops_the_server_name(void)
{
    const br_x509_class *vt = &br_x509_minimal_vtable;
    g_seen_name = "sentinel";
    xi_start_chain(&vt, "example.com");
    return g_seen_name == NULL ? 0 : 1;
}

/* --- The metrics config accepts --- */

/* config_load refuses any other metric name, so each one it accepts must have a value here. */
static int test_every_config_metric_has_a_value(void)
{
    static const char *const names[] = {ALERT_METRICS, NULL};
    config_t                 cfg = {.temp_scale = "celsius"};
    db_row_t                 row;
    memset(&row, 0, sizeof(row));
    row.cpu_valid = row.temp_valid = row.net_valid = row.swap_valid = row.swap_io_valid = 1;

    for (int i = 0; names[i]; i++) {
        int valid = 0;
        metric_value(&row, names[i], &cfg, 100.0, 12.5e6, 4, 1, 3600, &valid);
        if (!valid)
            return 1;
    }

    return 0;
}

/* The uptime is the one given: the row holds none. */
static int test_uptime_is_the_one_given(void)
{
    config_t cfg = {0};
    db_row_t row;
    memset(&row, 0, sizeof(row));
    int    valid = 0;
    double up = metric_value(&row, "uptime_seconds", &cfg, 100.0, 12.5e6, 4, 1, 7260, &valid);

    return valid && up == 7260.0 ? 0 : 1;
}

/* An uptime that could not be read does not fire. */
static int test_uptime_unreadable_is_not_valid(void)
{
    config_t cfg = {0};
    db_row_t row;
    memset(&row, 0, sizeof(row));
    int valid = 1;
    metric_value(&row, "uptime_seconds", &cfg, 100.0, 12.5e6, 4, 0, 7260, &valid);

    return valid ? 1 : 0;
}

/* Each memory metric reads its own field. */
static int test_mem_metrics_read_their_fields(void)
{
    static const char *const names[] = {"mem_available_mb", "mem_available_percent"};
    db_row_t                 row;
    memset(&row, 0, sizeof(row));
    row.mem_available_mb = 1.0;
    row.mem_available_percent = 2.0;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int valid = 0;
        if (get_metric_value(&row, names[i], &valid) != i + 1.0 || !valid)
            return 1;
    }

    return 0;
}

/* Each disk metric reads its own field. */
static int test_disk_metrics_read_their_fields(void)
{
    static const char *const names[] = {"disk_available_gb", "disk_available_percent"};
    db_row_t                 row;
    memset(&row, 0, sizeof(row));
    row.disk_available_gb = 1.0;
    row.disk_available_percent = 2.0;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int valid = 0;
        if (get_metric_value(&row, names[i], &valid) != i + 1.0 || !valid)
            return 1;
    }

    return 0;
}

/* A Raspberry Pi at 55 °C, its critical trip at 110 °C: 131 °F of 230 °F. */
static double temp_metric(const char *scale, const char *m, int *valid)
{
    config_t cfg = {0};
    db_row_t row;
    memset(&row, 0, sizeof(row));
    row.temp_valid = 1;
    row.temp_celsius = 55.0;
    snprintf(cfg.temp_scale, sizeof(cfg.temp_scale), "%s", scale);

    return metric_value(&row, m, &cfg, 110.0, 12.5e6, 4, 1, 3600, valid);
}

static int near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

/* In Celsius: the temperature in degrees and as a percent of the critical. */
static int test_temp_metrics_in_celsius(void)
{
    int v1, v2;

    int ok = near(temp_metric("celsius", "temp_degrees", &v1), 55.0) &&
             near(temp_metric("celsius", "temp_percent", &v2), 50.0);

    return ok && v1 && v2 ? 0 : 1;
}

/* In Fahrenheit the degrees and the percent are the scale's own: the percent counts from 0 °F. */
static int test_temp_metrics_in_fahrenheit(void)
{
    int v1, v2;

    int ok = near(temp_metric("fahrenheit", "temp_degrees", &v1), 131.0) &&
             near(temp_metric("fahrenheit", "temp_percent", &v2), 13100.0 / 230.0);

    return ok && v1 && v2 ? 0 : 1;
}

/* Without a sensor no temperature metric has a value. */
static int test_temp_metrics_without_a_sensor_have_no_value(void)
{
    static const char *const names[] = {"temp_degrees", "temp_percent"};
    config_t                 cfg = {.temp_scale = "celsius"};
    db_row_t                 row;
    memset(&row, 0, sizeof(row));

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int valid = 1;
        metric_value(&row, names[i], &cfg, 110.0, 12.5e6, 4, 1, 3600, &valid);
        if (valid)
            return 1;
    }

    return 0;
}

/* Over a 100 Mbit/s link (12.5e6 bytes/s), 2.5e6 bytes/s in is 20 % and 10e6 out is 80 %. */
static int test_net_percents_of_the_link(void)
{
    config_t cfg = {0};
    db_row_t row;
    memset(&row, 0, sizeof(row));
    row.net_valid = 1;
    row.net_rx_bps = 2.5e6;
    row.net_tx_bps = 10e6;

    int    rx, tx;
    double r = metric_value(&row, "net_rx_percent", &cfg, 100.0, 12.5e6, 4, 1, 3600, &rx);
    double t = metric_value(&row, "net_tx_percent", &cfg, 100.0, 12.5e6, 4, 1, 3600, &tx);

    return near(r, 20.0) && near(t, 80.0) && rx && tx ? 0 : 1;
}

/* Without a rate, as on the first sample, the percents have no value either. */
static int test_net_percents_without_a_rate_have_no_value(void)
{
    config_t cfg = {0};
    db_row_t row;
    memset(&row, 0, sizeof(row));

    int rx = 1, tx = 1;
    metric_value(&row, "net_rx_percent", &cfg, 100.0, 12.5e6, 4, 1, 3600, &rx);
    metric_value(&row, "net_tx_percent", &cfg, 100.0, 12.5e6, 4, 1, 3600, &tx);

    return !rx && !tx ? 0 : 1;
}

/* Each raw load metric reads its own field, whatever the CPU count. */
static int test_load_metrics_read_their_fields(void)
{
    static const char *const names[] = {"load_1m", "load_5m", "load_15m"};
    config_t                 cfg = {0};
    db_row_t                 row;
    memset(&row, 0, sizeof(row));
    row.load_1m = 1.0;
    row.load_5m = 2.0;
    row.load_15m = 3.0;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int valid = 0;
        if (metric_value(&row, names[i], &cfg, 100.0, 12.5e6, 4, 1, 3600, &valid) != i + 1.0 ||
            !valid)
            return 1;
    }

    return 0;
}

/* On 4 CPUs, loads of 1, 2 and 6 are 25, 50 and 150 %. */
static int test_load_percents_of_the_cpus(void)
{
    config_t cfg = {0};
    db_row_t row;
    memset(&row, 0, sizeof(row));
    row.load_1m = 1.0;
    row.load_5m = 2.0;
    row.load_15m = 6.0;

    int    v1, v5, v15;
    double l1 = metric_value(&row, "load_1m_percent", &cfg, 100.0, 12.5e6, 4, 1, 3600, &v1);
    double l5 = metric_value(&row, "load_5m_percent", &cfg, 100.0, 12.5e6, 4, 1, 3600, &v5);
    double l15 = metric_value(&row, "load_15m_percent", &cfg, 100.0, 12.5e6, 4, 1, 3600, &v15);

    return near(l1, 25.0) && near(l5, 50.0) && near(l15, 150.0) && v1 && v5 && v15 ? 0 : 1;
}

static int test_cpu_steal_reads_its_field(void)
{
    db_row_t row;
    memset(&row, 0, sizeof(row));
    row.cpu_valid = 1;
    row.cpu_steal_percent = 2.5;

    int    valid = 0;
    double steal = get_metric_value(&row, "cpu_steal_percent", &valid);

    return near(steal, 2.5) && valid ? 0 : 1;
}

static int test_cpu_steal_has_a_value_only_with_a_sample(void)
{
    db_row_t row;
    memset(&row, 0, sizeof(row));

    int without = 1, with = 0;
    get_metric_value(&row, "cpu_steal_percent", &without);
    row.cpu_valid = 1;
    get_metric_value(&row, "cpu_steal_percent", &with);

    return !without && with ? 0 : 1;
}

static int test_cpu_total_is_user_system_and_steal(void)
{
    db_row_t row;
    memset(&row, 0, sizeof(row));
    row.cpu_valid = 1;
    row.cpu_user_percent = 25.0;
    row.cpu_system_percent = 5.0;
    row.cpu_steal_percent = 2.5;

    int    valid = 0;
    double total = get_metric_value(&row, "cpu_total_percent", &valid);

    return near(total, 32.5) && valid ? 0 : 1;
}

static int test_cpu_total_has_a_value_only_with_a_sample(void)
{
    db_row_t row;
    memset(&row, 0, sizeof(row));

    int without = 1, with = 0;
    get_metric_value(&row, "cpu_total_percent", &without);
    row.cpu_valid = 1;
    get_metric_value(&row, "cpu_total_percent", &with);

    return !without && with ? 0 : 1;
}

static const char *const SWAP_METRICS[] = {"swap_free_mb", "swap_free_percent", "swap_in_bps",
                                           "swap_out_bps", "swap_in_ram",       "swap_out_ram"};

/* Each swap metric reads its own field. */
static int test_swap_metrics_read_their_fields(void)
{
    db_row_t row;
    memset(&row, 0, sizeof(row));
    row.swap_valid = row.swap_io_valid = 1;
    row.swap_free_mb = 1.0;
    row.swap_free_percent = 2.0;
    row.swap_in_bps = 3.0;
    row.swap_out_bps = 4.0;
    row.swap_in_ram = 5.0;
    row.swap_out_ram = 6.0;

    for (size_t i = 0; i < sizeof(SWAP_METRICS) / sizeof(SWAP_METRICS[0]); i++) {
        int valid = 0;
        if (get_metric_value(&row, SWAP_METRICS[i], &valid) != i + 1.0 || !valid)
            return 1;
    }

    return 0;
}

/* Without swap, a swap alert has nothing to compare. */
static int test_swap_metrics_without_swap_have_no_value(void)
{
    db_row_t row;
    memset(&row, 0, sizeof(row));

    for (size_t i = 0; i < sizeof(SWAP_METRICS) / sizeof(SWAP_METRICS[0]); i++) {
        int valid = 1;
        get_metric_value(&row, SWAP_METRICS[i], &valid);
        if (valid)
            return 1;
    }

    return 0;
}

/* Before the first swap I/O sample, the swap I/O alerts have nothing to compare; the others do. */
static int test_swap_io_metrics_without_a_sample_have_no_value(void)
{
    db_row_t row;
    memset(&row, 0, sizeof(row));
    row.swap_valid = 1;

    int in = 1, out = 1, in_ram = 1, out_ram = 1, free_pct = 0;
    get_metric_value(&row, "swap_in_bps", &in);
    get_metric_value(&row, "swap_out_bps", &out);
    get_metric_value(&row, "swap_in_ram", &in_ram);
    get_metric_value(&row, "swap_out_ram", &out_ram);
    get_metric_value(&row, "swap_free_percent", &free_pct);

    return !in && !out && !in_ram && !out_ram && free_pct ? 0 : 1;
}

/* --- The trust anchors --- */

static int test_no_system_store_points_to_ca_file(void)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/minimoni-test-anchors-%d.txt", getpid());

    fflush(stderr);
    if (!freopen(path, "w", stderr))
        return 1;

    trust_t trust;
    int     rc = load_anchors("", "hooks.example", &trust);

    fflush(stderr);
    if (!freopen("/dev/null", "w", stderr))
        return 1;

    char   err[256];
    FILE  *file = fopen(path, "r");
    size_t len = file ? fread(err, 1, sizeof(err) - 1, file) : 0;
    err[len] = '\0';

    if (file)
        fclose(file);
    unlink(path);

    return rc == -1 && strstr(err, "alerts: webhook to hooks.example not delivered, no system CA "
                                   "store; set ca_file under [alerts]\n")
               ? 0
               : 1;
}

/* --- The request: what does not fit is not sent --- */

static int test_fit_printf_exact_fit(void)
{
    char buf[6];
    return fit_printf(buf, sizeof(buf), "%s", "12345") == 5 && strcmp(buf, "12345") == 0 ? 0 : 1;
}

static int test_fit_printf_one_byte_short(void)
{
    char buf[5];
    return fit_printf(buf, sizeof(buf), "%s", "12345") == -1 ? 0 : 1;
}

/* `n` copies of 'a' as the host of an http URL with a path; url must hold n + 13. */
static const char *url_with_host(char *url, size_t n)
{
    memcpy(url, "http://", 7);
    memset(url + 7, 'a', n);
    memcpy(url + 7 + n, "/hook", 6);

    return url;
}

static int test_parse_url_host_at_max(void)
{
    char url[300], host[256], port[8], path[512];
    int  scheme = parse_url(url_with_host(url, 255), host, sizeof(host), port, sizeof(port), path,
                            sizeof(path));

    return scheme == 0 && strlen(host) == 255 && strcmp(path, "/hook") == 0 ? 0 : 1;
}

static int test_parse_url_host_over_max_refused(void)
{
    char url[300], host[256], port[8], path[512];

    return parse_url(url_with_host(url, 256), host, sizeof(host), port, sizeof(port), path,
                     sizeof(path)) == -1
               ? 0
               : 1;
}

static int test_parse_url_port_over_max_refused(void)
{
    char host[256], port[8], path[512];

    return parse_url("http://h:12345678/hook", host, sizeof(host), port, sizeof(port), path,
                     sizeof(path)) == -1
               ? 0
               : 1;
}

/* --- The reply: only a 2xx status line counts as delivered --- */

static int ok_after(const char *reply)
{
    head_t h = {"", 0};
    keep_head(&h, reply, strlen(reply));

    return reply_ok(&h);
}

static int test_reply_2xx_counts(void)
{
    return ok_after("HTTP/1.1 200 OK\r\n") && ok_after("HTTP/1.0 204 No Content\r\n") ? 0 : 1;
}

static int test_reply_other_codes_do_not_count(void)
{
    return !ok_after("HTTP/1.1 404 Not Found\r\n") && !ok_after("HTTP/1.1 500 Oops\r\n") &&
                   !ok_after("HTTP/1.1 301 Moved\r\n")
               ? 0
               : 1;
}

static int test_reply_that_is_not_http_does_not_count(void)
{
    return !ok_after("") && !ok_after("xxxxxxxx") && !ok_after("HTTP/1.1 2") &&
                   !ok_after("200 OK HTTP/1.1")
               ? 0
               : 1;
}

static int test_keep_head_keeps_only_what_fits(void)
{
    head_t h = {"", 0};
    keep_head(&h, "HTTP/1.1 200 OK, and a long tail", 32);
    keep_head(&h, "more", 4);

    return h.len == sizeof(h.text) - 1 && strncmp(h.text, "HTTP/1.1 200 OK", 15) == 0 ? 0 : 1;
}

/* --- The step: what an alert sends on each check --- */

static int test_alert_fires_when_its_condition_starts_to_hold(void)
{
    return alert_step(1, 0, 0) == ALERT_FIRE ? 0 : 1;
}

static int test_alert_within_its_cooldown_does_not_fire(void)
{
    return alert_step(1, 0, 1) == ALERT_KEEP ? 0 : 1;
}

static int test_firing_alert_sends_nothing_while_it_holds(void)
{
    return alert_step(1, 1, 0) == ALERT_KEEP && alert_step(1, 1, 1) == ALERT_KEEP ? 0 : 1;
}

static int test_firing_alert_resolves_when_it_stops_holding(void)
{
    return alert_step(0, 1, 0) == ALERT_RESOLVE && alert_step(0, 1, 1) == ALERT_RESOLVE ? 0 : 1;
}

static int test_quiet_alert_sends_nothing_while_it_does_not_hold(void)
{
    return alert_step(0, 0, 0) == ALERT_KEEP && alert_step(0, 0, 1) == ALERT_KEEP ? 0 : 1;
}

/* --- The text format's line --- */

static int line_is(const alert_cfg_t *alert, const char *status, double value, const char *want)
{
    char line[256];

    return alert_line(alert, status, value, "my-server", line, sizeof(line)) == (int)strlen(want) &&
           strcmp(line, want) == 0;
}

static int test_text_line_does_not_repeat_the_condition(void)
{
    alert_cfg_t alert = {.name = "disk_available_percent < 10",
                         .metric = "disk_available_percent",
                         .op = "<",
                         .threshold = 10};

    return line_is(&alert, "firing", 7.7, "[firing] disk_available_percent 7.7 < 10 on my-server")
               ? 0
               : 1;
}

static int test_text_line_puts_a_configured_name_first(void)
{
    alert_cfg_t alert = {
        .name = "hot", .named = 1, .metric = "temp_degrees", .op = ">", .threshold = 80};

    return line_is(&alert, "firing", 81.5, "[firing] hot: temp_degrees 81.5 > 80 on my-server") ? 0
                                                                                                : 1;
}

static int test_text_line_of_a_resolve(void)
{
    alert_cfg_t alert = {.name = "disk_available_percent < 10",
                         .metric = "disk_available_percent",
                         .op = "<",
                         .threshold = 10};

    return line_is(&alert, "resolved", 12.3,
                   "[resolved] disk_available_percent 12.3, no longer < 10 on my-server")
               ? 0
               : 1;
}

static int test_text_line_stays_one_line(void)
{
    alert_cfg_t alert = {.name = "disk\nfull\x01",
                         .named = 1,
                         .metric = "disk_available_percent",
                         .op = "<",
                         .threshold = 10};
    char        line[256];

    return alert_line(&alert, "firing", 7.7,
                      "web\x1f"
                      "01\x7f",
                      line, sizeof(line)) > 0 &&
                   strcmp(line,
                          "[firing] disk full : disk_available_percent 7.7 < 10 on web 01 ") == 0
               ? 0
               : 1;
}

static int test_text_line_keeps_utf_8(void)
{
    alert_cfg_t alert = {
        .name = "Z\xc3\xbcrich", .named = 1, .metric = "load_1m", .op = ">", .threshold = 1};

    return line_is(&alert, "firing", 2, "[firing] Z\xc3\xbcrich: load_1m 2 > 1 on my-server") ? 0
                                                                                              : 1;
}

static int test_text_line_that_does_not_fit_is_refused(void)
{
    alert_cfg_t alert = {.metric = "disk_available_percent", .op = "<", .threshold = 10};
    char        line[16];

    return alert_line(&alert, "firing", 7.7, "my-server", line, sizeof(line)) == -1 ? 0 : 1;
}

static int test_text_line_whose_status_does_not_fit_is_refused(void)
{
    alert_cfg_t alert = {.metric = "load_1m", .op = ">", .threshold = 1};
    char        line[64], status[71];

    memset(status, 's', sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';

    return alert_line(&alert, status, 2, "my-server", line, sizeof(line)) == -1 ? 0 : 1;
}

/* --- The body templates --- */

static int template_is(const char *template, const mark_t *marks, size_t count, size_t size,
                       const char *want)
{
    char  body[256];
    out_t out = {.size = size, .start = body};

    out_template(&out, template, marks, count);

    return want ? out.len == (int)strlen(want) && strcmp(body, want) == 0 : out.len == -1;
}

static int test_template_drops_the_whitespace_outside_its_strings(void)
{
    return template_is("{\n    \"a b\": [ 1, 2 ]\n}\n", NULL, 0, 256, "{\"a b\":[1,2]}") ? 0 : 1;
}

static int test_template_keeps_an_escaped_quote_inside_a_string(void)
{
    return template_is("{\"k\": \"x\\\" y\"}", NULL, 0, 256, "{\"k\":\"x\\\" y\"}") ? 0 : 1;
}

static int test_template_that_ends_after_a_backslash_stops_there(void)
{
    return template_is("\"x\\", NULL, 0, 256, "\"x\\") ? 0 : 1;
}

static int test_template_escapes_each_mark(void)
{
    const mark_t marks[] = {{"name", "a \"b\""}, {"value", "1.5"}};

    return template_is("{\"{{name}}\": {{value}}}", marks, 2, 256, "{\"a \\\"b\\\"\":1.5}") ? 0 : 1;
}

static int test_template_with_an_unknown_mark_is_refused(void)
{
    const mark_t marks[] = {{"name", "a"}};

    return template_is("{\"k\": \"{{nme}}\"}", marks, 1, 256, NULL) ? 0 : 1;
}

static int test_template_with_an_unclosed_mark_is_refused(void)
{
    const mark_t marks[] = {{"name", "a"}};

    return template_is("{\"k\": \"{{name\"}", marks, 1, 256, NULL) ? 0 : 1;
}

static int test_template_whose_mark_has_no_value_is_refused(void)
{
    const mark_t marks[] = {{"title", NULL}};

    return template_is("{\"k\": \"{{title}}\"}", marks, 1, 256, NULL) ? 0 : 1;
}

static int test_template_exact_fit(void)
{
    const mark_t marks[] = {{"name", "ab"}};

    return template_is("{\"k\": \"{{name}}\"}", marks, 1, 11, "{\"k\":\"ab\"}") &&
                   template_is("{\"k\": \"{{name}}\"}", marks, 1, 10, NULL)
               ? 0
               : 1;
}

/* --- The alertmanager format's body --- */

/* 2000-01-01T01:00:00Z. */
#define NOW 946688400

static int alertmanager_body_is(const alert_cfg_t *alert, const char *status, double value,
                                const char *started, const char *want)
{
    char body[BODY_SIZE];

    return alertmanager_body(alert, status, value, "my-server", started, NOW, body, sizeof(body)) ==
               (int)strlen(want) &&
           strcmp(body, want) == 0;
}

static int test_fingerprint_matches_prometheus(void)
{
    /* prometheus/common's model/signature_test.go: TestMetricToFingerprint. */
    const char *const labels[][2] = {{"fear", "love is not enough"}, {"name", "garland, briggs"}};

    return fingerprint(labels, 2) == UINT64_C(5799056148416392346) &&
                   fingerprint(labels, 0) == UINT64_C(14695981039346656037)
               ? 0
               : 1;
}

static int test_alertmanager_body_of_a_firing(void)
{
    alert_cfg_t alert = {
        .name = "hot", .named = 1, .metric = "temp_degrees", .op = ">", .threshold = 80};
    const char *want =
        "{\"receiver\":\"minimoni\",\"status\":\"firing\","
        "\"alerts\":[{\"status\":\"firing\","
        "\"labels\":{\"alertname\":\"hot\",\"instance\":\"my-server\"},"
        "\"annotations\":{\"summary\":\"hot: temp_degrees 81.5 > 80 on my-server\"},"
        "\"startsAt\":\"2000-01-01T01:00:00Z\",\"endsAt\":\"0001-01-01T00:00:00Z\","
        "\"generatorURL\":\"\",\"fingerprint\":\"6e15b56a0a36cfd6\"}],"
        "\"notification_reason\":\"first notification\","
        "\"groupLabels\":{\"alertname\":\"hot\",\"instance\":\"my-server\"},"
        "\"commonLabels\":{\"alertname\":\"hot\",\"instance\":\"my-server\"},"
        "\"commonAnnotations\":{\"summary\":\"hot: temp_degrees 81.5 > 80 on my-server\"},"
        "\"externalURL\":\"\",\"version\":\"4\","
        "\"groupKey\":\"{}:{alertname=\\\"hot\\\", instance=\\\"my-server\\\"}\","
        "\"truncatedAlerts\":0}";

    return alertmanager_body_is(&alert, "firing", 81.5, "", want) ? 0 : 1;
}

static int test_alertmanager_body_of_a_firing_starts_now(void)
{
    alert_cfg_t alert = {.name = "a", .named = 1, .metric = "load_1m", .op = ">", .threshold = 1};
    char        body[BODY_SIZE];

    return alertmanager_body(&alert, "firing", 5, "web", "2000-01-01T00:00:00Z", NOW, body,
                             sizeof(body)) > 0 &&
                   strstr(body, "\"startsAt\":\"2000-01-01T01:00:00Z\"")
               ? 0
               : 1;
}

static int test_alertmanager_body_of_a_resolve_keeps_its_start(void)
{
    alert_cfg_t alert = {.name = "disk_available_percent < 10",
                         .metric = "disk_available_percent",
                         .op = "<",
                         .threshold = 10};
    const char *want =
        "{\"receiver\":\"minimoni\",\"status\":\"resolved\","
        "\"alerts\":[{\"status\":\"resolved\","
        "\"labels\":{\"alertname\":\"disk_available_percent < 10\",\"instance\":\"my-server\"},"
        "\"annotations\":{\"summary\":\"disk_available_percent 12.3, no longer < 10 on "
        "my-server\"},"
        "\"startsAt\":\"2000-01-01T00:00:00Z\",\"endsAt\":\"2000-01-01T01:00:00Z\","
        "\"generatorURL\":\"\",\"fingerprint\":\"59fd074d5fd5c6e9\"}],"
        "\"notification_reason\":\"all alerts resolved\","
        "\"groupLabels\":{\"alertname\":\"disk_available_percent < 10\","
        "\"instance\":\"my-server\"},"
        "\"commonLabels\":{\"alertname\":\"disk_available_percent < 10\","
        "\"instance\":\"my-server\"},"
        "\"commonAnnotations\":{\"summary\":\"disk_available_percent 12.3, no longer < 10 on "
        "my-server\"},"
        "\"externalURL\":\"\",\"version\":\"4\","
        "\"groupKey\":\"{}:{alertname=\\\"disk_available_percent < 10\\\", "
        "instance=\\\"my-server\\\"}\","
        "\"truncatedAlerts\":0}";

    return alertmanager_body_is(&alert, "resolved", 12.3, "2000-01-01T00:00:00Z", want) ? 0 : 1;
}

static int test_alertmanager_body_of_a_resolve_with_no_start_starts_at_its_end(void)
{
    alert_cfg_t alert = {.name = "a", .named = 1, .metric = "load_1m", .op = ">", .threshold = 1};
    char        body[BODY_SIZE];

    return alertmanager_body(&alert, "resolved", 0.5, "", "", NOW, body, sizeof(body)) > 0 &&
                   strstr(body, "\"startsAt\":\"2000-01-01T01:00:00Z\","
                                "\"endsAt\":\"2000-01-01T01:00:00Z\"")
               ? 0
               : 1;
}

static int test_alertmanager_body_escapes_the_host_in_the_group_key_twice(void)
{
    alert_cfg_t alert = {.name = "a", .named = 1, .metric = "load_1m", .op = ">", .threshold = 1};
    char        body[BODY_SIZE];

    return alertmanager_body(&alert, "firing", 5, "web \"01\"", "", NOW, body, sizeof(body)) > 0 &&
                   strstr(body, "\"groupKey\":\"{}:{alertname=\\\"a\\\", "
                                "instance=\\\"web \\\\\\\"01\\\\\\\"\\\"}\"") &&
                   strstr(body, "\"labels\":{\"alertname\":\"a\",\"instance\":\"web \\\"01\\\"\"}")
               ? 0
               : 1;
}

static int test_alertmanager_body_escapes_the_name_in_the_group_key_twice(void)
{
    alert_cfg_t alert = {
        .name = "a \"b\"", .named = 1, .metric = "load_1m", .op = ">", .threshold = 1};
    char body[BODY_SIZE];

    return alertmanager_body(&alert, "firing", 5, "web \"01\"", "", NOW, body, sizeof(body)) > 0 &&
                   strstr(body, "\"groupKey\":\"{}:{alertname=\\\"a \\\\\\\"b\\\\\\\"\\\", "
                                "instance=\\\"web \\\\\\\"01\\\\\\\"\\\"}\"")
               ? 0
               : 1;
}

static int test_alertmanager_fingerprint_is_of_the_labels_unescaped(void)
{
    alert_cfg_t alert = {.name = "a", .named = 1, .metric = "load_1m", .op = ">", .threshold = 1};
    char        body[BODY_SIZE];

    return alertmanager_body(&alert, "firing", 5, "web \"01\"", "", NOW, body, sizeof(body)) > 0 &&
                   strstr(body, "\"fingerprint\":\"b4cd0b12e5123d5f\"")
               ? 0
               : 1;
}

static int test_alertmanager_body_escapes_the_start(void)
{
    alert_cfg_t alert = {.name = "a", .named = 1, .metric = "load_1m", .op = ">", .threshold = 1};
    char        body[BODY_SIZE];

    return alertmanager_body(&alert, "resolved", 0.5, "", "a\"b", NOW, body, sizeof(body)) > 0 &&
                   strstr(body, "\"startsAt\":\"a\\\"b\",")
               ? 0
               : 1;
}

static int test_alertmanager_summary_is_escaped(void)
{
    alert_cfg_t alert = {.name = "a", .named = 1, .metric = "load_1m", .op = ">", .threshold = 1};
    char        body[BODY_SIZE];

    return alertmanager_body(&alert, "firing", 5, "web \"01\"", "", NOW, body, sizeof(body)) > 0 &&
                   strstr(body, "\"annotations\":{\"summary\":"
                                "\"a: load_1m 5 > 1 on web \\\"01\\\"\"}")
               ? 0
               : 1;
}

static int test_alertmanager_body_of_the_longest_alert_fits(void)
{
    alert_cfg_t alert = {.named = 1, .op = ">=", .threshold = 1.23457e+22};
    char        body[BODY_SIZE], host[sizeof(((config_t *)0)->title)];

    memset(alert.name, 'n', sizeof(alert.name) - 1);
    memset(alert.metric, 'm', sizeof(alert.metric) - 1);
    memset(host, 'h', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    return alertmanager_body(&alert, "resolved", -1.23457e+100, host, "2000-01-01T00:00:00Z", NOW,
                             body, sizeof(body)) > 0
               ? 0
               : 1;
}

static int test_alertmanager_body_that_does_not_fit_is_refused(void)
{
    alert_cfg_t alert = {.metric = "load_1m", .op = ">", .threshold = 1};
    char        body[256];

    return alertmanager_body(&alert, "firing", 5, "my-server", "", NOW, body, sizeof(body)) == -1
               ? 0
               : 1;
}

static int test_alertmanager_body_that_does_not_fit_escaped_is_refused(void)
{
    alert_cfg_t alert = {.metric = "load_1m", .op = ">", .threshold = 1};
    char        body[BODY_SIZE], host[sizeof(((config_t *)0)->title)];

    memset(host, '"', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    return alertmanager_body(&alert, "firing", 5, host, "", NOW, body, sizeof(body)) == -1 ? 0 : 1;
}

static int test_alertmanager_body_whose_summary_does_not_fit_is_refused(void)
{
    alert_cfg_t alert = {.metric = "load_1m", .op = ">", .threshold = 1};
    char        body[16 * BODY_SIZE], host[PIECE_SIZE - 10];

    memset(host, 'h', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    return alertmanager_body(&alert, "firing", 5, host, "", NOW, body, sizeof(body)) == -1 ? 0 : 1;
}

static int test_alertmanager_body_whose_host_does_not_fit_escaped_is_refused(void)
{
    alert_cfg_t alert = {.metric = "load_1m", .op = ">", .threshold = 1};
    char        body[16 * BODY_SIZE], host[PIECE_SIZE / 2 + 100];

    memset(host, '"', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    return alertmanager_body(&alert, "firing", 5, host, "", NOW, body, sizeof(body)) == -1 ? 0 : 1;
}

/* Refused before any connection, and the log says so. */
static int test_alertmanager_webhook_whose_body_does_not_fit_is_not_sent(void)
{
    static config_t cfg;
    alert_cfg_t     alert = {.metric = "load_1m",
                             .op = ">",
                             .threshold = 1,
                             .webhook = "http://127.0.0.1:1/hook",
                             .format = ALERT_FORMAT_ALERTMANAGER};
    char            host[sizeof(cfg.title)], log[512], path[64];

    memset(alert.name, '"', sizeof(alert.name) - 1);
    memset(host, '"', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    snprintf(path, sizeof(path), "/tmp/minimoni-alerts-post-%d.log", (int)getpid());
    fflush(stderr);
    if (!freopen(path, "w", stderr))
        return 1;

    int sent = post_webhook(&cfg, &alert, "firing", 5, "2000-01-01T00:00:00Z", host, "", NOW);

    fflush(stderr);
    if (!freopen("/dev/null", "w", stderr))
        return 1;

    FILE  *file = fopen(path, "r");
    size_t len = file ? fread(log, 1, sizeof(log) - 1, file) : 0;

    if (file)
        fclose(file);
    log[len] = '\0';
    unlink(path);

    return sent == 0 && strstr(log, "webhook body does not fit in 2048 bytes; skipped") &&
                   !strstr(log, "connect")
               ? 0
               : 1;
}

/* --- The slack format's body --- */

static int test_slack_body_holds_the_line_as_its_text(void)
{
    alert_cfg_t alert = {.name = "disk_available_percent < 10",
                         .metric = "disk_available_percent",
                         .op = "<",
                         .threshold = 10};
    char        body[256];
    const char *want = "{\"text\":\"[firing] disk_available_percent 7.7 &lt; 10 on my-server\"}";

    return slack_body(&alert, "firing", 7.7, "my-server", body, sizeof(body)) ==
                       (int)strlen(want) &&
                   strcmp(body, want) == 0
               ? 0
               : 1;
}

static int test_slack_body_escapes_the_whole_line(void)
{
    alert_cfg_t alert = {
        .name = "a<b & c>", .named = 1, .metric = "load_1m", .op = ">", .threshold = 1};
    char        body[256];
    const char *want = "{\"text\":\"[firing] a&lt;b &amp; c&gt;: load_1m 5 &gt; 1 on x&lt;y&gt;\"}";

    return slack_body(&alert, "firing", 5, "x<y>", body, sizeof(body)) == (int)strlen(want) &&
                   strcmp(body, want) == 0
               ? 0
               : 1;
}

static int test_slack_body_escapes_quotes_for_json(void)
{
    alert_cfg_t alert = {.metric = "load_1m", .op = ">", .threshold = 1};
    char        body[256];
    const char *want = "{\"text\":\"[firing] load_1m 5 &gt; 1 on web \\\"01\\\"\"}";

    return slack_body(&alert, "firing", 5, "web \"01\"", body, sizeof(body)) == (int)strlen(want) &&
                   strcmp(body, want) == 0
               ? 0
               : 1;
}

static int test_slack_escape_exact_fit(void)
{
    char out[8];

    memset(out, 'x', sizeof(out));

    return slack_escape(out, sizeof(out), "ab<c") == 7 && strcmp(out, "ab&lt;c") == 0 ? 0 : 1;
}

static int test_slack_escape_one_byte_short(void)
{
    char out[8];

    return slack_escape(out, sizeof(out), "ab<cd") == -1 ? 0 : 1;
}

static int test_slack_body_exact_fit(void)
{
    /* The body takes 58 bytes besides the host. */
    alert_cfg_t alert = {.metric = "disk_available_percent", .op = "<", .threshold = 10};
    char        body[BODY_SIZE], host[BODY_SIZE - 58];

    memset(host, 'a', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    return slack_body(&alert, "firing", 7.7, host, body, sizeof(body)) == BODY_SIZE - 1 ? 0 : 1;
}

static int test_slack_body_that_does_not_fit_with_entities_is_refused(void)
{
    alert_cfg_t alert = {.metric = "disk_available_percent", .op = "<", .threshold = 10};
    char        body[4096], host[500];

    memset(host, '&', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    return slack_body(&alert, "firing", 7.7, host, body, sizeof(body)) == -1 ? 0 : 1;
}

static int test_slack_body_that_does_not_fit_json_escaped_is_refused(void)
{
    alert_cfg_t alert = {.metric = "disk_available_percent", .op = "<", .threshold = 10};
    char        body[4096], host[1100];

    memset(host, '"', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    return slack_body(&alert, "firing", 7.7, host, body, sizeof(body)) == -1 ? 0 : 1;
}

static int test_slack_body_that_does_not_fit_is_refused(void)
{
    alert_cfg_t alert = {.metric = "disk_available_percent", .op = "<", .threshold = 10};
    char        body[32];

    return slack_body(&alert, "firing", 7.7, "my-server", body, sizeof(body)) == -1 ? 0 : 1;
}

/* --- A check: what alerts_evaluate logs, from the alert log's answers --- */

/* The log of one check of an alert named 'a' on `metric` > 1 with no action, so nothing is
 * delivered: the load is `load`, the uptime 100 s, readable or not. */
static const char *check_log(const char *metric, double load, int uptime_valid)
{
    static config_t cfg;
    static char     log[1024];
    db_row_t        row = {.load_1m = load};

    memset(&cfg, 0, sizeof(cfg));
    cfg.alert_count = 1;
    snprintf(cfg.title, sizeof(cfg.title), "host");
    snprintf(cfg.alerts[0].name, sizeof(cfg.alerts[0].name), "a");
    snprintf(cfg.alerts[0].metric, sizeof(cfg.alerts[0].metric), "%s", metric);
    snprintf(cfg.alerts[0].op, sizeof(cfg.alerts[0].op), ">");
    cfg.alerts[0].threshold = 1;

    char path[64];
    snprintf(path, sizeof(path), "/tmp/minimoni-alerts-check-%d.log", (int)getpid());
    fflush(stderr);
    if (!freopen(path, "w", stderr))
        return "";

    alerts_evaluate(NULL, &cfg, &row, 85, 1e9, 1, uptime_valid, 100);
    fflush(stderr);
    if (!freopen("/dev/null", "w", stderr))
        return "";

    FILE  *file = fopen(path, "r");
    size_t len = file ? fread(log, 1, sizeof(log) - 1, file) : 0;
    if (file)
        fclose(file);
    log[len] = '\0';
    unlink(path);

    return log;
}

static int test_check_logs_the_resolve_of_a_firing_alert(void)
{
    g_firing = 1;
    int ok = strstr(check_log("load_1m", 0.5, 1), "alerts: resolved 'a'") != NULL;
    g_firing = 0;

    return ok ? 0 : 1;
}

static int test_check_after_a_failed_lookup_sends_nothing(void)
{
    g_firing = -1;
    int ok = strstr(check_log("load_1m", 0.5, 1), "alerts:") == NULL &&
             strstr(check_log("load_1m", 2, 1), "alerts:") == NULL;
    g_firing = 0;

    return ok ? 0 : 1;
}

static int test_firing_alert_whose_metric_has_no_value_keeps_its_state(void)
{
    g_firing = 1;
    int ok = strstr(check_log("uptime_seconds", 0, 0), "alerts:") == NULL;
    g_firing = 0;

    return ok ? 0 : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(drops_not_trusted),
    T(keeps_success),
    T(propagates_expired),
    T(propagates_bad_server_name),
    T(propagates_empty_chain),
    T(drops_the_server_name),
    T(every_config_metric_has_a_value),
    T(uptime_is_the_one_given),
    T(uptime_unreadable_is_not_valid),
    T(mem_metrics_read_their_fields),
    T(disk_metrics_read_their_fields),
    T(temp_metrics_in_celsius),
    T(temp_metrics_in_fahrenheit),
    T(temp_metrics_without_a_sensor_have_no_value),
    T(net_percents_of_the_link),
    T(net_percents_without_a_rate_have_no_value),
    T(load_metrics_read_their_fields),
    T(load_percents_of_the_cpus),
    T(cpu_steal_reads_its_field),
    T(cpu_steal_has_a_value_only_with_a_sample),
    T(cpu_total_is_user_system_and_steal),
    T(cpu_total_has_a_value_only_with_a_sample),
    T(swap_metrics_read_their_fields),
    T(swap_metrics_without_swap_have_no_value),
    T(swap_io_metrics_without_a_sample_have_no_value),
    T(no_system_store_points_to_ca_file),
    T(fit_printf_exact_fit),
    T(fit_printf_one_byte_short),
    T(parse_url_host_at_max),
    T(parse_url_host_over_max_refused),
    T(parse_url_port_over_max_refused),
    T(reply_2xx_counts),
    T(reply_other_codes_do_not_count),
    T(reply_that_is_not_http_does_not_count),
    T(keep_head_keeps_only_what_fits),
    T(alert_fires_when_its_condition_starts_to_hold),
    T(alert_within_its_cooldown_does_not_fire),
    T(firing_alert_sends_nothing_while_it_holds),
    T(firing_alert_resolves_when_it_stops_holding),
    T(quiet_alert_sends_nothing_while_it_does_not_hold),
    T(text_line_does_not_repeat_the_condition),
    T(text_line_puts_a_configured_name_first),
    T(text_line_of_a_resolve),
    T(text_line_stays_one_line),
    T(text_line_keeps_utf_8),
    T(text_line_that_does_not_fit_is_refused),
    T(text_line_whose_status_does_not_fit_is_refused),
    T(template_drops_the_whitespace_outside_its_strings),
    T(template_keeps_an_escaped_quote_inside_a_string),
    T(template_that_ends_after_a_backslash_stops_there),
    T(template_escapes_each_mark),
    T(template_with_an_unknown_mark_is_refused),
    T(template_with_an_unclosed_mark_is_refused),
    T(template_whose_mark_has_no_value_is_refused),
    T(template_exact_fit),
    T(fingerprint_matches_prometheus),
    T(alertmanager_body_of_a_firing),
    T(alertmanager_body_of_a_firing_starts_now),
    T(alertmanager_body_of_a_resolve_keeps_its_start),
    T(alertmanager_body_of_a_resolve_with_no_start_starts_at_its_end),
    T(alertmanager_body_escapes_the_host_in_the_group_key_twice),
    T(alertmanager_body_escapes_the_name_in_the_group_key_twice),
    T(alertmanager_fingerprint_is_of_the_labels_unescaped),
    T(alertmanager_body_escapes_the_start),
    T(alertmanager_summary_is_escaped),
    T(alertmanager_body_of_the_longest_alert_fits),
    T(alertmanager_body_that_does_not_fit_is_refused),
    T(alertmanager_body_that_does_not_fit_escaped_is_refused),
    T(alertmanager_body_whose_summary_does_not_fit_is_refused),
    T(alertmanager_body_whose_host_does_not_fit_escaped_is_refused),
    T(alertmanager_webhook_whose_body_does_not_fit_is_not_sent),
    T(slack_body_holds_the_line_as_its_text),
    T(slack_body_escapes_the_whole_line),
    T(slack_body_escapes_quotes_for_json),
    T(slack_escape_exact_fit),
    T(slack_escape_one_byte_short),
    T(slack_body_exact_fit),
    T(slack_body_that_does_not_fit_with_entities_is_refused),
    T(slack_body_that_does_not_fit_json_escaped_is_refused),
    T(slack_body_that_does_not_fit_is_refused),
    T(check_logs_the_resolve_of_a_firing_alert),
    T(check_after_a_failed_lookup_sends_nothing),
    T(firing_alert_whose_metric_has_no_value_keeps_its_state),
};

int main(void)
{
    /* Silence parse_url's refusals so the output stays readable. */
    if (!freopen("/dev/null", "w", stderr))
        return 2;

    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
