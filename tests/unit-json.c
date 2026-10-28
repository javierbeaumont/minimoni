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

/* Unit tests for src/json.c. Zero-dependency, no framework. Build with:
 *   make test
 *
 * json.c is `#include`d directly, together with units.c (the *_convert helpers
 * it calls) and config.c (config_has). Each test returns 0 on pass / 1 on fail;
 * the shared harness lives in runner.h. */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <string.h>

#include "runner.h"

#include "../src/config.c"
#include "../src/json.c"
#include "../src/units.c"

/* --- Fixtures & helpers --- */

/* A fully-valid snapshot; individual tests flip the *_valid flags as needed. */
static db_row_t sample_row(void)
{
    db_row_t r;
    memset(&r, 0, sizeof(r));
    strcpy(r.timestamp, "2026-06-01T00:00:00Z");
    r.load_1m = 1.0;
    r.load_5m = 0.5;
    r.load_15m = 0.25;
    r.cpu_valid = 1;
    r.cpu_user_percent = 10.0;
    r.cpu_system_percent = 5.0;
    r.cpu_idle_percent = 85.0;
    r.mem_total_mb = 1000.0;
    r.mem_used_mb = 750.0;
    r.mem_available_mb = 250.0;
    r.mem_used_percent = 75.0;
    r.mem_available_percent = 25.0;
    r.disk_total_gb = 100.0;
    r.disk_available_gb = 60.0;
    r.disk_used_gb = 40.0;
    r.disk_used_percent = 40.0;
    r.disk_available_percent = 60.0;
    r.temp_valid = 1;
    r.temp_celsius = 50.0;
    r.net_valid = 1;
    r.net_rx_bps = 1024.0;
    r.net_tx_bps = 2048.0;
    r.uptime_seconds = 3600.0;
    return r;
}

/* sample_row with swap: 256 of 1024 MB in use, 4096 B/s in and 8192 B/s out, as db.c's read_row
 * works them out. */
static db_row_t swap_row(void)
{
    db_row_t r = sample_row();
    r.swap_valid = 1;
    r.swap_total_mb = 1024.0;
    r.swap_free_mb = 768.0;
    r.swap_used_mb = 256.0;
    r.swap_used_percent = 25.0;
    r.swap_free_percent = 75.0;
    r.swap_io_valid = 1;
    r.swap_in_bps = 4096.0;
    r.swap_out_bps = 8192.0;
    r.swap_in_ram = 4096.0 * 60.0 * 100.0 / (r.mem_total_mb * 1024.0 * 1024.0);
    r.swap_out_ram = 2.0 * r.swap_in_ram;

    return r;
}

static config_t base_cfg(void)
{
    config_t c;
    config_defaults(&c); /* card_count == 0 -> show-all */
    return c;
}

/* Restrict the visible cards to a single entry. */
static void only_card(config_t *c, const char *name)
{
    strcpy(c->cards[0], name);
    c->card_count = 1;
}

/* Serialize row+cfg into buf exactly as the HTTP handlers do (begin/end wrap).
 * net_speed_mbit is 0 here, so net percent mode falls back to the config value. */
static void emit(char *buf, size_t cap, const db_row_t *r, const config_t *cfg, int num_cores,
                 int tc_valid, double tc)
{
    jbuf_t j;
    jbuf_init(&j, buf, cap);
    jbuf_begin(&j);
    json_serialize_current(&j, r, cfg, num_cores, tc_valid, tc, 0);
    jbuf_end(&j);
}

/* Same, with an explicit detected link speed (Mbit/s). */
static void emit_link(char *buf, size_t cap, const db_row_t *r, const config_t *cfg, int mbit)
{
    jbuf_t j;
    jbuf_init(&j, buf, cap);
    jbuf_begin(&j);
    json_serialize_current(&j, r, cfg, 4, 0, 0.0, mbit);
    jbuf_end(&j);
}

/* Same wrap for json_serialize_point (the /api/metrics short-key serializer). */
static void emit_point(char *buf, size_t cap, const db_row_t *r, const config_t *cfg, int num_cores,
                       int tc_valid, double tc)
{
    jbuf_t j;
    jbuf_init(&j, buf, cap);
    jbuf_begin(&j);
    json_serialize_point(&j, r, cfg, num_cores, tc_valid, tc, 0);
    jbuf_end(&j);
}

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

/* --- Writer (jbuf_*) --- */

static int test_jbuf_real_format(void)
{
    char   b[64];
    jbuf_t j;
    jbuf_init(&j, b, sizeof(b));
    jbuf_begin(&j);
    jbuf_real(&j, "x", 1.5);
    jbuf_end(&j);
    return strcmp(b, "{\"x\":1.5}") == 0 ? 0 : 1;
}

/* JSON has no NaN or Infinity: a value that is not finite goes out as null, which the dashboard
 * already handles, as a dash on a card and a gap in a chart. */
static int real_is_null(double val)
{
    char   b[64];
    jbuf_t j;
    jbuf_init(&j, b, sizeof(b));
    jbuf_begin(&j);
    jbuf_real(&j, "x", val);
    jbuf_end(&j);

    return strcmp(b, "{\"x\":null}") == 0;
}

static int test_jbuf_real_nan_is_null(void) { return real_is_null(NAN) ? 0 : 1; }

static int test_jbuf_real_infinity_is_null(void)
{
    return real_is_null(INFINITY) && real_is_null(-INFINITY) ? 0 : 1;
}

static int test_jbuf_pair_format(void)
{
    char   b[64];
    jbuf_t j;
    jbuf_init(&j, b, sizeof(b));
    jbuf_begin(&j);
    jbuf_pair(&j, "t", 70.0, 90.0);
    jbuf_end(&j);
    return strcmp(b, "{\"t\":[70,90]}") == 0 ? 0 : 1;
}

static int test_jbuf_str_format(void)
{
    char   b[64];
    jbuf_t j;
    jbuf_init(&j, b, sizeof(b));
    jbuf_begin(&j);
    jbuf_str(&j, "k", "v");
    jbuf_end(&j);
    return strcmp(b, "{\"k\":\"v\"}") == 0 ? 0 : 1;
}

