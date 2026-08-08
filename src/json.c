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

#include <stdio.h>
#include <string.h>

#include "json.h"

#include "units.h"
#include <math.h>

/* --- JSON output writer --- */

void jbuf_init(jbuf_t *j, char *buf, size_t cap)
{
    j->buf = buf;
    j->pos = 0;
    j->cap = cap;
    j->comma = 0;
    if (cap > 0)
        buf[0] = '\0';
}

void jbuf_raw(jbuf_t *j, const char *s)
{
    size_t n = strlen(s);
    if (j->pos + n < j->cap) {
        memcpy(j->buf + j->pos, s, n);
        j->pos += n;
        j->buf[j->pos] = '\0';
    }
}

void jbuf_sep(jbuf_t *j)
{
    if (j->comma)
        jbuf_raw(j, ",");
    j->comma = 1;
}

void jbuf_begin(jbuf_t *j)
{
    jbuf_raw(j, "{");
    j->comma = 0;
}

void jbuf_end(jbuf_t *j) { jbuf_raw(j, "}"); }

void jbuf_str(jbuf_t *j, const char *key, const char *val)
{
    jbuf_sep(j);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "\"%s\":\"%s\"", key, val);
    jbuf_raw(j, tmp);
}

void jbuf_real(jbuf_t *j, const char *key, double val)
{
    jbuf_sep(j);
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "\"%s\":%.4g", key, val);
    jbuf_raw(j, tmp);
}

void jbuf_long(jbuf_t *j, const char *key, long val)
{
    jbuf_sep(j);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "\"%s\":%ld", key, val);
    jbuf_raw(j, tmp);
}

void jbuf_null(jbuf_t *j, const char *key)
{
    jbuf_sep(j);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "\"%s\":null", key);
    jbuf_raw(j, tmp);
}

void jbuf_pair(jbuf_t *j, const char *key, double warn, double crit)
{
    jbuf_sep(j);
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "\"%s\":[%.6g,%.6g]", key, warn, crit);
    jbuf_raw(j, tmp);
}

/* --- Serializers --- */

