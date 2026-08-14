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

/* Unit tests for src/config.c - zero-dependency, no framework. Build with:
 *   make test
 *
 * config.c is `#include`d directly so static helpers are exercisable. Each
 * test returns 0 on pass / 1 on fail; the shared harness lives in runner.h. */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runner.h"

/* Pull the module under test in directly. */
#include "../src/config.c"

/* --- Test infrastructure --- */

static char g_tmpcfg_path[256];

/* Write `toml` to a temp file, run config_load on it. Returns config_load's
 * return code. Leaves cfg in whatever state config_load left it. */
static int load_cfg(config_t *cfg, const char *toml)
{
    snprintf(g_tmpcfg_path, sizeof(g_tmpcfg_path), "/tmp/minimoni-test-%d.toml", getpid());
    FILE *f = fopen(g_tmpcfg_path, "w");
    if (!f)
        return -2;
    fputs(toml, f);
    fclose(f);

    config_defaults(cfg);
    int rc = config_load(cfg, g_tmpcfg_path);
    unlink(g_tmpcfg_path);
    return rc;
}

/* --- Interval: values --- */

static int test_interval_negative(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = -10\n") == -1 ? 0 : 1;
}

static int test_interval_zero(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = 0\n") == -1 ? 0 : 1;
}

static int test_interval_min_boundary(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 1\n") != 0)
        return 1;
    return cfg.interval_seconds == 1 ? 0 : 1;
}

static int test_interval_default_value(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 60\n") != 0)
        return 1;
    return cfg.interval_seconds == 60 ? 0 : 1;
}

static int test_interval_max_boundary(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 3600\n") != 0)
        return 1;
    return cfg.interval_seconds == 3600 ? 0 : 1;
}

static int test_interval_clamp(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 3601\n") != 0)
        return 1;
    return cfg.interval_seconds == 3600 ? 0 : 1;
}

static int test_interval_clamp_huge(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 99999\n") != 0)
        return 1;
    return cfg.interval_seconds == 3600 ? 0 : 1;
}

static int test_interval_missing(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ndb = \"/tmp/x.db\"\n") != 0)
        return 1;
    return cfg.interval_seconds == 60 ? 0 : 1;
}

/* --- Interval: wrong types --- */

static int test_interval_legacy_string(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = \"1m\"\n") == -1 ? 0 : 1;
}

static int test_interval_string_digits(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = \"60\"\n") == -1 ? 0 : 1;
}

static int test_interval_string_bogus(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = \"abc\"\n") == -1 ? 0 : 1;
}

static int test_interval_string_empty(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[collect]\ninterval = \"\"\n") == -1 ? 0 : 1;
}

static int test_interval_float(void)
{
    config_t cfg;
    /* TOML_FP64 is unhandled; falls through to default (60). */
    if (load_cfg(&cfg, "[collect]\ninterval = 60.5\n") != 0)
        return 1;
    return cfg.interval_seconds == 60 ? 0 : 1;
}

static int test_interval_bool(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = true\n") != 0)
        return 1;
    return cfg.interval_seconds == 60 ? 0 : 1;
}

static int test_interval_array(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = [60]\n") != 0)
        return 1;
    return cfg.interval_seconds == 60 ? 0 : 1;
}

/* --- Ranges: valid --- */

static int test_ranges_valid_natural(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"1d\", \"7d\", \"30d\", \"90d\"]\n") != 0)
        return 1;
    if (cfg.range_count != 4)
        return 1;
    return strcmp(cfg.ranges[0], "1d") == 0 && strcmp(cfg.ranges[3], "90d") == 0 ? 0 : 1;
}

static int test_ranges_valid_minutes(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"1m\", \"1h\", \"1d\"]\n") != 0)
        return 1;
    return cfg.range_count == 3 ? 0 : 1;
}

static int test_ranges_empty(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = []\n") != 0)
        return 1;
    /* Empty array: ignored, defaults remain */
    return cfg.range_count == 4 ? 0 : 1;
}

