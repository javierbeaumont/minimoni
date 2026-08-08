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

/*
 * Unit tests for src/json.c. Zero-dependency, no framework. Build with:
 *   make test
 *
 * json.c is `#include`d directly, together with units.c (the *_convert helpers
 * it calls) and config.c (config_has). Each test returns 0 on pass / 1 on fail;
 * the shared harness lives in runner.h.
 */

#define _POSIX_C_SOURCE 200809L

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
    r.mem_used_mb = 500.0;
    r.mem_available_mb = 500.0;
    r.mem_percent = 50.0;
    r.disk_total_gb = 100.0;
    r.disk_used_gb = 40.0;
    r.disk_free_gb = 60.0;
    r.disk_percent = 40.0;
    r.temp_valid = 1;
    r.temp_celsius = 50.0;
    r.net_valid = 1;
    r.net_rx_bps = 1024.0;
    r.net_tx_bps = 2048.0;
    r.uptime_seconds = 3600.0;
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

/* --- Serializer: card gating --- */

static int test_show_all_emits_every_group(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"load_1m\"") && has(b, "\"cpu_user_percent\"") && has(b, "\"mem_percent\"") &&
            has(b, "\"disk_percent\"") && has(b, "\"temp\"") && has(b, "\"net_rx\"") &&
            has(b, "\"uptime_seconds\""))
               ? 0
               : 1;
}

static int test_gating_single_card(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    only_card(&c, "cpu_load");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    /* only the load group is emitted; the rest is gated out */
    return (has(b, "\"load_1m\"") && !has(b, "\"mem_percent\"") &&
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
    return (!has(b, "\"load_1m\"") && !has(b, "\"mem_percent\"") && !has(b, "\"temp\"") &&
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

static int test_mem_percent_only_when_pct_unit(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg(); /* memory_card_unit defaults to "%" */
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"mem_percent\"") && !has(b, "\"mem_used\"")) ? 0 : 1;
}

static int test_mem_absolute_emits_used(void)
{
    char     b[2048];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.memory_card_unit, "auto");
    emit(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"mem_used\"") && has(b, "\"mem_percent\"")) ? 0 : 1;
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

/* --- Serializer: history point (/api/metrics, short keys, chart units) --- */

static int test_point_emits_short_keys(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"t\":") && has(b, "\"l1\":") && has(b, "\"cu\":") && has(b, "\"mp\":") &&
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
    return (has(b, "\"l1\":") && !has(b, "\"mp\"") && !has(b, "\"cu\"") && !has(b, "\"nr\"")) ? 0
                                                                                              : 1;
}

static int test_point_hides_all_charts(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    c.chart_count = -1; /* explicit empty charts list -> every chart group gated out */
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (!has(b, "\"l1\"") && !has(b, "\"cu\"") && !has(b, "\"mp\"") && !has(b, "\"dp\"") &&
            !has(b, "\"tp\"") && !has(b, "\"nr\"") && has(b, "\"t\":") && has(b, "\"up\":"))
               ? 0
               : 1;
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
            !has(b, "\"mp\""))
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

static int test_point_mem_chart_pct_omits_abs(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg();
    strcpy(c.memory_chart_unit, "%");
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return (has(b, "\"mp\":") && !has(b, "\"mu\":")) ? 0 : 1;
}

static int test_point_mem_chart_abs_emits(void)
{
    char     b[1024];
    db_row_t r = sample_row();
    config_t c = base_cfg(); /* memory_chart_unit defaults to "auto" */
    emit_point(b, sizeof(b), &r, &c, 4, 1, 100.0);
    return has(b, "\"mu\":") ? 0 : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(jbuf_real_format),
    T(jbuf_pair_format),
    T(jbuf_str_format),
    T(jbuf_sep_inserts_comma),
    T(jbuf_truncates_without_overflow),
    T(show_all_emits_every_group),
    T(gating_single_card),
    T(hide_all_emits_no_metrics),
    T(thresh_load_abs_scales_with_cores),
    T(thresh_load_abs_eight_cores),
    T(thresh_load_percent_fixed),
    T(thresh_cpu_mem_disk_fixed),
    T(thresh_temp_from_critical),
    T(thresh_temp_fallback_no_critical),
    T(temp_null_when_sensor_invalid),
    T(net_null_when_invalid),
    T(net_percent_uses_detected_link),
    T(net_percent_defaults_to_gbe),
    T(net_percent_configured_max_wins),
    T(net_thresholds_percent),
    T(net_thresholds_absolute),
    T(units_smallest_that_fits),
    T(units_step_up_past_the_cap),
    T(units_absent_without_data),
    T(units_skips_percent_metrics),
    T(units_step_beyond_the_base_rung),
    T(units_ignore_rows_without_a_net_delta),
    T(units_bits_ladder),
    T(charts_echo_null_by_default),
    T(charts_echo_empty_when_hidden),
    T(charts_echo_list),
    T(cpu_null_when_invalid),
    T(mem_percent_only_when_pct_unit),
    T(mem_absolute_emits_used),
    T(disk_absolute_emits_used),
    T(point_emits_short_keys),
    T(point_temp_shown_when_chart_enabled),
    T(point_temp_gated_out_by_charts),
    T(point_gates_excluded_charts),
    T(point_hides_all_charts),
    T(point_gates_net_only),
    T(point_temp_null_when_invalid),
    T(point_cpu_null_when_invalid),
    T(point_net_null_when_invalid),
    T(point_mem_chart_pct_omits_abs),
    T(point_mem_chart_abs_emits),
};

int main(void) { return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0])); }
