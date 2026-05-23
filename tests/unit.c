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