static int test_ranges_missing(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ndb = \"/tmp/x.db\"\n") != 0)
        return 1;
    return cfg.range_count == 4 ? 0 : 1;
}

/* --- Ranges: wrong types --- */

static int test_ranges_string_not_array(void)
{
    config_t cfg;
    /* String instead of array - silently ignored, defaults kept */
    if (load_cfg(&cfg, "[dashboard]\nranges = \"1d\"\n") != 0)
        return 1;
    return cfg.range_count == 4 ? 0 : 1;
}

static int test_ranges_int_array(void)
{
    config_t cfg;
    /* Ints get skipped (not strings); all skipped -> abort */
    return load_cfg(&cfg, "[dashboard]\nranges = [1, 2, 3]\n") == -1 ? 0 : 1;
}

static int test_ranges_bool_array(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [true, false]\n") == -1 ? 0 : 1;
}

static int test_ranges_nested_array(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [[\"1d\"]]\n") == -1 ? 0 : 1;
}

/* --- Ranges: invented / edge units --- */

static int test_ranges_weeks_unit(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"5w\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_years_unit(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"1y\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_uppercase_h(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"1H\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_uppercase_d(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"1D\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_extra_suffix(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"100ms\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_no_unit(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"1\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_no_number(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"d\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_empty_string(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_negative(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"-1d\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_with_space(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"1 d\"]\n") == -1 ? 0 : 1;
}

/* --- Ranges: per-unit upper bounds (caps) --- */

static int test_ranges_minutes_at_cap(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"120m\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_ranges_minutes_above_cap(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"121m\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_hours_at_cap(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"72h\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_ranges_hours_above_cap(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"73h\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_days_at_cap(void)
{
    config_t cfg;
    /* 3653d = max days in any 10-calendar-year window (3 leap years) */
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"3653d\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_ranges_days_above_cap(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard]\nranges = [\"3654d\"]\n") == -1 ? 0 : 1;
}

static int test_ranges_days_huge(void)
{
    config_t cfg;
    /* Typo where someone meant 3650d but added a zero - 36500d is ~100 years,
     * a plausible misconfiguration that should fail loud. */
    return load_cfg(&cfg, "[dashboard]\nranges = [\"36500d\"]\n") == -1 ? 0 : 1;
}

/* --- Combinations: interval + ranges --- */

static int test_combo_interval_eq_range_min(void)
{
    config_t cfg;
    /* interval=60 (= 1m), ranges=["1m"] -> 1m equals interval, ok */
    if (load_cfg(&cfg, "[collect]\ninterval = 60\n[dashboard]\nranges = [\"1m\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_combo_interval_eq_range_hour(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ninterval = 3600\n[dashboard]\nranges = [\"1h\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_combo_interval_eq_range_60m(void)
{
    config_t cfg;
    /* 60m == 1h == 3600s - same as interval, ok */
    if (load_cfg(&cfg, "[collect]\ninterval = 3600\n[dashboard]\nranges = [\"60m\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_combo_interval_skip_range(void)
{
    config_t cfg;
    /* 59m < interval (3600s), skipped, all skipped -> abort */
    return load_cfg(&cfg, "[collect]\ninterval = 3600\n[dashboard]\nranges = [\"59m\"]\n") == -1
               ? 0
               : 1;
}

static int test_combo_partial_skip(void)
{
    config_t cfg;
    /* 5m < interval, skipped; 1h valid -> 1 range */
    if (load_cfg(&cfg, "[collect]\ninterval = 600\n[dashboard]\nranges = [\"5m\", \"1h\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 && strcmp(cfg.ranges[0], "1h") == 0 ? 0 : 1;
}

static int test_combo_clamp_and_range(void)
{
    config_t cfg;
    /* interval clamped to 3600, then 1h range matches */
    if (load_cfg(&cfg, "[collect]\ninterval = 3601\n[dashboard]\nranges = [\"1h\"]\n") != 0)
        return 1;
    return cfg.interval_seconds == 3600 && cfg.range_count == 1 ? 0 : 1;
}

/* --- Order independence (retention = max regardless of position) --- */

static int test_order_largest_first(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"90d\", \"1d\", \"7d\"]\n") != 0)
        return 1;
    /* All three valid, stored in given order */
    return cfg.range_count == 3 && strcmp(cfg.ranges[0], "90d") == 0 &&
                   strcmp(cfg.ranges[1], "1d") == 0 && strcmp(cfg.ranges[2], "7d") == 0
               ? 0
               : 1;
}

static int test_order_largest_middle(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"1d\", \"90d\", \"7d\"]\n") != 0)
        return 1;
    return cfg.range_count == 3 && strcmp(cfg.ranges[1], "90d") == 0 ? 0 : 1;
}

/* --- Mixed valid + invalid --- */

static int test_mixed_some_invalid(void)
{
    config_t cfg;
    /* "bogus" skipped, others valid -> 2 entries */
    if (load_cfg(&cfg, "[dashboard]\nranges = [\"1d\", \"bogus\", \"7d\"]\n") != 0)
        return 1;
    return cfg.range_count == 2 && strcmp(cfg.ranges[0], "1d") == 0 &&
                   strcmp(cfg.ranges[1], "7d") == 0
               ? 0
               : 1;
}

static int test_mixed_skip_and_valid(void)
{
    config_t cfg;
    /* 5m < interval 600 -> skip; 1d valid -> 1 entry */
    if (load_cfg(&cfg, "[collect]\ninterval = 600\n[dashboard]\nranges = [\"1d\", \"5m\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 && strcmp(cfg.ranges[0], "1d") == 0 ? 0 : 1;
}

/* --- Points: removed key is ignored --- */

static int test_points_in_config_ignored(void)
{
    config_t cfg;
    /* dashboard.points was moved to a query parameter and removed from config.
     * A v0.1 config that still sets it must be silently ignored, not aborted. */
    if (load_cfg(&cfg, "[dashboard]\npoints = 999\n") != 0)
        return 1;
    /* ranges untouched -> defaults remain */
    return cfg.range_count == 4 ? 0 : 1;
}

/* --- parse_duration: per-unit 10-year caps --- */

static int test_duration_basics(void)
{
    return parse_duration("60s") == 60 && parse_duration("2m") == 120 &&
                   parse_duration("3h") == 10800 && parse_duration("1d") == 86400 &&
                   parse_duration("0d") == -1 && parse_duration("5x") == -1 &&
                   parse_duration("10") == -1
               ? 0
               : 1;
}

static int test_duration_caps(void)
{
    return parse_duration("3653d") == 3653L * 86400 && parse_duration("3654d") == -1 &&
                   parse_duration("87672h") == 3653L * 86400 && parse_duration("87673h") == -1 &&
                   parse_duration("315619200s") == 3653L * 86400 &&
                   parse_duration("315619201s") == -1
               ? 0
               : 1;
}

static int test_duration_overflow_typo(void)
{
    /* The pre-multiply guard: 999999999d would overflow a 32-bit long. */
    return parse_duration("999999999d") == -1 ? 0 : 1;
}

static int test_alert_cooldown_capped(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[[alert]]\nname = \"a\"\nmetric = \"m\"\noperator = \">\"\n"
                       "threshold = 1\nwebhook = \"http://x\"\ncooldown = \"999999999d\"\n") != 0)
        return 1;
    /* Invalid cooldown warns and stays 0 (alert itself is kept). */
    return cfg.alert_count == 1 && cfg.alerts[0].cooldown_seconds == 0 ? 0 : 1;
}

/* --- Unit values --- */

static int test_unit_invalid_keeps_default(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nmemory_card_unit = \"xyz\"\n") != 0)
        return 1;
    return strcmp(cfg.memory_card_unit, "%") == 0 ? 0 : 1;
}

static int test_unit_valid_applied(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nmemory_card_unit = \"auto\"\nnet_chart_unit = \"bits\"\n"
                       "temp_card_unit = \"f\"\nuptime_unit = \"d\"\n") != 0)
        return 1;
    return strcmp(cfg.memory_card_unit, "auto") == 0 && strcmp(cfg.net_chart_unit, "bits") == 0 &&
                   strcmp(cfg.temp_card_unit, "f") == 0 && strcmp(cfg.uptime_unit, "d") == 0
               ? 0
               : 1;
}

static int test_unit_case_sensitive(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nnet_card_unit = \"BYTES\"\n") != 0)
        return 1;
    return strcmp(cfg.net_card_unit, "bytes") == 0 ? 0 : 1;
}

/* Magnitudes are gone from the config surface (pre-1.0 break): the dashboard
 * picks them. A leftover "tb" or "mb" warns and falls back to the default. */
static int test_unit_magnitude_rejected(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nmemory_chart_unit = \"gb\"\ndisk_chart_unit = \"tb\"\n"
                       "net_card_unit = \"kb\"\n") != 0)
        return 1;
    return strcmp(cfg.memory_chart_unit, "auto") == 0 && strcmp(cfg.disk_chart_unit, "auto") == 0 &&
                   strcmp(cfg.net_card_unit, "bytes") == 0
               ? 0
               : 1;
}

/* --- [server] --- */

static int test_server_listen_and_max_dashboards(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[server]\nlisten = \"127.0.0.1:9999\"\nmax_dashboards = 16\n"
                       "sse_keepalive = 5\n") != 0)
        return 1;
    return strcmp(cfg.listen, "127.0.0.1:9999") == 0 && cfg.max_dashboards == 16 &&
                   cfg.sse_keepalive_seconds == 5
               ? 0
               : 1;
}