static int test_jbuf_sep_inserts_comma(void)
{
    char   b[64];
    jbuf_t j;
    jbuf_init(&j, b, sizeof(b));
    jbuf_begin(&j);
    jbuf_long(&j, "a", 1);
    jbuf_long(&j, "b", 2);
    jbuf_end(&j);
    return strcmp(b, "{\"a\":1,\"b\":2}") == 0 ? 0 : 1;
}

static int test_jbuf_truncates_without_overflow(void)
{
    char   b[8];
    jbuf_t j;
    jbuf_init(&j, b, sizeof(b));
    jbuf_begin(&j);
    jbuf_str(&j, "key", "a value far larger than the buffer");
    jbuf_end(&j);
    /* the oversized field is dropped whole; output never exceeds cap and stays
     * NUL-terminated */
    return (j.pos < sizeof(b) && b[j.pos] == '\0') ? 0 : 1;
}

static void put_real(jbuf_t *j, const char *k) { jbuf_real(j, k, 2.5); }
static void put_long(jbuf_t *j, const char *k) { jbuf_long(j, k, 25); }
static void put_null(jbuf_t *j, const char *k) { jbuf_null(j, k); }
static void put_pair(jbuf_t *j, const char *k) { jbuf_pair(j, k, 70.0, 90.0); }
static void put_str(jbuf_t *j, const char *k) { jbuf_str(j, k, "v"); }

/* The writers but jbuf_str once wrote the comma first: {"a":1,}. */
static int test_dropped_field_leaves_no_comma(void)
{
    void (*const put[])(jbuf_t *, const char *) = {put_real, put_long, put_null, put_pair, put_str};

    for (size_t i = 0; i < sizeof(put) / sizeof(put[0]); i++) {
        char   b[12];
        jbuf_t j;
        jbuf_init(&j, b, sizeof(b));
        jbuf_begin(&j);
        jbuf_long(&j, "a", 1);
        put[i](&j, "bbbbbbbbbbbb");
        jbuf_end(&j);

        if (strcmp(b, "{\"a\":1}") != 0 || !j.overflow)
            return 1;
    }

    return 0;
}

static int test_overflow_marks_the_buffer(void)
{
    char   fits[64], unclosed[7], escaped[1024], quotes[600];
    jbuf_t j;
    memset(quotes, '"', sizeof(quotes) - 1);
    quotes[sizeof(quotes) - 1] = '\0';

    jbuf_init(&j, fits, sizeof(fits));
    jbuf_begin(&j);
    jbuf_long(&j, "a", 1);
    jbuf_end(&j);
    int ok = !j.overflow;

    jbuf_init(&j, unclosed, sizeof(unclosed));
    jbuf_begin(&j);
    jbuf_long(&j, "a", 1);
    jbuf_end(&j);
    ok = ok && j.overflow;

    jbuf_init(&j, escaped, sizeof(escaped));
    jbuf_str(&j, "t", quotes);
    ok = ok && j.overflow;

    return ok ? 0 : 1;
}

/* --- Serializer: card gating --- */

static int test_show_all_emits_every_group(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int cpu = has(b, "\"load_1m\"") && has(b, "\"cpu_user_percent\"");
    int memory = has(b, "\"mem_available\"") && has(b, "\"swap_free\"") && has(b, "\"swap_in\"");
    int others = has(b, "\"disk_percent\"") && has(b, "\"temp\"") && has(b, "\"net_rx\"") &&
                 has(b, "\"uptime_seconds\"");

    return cpu && memory && others ? 0 : 1;
}

static int test_gating_single_card(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    only_card(&c, "cpu_load");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    /* only the load group is emitted; the rest is gated out */
    return (has(b, "\"load_1m\"") && !has(b, "\"mem_available\"") &&
            !has(b, "\"cpu_user_percent\"") && !has(b, "\"net_rx\""))
               ? 0
               : 1;
}

static int test_hide_all_emits_no_metrics(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    c.card_count = -1; /* explicit empty list -> hide all */
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    /* no metric fields, but the config block (theme, units) still ships */
    return (!has(b, "\"load_1m\"") && !has(b, "\"mem_available\"") && !has(b, "\"temp\"") &&
            has(b, "\"theme\""))
               ? 0
               : 1;
}

/* --- Serializer: thresholds --- */

static int test_thresh_load_abs_scales_with_cores(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.cpu_load_card_unit, "abs");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0); /* [0.75*4, 1.0*4] */
    return has(b, "\"thresh_load\":[3,4]") ? 0 : 1;
}

static int test_thresh_load_abs_eight_cores(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.cpu_load_card_unit, "abs");
    emit(b, sizeof(b), &r, &c, 8, 1, 100.0); /* adapts to the core count */
    return has(b, "\"thresh_load\":[6,8]") ? 0 : 1;
}

static int test_thresh_load_percent_fixed(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.cpu_load_card_unit, "%");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return has(b, "\"thresh_load\":[70,90]") ? 0 : 1;
}

static int test_thresh_cpu_mem_disk_fixed(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"thresh_cpu\":[70,90]") && has(b, "\"thresh_mem\":[70,90]") &&
            has(b, "\"thresh_disk\":[80,90]"))
               ? 0
               : 1;
}

static int test_thresh_swap_fixed(void)
{
    char     b[2048];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return has(b, "\"thresh_swap\":[80,95]") ? 0 : 1;
}

/* 1 and 5 % of the RAM per minute: on sample_row's 1000 MB, 174763 and 873813 B/s. */
static int test_thresh_swap_io_in_ram_or_bytes(void)
{
    char     ram[2048], bytes[2048];
    db_row_t r = swap_row();
    config_t c = base_cfg(); /* swap_io_card_unit defaults to "bytes" */
    emit(bytes, sizeof(bytes), &r, &c, 4, 1, 100.0);
    strcpy(c.swap_io_card_unit, "ram");
    emit(ram, sizeof(ram), &r, &c, 4, 1, 100.0);

    int in_ram = has(ram, "\"thresh_swap_io\":[1,5]");
    int in_bytes = has(bytes, "\"thresh_swap_io\":[174763,873813]");

    return in_ram && in_bytes ? 0 : 1;
}