void json_serialize_current(jbuf_t *j, const db_row_t *r, const config_t *cfg, int num_cores,
                            int temp_critical_valid, double temp_critical, int detected_speed_mbit)
{
    const char *lu = cfg->cpu_load_card_unit;
    const char *mu = cfg->memory_card_unit;
    const char *du = cfg->disk_card_unit;
    const char *nu = cfg->net_card_unit;
    double      nref = net_ref_bps(cfg->net_max_speed, detected_speed_mbit);
    double      tref = temp_ref(temp_critical_valid, temp_critical, cfg->temp_critical_fallback);

    jbuf_str(j, "timestamp", r->timestamp);

    /* Each metric group is emitted only when its card is in the cards config
     * (or the config is the default show-all). Absent fields tell the
     * dashboard "this card isn't configured"; null values tell it
     * "configured but no data yet" (first collect, sensor failure). */
    if (config_has(cfg->cards, cfg->card_count, "cpu_load")) {
        jbuf_real(j, "load_1m", load_convert(r->load_1m, num_cores, lu));
        jbuf_real(j, "load_5m", load_convert(r->load_5m, num_cores, lu));
        jbuf_real(j, "load_15m", load_convert(r->load_15m, num_cores, lu));
    }

    if (config_has(cfg->cards, cfg->card_count, "cpu_usage")) {
        if (r->cpu_valid) {
            jbuf_real(j, "cpu_user_percent", r->cpu_user_percent);
            jbuf_real(j, "cpu_system_percent", r->cpu_system_percent);
            jbuf_real(j, "cpu_idle_percent", r->cpu_idle_percent);
        } else {
            jbuf_null(j, "cpu_user_percent");
            jbuf_null(j, "cpu_system_percent");
            jbuf_null(j, "cpu_idle_percent");
        }
    }

    if (config_has(cfg->cards, cfg->card_count, "memory")) {
        if (mu[0] != '%') {
            jbuf_real(j, "mem_used", r->mem_used_mb);
            jbuf_real(j, "mem_available", r->mem_available_mb);
            jbuf_real(j, "mem_total", r->mem_total_mb);
        }
        jbuf_real(j, "mem_percent", r->mem_percent);
    }

    if (config_has(cfg->cards, cfg->card_count, "disk")) {
        if (du[0] != '%') {
            jbuf_real(j, "disk_used", r->disk_used_gb);
            jbuf_real(j, "disk_total", r->disk_total_gb);
            jbuf_real(j, "disk_free", r->disk_free_gb);
        }
        jbuf_real(j, "disk_percent", r->disk_percent);
    }

    if (config_has(cfg->cards, cfg->card_count, "temp")) {
        if (r->temp_valid)
            jbuf_real(j, "temp", temp_convert(r->temp_celsius, cfg->temp_card_unit, tref));
        else
            jbuf_null(j, "temp");
    }

    if (config_has(cfg->cards, cfg->card_count, "net")) {
        if (r->net_valid) {
            jbuf_real(j, "net_rx", net_convert(r->net_rx_bps, nu, nref));
            jbuf_real(j, "net_tx", net_convert(r->net_tx_bps, nu, nref));
        } else {
            jbuf_null(j, "net_rx");
            jbuf_null(j, "net_tx");
        }
    }

    if (config_has(cfg->cards, cfg->card_count, "uptime"))
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

    /* Thresholds. Computed server-side so the dashboard needs no unit-conversion
     * logic and adapts to the machine's hardware (core count, thermal trip point)
     * and configured display units.
     *   load : [0.75*cores, 1.0*cores] for abs; [70, 90] for %
     *   cpu  : [70, 90]   (always %)
     *   mem  : [70, 90]   (compared against mem_percent)
     *   disk : [80, 90]   (compared against disk_percent)
     *   temp : [trip-20, trip-10] converted to the card unit; [70, 80] fallback
     *   net  : [85%, 98%] of the link ceiling, converted to the card unit */
    if (config_has(cfg->cards, cfg->card_count, "cpu_load")) {
        double lw, lc;
        if (lu[0] == '%') {
            lw = 70.0;
            lc = 90.0;
        } else {
            lw = num_cores > 0 ? 0.75 * num_cores : 2.0;
            lc = num_cores > 0 ? 1.0 * num_cores : 3.5;
        }
        jbuf_pair(j, "thresh_load", lw, lc);
    }
    if (config_has(cfg->cards, cfg->card_count, "cpu_usage"))
        jbuf_pair(j, "thresh_cpu", 70.0, 90.0);
    if (config_has(cfg->cards, cfg->card_count, "memory"))
        jbuf_pair(j, "thresh_mem", 70.0, 90.0);
    if (config_has(cfg->cards, cfg->card_count, "disk"))
        jbuf_pair(j, "thresh_disk", 80.0, 90.0);
    if (config_has(cfg->cards, cfg->card_count, "temp")) {
        double tw, tc;
        if (temp_critical_valid) {
            tw = temp_convert(temp_critical - 20.0, cfg->temp_card_unit, tref);
            tc = temp_convert(temp_critical - 10.0, cfg->temp_card_unit, tref);
        } else {
            tw = temp_convert(70.0, cfg->temp_card_unit, tref);
            tc = temp_convert(80.0, cfg->temp_card_unit, tref);
        }
        jbuf_pair(j, "thresh_temp", tw, tc);
    }
    if (config_has(cfg->cards, cfg->card_count, "net"))
        jbuf_pair(j, "thresh_net", net_convert(0.85 * nref, nu, nref),
                  net_convert(0.98 * nref, nu, nref));
}

static const char *const MEM_LADDER[] = {"MB", "GB", "TB"};
static const char *const DISK_LADDER[] = {"GB", "TB", "PB"};
static const char *const NET_BYTES[] = {"KB/s", "MB/s", "GB/s"};
/* A kbit is 1000 bits, not 1024, hence the factor this one is served with. */
static const char *const NET_BITS[] = {"Kbps", "Mbps", "Gbps"};

static double max2(double a, double b) { return a > b ? a : b; }

static void unit_entry(jbuf_t *j, const char *key, double max, const char *const *ladder, int steps,
                       double mul, double factor, int base)
{
    int  step = unit_step(max * mul / pow(factor, base), steps, factor);
    char buf[96];
    if (step < 0)
        return;
    snprintf(buf, sizeof(buf), "\"%s\":{\"sym\":\"%s\",\"mul\":%g,\"div\":%g}", key, ladder[step],
             mul, pow(factor, step + base));
    jbuf_sep(j);
    jbuf_raw(j, buf);
}

