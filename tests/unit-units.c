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

/* --- net_convert: "%" of the link, or bytes/s untouched --- */

static int test_net_bytes_passthrough(void)
{
    return approx(net_convert(1048576.0, "bytes", 0.0), 1048576.0) ? 0 : 1;
}

static int test_net_bits_passthrough(void)
{
    return approx(net_convert(1048576.0, "bits", 0.0), 1048576.0) ? 0 : 1;
}

static int test_net_null_passthrough(void)
{
    return approx(net_convert(1048576.0, NULL, 0.0), 1048576.0) ? 0 : 1;
}

/* --- net percent + link-speed reference --- */

static int test_net_ref_configured_wins(void)
{
    /* The NIC may be faster than the uplink: 100 Mbit configured beats a 1 GbE link. */
    return approx(net_ref_bps(100, 1000), 12500000.0) ? 0 : 1;
}

static int test_net_ref_detected(void) { return approx(net_ref_bps(0, 1000), 125000000.0) ? 0 : 1; }

/* Nothing configured and nothing detected (wifi, virtual NIC): assume 1 GbE. */
static int test_net_ref_defaults_to_gbe(void)
{
    return approx(net_ref_bps(0, 0), 125000000.0) ? 0 : 1;
}

static int test_net_percent_half(void)
{
    /* Full duplex: each direction is scaled against the whole link. */
    double ref = net_ref_bps(1000, 0);
    return approx(net_convert(ref / 2.0, "%", ref), 50.0) ? 0 : 1;
}

static int test_net_percent_full(void)
{
    double ref = net_ref_bps(100, 0);
    return approx(net_convert(ref, "%", ref), 100.0) ? 0 : 1;
}

static int test_net_percent_no_ref(void)
{
    return approx(net_convert(1.0e6, "%", 0.0), 0.0) ? 0 : 1;
}

static int test_temp_null_unit_passthrough(void)
{
    return approx(temp_convert(50.0, NULL, "celsius", 85.0), 50.0) ? 0 : 1;
}

/* --- temp_convert (°C -> degrees or % in a scale) --- */

static int test_temp_celsius(void)
{
    return approx(temp_convert(50.0, "degrees", "celsius", 85.0), 50.0) ? 0 : 1;
}

static int test_temp_fahrenheit(void)
{
    return approx(temp_convert(100.0, "degrees", "fahrenheit", 85.0), 212.0) ? 0 : 1;
}

static int test_temp_fahrenheit_zero(void)
{
    return approx(temp_convert(0.0, "degrees", "fahrenheit", 85.0), 32.0) ? 0 : 1;
}

static int test_temp_percent_at_ref(void)
{
    return approx(temp_convert(85.0, "%", "celsius", 85.0), 100.0) ? 0 : 1;
}

static int test_temp_percent_half(void)
{
    return approx(temp_convert(42.5, "%", "celsius", 85.0), 50.0) ? 0 : 1;
}

/* In Fahrenheit the percent counts from 0 °F: 55 °C of 110 °C is 131 °F of 230 °F. */
static int test_temp_percent_in_fahrenheit(void)
{
    return approx(temp_convert(55.0, "%", "fahrenheit", 110.0), 13100.0 / 230.0) ? 0 : 1;
}

static int test_temp_percent_zero_ref_safe(void)
{
    /* ref <= 0 must not divide by zero; returns the degrees unchanged */
    return approx(temp_convert(50.0, "%", "celsius", 0.0), 50.0) ? 0 : 1;
}

/* --- load_convert --- */

static int test_load_percent(void) { return approx(load_convert(2.0, 4, "%"), 50.0) ? 0 : 1; }

static int test_load_abs(void) { return approx(load_convert(2.0, 4, "abs"), 2.0) ? 0 : 1; }

static int test_load_percent_zero_cores_safe(void)
{
    return approx(load_convert(2.0, 0, "%"), 2.0) ? 0 : 1;
}

/* --- temp_ref (the configured critical, else the sensor's, else 85) --- */

static int test_temp_ref_configured_wins_lower(void)
{
    return approx(temp_ref(80.0, 1, 110.0), 80.0) ? 0 : 1;
}

static int test_temp_ref_configured_wins_higher(void)
{
    return approx(temp_ref(120.0, 1, 110.0), 120.0) ? 0 : 1;
}

static int test_temp_ref_sensor_when_unset(void)
{
    return approx(temp_ref(0.0, 1, 105.0), 105.0) ? 0 : 1;
}

static int test_temp_ref_default_without_either(void)
{
    return approx(temp_ref(0.0, 0, 105.0), 85.0) ? 0 : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(net_bytes_passthrough),
    T(net_bits_passthrough),
    T(net_null_passthrough),
    T(net_ref_configured_wins),
    T(net_ref_detected),
    T(net_ref_defaults_to_gbe),
    T(net_percent_half),
    T(net_percent_full),
    T(net_percent_no_ref),
    T(temp_null_unit_passthrough),
    T(temp_celsius),
    T(temp_fahrenheit),
    T(temp_fahrenheit_zero),
    T(temp_percent_at_ref),
    T(temp_percent_half),
    T(temp_percent_in_fahrenheit),
    T(temp_percent_zero_ref_safe),
    T(load_percent),
    T(load_abs),
    T(load_percent_zero_cores_safe),
    T(temp_ref_configured_wins_lower),
    T(temp_ref_configured_wins_higher),
    T(temp_ref_sensor_when_unset),
    T(temp_ref_default_without_either),
};

int main(void) { return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0])); }