static int test_thresh_temp_from_critical(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.temp_card_unit, "c");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0); /* trip 100 -> [trip-20, trip-10] */
    return has(b, "\"thresh_temp\":[80,90]") ? 0 : 1;
}

static int test_thresh_temp_fallback_no_critical(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.temp_card_unit, "c");
    emit(b, sizeof(b), &r, &c, 4, 0, 0.0); /* no sysfs trip -> [70,80] degC */
    return has(b, "\"thresh_temp\":[70,80]") ? 0 : 1;
}

/* --- Serializer: null handling & unit omission --- */

static int test_temp_null_when_sensor_invalid(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    r.temp_valid = 0;
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return has(b, "\"temp\":null") ? 0 : 1;
}

static int test_net_null_when_invalid(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    r.net_valid = 0;
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"net_rx\":null") && has(b, "\"net_tx\":null")) ? 0 : 1;
}

static int test_net_percent_uses_detected_link(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    r.net_valid = 1;
    r.net_rx_bps = 6250000.0; /* half of a 100 Mbit/s link (12.5 MB/s) */
    config_t c = base_cfg();
    strcpy(c.net_card_unit, "%");
    emit_link(b, sizeof(b), &r, &c, 100);
    return has(b, "\"net_rx\":50") ? 0 : 1;
}

static int test_net_percent_defaults_to_gbe(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    r.net_valid = 1;
    r.net_rx_bps = 12500000.0; /* 10% of the assumed 1 GbE */
    config_t c = base_cfg();
    strcpy(c.net_card_unit, "%");
    emit_link(b, sizeof(b), &r, &c, 0); /* nothing configured, nothing detected */
    return has(b, "\"net_rx\":10") ? 0 : 1;
}

static int test_net_percent_configured_max_wins(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    r.net_valid = 1;
    r.net_rx_bps = 12500000.0;
    config_t c = base_cfg();
    strcpy(c.net_card_unit, "%");
    c.net_max_speed = 100;                 /* the uplink, slower than the NIC */
    emit_link(b, sizeof(b), &r, &c, 1000); /* sysfs says 1 GbE; config must win */
    return has(b, "\"net_rx\":100") ? 0 : 1;
}

static int test_net_thresholds_percent(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    r.net_valid = 1;
    config_t c = base_cfg();
    strcpy(c.net_card_unit, "%");
    emit_link(b, sizeof(b), &r, &c, 1000);
    return (has(b, "\"thresh_net\":[85,98]")) ? 0 : 1;
}

/* Outside "%" everything net ships in base bytes/s, thresholds included, so the
 * dashboard scales values and lines with one factor. */
static int test_net_thresholds_absolute(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    r.net_valid = 1;
    config_t c = base_cfg();
    strcpy(c.net_card_unit, "bytes");
    c.net_max_speed = 100; /* 100 Mbit = 12500000 B/s; 85% and 98% of it */
    emit_link(b, sizeof(b), &r, &c, 0);
    return has(b, "\"thresh_net\":[1.0625e+07,1.225e+07]") ? 0 : 1;
}

/* --- json_escape (RFC 8259) --- */

static int esc_is(const char *in, const char *want)
{
    char out[64];
    return json_escape(out, sizeof(out), in) == (int)strlen(want) && strcmp(out, want) == 0;
}

static int test_escape_mandatory_pair(void)
{
    /* A quote would end the string early, a backslash would eat what follows. */
    return (esc_is("a\"b", "a\\\"b") && esc_is("C:\\prod", "C:\\\\prod")) ? 0 : 1;
}

static int test_escape_short_forms(void)
{
    return esc_is("a\nb\rc\td\be\ff", "a\\nb\\rc\\td\\be\\ff") ? 0 : 1;
}

/* No short form: \u00XX, lowercase hex. */
static int test_escape_other_controls(void)
{
    return (esc_is("\x01", "\\u0001") && esc_is("\x1f", "\\u001f")) ? 0 : 1;
}

static int test_escape_passes_utf8_through(void)
{
    return (esc_is("plain text", "plain text") && esc_is("\xc3\xa9\xc3\xb1", "\xc3\xa9\xc3\xb1"))
               ? 0
               : 1;
}

static int test_escape_reports_overflow(void)
{
    char out[4];
    return (json_escape(out, sizeof(out), "abcdef") < 0 && json_escape(out, 0, "a") < 0) ? 0 : 1;
}

static int test_escape_empty(void)
{
    char out[8];
    return (json_escape(out, sizeof(out), "") == 0 && out[0] == '\0') ? 0 : 1;
}

static int test_str_field_escapes_the_value(void)
{
    char   b[256];
    jbuf_t j;
    jbuf_init(&j, b, sizeof(b));
    jbuf_begin(&j);
    jbuf_str(&j, "title", "web\"01");
    jbuf_end(&j);
    return strcmp(b, "{\"title\":\"web\\\"01\"}") == 0 ? 0 : 1;
}

/* A dangling comma is as broken as the unescaped quote this replaced. */
static int test_str_field_drops_without_dangling_comma(void)
{
    char   b[512];
    char   huge[600];
    jbuf_t j;
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    jbuf_init(&j, b, sizeof(b));
    jbuf_begin(&j);
    jbuf_str(&j, "theme", "auto");
    jbuf_str(&j, "title", huge);
    jbuf_end(&j);
    return strcmp(b, "{\"theme\":\"auto\"}") == 0 ? 0 : 1;
}

