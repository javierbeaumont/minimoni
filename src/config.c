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

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "config.h"
#include "tomlc17.h"

/* --- Helpers --- */

/* Per-unit caps = 10 years (the same 3653d ceiling as range_in_bounds), checked
 * BEFORE the multiply so a 32-bit long cannot overflow. */
static long parse_duration(const char *s)
{
    char *end;
    long  n = strtol(s, &end, 10);

    if (n <= 0 || !*end || end[1])
        return -1;

    switch (*end) {
    case 's':
        return n <= 3653L * 86400 ? n : -1;
    case 'm':
        return n <= 3653L * 1440 ? n * 60 : -1;
    case 'h':
        return n <= 3653L * 24 ? n * 3600 : -1;
    case 'd':
        return n <= 3653 ? n * 86400 : -1;
    default:
        return -1;
    }
}

/* Copy a string value into its fixed field. A value that does not fit is an error, never cut: a
 * truncated path, command or URL is a different one. A missing or non-string value copies nothing,
 * so the field keeps its default. */
[[nodiscard]] [[gnu::format(printf, 4, 5)]] static int
str_copy(char *dst, size_t dsize, toml_datum_t v, const char *key, ...)
{
    if (v.type != TOML_STRING)
        return 0;

    size_t len = strlen(v.u.s);
    if (len >= dsize) {
        va_list ap;
        va_start(ap, key);
        fprintf(stderr, "config: ");
        vfprintf(stderr, key, ap);
        va_end(ap);
        fprintf(stderr, " is %zu bytes; the maximum is %zu\n", len, dsize - 1);
        return -1;
    }

    memcpy(dst, v.u.s, len + 1);
    return 0;
}

/* 1 when a line opens a PEM certificate, 0 when none does, -1 with errno when the file cannot be
 * read. Only a first check: the anchors are decoded at delivery. */
static int has_certificate(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[64];
    int  found = 0;
    while (!found && fgets(line, sizeof(line), f))
        found = strncmp(line, "-----BEGIN CERTIFICATE-----", 27) == 0;

    int err = ferror(f) ? errno : 0;
    fclose(f);

    if (err) {
        errno = err;
        return -1;
    }

    return found;
}

static int valid_range(const char *s)
{
    char *end;
    long  n = strtol(s, &end, 10);
    return n > 0 && (*end == 'm' || *end == 'h' || *end == 'd') && *(end + 1) == '\0';
}

/* Per-unit upper bounds: above these, the next-larger unit is the natural
 * expression (e.g. 121m -> 2h+) and 3653d is the maximum number of days in
 * any 10-calendar-year window (worst case: 3 leap years). Anything past
 * these is either a typo or an unrealistic value that risks integer
 * overflow downstream in parse_duration's multiplication by 60/3600/86400.
 * User-facing messages talk about "10 years" for clarity. */
static int range_in_bounds(const char *s)
{
    long n = strtol(s, NULL, 10);
    char u = s[strlen(s) - 1];
    if (u == 'm')
        return n <= 120;
    if (u == 'h')
        return n <= 72;
    if (u == 'd')
        return n <= 3653;
    return 0;
}

static int valid_op(const char *s)
{
    return strcmp(s, ">") == 0 || strcmp(s, "<") == 0 || strcmp(s, ">=") == 0 ||
           strcmp(s, "<=") == 0;
}

/* --- Unit validation --- */

/* Allowed values per *_unit key, in the order config.example.toml documents. */
static const char *const SIZE_UNITS[] = {"%", "auto", NULL};

static const char *const TEMP_UNITS[] = {"%", "c", "f", NULL};
static const char *const LOAD_UNITS[] = {"%", "abs", NULL};
static const char *const NET_UNITS[] = {"%", "bytes", "bits", NULL};
static const char *const UPTIME_UNITS[] = {"auto", "h", "d", NULL};

/* str_copy, but only when the value is in `allowed`; else warn and keep the
 * default already in dst (an invalid unit would otherwise silently fall through
 * the converters in http.c and show raw values). */