void json_serialize_units(jbuf_t *j, const db_row_t *rows, int cnt, const config_t *cfg)
{
    double mem = 0.0, disk = 0.0, net = 0.0;

    for (int i = 0; i < cnt; i++) {
        mem = max2(mem, max2(rows[i].mem_used_mb, rows[i].mem_available_mb));
        disk = max2(disk, max2(rows[i].disk_used_gb, rows[i].disk_free_gb));
        if (rows[i].net_valid)
            net = max2(net, max2(rows[i].net_rx_bps, rows[i].net_tx_bps));
    }

    jbuf_sep(j);
    jbuf_raw(j, "\"units\":{");
    j->comma = 0; /* nested object: the first entry takes no leading comma */
    if (cfg->memory_chart_unit[0] != '%')
        unit_entry(j, "mem", mem, MEM_LADDER, 3, 1.0, 1024.0, 0);
    if (cfg->disk_chart_unit[0] != '%')
        unit_entry(j, "disk", disk, DISK_LADDER, 3, 1.0, 1024.0, 0);
    if (cfg->net_chart_unit[0] != '%') {
        int bits = cfg->net_chart_unit[0] == 'b' && cfg->net_chart_unit[1] == 'i';
        unit_entry(j, "net", net, bits ? NET_BITS : NET_BYTES, 3, bits ? 8.0 : 1.0,
                   bits ? 1000.0 : 1024.0, 1);
    }
    jbuf_raw(j, "}");
    j->comma = 1;
}

void json_serialize_point(jbuf_t *j, const db_row_t *r, const config_t *cfg, int num_cores,
                          int temp_critical_valid, double temp_critical, int detected_speed_mbit)
{
    const char *lu = cfg->cpu_load_chart_unit;
    const char *mu = cfg->memory_chart_unit;
    const char *du = cfg->disk_chart_unit;
    const char *nu = cfg->net_chart_unit;
    double      tref = temp_ref(temp_critical_valid, temp_critical, cfg->temp_critical_fallback);
    double      nref = net_ref_bps(cfg->net_max_speed, detected_speed_mbit);

    jbuf_long(j, "t", r->unix_time);

    /* Each metric group is emitted only when its chart is in the charts config. */
    if (config_has(cfg->charts, cfg->chart_count, "cpu_load")) {
        jbuf_real(j, "l1", load_convert(r->load_1m, num_cores, lu));
        jbuf_real(j, "l5", load_convert(r->load_5m, num_cores, lu));
        jbuf_real(j, "l15", load_convert(r->load_15m, num_cores, lu));
    }

    if (config_has(cfg->charts, cfg->chart_count, "cpu_usage")) {
        if (r->cpu_valid) {
            jbuf_real(j, "cu", r->cpu_user_percent);
            jbuf_real(j, "cs", r->cpu_system_percent);
            jbuf_real(j, "ci", r->cpu_idle_percent);
        } else {
            jbuf_null(j, "cu");
            jbuf_null(j, "cs");
            jbuf_null(j, "ci");
        }
    }

    if (config_has(cfg->charts, cfg->chart_count, "memory")) {
        if (mu[0] != '%') {
            jbuf_real(j, "mu", r->mem_used_mb);
            jbuf_real(j, "ma", r->mem_available_mb);
            jbuf_real(j, "mt", r->mem_total_mb);
        }
        jbuf_real(j, "mp", r->mem_percent);
    }

    if (config_has(cfg->charts, cfg->chart_count, "disk")) {
        if (du[0] != '%') {
            jbuf_real(j, "du", r->disk_used_gb);
            jbuf_real(j, "dt", r->disk_total_gb);
            jbuf_real(j, "df", r->disk_free_gb);
        }
        jbuf_real(j, "dp", r->disk_percent);
    }

    if (config_has(cfg->charts, cfg->chart_count, "temp")) {
        if (r->temp_valid)
            jbuf_real(j, "tp", temp_convert(r->temp_celsius, cfg->temp_chart_unit, tref));
        else
            jbuf_null(j, "tp");
    }

    if (config_has(cfg->charts, cfg->chart_count, "net")) {
        if (r->net_valid) {
            jbuf_real(j, "nr", net_convert(r->net_rx_bps, nu, nref));
            jbuf_real(j, "nt", net_convert(r->net_tx_bps, nu, nref));
        } else {
            jbuf_null(j, "nr");
            jbuf_null(j, "nt");
        }
    }

    jbuf_real(j, "up", r->uptime_seconds);
}
