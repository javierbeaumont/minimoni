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

/* Unit tests for the X.509 policy in src/alerts.c (zero-dependency, no framework).
 * Build with: make test. The module is #included directly.
 *
 * BearSSL is replaced at the link seam, the way unit-http.c replaces civetweb: a fake vtable can
 * report any verdict, where the real engine only produces one per certificate it is handed. That
 * the policy plus real BearSSL actually delivers a webhook is asserted in tests/e2e-webhook.sh. */

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
 * the real ones would drag vendored code into a unit build. */
int json_escape(char *dst, size_t cap, const char *src)
{
    (void)src;
    if (cap)
        dst[0] = '\0';
    return 0;
}

int db_alert_on_cooldown(db_t *db, const char *alert_name, long cooldown_seconds)
{
    (void)db;
    (void)alert_name;
    (void)cooldown_seconds;
    return 1;
}

int db_alert_log_fire(db_t *db, const char *alert_name)
{
    (void)db;
    (void)alert_name;
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
        metric_value(&row, names[i], &cfg, 100.0, 12.5e6, 4, &valid);
        if (!valid)
            return 1;
    }

    return 0;
}

/* Each memory metric reads its own field. */
static int test_mem_metrics_read_their_fields(void)
{
    static const char *const names[] = {"mem_used_mb", "mem_available_mb", "mem_used_percent",
                                        "mem_available_percent"};
    db_row_t                 row;
    memset(&row, 0, sizeof(row));
    row.mem_used_mb = 1.0;
    row.mem_available_mb = 2.0;
    row.mem_used_percent = 3.0;
    row.mem_available_percent = 4.0;

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
    static const char *const names[] = {"disk_used_gb", "disk_available_gb", "disk_used_percent",
                                        "disk_available_percent"};
    db_row_t                 row;
    memset(&row, 0, sizeof(row));
    row.disk_used_gb = 1.0;
    row.disk_available_gb = 2.0;
    row.disk_used_percent = 3.0;
    row.disk_available_percent = 4.0;

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

    return metric_value(&row, m, &cfg, 110.0, 12.5e6, 4, valid);
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
        metric_value(&row, names[i], &cfg, 110.0, 12.5e6, 4, &valid);
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
    double r = metric_value(&row, "net_rx_percent", &cfg, 100.0, 12.5e6, 4, &rx);
    double t = metric_value(&row, "net_tx_percent", &cfg, 100.0, 12.5e6, 4, &tx);

    return near(r, 20.0) && near(t, 80.0) && rx && tx ? 0 : 1;
}

/* Without a rate, as on the first sample, the percents have no value either. */
static int test_net_percents_without_a_rate_have_no_value(void)
{
    config_t cfg = {0};
    db_row_t row;
    memset(&row, 0, sizeof(row));

    int rx = 1, tx = 1;
    metric_value(&row, "net_rx_percent", &cfg, 100.0, 12.5e6, 4, &rx);
    metric_value(&row, "net_tx_percent", &cfg, 100.0, 12.5e6, 4, &tx);

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
        if (metric_value(&row, names[i], &cfg, 100.0, 12.5e6, 4, &valid) != i + 1.0 || !valid)
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
    double l1 = metric_value(&row, "load_1m_percent", &cfg, 100.0, 12.5e6, 4, &v1);
    double l5 = metric_value(&row, "load_5m_percent", &cfg, 100.0, 12.5e6, 4, &v5);
    double l15 = metric_value(&row, "load_15m_percent", &cfg, 100.0, 12.5e6, 4, &v15);

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

static const char *const SWAP_METRICS[] = {"swap_free_mb",      "swap_used_mb", "swap_free_percent",
                                           "swap_used_percent", "swap_in_bps",  "swap_out_bps",
                                           "swap_in_ram",       "swap_out_ram"};

/* Each swap metric reads its own field. */
static int test_swap_metrics_read_their_fields(void)
{
    db_row_t row;
    memset(&row, 0, sizeof(row));
    row.swap_valid = row.swap_io_valid = 1;
    row.swap_free_mb = 1.0;
    row.swap_used_mb = 2.0;
    row.swap_free_percent = 3.0;
    row.swap_used_percent = 4.0;
    row.swap_in_bps = 5.0;
    row.swap_out_bps = 6.0;
    row.swap_in_ram = 7.0;
    row.swap_out_ram = 8.0;

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

    int in = 1, out = 1, in_ram = 1, out_ram = 1, used = 0;
    get_metric_value(&row, "swap_in_bps", &in);
    get_metric_value(&row, "swap_out_bps", &out);
    get_metric_value(&row, "swap_in_ram", &in_ram);
    get_metric_value(&row, "swap_out_ram", &out_ram);
    get_metric_value(&row, "swap_used_percent", &used);

    return !in && !out && !in_ram && !out_ram && used ? 0 : 1;
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

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(drops_not_trusted),
    T(keeps_success),
    T(propagates_expired),
    T(propagates_bad_server_name),
    T(propagates_empty_chain),
    T(drops_the_server_name),
    T(every_config_metric_has_a_value),
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
    T(fit_printf_exact_fit),
    T(fit_printf_one_byte_short),
    T(parse_url_host_at_max),
    T(parse_url_host_over_max_refused),
    T(parse_url_port_over_max_refused),
    T(reply_2xx_counts),
    T(reply_other_codes_do_not_count),
    T(reply_that_is_not_http_does_not_count),
    T(keep_head_keeps_only_what_fits),
};

int main(void)
{
    /* Silence parse_url's refusals so the output stays readable. */
    if (!freopen("/dev/null", "w", stderr))
        return 2;

    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