static void unit_copy(char *dst, size_t dsize, toml_datum_t v, const char *key,
                      const char *const *allowed)
{
    if (v.type != TOML_STRING)
        return;
    for (int i = 0; allowed[i]; i++) {
        if (strcmp(v.u.s, allowed[i]) == 0) {
            snprintf(dst, dsize, "%s", v.u.s);
            return;
        }
    }
    fprintf(stderr, "config: %s: invalid unit '%s' (use", key, v.u.s);
    for (int i = 0; allowed[i]; i++)
        fprintf(stderr, "%s %s", i ? "," : "", allowed[i]);
    fprintf(stderr, "); using default\n");
}

/* --- Chart and card names --- */

/* The names the dashboard draws. */
#define CHARTS "cpu_load", "cpu_usage", "memory", "swap", "disk", "temp", "net"
static const char *const CHART_NAMES[] = {CHARTS, NULL};
static const char *const CARD_NAMES[] = {CHARTS, "uptime", NULL};
#undef CHARTS

static const char *const METRIC_NAMES[] = {ALERT_METRICS, NULL};

static int in_list(const char *name, const char *const *list)
{
    for (int k = 0; list[k]; k++)
        if (strcmp(name, list[k]) == 0)
            return 1;

    return 0;
}

/* Not a plain cast: past INT_MAX that wraps negative. */
static int to_int(int64_t v) { return v > INT_MAX ? INT_MAX : (int)v; }

/* 1 if `name` is known; else warn and 0. */
static int known_name(const char *name, const char *const *known, const char *key, int i)
{
    if (in_list(name, known))
        return 1;

    fprintf(stderr, "config: dashboard.%s[%d]: unknown name '%s', skipping (use", key, i, name);
    for (int k = 0; known[k]; k++)
        fprintf(stderr, "%s %s", k ? "," : "", known[k]);
    fprintf(stderr, ")\n");

    return 0;
}

/* --- Unknown-key detection --- */

/* Canonical keys per table; a key outside these is a typo the TOML reader would
 * otherwise silently ignore, leaving the user wondering why their setting did
 * not take effect. */
static const char *const SERVER_KEYS[] = {"listen", "max_dashboards", "sse_keepalive"};
static const char *const COLLECT_KEYS[] = {"db", "disk_path", "interval"};
static const char *const WEBHOOK_KEYS[] = {"ca_file", "insecure_skip_verify"};
/* One key per line, so a new one never reflows the list. */
/* clang-format off */
static const char *const DASHBOARD_KEYS[] = {"cards",
                                             "charts",
                                             "cpu_load_card_unit",
                                             "cpu_load_chart_unit",
                                             "disk_card_unit",
                                             "disk_chart_unit",
                                             "memory_card_unit",
                                             "memory_chart_unit",
                                             "net_card_unit",
                                             "net_chart_unit",
                                             "net_max_speed",
                                             "ranges",
                                             "refresh",
                                             "show_footer",
                                             "swap_card_unit",
                                             "swap_chart_unit",
                                             "temp_card_unit",
                                             "temp_chart_unit",
                                             "temp_critical_fallback",
                                             "theme",
                                             "title",
                                             "uptime_unit"};
/* clang-format on */
static const char *const ALERT_KEYS[] = {"command",  "cooldown",  "metric", "name",
                                         "operator", "threshold", "webhook"};

/* Levenshtein distance, for the did-you-mean hint. */
static int key_distance(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    int row[64];
    if (lb >= 64)
        return 99;
    for (int j = 0; j <= lb; j++)
        row[j] = j;
    for (int i = 1; i <= la; i++) {
        int diag = row[0]; /* previous row's [j-1] */
        row[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cur = row[j];
            int best = diag + (a[i - 1] == b[j - 1] ? 0 : 1);
            if (row[j] + 1 < best)
                best = row[j] + 1;
            if (row[j - 1] + 1 < best)
                best = row[j - 1] + 1;
            row[j] = best;
            diag = cur;
        }
    }
    return row[lb];
}

/* Warn for each key of `tab` not in known[]; suggests the closest canonical
 * name within distance 2. Returns the unknown count (warnings only). */