/* charts and cards reach the array unchecked: config copies them verbatim. */
static int test_array_elements_are_escaped(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    snprintf(c.charts[0], sizeof(c.charts[0]), "%s", "my\"chart");
    c.chart_count = 1;
    snprintf(c.cards[0], sizeof(c.cards[0]), "%s", "back\\slash");
    c.card_count = 1;
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"charts\":[\"my\\\"chart\"]") && has(b, "\"cards\":[\"back\\\\slash\"]")) ? 0
                                                                                               : 1;
}

/* --- units envelope: chosen from the window, shared by card and chart --- */

static void emit_units(char *b, size_t n, db_row_t *rows, int cnt, config_t *c)
{
    jbuf_t j;
    jbuf_init(&j, b, n);
    jbuf_begin(&j);
    json_serialize_units(&j, rows, cnt, c);
    jbuf_end(&j);
}

static int test_units_smallest_that_fits(void)
{
    char     b[512];
    db_row_t rows[2];
    memset(rows, 0, sizeof(rows));
    rows[0].mem_used_mb = 300.0;
    rows[1].mem_available_mb = 1748.0; /* under 9999: stays in MB */
    rows[0].disk_used_gb = 20.0;
    rows[1].net_valid = 1;
    rows[1].net_tx_bps = 5242880.0; /* 5 MB/s = 5120 KB/s, still four digits */
    config_t c = base_cfg();
    emit_units(b, sizeof(b), rows, 2, &c);
    return (has(b, "\"mem\":{\"sym\":\"MB\",\"mul\":1,\"div\":1}") &&
            has(b, "\"disk\":{\"sym\":\"GB\",\"mul\":1,\"div\":1}") &&
            has(b, "\"net\":{\"sym\":\"KB/s\",\"mul\":1,\"div\":1024}"))
               ? 0
               : 1;
}

/* Past the five digits the card can show, the next unit up takes over. */
static int test_units_step_up_past_the_cap(void)
{
    char     b[512];
    db_row_t rows[1];
    memset(rows, 0, sizeof(rows));
    rows[0].mem_used_mb = 32768.0; /* 32 GB */
    config_t c = base_cfg();
    emit_units(b, sizeof(b), rows, 1, &c);
    return has(b, "\"mem\":{\"sym\":\"GB\",\"mul\":1,\"div\":1024}") ? 0 : 1;
}

/* No data, no unit: nothing is on screen to give it a scale. */
static int test_units_absent_without_data(void)
{
    char     b[512];
    db_row_t rows[1];
    memset(rows, 0, sizeof(rows));
    config_t c = base_cfg();
    emit_units(b, sizeof(b), rows, 1, &c);
    return (has(b, "\"units\":{}") && !has(b, "\"mem\"")) ? 0 : 1;
}

/* A "%" metric has no magnitude to choose. */
static int test_units_skips_percent_metrics(void)
{
    char     b[512];
    db_row_t rows[1];
    memset(rows, 0, sizeof(rows));
    rows[0].mem_used_mb = 500.0;
    rows[0].disk_used_gb = 20.0;
    config_t c = base_cfg();
    strcpy(c.memory_chart_unit, "%");
    emit_units(b, sizeof(b), rows, 1, &c);
    return (!has(b, "\"mem\"") && has(b, "\"disk\"")) ? 0 : 1;
}

/* Beyond the first rung the divisor is factor^(step + base), the arithmetic a
 * plain off-by-one would survive while every reading stayed one rung low. */
static int test_units_step_beyond_the_base_rung(void)
{
    char     b[512];
    db_row_t rows[1];
    memset(rows, 0, sizeof(rows));
    rows[0].net_valid = 1;
    rows[0].net_tx_bps = 52428800.0; /* 50 MB/s: 51200 KB/s needs the next rung */
    config_t c = base_cfg();
    emit_units(b, sizeof(b), rows, 1, &c);
    return has(b, "\"net\":{\"sym\":\"MB/s\",\"mul\":1,\"div\":1.04858e+06}") ? 0 : 1;
}

/* The first sample after start has no delta, so its net counters are noise: it
 * must not set the window's magnitude. */
static int test_units_ignore_rows_without_a_net_delta(void)
{
    char     b[512];
    db_row_t rows[2];
    memset(rows, 0, sizeof(rows));
    rows[0].net_valid = 0;
    rows[0].net_tx_bps = 9.0e9; /* garbage from the priming sample */
    rows[1].net_valid = 1;
    rows[1].net_tx_bps = 2048.0;
    config_t c = base_cfg();
    emit_units(b, sizeof(b), rows, 2, &c);
    return has(b, "\"net\":{\"sym\":\"KB/s\",\"mul\":1,\"div\":1024}") ? 0 : 1;
}

static int test_units_bits_ladder(void)
{
    char     b[512];
    db_row_t rows[1];
    memset(rows, 0, sizeof(rows));
    rows[0].net_valid = 1;
    rows[0].net_tx_bps = 125000.0; /* x8 = 1e6 bit/s = 1000 Kbps */
    config_t c = base_cfg();
    strcpy(c.net_chart_unit, "bits");
    emit_units(b, sizeof(b), rows, 1, &c);
    return has(b, "\"net\":{\"sym\":\"Kbps\",\"mul\":8,\"div\":1000}") ? 0 : 1;
}

/* Only the rows with swap set its magnitude. */
static int test_units_swap_ignores_rows_without_swap(void)
{
    char     b[512];
    db_row_t rows[2];
    memset(rows, 0, sizeof(rows));
    rows[0].swap_used_mb = rows[0].swap_free_mb = 9.0e9; /* not read: swap_valid is 0 */
    rows[1].swap_valid = 1;
    rows[1].swap_used_mb = rows[1].swap_free_mb = 100.0;
    config_t c = base_cfg();
    emit_units(b, sizeof(b), rows, 2, &c);

    return has(b, "\"swap\":{\"sym\":\"MB\",\"mul\":1,\"div\":1}") ? 0 : 1;
}

