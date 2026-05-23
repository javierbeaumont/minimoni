/*
 * minimoni — zero-dependency system monitoring
 * Copyright (C) 2026 Javier Beaumont <javierbeaumont@users.noreply.github.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

/*
 * Unit tests — zero-dependency, no framework. Build with:
 *   make test
 *
 * Each module under test is `#include`d directly so static helpers are
 * exercisable. The runner walks an array of {name, fn} pairs, prints
 * pass/fail, and exits non-zero on any failure.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Pull the module under test in directly. */
#include "../src/config.c"

/* --- Test infrastructure ------------------------------------------------- */

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

/* --- Interval: values --------------------------------------------------- */

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

/* --- Interval: wrong types ---------------------------------------------- */

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

/* --- Ranges: valid ------------------------------------------------------ */

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

/* --- Ranges: wrong types ----------------------------------------------- */

static int test_ranges_string_not_array(void)
{
    config_t cfg;
    /* String instead of array — silently ignored, defaults kept */
    if (load_cfg(&cfg, "[dashboard]\nranges = \"1d\"\n") != 0)
        return 1;
    return cfg.range_count == 4 ? 0 : 1;
}

static int test_ranges_int_array(void)
{
    config_t cfg;
    /* Ints get skipped (not strings); all skipped → abort */
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

/* --- Ranges: invented / edge units ------------------------------------- */

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

/* --- Ranges: per-unit upper bounds (caps) ------------------------------ */

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
    /* Typo where someone meant 3650d but added a zero — 36500d is ~100 years,
     * a plausible misconfiguration that should fail loud. */
    return load_cfg(&cfg, "[dashboard]\nranges = [\"36500d\"]\n") == -1 ? 0 : 1;
}

/* --- Combinations: interval + ranges ------------------------------------ */

static int test_combo_interval_eq_range_min(void)
{
    config_t cfg;
    /* interval=60 (= 1m), ranges=["1m"] → 1m equals interval, ok */
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
    /* 60m == 1h == 3600s — same as interval, ok */
    if (load_cfg(&cfg, "[collect]\ninterval = 3600\n[dashboard]\nranges = [\"60m\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 ? 0 : 1;
}

static int test_combo_interval_skip_range(void)
{
    config_t cfg;
    /* 59m < interval (3600s), skipped, all skipped → abort */
    return load_cfg(&cfg, "[collect]\ninterval = 3600\n[dashboard]\nranges = [\"59m\"]\n") == -1
               ? 0
               : 1;
}

static int test_combo_partial_skip(void)
{
    config_t cfg;
    /* 5m < interval, skipped; 1h valid → 1 range */
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

/* --- Order independence (retention = max regardless of position) ------- */

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

/* --- Mixed valid + invalid --------------------------------------------- */

static int test_mixed_some_invalid(void)
{
    config_t cfg;
    /* "bogus" skipped, others valid → 2 entries */
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
    /* 5m < interval 600 → skip; 1d valid → 1 entry */
    if (load_cfg(&cfg, "[collect]\ninterval = 600\n[dashboard]\nranges = [\"1d\", \"5m\"]\n") != 0)
        return 1;
    return cfg.range_count == 1 && strcmp(cfg.ranges[0], "1d") == 0 ? 0 : 1;
}

/* --- Runner ------------------------------------------------------------ */

struct test {
    const char *name;
    int (*fn)(void);
};

#define T(n) {#n, test_##n}

static const struct test ALL_TESTS[] = {
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
};

#define NUM_TESTS (sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]))

int main(void)
{
    /* Silence config_load's diagnostics during tests so the output stays
     * readable. Re-open later if a test needs to inspect stderr. */
    if (!freopen("/dev/null", "w", stderr))
        return 2;

    int failed = 0;
    for (size_t i = 0; i < NUM_TESTS; i++) {
        printf("  %-35s ", ALL_TESTS[i].name);
        fflush(stdout);
        int r = ALL_TESTS[i].fn();
        if (r) {
            failed++;
            printf("FAIL\n");
        } else {
            printf("ok\n");
        }
    }

    printf("\n  %zu/%zu tests passed\n", NUM_TESTS - failed, NUM_TESTS);
    return failed ? 1 : 0;
}