static int warn_unknown_in(toml_datum_t tab, const char *where, const char *const *known, int nkeys)
{
    int unknown = 0;
    for (int i = 0; i < tab.u.tab.size; i++) {
        const char *k = tab.u.tab.key[i];
        int         found = 0;
        for (int j = 0; j < nkeys && !found; j++)
            found = strcmp(k, known[j]) == 0;
        if (found)
            continue;
        unknown++;
        const char *close = NULL;
        int         bestd = 3;
        for (int j = 0; j < nkeys; j++) {
            int d = key_distance(k, known[j]);
            if (d < bestd) {
                bestd = d;
                close = known[j];
            }
        }
        if (close)
            fprintf(stderr, "config: unknown key '%s.%s' (typo of '%s'?)\n", where, k, close);
        else
            fprintf(stderr, "config: unknown key '%s.%s'\n", where, k);
    }
    return unknown;
}

#define N_KEYS(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Walk the parsed document's top level and every known table one level deep.
 * Exact-named tables of the wrong type are left to the readers' own checks. */
static int config_warn_unknown(toml_datum_t root)
{
    int unknown = 0;
    for (int i = 0; i < root.u.tab.size; i++) {
        const char  *k = root.u.tab.key[i];
        toml_datum_t v = root.u.tab.value[i];
        if (strcmp(k, "alert") == 0) {
            if (v.type != TOML_ARRAY)
                continue;
            for (int a = 0; a < v.u.arr.size; a++) {
                if (v.u.arr.elem[a].type != TOML_TABLE)
                    continue;
                char where[24];
                snprintf(where, sizeof(where), "alert[%d]", a);
                unknown += warn_unknown_in(v.u.arr.elem[a], where, ALERT_KEYS, N_KEYS(ALERT_KEYS));
            }
        } else if (strcmp(k, "collect") == 0) {
            if (v.type == TOML_TABLE)
                unknown += warn_unknown_in(v, k, COLLECT_KEYS, N_KEYS(COLLECT_KEYS));
        } else if (strcmp(k, "dashboard") == 0) {
            if (v.type == TOML_TABLE)
                unknown += warn_unknown_in(v, k, DASHBOARD_KEYS, N_KEYS(DASHBOARD_KEYS));
        } else if (strcmp(k, "server") == 0) {
            if (v.type == TOML_TABLE)
                unknown += warn_unknown_in(v, k, SERVER_KEYS, N_KEYS(SERVER_KEYS));
        } else if (strcmp(k, "webhook") == 0) {
            if (v.type == TOML_TABLE)
                unknown += warn_unknown_in(v, k, WEBHOOK_KEYS, N_KEYS(WEBHOOK_KEYS));
        } else {
            static const char *const TABLES[] = {"alert", "collect", "dashboard", "server",
                                                 "webhook"};
            unknown++;
            const char *close = NULL;
            for (int j = 0; j < N_KEYS(TABLES) && !close; j++)
                if (key_distance(k, TABLES[j]) <= 2)
                    close = TABLES[j];
            if (close)
                fprintf(stderr, "config: unknown top-level key '%s' (typo of '%s'?)\n", k, close);
            else
                fprintf(stderr, "config: unknown top-level key '%s'\n", k);
        }
    }
    return unknown;
}

