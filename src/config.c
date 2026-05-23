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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "tomlc17.h"

/* --- Helpers ---------------------------------------------------------------- */

static long parse_duration(const char *s)
{
    char *end;
    long  n = strtol(s, &end, 10);
    if (n <= 0 || !*end)
        return -1;
    switch (*end) {
    case 's':
        return n;
    case 'm':
        return n * 60;
    case 'h':
        return n * 3600;
    case 'd':
        return n * 86400;
    default:
        return -1;
    }
}

static void str_copy(char *dst, size_t dsize, toml_datum_t v)
{
    if (v.type == TOML_STRING)
        snprintf(dst, dsize, "%s", v.u.s);
}

static int valid_range(const char *s)
{
    char *end;
    long  n = strtol(s, &end, 10);
    return n > 0 && (*end == 'h' || *end == 'd') && *(end + 1) == '\0';
}

static int valid_op(const char *s)
{
    return strcmp(s, ">") == 0 || strcmp(s, "<") == 0 || strcmp(s, ">=") == 0 ||
           strcmp(s, "<=") == 0 || strcmp(s, "==") == 0;
}

/* --- Public API ------------------------------------------------------------ */

void config_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->listen, sizeof(cfg->listen), "%s", "0.0.0.0:8080");
    snprintf(cfg->db_path, sizeof(cfg->db_path), "%s", "./metrics.db");
    snprintf(cfg->disk_path, sizeof(cfg->disk_path), "%s", "/");
    snprintf(cfg->title, sizeof(cfg->title), "%s", "minimoni");
    snprintf(cfg->theme, sizeof(cfg->theme), "%s", "auto");
    cfg->show_footer = 1;
    snprintf(cfg->memory_card_unit, sizeof(cfg->memory_card_unit), "%s", "%");
    snprintf(cfg->memory_chart_unit, sizeof(cfg->memory_chart_unit), "%s", "mb");
    snprintf(cfg->disk_card_unit, sizeof(cfg->disk_card_unit), "%s", "%");
    snprintf(cfg->disk_chart_unit, sizeof(cfg->disk_chart_unit), "%s", "gb");
    snprintf(cfg->temp_card_unit, sizeof(cfg->temp_card_unit), "%s", "c");
    snprintf(cfg->temp_chart_unit, sizeof(cfg->temp_chart_unit), "%s", "c");
    cfg->temp_max = 100.0f;
    snprintf(cfg->cpu_load_card_unit, sizeof(cfg->cpu_load_card_unit), "%s", "abs");
    snprintf(cfg->cpu_load_chart_unit, sizeof(cfg->cpu_load_chart_unit), "%s", "abs");
    snprintf(cfg->net_card_unit, sizeof(cfg->net_card_unit), "%s", "mb");
    snprintf(cfg->net_chart_unit, sizeof(cfg->net_chart_unit), "%s", "mb");
    snprintf(cfg->uptime_unit, sizeof(cfg->uptime_unit), "%s", "auto");
    cfg->chart_count = 0; /* 0 = show all in default order */
    cfg->card_count = 0;
    snprintf(cfg->ranges[0], sizeof(cfg->ranges[0]), "%s", "1d");
    snprintf(cfg->ranges[1], sizeof(cfg->ranges[1]), "%s", "7d");
    snprintf(cfg->ranges[2], sizeof(cfg->ranges[2]), "%s", "30d");
    snprintf(cfg->ranges[3], sizeof(cfg->ranges[3]), "%s", "90d");
    cfg->range_count = 4;
    cfg->points = 300;
    cfg->threads = 8;
    cfg->sse_keepalive_seconds = 1;
    cfg->interval_seconds = 60;
    cfg->refresh_seconds = 30;
}