/* One dashboard is the smallest useful server; zero would serve the API and
 * never update a page, which is a config nobody means to write. */
static int test_server_max_dashboards_below_min_aborts(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[server]\nmax_dashboards = 0\n") == -1 ? 0 : 1;
}

static int test_server_max_dashboards_of_one_is_valid(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[server]\nmax_dashboards = 1\n") != 0)
        return 1;
    return cfg.max_dashboards == 1 ? 0 : 1;
}

static int test_server_max_dashboards_above_max_defaults(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[server]\nmax_dashboards = 999\n") != 0)
        return 1;
    return cfg.max_dashboards == 4 ? 0 : 1;
}

static int test_server_sse_keepalive_invalid_defaults(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[server]\nsse_keepalive = 0\n") != 0)
        return 1;
    return cfg.sse_keepalive_seconds == 1 ? 0 : 1;
}

/* --- [collect] paths --- */

static int test_collect_paths(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[collect]\ndb = \"/var/x.db\"\ndisk_path = \"/mnt\"\n") != 0)
        return 1;
    return strcmp(cfg.db_path, "/var/x.db") == 0 && strcmp(cfg.disk_path, "/mnt") == 0 ? 0 : 1;
}

/* --- [dashboard] scalars --- */

static int test_dashboard_title_footer_theme(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\ntitle = \"Pi\"\nshow_footer = false\ntheme = \"dark\"\n"
                       "refresh = 10\n") != 0)
        return 1;
    return strcmp(cfg.title, "Pi") == 0 && cfg.show_footer == 0 && strcmp(cfg.theme, "dark") == 0 &&
                   cfg.refresh_seconds == 10
               ? 0
               : 1;
}