/* The free line is drawn too, so it sets the magnitude when it is the larger one. */
static int test_units_swap_free_sets_the_magnitude(void)
{
    char     b[512];
    db_row_t rows[1];
    memset(rows, 0, sizeof(rows));
    rows[0].swap_valid = 1;
    rows[0].swap_used_mb = 100.0;
    rows[0].swap_free_mb = 12000.0; /* five digits */
    config_t c = base_cfg();
    emit_units(b, sizeof(b), rows, 1, &c);

    return has(b, "\"swap\":{\"sym\":\"GB\",\"mul\":1,\"div\":1024}") ? 0 : 1;
}

static int test_units_swap_absent_without_swap(void)
{
    char     b[512];
    db_row_t rows[1];
    memset(rows, 0, sizeof(rows));
    rows[0].mem_used_mb = 500.0;
    config_t c = base_cfg();
    emit_units(b, sizeof(b), rows, 1, &c);

    return (has(b, "\"mem\"") && !has(b, "\"swap\"")) ? 0 : 1;
}

static int test_units_swap_skipped_in_percent(void)
{
    char     b[512];
    db_row_t rows[1];
    memset(rows, 0, sizeof(rows));
    rows[0].swap_valid = 1;
    rows[0].swap_used_mb = 256.0;
    rows[0].swap_free_mb = 768.0;
    config_t c = base_cfg();
    strcpy(c.swap_chart_unit, "%");
    emit_units(b, sizeof(b), rows, 1, &c);

    return !has(b, "\"swap\"") ? 0 : 1;
}

/* Only the rows with a swap I/O sample set its magnitude, in bytes/s. */
static int test_units_swap_io_from_rows_with_a_sample(void)
{
    char     b[512];
    db_row_t rows[2];
    memset(rows, 0, sizeof(rows));
    rows[0].swap_in_bps = 9.0e12; /* not read: swap_io_valid is 0 */
    rows[1].swap_io_valid = 1;
    rows[1].swap_in_bps = 2.0e6; /* 1953 KB/s */
    config_t c = base_cfg();
    emit_units(b, sizeof(b), rows, 2, &c);

    return has(b, "\"swap_io\":{\"sym\":\"KB/s\",\"mul\":1,\"div\":1024}") ? 0 : 1;
}

static int test_units_swap_io_skipped_in_ram(void)
{
    char     b[512];
    db_row_t rows[1];
    memset(rows, 0, sizeof(rows));
    rows[0].swap_io_valid = 1;
    rows[0].swap_in_bps = 2.0e6;
    config_t c = base_cfg();
    strcpy(c.swap_io_chart_unit, "ram");
    emit_units(b, sizeof(b), rows, 1, &c);

    return !has(b, "\"swap_io\"") ? 0 : 1;
}

static int test_largest_units_fit_their_buffer(void)
{
    db_row_t rows[1];
    memset(rows, 0, sizeof(rows));
    rows[0].mem_used_mb = rows[0].disk_used_gb = 1.0e18;
    rows[0].swap_valid = rows[0].swap_io_valid = rows[0].net_valid = 1;
    rows[0].swap_used_mb = rows[0].swap_free_mb = rows[0].net_rx_bps = 1.0e18;
    rows[0].swap_in_bps = 1.0e18;
    config_t c = base_cfg(); /* net in bytes: the longer divisor */

    char   buf[JSON_UNITS_CAP];
    jbuf_t j;
    jbuf_init(&j, buf, sizeof(buf));
    j.comma = 0;
    json_serialize_units(&j, rows, 1, &c);

    int widest = has(buf, "\"swap\":{\"sym\":\"TB\"") && has(buf, "\"sym\":\"PB\"") &&
                 has(buf, "\"swap_io\":{\"sym\":\"GB/s\"");

    return !j.overflow && widest ? 0 : 1;
}

/* --- charts/cards echo: the three visibility states --- */

static int test_charts_echo_null_by_default(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 0, 0.0);
    return (has(b, "\"charts\":null") && has(b, "\"cards\":null")) ? 0 : 1;
}

static int test_charts_echo_empty_when_hidden(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    c.chart_count = -1; /* explicit empty list: hide all */
    c.card_count = -1;
    emit(b, sizeof(b), &r, &c, 4, 0, 0.0);
    return (has(b, "\"charts\":[]") && has(b, "\"cards\":[]")) ? 0 : 1;
}

static int test_charts_echo_list(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.charts[0], "cpu_load");
    strcpy(c.charts[1], "memory");
    c.chart_count = 2;
    strcpy(c.cards[0], "temp");
    c.card_count = 1;
    emit(b, sizeof(b), &r, &c, 4, 0, 0.0);
    return (has(b, "\"charts\":[\"cpu_load\",\"memory\"]") && has(b, "\"cards\":[\"temp\"]")) ? 0
                                                                                              : 1;
}

static int test_cpu_null_when_invalid(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    r.cpu_valid = 0;
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return has(b, "\"cpu_user_percent\":null") ? 0 : 1;
}

/* In "%" just the available share, so a public dashboard shows no size. */
static int test_mem_card_pct_sends_the_available_share_alone(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg(); /* memory_card_unit defaults to "%" */
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int share = has(b, "\"mem_available\":25,");
    int no_size = !has(b, "\"mem_total\"") && !has(b, "\"mem_used\"") && !has(b, "\"mem_percent\"");

    return share && no_size ? 0 : 1;
}

/* In "auto" the total and available the kernel gives; the dashboard computes the rest. */
static int test_mem_card_auto_sends_total_and_available(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.memory_card_unit, "auto");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int sizes = has(b, "\"mem_total\":1000") && has(b, "\"mem_available\":250");
    int no_used = !has(b, "\"mem_used\"") && !has(b, "\"mem_percent\"");

    return sizes && no_used ? 0 : 1;
}

static int test_disk_absolute_emits_used(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.disk_card_unit, "auto");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"disk_used\"") && has(b, "\"disk_percent\"")) ? 0 : 1;
}

