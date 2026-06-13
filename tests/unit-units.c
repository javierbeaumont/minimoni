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
 * Unit tests for src/units.c - zero-dependency, no framework. Build with:
 *   make test. The module is #included directly.
 */

#include "runner.h"

#include "../src/units.c"

static int approx(double a, double b)
{
    double d = a - b;
    if (d < 0)
        d = -d;
    return d < 1e-9;
}

/* --- net_convert (bps -> mb/gb/mbps/gbps) ------------------------------ */

static int test_net_mb(void) { return approx(net_convert(1048576.0, "mb"), 1.0) ? 0 : 1; }

static int test_net_gb(void) { return approx(net_convert(1073741824.0, "gb"), 1.0) ? 0 : 1; }

static int test_net_mbps(void) { return approx(net_convert(1.0e6, "mbps"), 8.0) ? 0 : 1; }

static int test_net_gbps(void) { return approx(net_convert(1.0e9, "gbps"), 8.0) ? 0 : 1; }

static int test_net_null_defaults_mb(void)
{
    return approx(net_convert(1048576.0, NULL), 1.0) ? 0 : 1;
}

/* --- mem_convert / disk_convert ---------------------------------------- */

static int test_mem_gb(void) { return approx(mem_convert(1024.0, "gb"), 1.0) ? 0 : 1; }

static int test_mem_mb_passthrough(void) { return approx(mem_convert(512.0, "mb"), 512.0) ? 0 : 1; }

static int test_disk_tb(void) { return approx(disk_convert(1024.0, "tb"), 1.0) ? 0 : 1; }

static int test_disk_gb_passthrough(void) { return approx(disk_convert(50.0, "gb"), 50.0) ? 0 : 1; }

/* --- temp_convert (celsius -> c/f/%) ----------------------------------- */

static int test_temp_celsius(void) { return approx(temp_convert(50.0, "c", 85.0), 50.0) ? 0 : 1; }

static int test_temp_fahrenheit(void)
{
    return approx(temp_convert(100.0, "f", 85.0), 212.0) ? 0 : 1;
}

static int test_temp_fahrenheit_zero(void)
{
    return approx(temp_convert(0.0, "f", 85.0), 32.0) ? 0 : 1;
}

static int test_temp_percent_at_ref(void)
{
    return approx(temp_convert(85.0, "%", 85.0), 100.0) ? 0 : 1;
}

static int test_temp_percent_half(void)
{
    return approx(temp_convert(42.5, "%", 85.0), 50.0) ? 0 : 1;
}

static int test_temp_percent_zero_ref_safe(void)
{
    /* ref <= 0 must not divide by zero; returns celsius unchanged */
    return approx(temp_convert(50.0, "%", 0.0), 50.0) ? 0 : 1;
}

/* --- load_convert ------------------------------------------------------ */

static int test_load_percent(void) { return approx(load_convert(2.0, 4, "%"), 50.0) ? 0 : 1; }

static int test_load_abs(void) { return approx(load_convert(2.0, 4, "abs"), 2.0) ? 0 : 1; }

static int test_load_percent_zero_cores_safe(void)
{
    return approx(load_convert(2.0, 0, "%"), 2.0) ? 0 : 1;
}

/* --- temp_ref (critical when valid, else fallback) --------------------- */

static int test_temp_ref_uses_critical(void)
{
    return approx(temp_ref(1, 105.0, 85.0), 105.0) ? 0 : 1;
}

static int test_temp_ref_falls_back(void) { return approx(temp_ref(0, 105.0, 85.0), 85.0) ? 0 : 1; }

/* --- Runner ------------------------------------------------------------ */

static const test_t ALL_TESTS[] = {
    T(net_mb),
    T(net_gb),
    T(net_mbps),
    T(net_gbps),
    T(net_null_defaults_mb),
    T(mem_gb),
    T(mem_mb_passthrough),
    T(disk_tb),
    T(disk_gb_passthrough),
    T(temp_celsius),
    T(temp_fahrenheit),
    T(temp_fahrenheit_zero),
    T(temp_percent_at_ref),
    T(temp_percent_half),
    T(temp_percent_zero_ref_safe),
    T(load_percent),
    T(load_abs),
    T(load_percent_zero_cores_safe),
    T(temp_ref_uses_critical),
    T(temp_ref_falls_back),
};

int main(void) { return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0])); }