static int test_dashboard_theme_invalid_defaults(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\ntheme = \"neon\"\n") != 0)
        return 1;
    return strcmp(cfg.theme, "auto") == 0 ? 0 : 1;
}

static int test_dashboard_refresh_invalid_defaults(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nrefresh = 0\n") != 0)
        return 1;
    return cfg.refresh_seconds == 30 ? 0 : 1;
}

static int test_temp_critical_fallback_variants(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\ntemp_critical_fallback = 95.5\n") != 0)
        return 1;
    if (cfg.temp_critical_fallback < 95.4f || cfg.temp_critical_fallback > 95.6f)
        return 1;
    if (load_cfg(&cfg, "[dashboard]\ntemp_critical_fallback = 90\n") != 0) /* int form */
        return 1;
    if (cfg.temp_critical_fallback < 89.9f || cfg.temp_critical_fallback > 90.1f)
        return 1;
    if (load_cfg(&cfg, "[dashboard]\ntemp_critical_fallback = -5\n") != 0) /* invalid */
        return 1;
    return cfg.temp_critical_fallback > 84.9f && cfg.temp_critical_fallback < 85.1f ? 0 : 1;
}

/* --- charts / cards visibility lists --- */

static int test_charts_cards_lists(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\ncharts = [\"cpu_load\", \"memory\"]\n"
                       "cards = [\"temp\"]\n") != 0)
        return 1;
    return cfg.chart_count == 2 && strcmp(cfg.charts[1], "memory") == 0 && cfg.card_count == 1 &&
                   strcmp(cfg.cards[0], "temp") == 0
               ? 0
               : 1;
}