/* --- Public API --- */

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
    snprintf(cfg->memory_chart_unit, sizeof(cfg->memory_chart_unit), "%s", "auto");
    snprintf(cfg->swap_card_unit, sizeof(cfg->swap_card_unit), "%s", "%");
    snprintf(cfg->swap_chart_unit, sizeof(cfg->swap_chart_unit), "%s", "auto");
    snprintf(cfg->disk_card_unit, sizeof(cfg->disk_card_unit), "%s", "%");
    snprintf(cfg->disk_chart_unit, sizeof(cfg->disk_chart_unit), "%s", "auto");
    snprintf(cfg->temp_card_unit, sizeof(cfg->temp_card_unit), "%s", "c");
    snprintf(cfg->temp_chart_unit, sizeof(cfg->temp_chart_unit), "%s", "c");
    cfg->temp_critical_fallback = 85.0f;
    snprintf(cfg->cpu_load_card_unit, sizeof(cfg->cpu_load_card_unit), "%s", "abs");
    snprintf(cfg->cpu_load_chart_unit, sizeof(cfg->cpu_load_chart_unit), "%s", "abs");
    snprintf(cfg->net_card_unit, sizeof(cfg->net_card_unit), "%s", "bytes");
    snprintf(cfg->net_chart_unit, sizeof(cfg->net_chart_unit), "%s", "bytes");
    cfg->net_max_speed = 0; /* unset: net_ref_bps prefers the detected link */
    snprintf(cfg->uptime_unit, sizeof(cfg->uptime_unit), "%s", "auto");
    cfg->chart_count = 0; /* 0 = show all in default order */
    cfg->card_count = 0;
    snprintf(cfg->ranges[0], sizeof(cfg->ranges[0]), "%s", "1d");
    snprintf(cfg->ranges[1], sizeof(cfg->ranges[1]), "%s", "7d");
    snprintf(cfg->ranges[2], sizeof(cfg->ranges[2]), "%s", "30d");
    snprintf(cfg->ranges[3], sizeof(cfg->ranges[3]), "%s", "90d");
    cfg->range_count = 4;
    cfg->max_dashboards = 4;
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
    int          too_long = 0;

    config_warn_unknown(root);

    /* [server] */
    v = toml_seek(root, "server.listen");
    too_long |= str_copy(cfg->listen, sizeof(cfg->listen), v, "server.listen") != 0;

    v = toml_seek(root, "server.max_dashboards");
    if (v.type == TOML_INT64 && v.u.int64 >= 1 && v.u.int64 <= 256)
        cfg->max_dashboards = (int)v.u.int64;
    else if (v.type == TOML_INT64 && v.u.int64 < 1) {
        fprintf(stderr, "config: max_dashboards must be >= 1 (got %ld); aborting\n",
                (long)v.u.int64);
        toml_free(res);

        return -1;
    } else if (v.type == TOML_INT64)
        fprintf(stderr, "config: max_dashboards must be <= 256 (got %ld); using default\n",
                (long)v.u.int64);

    v = toml_seek(root, "server.sse_keepalive");
    if (v.type == TOML_INT64 && v.u.int64 > 0)
        cfg->sse_keepalive_seconds = to_int(v.u.int64);
    else if (v.type == TOML_INT64)
        fprintf(stderr, "config: sse_keepalive must be > 0 (got %ld); using default\n",
                (long)v.u.int64);

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
            fprintf(stderr, "config: interval must be >= 1 second (got %ld); aborting\n",
                    (long)v.u.int64);
            toml_free(res);

            return -1;
        }

        if (v.u.int64 > 3600) {
            fprintf(stderr,
                    "config: interval must be <= 3600 seconds / 1 hour (got %ld); "
                    "clamping to 3600\n",
                    (long)v.u.int64);
            cfg->interval_seconds = 3600;
        } else {
            cfg->interval_seconds = (long)v.u.int64;
        }
    }

    v = toml_seek(root, "collect.db");
    too_long |= str_copy(cfg->db_path, sizeof(cfg->db_path), v, "collect.db") != 0;

    v = toml_seek(root, "collect.disk_path");
    too_long |= str_copy(cfg->disk_path, sizeof(cfg->disk_path), v, "collect.disk_path") != 0;

    struct statvfs sv;
    if (v.type == TOML_STRING && statvfs(cfg->disk_path, &sv) != 0) {
        fprintf(stderr, "config: collect.disk_path '%s': %s; aborting\n", cfg->disk_path,
                strerror(errno));
        toml_free(res);

        return -1;
    }

    /* [webhook] */
    v = toml_seek(root, "webhook.insecure_skip_verify");
    if (v.type == TOML_BOOLEAN)
        cfg->insecure_skip_verify = v.u.boolean ? 1 : 0;

    v = toml_seek(root, "webhook.ca_file");
    int ca_ok = str_copy(cfg->ca_file, sizeof(cfg->ca_file), v, "webhook.ca_file") == 0;
    too_long |= !ca_ok;

    if (v.type == TOML_STRING && ca_ok && cfg->insecure_skip_verify) {
        fprintf(stderr, "config: webhook.ca_file is ignored while insecure_skip_verify is true\n");
    } else if (v.type == TOML_STRING && ca_ok) {
        int has = has_certificate(cfg->ca_file);
        if (has <= 0) {
            if (has < 0)
                fprintf(stderr, "config: webhook.ca_file '%s': %s; aborting\n", cfg->ca_file,
                        strerror(errno));
            else
                fprintf(stderr, "config: webhook.ca_file '%s' has no certificate; aborting\n",
                        cfg->ca_file);
            toml_free(res);

            return -1;
        }
    }

    /* [dashboard] */
    v = toml_seek(root, "dashboard.title");
    too_long |= str_copy(cfg->title, sizeof(cfg->title), v, "dashboard.title") != 0;

    v = toml_seek(root, "dashboard.show_footer");
    if (v.type == TOML_BOOLEAN)
        cfg->show_footer = v.u.boolean ? 1 : 0;

    v = toml_seek(root, "dashboard.theme");
    if (v.type == TOML_STRING) {
        if (strcmp(v.u.s, "light") == 0 || strcmp(v.u.s, "dark") == 0 || strcmp(v.u.s, "auto") == 0)
            too_long |= str_copy(cfg->theme, sizeof(cfg->theme), v, "dashboard.theme") != 0;
        else
            fprintf(stderr, "config: invalid theme '%s', using default\n", v.u.s);
    }

    v = toml_seek(root, "dashboard.refresh");
    if (v.type == TOML_INT64 && v.u.int64 > 0)
        cfg->refresh_seconds = to_int(v.u.int64);
    else if (v.type == TOML_INT64)
        fprintf(stderr, "config: refresh must be > 0 (got %ld); using default\n", (long)v.u.int64);

    v = toml_seek(root, "dashboard.memory_card_unit");
    unit_copy(cfg->memory_card_unit, sizeof(cfg->memory_card_unit), v, "memory_card_unit",
              SIZE_UNITS);
    v = toml_seek(root, "dashboard.memory_chart_unit");
    unit_copy(cfg->memory_chart_unit, sizeof(cfg->memory_chart_unit), v, "memory_chart_unit",
              SIZE_UNITS);
    v = toml_seek(root, "dashboard.swap_card_unit");
    unit_copy(cfg->swap_card_unit, sizeof(cfg->swap_card_unit), v, "swap_card_unit", SIZE_UNITS);
    v = toml_seek(root, "dashboard.swap_chart_unit");
    unit_copy(cfg->swap_chart_unit, sizeof(cfg->swap_chart_unit), v, "swap_chart_unit", SIZE_UNITS);
    v = toml_seek(root, "dashboard.disk_card_unit");
    unit_copy(cfg->disk_card_unit, sizeof(cfg->disk_card_unit), v, "disk_card_unit", SIZE_UNITS);
    v = toml_seek(root, "dashboard.disk_chart_unit");
    unit_copy(cfg->disk_chart_unit, sizeof(cfg->disk_chart_unit), v, "disk_chart_unit", SIZE_UNITS);
    v = toml_seek(root, "dashboard.cpu_load_card_unit");
    unit_copy(cfg->cpu_load_card_unit, sizeof(cfg->cpu_load_card_unit), v, "cpu_load_card_unit",
              LOAD_UNITS);
    v = toml_seek(root, "dashboard.cpu_load_chart_unit");
    unit_copy(cfg->cpu_load_chart_unit, sizeof(cfg->cpu_load_chart_unit), v, "cpu_load_chart_unit",
              LOAD_UNITS);
    v = toml_seek(root, "dashboard.net_card_unit");
    unit_copy(cfg->net_card_unit, sizeof(cfg->net_card_unit), v, "net_card_unit", NET_UNITS);
    v = toml_seek(root, "dashboard.net_chart_unit");
    unit_copy(cfg->net_chart_unit, sizeof(cfg->net_chart_unit), v, "net_chart_unit", NET_UNITS);
    v = toml_seek(root, "dashboard.uptime_unit");
    unit_copy(cfg->uptime_unit, sizeof(cfg->uptime_unit), v, "uptime_unit", UPTIME_UNITS);

    v = toml_seek(root, "dashboard.net_max_speed");
    if (v.type == TOML_INT64 && v.u.int64 > 0 && v.u.int64 <= 1000000)
        cfg->net_max_speed = (int)v.u.int64;
    else if (v.type == TOML_INT64)
        fprintf(stderr,
                "config: net_max_speed must be 1..1000000 Mbit/s (got %ld); "
                "using default\n",
                (long)v.u.int64);

    v = toml_seek(root, "dashboard.charts");
    if (v.type == TOML_ARRAY) {
        if (v.u.arr.size == 0) {
            cfg->chart_count = -1; /* explicit empty: hide all */
        } else {
            for (int i = 0; i < v.u.arr.size && cfg->chart_count < MAX_CHARTS; i++) {
                toml_datum_t e = v.u.arr.elem[i];
                if (e.type != TOML_STRING)
                    continue;

                char *dst = cfg->charts[cfg->chart_count];
                if (str_copy(dst, sizeof(cfg->charts[0]), e, "dashboard.charts[%d]", i) != 0)
                    too_long = 1;
                else if (known_name(dst, CHART_NAMES, "charts", i))
                    cfg->chart_count++;
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
                if (e.type != TOML_STRING)
                    continue;

                char *dst = cfg->cards[cfg->card_count];
                if (str_copy(dst, sizeof(cfg->cards[0]), e, "dashboard.cards[%d]", i) != 0)
                    too_long = 1;
                else if (known_name(dst, CARD_NAMES, "cards", i))
                    cfg->card_count++;
            }
        }
    }

    v = toml_seek(root, "dashboard.temp_card_unit");
    unit_copy(cfg->temp_card_unit, sizeof(cfg->temp_card_unit), v, "temp_card_unit", TEMP_UNITS);
    v = toml_seek(root, "dashboard.temp_chart_unit");
    unit_copy(cfg->temp_chart_unit, sizeof(cfg->temp_chart_unit), v, "temp_chart_unit", TEMP_UNITS);

    v = toml_seek(root, "dashboard.temp_critical_fallback");
    if (v.type == TOML_FP64 && v.u.fp64 > 0)
        cfg->temp_critical_fallback = (float)v.u.fp64;
    else if (v.type == TOML_INT64 && v.u.int64 > 0)
        cfg->temp_critical_fallback = (float)v.u.int64;
    else if (v.type == TOML_FP64 || v.type == TOML_INT64)
        fprintf(stderr, "config: temp_critical_fallback must be > 0; using default\n");

    v = toml_seek(root, "dashboard.ranges");
    if (v.type == TOML_ARRAY && v.u.arr.size > MAX_RANGES) {
        fprintf(stderr, "config: dashboard.ranges has %d entries; the maximum is %d; aborting\n",
                v.u.arr.size, MAX_RANGES);
        toml_free(res);

        return -1;
    }

    if (v.type == TOML_ARRAY && v.u.arr.size > 0) {
        int count = 0;
        for (int i = 0; i < v.u.arr.size && count < MAX_RANGES; i++) {
            toml_datum_t e = v.u.arr.elem[i];
            if (e.type != TOML_STRING) {
                fprintf(stderr, "config: ranges[%d]: not a string, skipping\n", i);
                continue;
            }

            if (!valid_range(e.u.s)) {
                fprintf(stderr, "config: ranges[%d]: invalid '%s' (use <n>m, <n>h or <n>d)\n", i,
                        e.u.s);
                continue;
            }

            if (!range_in_bounds(e.u.s)) {
                fprintf(stderr,
                        "config: ranges[%d]: '%s' exceeds limit "
                        "(max 120 minutes, 72 hours, or 10 years)\n",
                        i, e.u.s);
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

            too_long |= str_copy(cfg->ranges[count++], sizeof(cfg->ranges[0]), e,
                                 "dashboard.ranges[%d]", i) != 0;
        }

        if (count > 0) {
            cfg->range_count = count;
        } else {
            /* Every entry was rejected: fail loud rather than fall back to the defaults. */
            fprintf(stderr, "config: dashboard.ranges has no valid entries (all rejected or "
                            "< interval); aborting\n");
            toml_free(res);

            return -1;
        }
    }

    /* [[alert]] */
    v = toml_seek(root, "alert");
    if (v.type != TOML_ARRAY && v.type != TOML_UNKNOWN) {
        fprintf(stderr,
                "config: [alert] must be an array of tables, written [[alert]]; aborting\n");
        toml_free(res);

        return -1;
    }

    if (v.type == TOML_ARRAY && v.u.arr.size > MAX_ALERTS) {
        fprintf(stderr, "config: %d [[alert]] tables; the maximum is %d; aborting\n", v.u.arr.size,
                MAX_ALERTS);
        toml_free(res);

        return -1;
    }

    if (v.type == TOML_ARRAY) {
        for (int i = 0; i < v.u.arr.size && cfg->alert_count < MAX_ALERTS; i++) {
            toml_datum_t e = v.u.arr.elem[i];
            if (e.type != TOML_TABLE) {
                fprintf(stderr, "config: alert[%d]: not a table; aborting\n", i);
                toml_free(res);

                return -1;
            }

            toml_datum_t dname = toml_get(e, "name");
            toml_datum_t dmet = toml_get(e, "metric");
            toml_datum_t dop = toml_get(e, "operator");
            toml_datum_t dthr = toml_get(e, "threshold");

            if (dname.type != TOML_STRING || dmet.type != TOML_STRING || dop.type != TOML_STRING) {
                fprintf(stderr, "config: alert[%d]: missing name, metric or operator; aborting\n",
                        i);
                toml_free(res);

                return -1;
            }

            /* A measured value almost never equals a threshold exactly, and no metric is below 0,
             * so "is zero" is <= 0. */
            if (strcmp(dop.u.s, "==") == 0) {
                fprintf(stderr,
                        "config: alert '%s': '==' is not supported; use '<= 0' to check for zero, "
                        "or '>=' or '<='; aborting\n",
                        dname.u.s);
                toml_free(res);

                return -1;
            }

            if (!valid_op(dop.u.s)) {
                fprintf(stderr, "config: alert '%s': unknown operator '%s'; aborting\n", dname.u.s,
                        dop.u.s);
                toml_free(res);

                return -1;
            }

            if (!in_list(dmet.u.s, METRIC_NAMES)) {
                fprintf(stderr, "config: alert '%s': unknown metric '%s'; aborting\n", dname.u.s,
                        dmet.u.s);
                toml_free(res);

                return -1;
            }

            double thr_val;
            if (dthr.type == TOML_FP64)
                thr_val = dthr.u.fp64;
            else if (dthr.type == TOML_INT64)
                thr_val = (double)dthr.u.int64;
            else {
                fprintf(stderr, "config: alert '%s': missing threshold; aborting\n", dname.u.s);
                toml_free(res);

                return -1;
            }

            alert_cfg_t *a = &cfg->alerts[cfg->alert_count];
            memset(a, 0, sizeof(*a));
            too_long |= str_copy(a->name, sizeof(a->name), dname, "alert[%d].name", i) != 0;
            too_long |= str_copy(a->metric, sizeof(a->metric), dmet, "alert[%d].metric", i) != 0;
            too_long |= str_copy(a->op, sizeof(a->op), dop, "alert[%d].operator", i) != 0;
            a->threshold = thr_val;
            too_long |= str_copy(a->webhook, sizeof(a->webhook), toml_get(e, "webhook"),
                                 "alert[%d].webhook", i) != 0;
            too_long |= str_copy(a->command, sizeof(a->command), toml_get(e, "command"),
                                 "alert[%d].command", i) != 0;

            toml_datum_t dcool = toml_get(e, "cooldown");
            if (dcool.type == TOML_STRING) {
                long d = parse_duration(dcool.u.s);
                if (d > 0)
                    a->cooldown_seconds = d;
                else
                    fprintf(stderr,
                            "config: alert '%s': invalid cooldown '%s'; no cooldown, it fires "
                            "every cycle\n",
                            a->name, dcool.u.s);
            } else if (dcool.type != TOML_UNKNOWN) {
                fprintf(stderr,
                        "config: alert '%s': cooldown is not a duration string such as \"5m\"; no "
                        "cooldown, it fires every cycle\n",
                        a->name);
            }

            if (!a->webhook[0] && !a->command[0]) {
                fprintf(stderr, "config: alert '%s': needs a webhook or a command; aborting\n",
                        a->name);
                toml_free(res);

                return -1;
            }

            cfg->alert_count++;
        }
    }

    toml_free(res);

    if (too_long)
        return -1;

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

int config_has(const char list[][16], int count, const char *name)
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
