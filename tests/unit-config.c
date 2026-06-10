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
 * Unit tests for src/config.c - zero-dependency, no framework. Build with:
 *   make test
 *
 * config.c is `#include`d directly so static helpers are exercisable. Each
 * test returns 0 on pass / 1 on fail; the shared harness lives in runner.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runner.h"

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
};

int main(void)
{
    /* Silence config_load's diagnostics during tests so the output stays
     * readable. Re-open later if a test needs to inspect stderr. */
    if (!freopen("/dev/null", "w", stderr))
        return 2;
    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