static int test_charts_cards_empty_hides_all(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\ncharts = []\ncards = []\n") != 0)
        return 1;
    /* -1 is the explicit "hide all" marker; config_has must agree. */
    return cfg.chart_count == -1 && cfg.card_count == -1 &&
                   config_has(cfg.charts, cfg.chart_count, "memory") == 0
               ? 0
               : 1;
}

static int test_config_has_states(void)
{
    config_t cfg;
    config_defaults(&cfg);
    if (!config_has(cfg.charts, 0, "anything")) /* count 0 = show all */
        return 1;
    strcpy(cfg.charts[0], "temp");
    return config_has(cfg.charts, 1, "temp") == 1 && config_has(cfg.charts, 1, "net") == 0 ? 0 : 1;
}

/* --- [[alert]] --- */

static int test_alert_full_entry(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[[alert]]\nname = \"hot\"\nmetric = \"temp\"\noperator = \">=\"\n"
                       "threshold = 80.5\nwebhook = \"http://h\"\ncommand = \"echo hi\"\n"
                       "cooldown = \"5m\"\n") != 0)
        return 1;
    const alert_cfg_t *a = &cfg.alerts[0];
    return cfg.alert_count == 1 && strcmp(a->name, "hot") == 0 && strcmp(a->op, ">=") == 0 &&
                   a->threshold > 80.4 && a->threshold < 80.6 && a->cooldown_seconds == 300 &&
                   strcmp(a->webhook, "http://h") == 0 && strcmp(a->command, "echo hi") == 0
               ? 0
               : 1;
}

static int test_alert_int_threshold(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[[alert]]\nname = \"a\"\nmetric = \"m\"\noperator = \"<\"\n"
                       "threshold = 5\nwebhook = \"w\"\n") != 0)
        return 1;
    return cfg.alert_count == 1 && cfg.alerts[0].threshold > 4.9 ? 0 : 1;
}

static int test_alert_skips_incomplete(void)
{
    config_t cfg;
    /* missing operator, unknown operator, missing threshold, and no action:
     * each entry is skipped with a warning, none abort the load. */
    if (load_cfg(&cfg,
                 "[[alert]]\nname = \"a\"\nmetric = \"m\"\n"
                 "[[alert]]\nname = \"b\"\nmetric = \"m\"\noperator = \"~\"\n"
                 "threshold = 1\nwebhook = \"w\"\n"
                 "[[alert]]\nname = \"c\"\nmetric = \"m\"\noperator = \">\"\nwebhook = \"w\"\n"
                 "[[alert]]\nname = \"d\"\nmetric = \"m\"\noperator = \">\"\n"
                 "threshold = 1\n") != 0)
        return 1;
    return cfg.alert_count == 0 ? 0 : 1;
}

/* --- File-level errors --- */

static int test_missing_file_fails(void)
{
    config_t cfg;
    config_defaults(&cfg);
    return config_load(&cfg, "/nonexistent/minimoni-test.toml") == -1 ? 0 : 1;
}

static int test_malformed_toml_fails(void)
{
    config_t cfg;
    return load_cfg(&cfg, "[dashboard\ntitle = \n") == -1 ? 0 : 1;
}