/* In "%" just the free share, so a public dashboard shows no size. */
static int test_swap_card_pct_sends_the_free_share_alone(void)
{
    char     b[2048];
    db_row_t r = swap_row();
    config_t c = base_cfg(); /* swap_card_unit defaults to "%" */
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int free_share = has(b, "\"swap_free\":75");
    int no_size = !has(b, "\"swap_total\"") && !has(b, "\"swap_used");

    return free_share && no_size ? 0 : 1;
}

/* In "auto" the total and free the kernel gives; the dashboard computes the rest. */
static int test_swap_card_auto_sends_total_and_free(void)
{
    char     b[2048];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    strcpy(c.swap_card_unit, "auto");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int sizes = has(b, "\"swap_total\":1024") && has(b, "\"swap_free\":768");
    int no_used = !has(b, "\"swap_used");

    return sizes && no_used ? 0 : 1;
}

/* No swap: the card stays on screen with a dash, so its fields are null, not missing. */
static int test_swap_null_without_swap(void)
{
    char     pct[2048], abs[2048];
    db_row_t r = sample_row(); /* swap_valid 0 */
    config_t c = base_cfg();
    emit(pct, sizeof(pct), &r, &c, 4, 1, 100.0);
    strcpy(c.swap_card_unit, "auto");
    emit(abs, sizeof(abs), &r, &c, 4, 1, 100.0);

    int pct_null = has(pct, "\"swap_free\":null") && !has(pct, "\"swap_total\"");
    int abs_null = has(abs, "\"swap_total\":null") && has(abs, "\"swap_free\":null");

    return pct_null && abs_null ? 0 : 1;
}

static int test_swap_gated_out_by_cards(void)
{
    char     b[2048];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    only_card(&c, "memory");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int no_swap = !has(b, "\"swap_free\"") && !has(b, "\"thresh_swap\"");

    return no_swap && has(b, "\"mem_available\"") ? 0 : 1;
}

static int test_swap_units_echoed(void)
{
    char     b[2048];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return (has(b, "\"swap_card_unit\":\"%\"") && has(b, "\"swap_chart_unit\":\"auto\"")) ? 0 : 1;
}

static int test_swap_io_card_in_bytes(void)
{
    char     b[2048];
    db_row_t r = swap_row();
    config_t c = base_cfg(); /* swap_io_card_unit defaults to "bytes" */
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return has(b, "\"swap_in\":4096") && has(b, "\"swap_out\":8192") ? 0 : 1;
}

/* In "ram", % of the RAM per minute: 4096 B/s on 1000 MB is 0.0234 %. */
static int test_swap_io_card_in_ram(void)
{
    char     b[2048];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    strcpy(c.swap_io_card_unit, "ram");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return has(b, "\"swap_in\":0.02344") && has(b, "\"swap_out\":0.04688") ? 0 : 1;
}

/* No sample yet, or no swap: the card stays on screen with a dash. */
static int test_swap_io_null_without_a_sample(void)
{
    char     b[2048];
    db_row_t r = sample_row(); /* swap_io_valid 0 */
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return has(b, "\"swap_in\":null") && has(b, "\"swap_out\":null") ? 0 : 1;
}

static int test_swap_io_gated_out_by_cards(void)
{
    char     b[2048];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    only_card(&c, "memory");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int no_swap_io = !has(b, "\"swap_in\"") && !has(b, "\"thresh_swap_io\"");

    return no_swap_io && has(b, "\"mem_available\"") ? 0 : 1;
}

static int test_swap_io_units_echoed(void)
{
    char     b[2048];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int card = has(b, "\"swap_io_card_unit\":\"bytes\"");
    int chart = has(b, "\"swap_io_chart_unit\":\"bytes\"");

    return card && chart ? 0 : 1;
}

/* --- Serializer: history point (/api/metrics, short keys, chart units) --- */

static int test_point_emits_short_keys(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"t\":") && has(b, "\"l1\":") && has(b, "\"cu\":") && has(b, "\"ma\":") &&
            has(b, "\"dp\":") && has(b, "\"nr\":") && has(b, "\"up\":"))
               ? 0
               : 1;
}

static int test_point_temp_shown_when_chart_enabled(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg(); /* chart_count 0 -> show all charts */
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return has(b, "\"tp\":") ? 0 : 1;
}

static int test_point_temp_gated_out_by_charts(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.charts[0], "cpu_load");
    c.chart_count = 1; /* temp not listed -> tp omitted */
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return !has(b, "\"tp\"") ? 0 : 1;
}

static int test_point_gates_excluded_charts(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.charts[0], "cpu_load");
    c.chart_count = 1; /* only cpu_load charted -> the other groups gated out */
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"l1\":") && !has(b, "\"ma\"") && !has(b, "\"cu\"") && !has(b, "\"nr\"")) ? 0
                                                                                              : 1;
}

static int test_point_hides_all_charts(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    c.chart_count = -1; /* explicit empty charts list -> every chart group gated out */
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int no_chart = !has(b, "\"l1\"") && !has(b, "\"cu\"") && !has(b, "\"ma\"") &&
                   !has(b, "\"sf\"") && !has(b, "\"si\"") && !has(b, "\"dp\"") &&
                   !has(b, "\"tp\"") && !has(b, "\"nr\"");

    return no_chart && has(b, "\"t\":") && has(b, "\"up\":") ? 0 : 1;
}

static int test_point_gates_net_only(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.charts[0], "net");
    c.chart_count = 1; /* only net charted -> gating is symmetric across groups */
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"nr\":") && has(b, "\"nt\":") && !has(b, "\"l1\"") && !has(b, "\"cu\"") &&
            !has(b, "\"ma\""))
               ? 0
               : 1;
}

static int test_point_temp_null_when_invalid(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    r.temp_valid = 0;
    config_t c = base_cfg();
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return has(b, "\"tp\":null") ? 0 : 1;
}

static int test_point_cpu_null_when_invalid(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    r.cpu_valid = 0;
    config_t c = base_cfg();
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"cu\":null") && has(b, "\"ci\":null")) ? 0 : 1;
}

