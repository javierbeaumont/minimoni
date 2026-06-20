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

/* Unit tests for src/metrics.c net_rate() - the pure rx/tx throughput
 * computation extracted from collect_net. Zero-dependency, no framework; build
 * with `make test`. metrics.c is #included directly so the static helper is
 * exercisable without reading /proc; each test feeds two synthetic snapshots. */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <time.h>

#include "runner.h"

#include "../src/metrics.c"

/* --- net_rate --- */

static net_raw_t snap(int64_t rx, int64_t tx, long sec, long nsec)
{
    net_raw_t s;
    s.rx = rx;
    s.tx = tx;
    s.t.tv_sec = sec;
    s.t.tv_nsec = nsec;
    return s;
}

static int approx(double a, double b)
{
    double d = a - b;
    return (d < 0 ? -d : d) < 1e-6;
}

static int test_net_rate_basic(void)
{
    net_raw_t prev = snap(0, 0, 0, 0);
    net_raw_t cur = snap(1000, 2000, 1, 0);
    double    rx = 0, tx = 0;
    int       ok = net_rate(&prev, &cur, &rx, &tx);
    return (ok && approx(rx, 1000.0) && approx(tx, 2000.0)) ? 0 : 1;
}

static int test_net_rate_subsecond(void)
{
    /* 250 ms gap (the one-shot collect-mode interval): dt must use the
     * nanoseconds and not round to 0. 250 B over 0.25 s = 1000 B/s. */
    net_raw_t prev = snap(0, 0, 0, 0);
    net_raw_t cur = snap(250, 500, 0, 250000000L);
    double    rx = 0, tx = 0;
    int       ok = net_rate(&prev, &cur, &rx, &tx);
    return (ok && approx(rx, 1000.0) && approx(tx, 2000.0)) ? 0 : 1;
}

static int test_net_rate_subsecond_across_second(void)
{
    /* 10.8 s -> 11.05 s = 0.25 s, with nsec smaller in cur (crosses a second). */
    net_raw_t prev = snap(0, 0, 10, 800000000L);
    net_raw_t cur = snap(250, 500, 11, 50000000L);
    double    rx = 0, tx = 0;
    int       ok = net_rate(&prev, &cur, &rx, &tx);
    return (ok && approx(rx, 1000.0) && approx(tx, 2000.0)) ? 0 : 1;
}

static int test_net_rate_zero_dt(void)
{
    /* Same instant: no elapsed time, so a rate is undefined -> gap. */
    net_raw_t prev = snap(0, 0, 5, 0);
    net_raw_t cur = snap(1000, 1000, 5, 0);
    double    rx = -1, tx = -1;
    return net_rate(&prev, &cur, &rx, &tx) == 0 ? 0 : 1;
}

static int test_net_rate_negative_dt(void)
{
    /* Clock stepped backwards across the sample -> gap. */
    net_raw_t prev = snap(0, 0, 5, 0);
    net_raw_t cur = snap(1000, 1000, 4, 0);
    double    rx = -1, tx = -1;
    return net_rate(&prev, &cur, &rx, &tx) == 0 ? 0 : 1;
}

static int test_net_rate_counter_reset_rx(void)
{
    /* rx counter went backwards (interface reset): gap, not a negative rate. */
    net_raw_t prev = snap(1000, 1000, 0, 0);
    net_raw_t cur = snap(500, 2000, 1, 0);
    double    rx = -1, tx = -1;
    return net_rate(&prev, &cur, &rx, &tx) == 0 ? 0 : 1;
}

static int test_net_rate_counter_reset_tx(void)
{
    net_raw_t prev = snap(1000, 1000, 0, 0);
    net_raw_t cur = snap(2000, 500, 1, 0);
    double    rx = -1, tx = -1;
    return net_rate(&prev, &cur, &rx, &tx) == 0 ? 0 : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(net_rate_basic),
    T(net_rate_subsecond),
    T(net_rate_subsecond_across_second),
    T(net_rate_zero_dt),
    T(net_rate_negative_dt),
    T(net_rate_counter_reset_rx),
    T(net_rate_counter_reset_tx),
};

int main(void) { return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0])); }