static int test_config_open_keeps_defaults_without_file(void)
{
    config_t cfg;
    /* No explicit path and no config in the search paths: defaults, rc 0. */
    if (chdir("/tmp") != 0)
        return 1;
    unlink("/tmp/config.toml");
    if (config_open(&cfg, NULL) != 0)
        return 1;
    return cfg.interval_seconds == 60 && strcmp(cfg.title, "minimoni") == 0 ? 0 : 1;
}

/* --- Net link-speed fallback --- */

static int test_net_max_speed_default(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\ntitle = \"x\"\n") != 0)
        return 1;
    return cfg.net_max_speed == 0 ? 0 : 1; /* unset: the detected link is used */
}

static int test_net_max_speed_applied(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nnet_max_speed = 300\n") != 0)
        return 1;
    return cfg.net_max_speed == 300 ? 0 : 1;
}

static int test_net_max_speed_invalid(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nnet_max_speed = 0\n") != 0)
        return 1;
    return cfg.net_max_speed == 0 ? 0 : 1;
}

static int test_net_unit_percent_accepted(void)
{
    config_t cfg;
    if (load_cfg(&cfg, "[dashboard]\nnet_card_unit = \"%\"\n") != 0)
        return 1;
    return strcmp(cfg.net_card_unit, "%") == 0 ? 0 : 1;
}

/* --- Unknown keys --- */

/* Parse `toml` from memory and count the unknown keys it warns about. */
static int count_unknown(const char *toml)
{
    toml_result_t r = toml_parse(toml, (int)strlen(toml));
    if (!r.ok)
        return -9;
    int n = config_warn_unknown(r.toptab);
    toml_free(r);
    return n;
}

static int test_keys_all_known(void)
{
    return count_unknown("[server]\nlisten = \"0.0.0.0:1\"\nmax_dashboards = 4\n"
                         "[collect]\ninterval = 60\ndb = \"x\"\ndisk_path = \"/\"\n"
                         "[dashboard]\ntitle = \"t\"\ntheme = \"dark\"\nranges = [\"1d\"]\n"
                         "[[alert]]\nname = \"a\"\nmetric = \"m\"\noperator = \">\"\n"
                         "threshold = 1\nwebhook = \"w\"\ncooldown = \"5m\"\n") == 0
               ? 0
               : 1;
}
static int test_keys_typo_in_dashboard(void)
{
    return count_unknown("[dashboard]\ntitel = \"x\"\n") == 1 ? 0 : 1;
}

static int test_keys_typo_in_collect(void)
{
    return count_unknown("[collect]\nintevral = 60\ndsik_path = \"/\"\n") == 2 ? 0 : 1;
}

static int test_keys_typo_in_alert(void)
{
    return count_unknown("[[alert]]\nname = \"a\"\ncoolddown = \"5m\"\n") == 1 ? 0 : 1;
}

static int test_keys_unknown_table(void)
{
    const char *toml = "[dashbord]\ntitle = \"x\"\n"; /* codespell:ignore dashbord */
    return count_unknown(toml) == 1 ? 0 : 1;
}

static int test_keys_root_scalar(void) { return count_unknown("title = \"x\"\n") == 1 ? 0 : 1; }

static int test_keys_removed_points_warns(void)
{
    /* The removed v0.1 dashboard.points key: warned as unknown, never fatal. */
    return count_unknown("[dashboard]\npoints = 999\n") == 1 ? 0 : 1;
}

static int test_keys_typo_load_still_succeeds(void)
{
    config_t cfg;
    /* Typos warn but must not abort; the real key keeps its default. */
    if (load_cfg(&cfg, "[dashboard]\ntitel = \"My Server\"\n") != 0)
        return 1;
    return strcmp(cfg.title, "minimoni") == 0 ? 0 : 1;
}