static int test_point_net_null_when_invalid(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    r.net_valid = 0;
    config_t c = base_cfg();
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"nr\":null") && has(b, "\"nt\":null")) ? 0 : 1;
}

static int test_point_mem_pct_sends_the_available_share_alone(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.memory_chart_unit, "%");
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int share = has(b, "\"ma\":25,");
    int no_size = !has(b, "\"mt\"") && !has(b, "\"mu\"") && !has(b, "\"mp\"");

    return share && no_size ? 0 : 1;
}

static int test_point_mem_auto_sends_total_and_available(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg(); /* memory_chart_unit defaults to "auto" */
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    int sizes = has(b, "\"mt\":1000") && has(b, "\"ma\":250");
    int no_used = !has(b, "\"mu\"") && !has(b, "\"mp\"");

    return sizes && no_used ? 0 : 1;
}

static int test_point_swap_auto_sends_total_and_free(void)
{
    char     b[1024];
    db_row_t r = swap_row();
    config_t c = base_cfg(); /* swap_chart_unit defaults to "auto" */
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return has(b, "\"st\":1024") && has(b, "\"sf\":768") ? 0 : 1;
}

static int test_point_swap_pct_sends_the_free_share_alone(void)
{
    char     b[1024];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    strcpy(c.swap_chart_unit, "%");
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return has(b, "\"sf\":75") && !has(b, "\"st\"") ? 0 : 1;
}

/* No swap: the chart stays on screen and empty. */
static int test_point_swap_null_without_swap(void)
{
    char     b[1024];
    db_row_t r = sample_row(); /* swap_valid 0 */
    config_t c = base_cfg();
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return (has(b, "\"st\":null") && has(b, "\"sf\":null")) ? 0 : 1;
}

static int test_point_swap_gated_out_by_charts(void)
{
    char     b[1024];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    strcpy(c.charts[0], "memory");
    c.chart_count = 1;
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return (has(b, "\"ma\":") && !has(b, "\"st\"") && !has(b, "\"sf\"")) ? 0 : 1;
}

static int test_point_swap_io_in_bytes(void)
{
    char     b[1024];
    db_row_t r = swap_row();
    config_t c = base_cfg(); /* swap_io_chart_unit defaults to "bytes" */
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return has(b, "\"si\":4096") && has(b, "\"so\":8192") ? 0 : 1;
}

static int test_point_swap_io_in_ram(void)
{
    char     b[1024];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    strcpy(c.swap_io_chart_unit, "ram");
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return has(b, "\"si\":0.02344") && has(b, "\"so\":0.04688") ? 0 : 1;
}

/* No sample yet, or no swap: the chart stays on screen and empty. */
static int test_point_swap_io_null_without_a_sample(void)
{
    char     b[1024];
    db_row_t r = sample_row(); /* swap_io_valid 0 */
    config_t c = base_cfg();
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return has(b, "\"si\":null") && has(b, "\"so\":null") ? 0 : 1;
}

static int test_point_swap_io_gated_out_by_charts(void)
{
    char     b[1024];
    db_row_t r = swap_row();
    config_t c = base_cfg();
    strcpy(c.charts[0], "memory");
    c.chart_count = 1;
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);

    return has(b, "\"ma\":") && !has(b, "\"si\"") ? 0 : 1;
}

/* Every chart in the units that send the most fields, each value at its widest. */
static int test_largest_point_fits_its_buffer(void)
{
    db_row_t r = sample_row();
    double   w = -1.797e308;
    r.load_1m = r.load_5m = r.load_15m = w;
    r.cpu_user_percent = r.cpu_system_percent = r.cpu_idle_percent = w;
    r.mem_total_mb = r.mem_used_mb = r.mem_available_mb = w;
    r.mem_used_percent = r.mem_available_percent = w;
    r.swap_valid = r.swap_io_valid = 1;
    r.swap_total_mb = r.swap_free_mb = r.swap_in_bps = r.swap_out_bps = w;
    r.disk_total_gb = r.disk_available_gb = r.disk_used_gb = w;
    r.disk_used_percent = r.disk_available_percent = w;
    r.temp_celsius = r.net_rx_bps = r.net_tx_bps = r.uptime_seconds = w;
    config_t c = base_cfg();

    char   buf[JSON_POINT_CAP];
    jbuf_t j;
    jbuf_init(&j, buf, sizeof(buf));
    jbuf_begin(&j);
    json_serialize_point(&j, &r, &c, 1 << 20, 1, w, 1000000);
    jbuf_end(&j);

    return !j.overflow && has(buf, "\"sf\":-1.797e+308") ? 0 : 1;
}

/* --- The largest /api/current a config can produce --- */

/* `n` control characters as TOML escapes, each six bytes once escaped in JSON. */
static char *ctl(char *out, int n)
{
    out[0] = '\0';
    for (int i = 0; i < n; i++)
        strcat(out, "\\u0001");

    return out;
}

/* Every list full; cards start with the known names. */
static void largest_toml(char *out, size_t cap, const char *title, const char *chart,
                         const char *card)
{
    int known = 0;
    while (CARD_NAMES[known])
        known++;

    snprintf(out, cap,
             "[dashboard]\ntitle = \"%s\"\nmemory_card_unit = \"auto\"\nswap_card_unit = \"auto\"\n"
             "disk_card_unit = \"auto\"\n"
             "ranges = [\"000120m\", \"000120m\", \"000120m\", \"000120m\", \"000120m\", "
             "\"000120m\", \"000120m\", \"000120m\"]\ncharts = [",
             title);
    for (int i = 0; i < MAX_CHARTS; i++)
        snprintf(out + strlen(out), cap - strlen(out), "%s\"%s\"", i ? ", " : "", chart);

    snprintf(out + strlen(out), cap - strlen(out), "]\ncards = [");
    for (int i = 0; i < MAX_CARDS; i++)
        snprintf(out + strlen(out), cap - strlen(out), "%s\"%s\"", i ? ", " : "",
                 i < known ? CARD_NAMES[i] : card);
    snprintf(out + strlen(out), cap - strlen(out), "]\n");
}