int config_load(config_t *cfg, const char *path)
{
    toml_result_t res = toml_parse_file_ex(path);
    if (!res.ok) {
        fprintf(stderr, "config: %s: %s\n", path, res.errmsg);
        return -1;
    }

    toml_datum_t root = res.toptab;
    toml_datum_t v;

    /* [server] */
    v = toml_seek(root, "server.listen");
    str_copy(cfg->listen, sizeof(cfg->listen), v);
    v = toml_seek(root, "server.threads");
    if (v.type == TOML_INT64 && v.u.int64 >= 2 && v.u.int64 <= 256)
        cfg->threads = (int)v.u.int64;
    else if (v.type == TOML_INT64 && v.u.int64 < 2) {
        fprintf(stderr, "config: threads must be >= 2 (got %lld); aborting\n", v.u.int64);
        toml_free(res);
        return -1;
    } else if (v.type == TOML_INT64)
        fprintf(stderr, "config: threads must be <= 256 (got %lld); using default\n", v.u.int64);
    v = toml_seek(root, "server.sse_keepalive");
    if (v.type == TOML_INT64 && v.u.int64 > 0)
        cfg->sse_keepalive_seconds = (int)v.u.int64;
    else if (v.type == TOML_INT64)
        fprintf(stderr, "config: sse_keepalive must be > 0 (got %lld); using default\n", v.u.int64);

    /* [collect] */
    v = toml_seek(root, "collect.interval");
    if (v.type == TOML_STRING) {
        fprintf(stderr,
                "config: interval is now an integer (seconds); got string '%s'. "
                "Replace with e.g. `interval = 60` for 1 minute.\n",
                v.u.s);
        toml_free(res);
        return -1;
    }
    if (v.type == TOML_INT64) {
        if (v.u.int64 < 1) {
            fprintf(stderr, "config: interval must be >= 1 second (got %lld); aborting\n",
                    v.u.int64);
            toml_free(res);
            return -1;
        }
        if (v.u.int64 > 3600) {
            fprintf(stderr,
                    "config: interval must be <= 3600 seconds / 1 hour (got %lld); "
                    "clamping to 3600\n",
                    v.u.int64);
            cfg->interval_seconds = 3600;
        } else {
            cfg->interval_seconds = (long)v.u.int64;
        }
    }
    v = toml_seek(root, "collect.db");
    str_copy(cfg->db_path, sizeof(cfg->db_path), v);
    v = toml_seek(root, "collect.disk_path");
    str_copy(cfg->disk_path, sizeof(cfg->disk_path), v);

    /* [dashboard] */
    v = toml_seek(root, "dashboard.title");
    str_copy(cfg->title, sizeof(cfg->title), v);
    v = toml_seek(root, "dashboard.show_footer");
    if (v.type == TOML_BOOLEAN)
        cfg->show_footer = v.u.boolean ? 1 : 0;
    v = toml_seek(root, "dashboard.theme");
    if (v.type == TOML_STRING) {
        if (strcmp(v.u.s, "light") == 0 || strcmp(v.u.s, "dark") == 0 || strcmp(v.u.s, "auto") == 0)
            str_copy(cfg->theme, sizeof(cfg->theme), v);
        else
            fprintf(stderr, "config: invalid theme '%s', using default\n", v.u.s);
    }
    v = toml_seek(root, "dashboard.refresh");
    if (v.type == TOML_INT64 && v.u.int64 > 0)
        cfg->refresh_seconds = (int)v.u.int64;
    else if (v.type == TOML_INT64)
        fprintf(stderr, "config: refresh must be > 0 (got %lld); using default\n", v.u.int64);
    v = toml_seek(root, "dashboard.memory_card_unit");
    str_copy(cfg->memory_card_unit, sizeof(cfg->memory_card_unit), v);
    v = toml_seek(root, "dashboard.memory_chart_unit");
    str_copy(cfg->memory_chart_unit, sizeof(cfg->memory_chart_unit), v);
    v = toml_seek(root, "dashboard.disk_card_unit");
    str_copy(cfg->disk_card_unit, sizeof(cfg->disk_card_unit), v);
    v = toml_seek(root, "dashboard.disk_chart_unit");
    str_copy(cfg->disk_chart_unit, sizeof(cfg->disk_chart_unit), v);
    v = toml_seek(root, "dashboard.cpu_load_card_unit");
    str_copy(cfg->cpu_load_card_unit, sizeof(cfg->cpu_load_card_unit), v);
    v = toml_seek(root, "dashboard.cpu_load_chart_unit");
    str_copy(cfg->cpu_load_chart_unit, sizeof(cfg->cpu_load_chart_unit), v);
    v = toml_seek(root, "dashboard.net_card_unit");
    str_copy(cfg->net_card_unit, sizeof(cfg->net_card_unit), v);
    v = toml_seek(root, "dashboard.net_chart_unit");
    str_copy(cfg->net_chart_unit, sizeof(cfg->net_chart_unit), v);
    v = toml_seek(root, "dashboard.uptime_unit");
    str_copy(cfg->uptime_unit, sizeof(cfg->uptime_unit), v);
    v = toml_seek(root, "dashboard.charts");
    if (v.type == TOML_ARRAY) {
        if (v.u.arr.size == 0) {
            cfg->chart_count = -1; /* explicit empty: hide all */
        } else {
            for (int i = 0; i < v.u.arr.size && cfg->chart_count < MAX_CHARTS; i++) {
                toml_datum_t e = v.u.arr.elem[i];
                if (e.type == TOML_STRING)
                    snprintf(cfg->charts[cfg->chart_count++], 16, "%s", e.u.s);
            }
        }
    }
    v = toml_seek(root, "dashboard.cards");
    if (v.type == TOML_ARRAY) {
        if (v.u.arr.size == 0) {
            cfg->card_count = -1; /* explicit empty: hide all */
        } else {
            for (int i = 0; i < v.u.arr.size && cfg->card_count < MAX_CARDS; i++) {
                toml_datum_t e = v.u.arr.elem[i];
                if (e.type == TOML_STRING)
                    snprintf(cfg->cards[cfg->card_count++], 16, "%s", e.u.s);
            }
        }
    }
    v = toml_seek(root, "dashboard.temp_card_unit");
    str_copy(cfg->temp_card_unit, sizeof(cfg->temp_card_unit), v);
    v = toml_seek(root, "dashboard.temp_chart_unit");
    str_copy(cfg->temp_chart_unit, sizeof(cfg->temp_chart_unit), v);
    v = toml_seek(root, "dashboard.temp_max");
    if (v.type == TOML_FP64 && v.u.fp64 > 0)
        cfg->temp_max = (float)v.u.fp64;
    else if (v.type == TOML_INT64 && v.u.int64 > 0)
        cfg->temp_max = (float)v.u.int64;
    else if (v.type == TOML_FP64 || v.type == TOML_INT64)
        fprintf(stderr, "config: temp_max must be > 0; using default\n");
    v = toml_seek(root, "dashboard.points");
    if (v.type == TOML_INT64 && v.u.int64 > 0)
        cfg->points = (int)v.u.int64;
    else if (v.type == TOML_INT64)
        fprintf(stderr, "config: points must be > 0 (got %lld); using default\n", v.u.int64);
    v = toml_seek(root, "dashboard.ranges");
    if (v.type == TOML_ARRAY && v.u.arr.size > 0) {
        int count = 0;
        for (int i = 0; i < v.u.arr.size && count < MAX_RANGES; i++) {
            toml_datum_t e = v.u.arr.elem[i];
            if (e.type != TOML_STRING) {
                fprintf(stderr, "config: ranges[%d]: not a string, skipping\n", i);
                continue;
            }
            if (!valid_range(e.u.s)) {
                fprintf(stderr, "config: ranges[%d]: invalid '%s' (use <n>h or <n>d)\n", i, e.u.s);
                continue;
            }
            long d = parse_duration(e.u.s);
            if (d < cfg->interval_seconds) {
                fprintf(stderr,
                        "config: ranges[%d]: '%s' is shorter than collect interval, "
                        "skipping\n",
                        i, e.u.s);
                continue;
            }
            snprintf(cfg->ranges[count++], sizeof(cfg->ranges[0]), "%s", e.u.s);
        }
        if (count > 0)
            cfg->range_count = count;
        else
            fprintf(stderr, "config: ranges: all values invalid, using defaults\n");
    }

    /* [[alert]] */
    v = toml_seek(root, "alert");
    if (v.type == TOML_ARRAY) {
        for (int i = 0; i < v.u.arr.size && cfg->alert_count < MAX_ALERTS; i++) {
            toml_datum_t e = v.u.arr.elem[i];
            if (e.type != TOML_TABLE)
                continue;

            toml_datum_t dname = toml_get(e, "name");
            toml_datum_t dmet = toml_get(e, "metric");
            toml_datum_t dop = toml_get(e, "operator");
            toml_datum_t dthr = toml_get(e, "threshold");

            if (dname.type != TOML_STRING || dmet.type != TOML_STRING || dop.type != TOML_STRING) {
                fprintf(stderr, "config: alert[%d]: missing name/metric/operator, skipping\n", i);
                continue;
            }
            if (!valid_op(dop.u.s)) {
                fprintf(stderr, "config: alert[%d]: unknown operator '%s', skipping\n", i, dop.u.s);
                continue;
            }

            double thr_val;
            if (dthr.type == TOML_FP64)
                thr_val = dthr.u.fp64;
            else if (dthr.type == TOML_INT64)
                thr_val = (double)dthr.u.int64;
            else {
                fprintf(stderr, "config: alert '%s': missing threshold, skipping\n", dname.u.s);
                continue;
            }

            alert_cfg_t *a = &cfg->alerts[cfg->alert_count];
            memset(a, 0, sizeof(*a));
            str_copy(a->name, sizeof(a->name), dname);
            str_copy(a->metric, sizeof(a->metric), dmet);
            str_copy(a->op, sizeof(a->op), dop);
            a->threshold = thr_val;
            str_copy(a->webhook, sizeof(a->webhook), toml_get(e, "webhook"));
            str_copy(a->command, sizeof(a->command), toml_get(e, "command"));

            toml_datum_t dcool = toml_get(e, "cooldown");
            if (dcool.type == TOML_STRING) {
                long d = parse_duration(dcool.u.s);
                if (d > 0)
                    a->cooldown_seconds = d;
                else
                    fprintf(stderr, "config: alert '%s': invalid cooldown '%s'\n", a->name,
                            dcool.u.s);
            }

            if (!a->webhook[0] && !a->command[0]) {
                fprintf(stderr, "config: alert '%s': needs webhook or command, skipping\n",
                        a->name);
                continue;
            }
            cfg->alert_count++;
        }
    }

    toml_free(res);

    if (cfg->refresh_seconds > cfg->interval_seconds) {
        fprintf(stderr, "config: refresh (%ds) > interval (%lds); clamping refresh to interval\n",
                cfg->refresh_seconds, cfg->interval_seconds);
        cfg->refresh_seconds = (int)cfg->interval_seconds;
    }

    if (cfg->sse_keepalive_seconds >= cfg->refresh_seconds)
        fprintf(stderr, "config: sse_keepalive (%ds) >= refresh (%ds); keepalive inactive\n",
                cfg->sse_keepalive_seconds, cfg->refresh_seconds);

    return 0;
}

int config_open(config_t *cfg, const char *explicit_path)
{
    config_defaults(cfg);

    if (explicit_path)
        return config_load(cfg, explicit_path);

    if (access("./config.toml", R_OK) == 0)
        return config_load(cfg, "./config.toml");

    if (access("/etc/minimoni/config.toml", R_OK) == 0)
        return config_load(cfg, "/etc/minimoni/config.toml");

    return 0;
}