static int test_key_distance(void)
{
    if (key_distance("titel", "title") != 2) /* codespell:ignore titel */
        return 1;
    return key_distance("intevral", "interval") == 2 && key_distance("title", "title") == 0 &&
                   key_distance("points", "title") > 2
               ? 0
               : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    /* interval: values */
    T(interval_negative),
    T(interval_zero),
    T(interval_min_boundary),
    T(interval_default_value),
    T(interval_max_boundary),
    T(interval_clamp),
    T(interval_clamp_huge),
    T(interval_missing),
    /* interval: wrong types */
    T(interval_legacy_string),
    T(interval_string_digits),
    T(interval_string_bogus),
    T(interval_string_empty),
    T(interval_float),
    T(interval_bool),
    T(interval_array),
    /* ranges: valid */
    T(ranges_valid_natural),
    T(ranges_valid_minutes),
    T(ranges_empty),
    T(ranges_missing),
    /* ranges: wrong types */
    T(ranges_string_not_array),
    T(ranges_int_array),
    T(ranges_bool_array),
    T(ranges_nested_array),
    /* ranges: invented / edge units */
    T(ranges_weeks_unit),
    T(ranges_years_unit),
    T(ranges_uppercase_h),
    T(ranges_uppercase_d),
    T(ranges_extra_suffix),
    T(ranges_no_unit),
    T(ranges_no_number),
    T(ranges_empty_string),
    T(ranges_negative),
    T(ranges_with_space),
    /* ranges: per-unit caps */
    T(ranges_minutes_at_cap),
    T(ranges_minutes_above_cap),
    T(ranges_hours_at_cap),
    T(ranges_hours_above_cap),
    T(ranges_days_at_cap),
    T(ranges_days_above_cap),
    T(ranges_days_huge),
    /* combinations */
    T(combo_interval_eq_range_min),
    T(combo_interval_eq_range_hour),
    T(combo_interval_eq_range_60m),
    T(combo_interval_skip_range),
    T(combo_partial_skip),
    T(combo_clamp_and_range),
    /* order independence */
    T(order_largest_first),
    T(order_largest_middle),
    /* mixed */
    T(mixed_some_invalid),
    T(mixed_skip_and_valid),
    /* points: removed key ignored */
    T(points_in_config_ignored),
    /* parse_duration caps */
    T(duration_basics),
    T(duration_caps),
    T(duration_overflow_typo),
    T(alert_cooldown_capped),
    /* unit values */
    T(unit_invalid_keeps_default),
    T(unit_valid_applied),
    T(unit_case_sensitive),
    T(unit_magnitude_rejected),
    /* [server] */
    T(server_listen_and_max_dashboards),
    T(server_max_dashboards_below_min_aborts),
    T(server_max_dashboards_of_one_is_valid),
    T(server_max_dashboards_above_max_defaults),
    T(server_sse_keepalive_invalid_defaults),
    /* [collect] paths */
    T(collect_paths),
    /* [dashboard] scalars */
    T(dashboard_title_footer_theme),
    T(dashboard_theme_invalid_defaults),
    T(dashboard_refresh_invalid_defaults),
    T(temp_critical_fallback_variants),
    /* charts / cards */
    T(charts_cards_lists),
    T(charts_cards_empty_hides_all),
    T(config_has_states),
    /* alerts */
    T(alert_full_entry),
    T(alert_int_threshold),
    T(alert_skips_incomplete),
    /* file-level errors */
    T(missing_file_fails),
    T(malformed_toml_fails),
    T(config_open_keeps_defaults_without_file),
    /* net link-speed fallback */
    T(net_max_speed_default),
    T(net_max_speed_applied),
    T(net_max_speed_invalid),
    T(net_unit_percent_accepted),
    /* unknown keys */
    T(keys_all_known),
    T(keys_typo_in_dashboard),
    T(keys_typo_in_collect),
    T(keys_typo_in_alert),
    T(keys_unknown_table),
    T(keys_root_scalar),
    T(keys_removed_points_warns),
    T(keys_typo_load_still_succeeds),
    T(key_distance),
};

int main(void)
{
    /* Silence config_load's diagnostics during tests so the output stays
     * readable. Re-open later if a test needs to inspect stderr. */
    if (!freopen("/dev/null", "w", stderr))
        return 2;
    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