/* 1 if /api/current for `toml` overflows, every metric at its widest; -1 if it fails to load. */
static int current_overflows(const char *toml)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/minimoni-json-%d.toml", (int)getpid());

    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(toml, f);
    fclose(f);

    config_t cfg;
    config_defaults(&cfg);
    int rc = config_load(&cfg, path);
    unlink(path);

    if (rc != 0)
        return -1;

    db_row_t r = sample_row();
    double   w = -1.797e308;
    r.load_1m = r.load_5m = r.load_15m = w;
    r.cpu_user_percent = r.cpu_system_percent = r.cpu_idle_percent = w;
    r.mem_total_mb = r.mem_used_mb = r.mem_available_mb = w;
    r.mem_used_percent = r.mem_available_percent = w;
    r.swap_valid = r.swap_io_valid = 1;
    r.swap_total_mb = r.swap_free_mb = r.swap_free_percent = w;
    r.swap_in_bps = r.swap_out_bps = w;
    r.disk_total_gb = r.disk_available_gb = r.disk_used_gb = w;
    r.disk_used_percent = r.disk_available_percent = w;
    r.temp_celsius = r.net_rx_bps = r.net_tx_bps = r.uptime_seconds = w;

    char   buf[JSON_CURRENT_CAP];
    jbuf_t j;
    jbuf_init(&j, buf, sizeof(buf));
    jbuf_begin(&j);
    json_serialize_current(&j, &r, &cfg, 1 << 20, 1, w, 1000000);
    jbuf_end(&j);

    return j.overflow;
}

/* Unknown chart and card names were once kept: the largest config came to 4233 bytes of 4096. */
static int test_largest_current_fits_its_buffer(void)
{
    char title[1024], junk[128], written[16384], kept[16384];
    ctl(title, 127);
    ctl(junk, 15);

    largest_toml(written, sizeof(written), title, junk, junk);
    largest_toml(kept, sizeof(kept), title, "cpu_usage", "cpu_usage");

    return current_overflows(written) == 0 && current_overflows(kept) == 0 ? 0 : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(jbuf_real_format),
    T(jbuf_real_nan_is_null),
    T(jbuf_real_infinity_is_null),
    T(jbuf_pair_format),
    T(jbuf_str_format),
    T(jbuf_sep_inserts_comma),
    T(jbuf_truncates_without_overflow),
    T(dropped_field_leaves_no_comma),
    T(overflow_marks_the_buffer),
    T(largest_current_fits_its_buffer),
    T(largest_point_fits_its_buffer),
    T(largest_units_fit_their_buffer),
    T(show_all_emits_every_group),
    T(gating_single_card),
    T(hide_all_emits_no_metrics),
    T(thresh_load_abs_scales_with_cores),
    T(thresh_load_abs_eight_cores),
    T(thresh_load_percent_fixed),
    T(thresh_cpu_mem_disk_fixed),
    T(thresh_swap_fixed),
    T(thresh_swap_io_in_ram_or_bytes),
    T(thresh_temp_from_critical),
    T(thresh_temp_fallback_no_critical),
    T(temp_null_when_sensor_invalid),
    T(net_null_when_invalid),
    T(net_percent_uses_detected_link),
    T(net_percent_defaults_to_gbe),
    T(net_percent_configured_max_wins),
    T(net_thresholds_percent),
    T(net_thresholds_absolute),
    T(array_elements_are_escaped),
    T(escape_mandatory_pair),
    T(escape_short_forms),
    T(escape_other_controls),
    T(escape_passes_utf8_through),
    T(escape_reports_overflow),
    T(escape_empty),
    T(str_field_escapes_the_value),
    T(str_field_drops_without_dangling_comma),
    T(units_smallest_that_fits),
    T(units_step_up_past_the_cap),
    T(units_absent_without_data),
    T(units_skips_percent_metrics),
    T(units_step_beyond_the_base_rung),
    T(units_ignore_rows_without_a_net_delta),
    T(units_bits_ladder),
    T(units_swap_ignores_rows_without_swap),
    T(units_swap_free_sets_the_magnitude),
    T(units_swap_absent_without_swap),
    T(units_swap_skipped_in_percent),
    T(units_swap_io_from_rows_with_a_sample),
    T(units_swap_io_skipped_in_ram),
    T(charts_echo_null_by_default),
    T(charts_echo_empty_when_hidden),
    T(charts_echo_list),
    T(cpu_null_when_invalid),
    T(mem_card_pct_sends_the_available_share_alone),
    T(mem_card_auto_sends_total_and_available),
    T(disk_absolute_emits_used),
    T(swap_card_pct_sends_the_free_share_alone),
    T(swap_card_auto_sends_total_and_free),
    T(swap_null_without_swap),
    T(swap_gated_out_by_cards),
    T(swap_units_echoed),
    T(swap_io_card_in_bytes),
    T(swap_io_card_in_ram),
    T(swap_io_null_without_a_sample),
    T(swap_io_gated_out_by_cards),
    T(swap_io_units_echoed),
    T(point_emits_short_keys),
    T(point_temp_shown_when_chart_enabled),
    T(point_temp_gated_out_by_charts),
    T(point_gates_excluded_charts),
    T(point_hides_all_charts),
    T(point_gates_net_only),
    T(point_temp_null_when_invalid),
    T(point_cpu_null_when_invalid),
    T(point_net_null_when_invalid),
    T(point_mem_pct_sends_the_available_share_alone),
    T(point_mem_auto_sends_total_and_available),
    T(point_swap_auto_sends_total_and_free),
    T(point_swap_pct_sends_the_free_share_alone),
    T(point_swap_null_without_swap),
    T(point_swap_gated_out_by_charts),
    T(point_swap_io_in_bytes),
    T(point_swap_io_in_ram),
    T(point_swap_io_null_without_a_sample),
    T(point_swap_io_gated_out_by_charts),
};

int main(void) { return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0])); }
