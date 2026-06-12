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
 * Unit tests for src/downsample.c (pick_bucket) - zero-dependency, no
 * framework. Build with: make test. The module is #included directly.
 *
 * BUCKETS = {60,120,300,600,900,1800,3600,7200,10800,21600,43200,86400}.
 * Expected bucket values below are computed by hand from pick_bucket's rule:
 * pick the bucket b (divisible by interval) minimising |range/b - points|.
 */

#include "runner.h"

#include "../src/downsample.c"

/* --- Raw (no aggregation) cases --- */

static int test_raw_when_fewer_rows(void)
{
    /* actual_count (100) <= points (480) -> serve raw */
    return pick_bucket(86400, 60, 480, 100) == 0 ? 0 : 1;
}

static int test_raw_when_count_equals_points(void)
{
    return pick_bucket(86400, 60, 480, 480) == 0 ? 0 : 1;
}

static int test_raw_when_ideal_le_interval(void)
{
    /* range 3600 / 480 = 7s ideal, <= interval 60 -> raw */
    return pick_bucket(3600, 60, 480, -1) == 0 ? 0 : 1;
}

static int test_raw_zero_range(void) { return pick_bucket(0, 60, 480, -1) == 0 ? 0 : 1; }

/* --- Default points (<=0 uses 240) --- */

static int test_default_points_matches_240(void)
{
    /* points<=0 must behave exactly like points==240 */
    return pick_bucket(2592000, 60, 0, -1) == pick_bucket(2592000, 60, 240, -1) ? 0 : 1;
}

static int test_default_points_negative(void)
{
    /* 30d at interval 60, default 240: 2592000/10800 = 240 exactly -> 10800 */
    return pick_bucket(2592000, 60, -5, -1) == 10800 ? 0 : 1;
}

/* --- Bucket selection (exact, hand-computed) --- */

static int test_30d_480_picks_7200(void)
{
    /* 2592000/7200 = 360, |360-480|=120 is the minimum diff */
    return pick_bucket(2592000, 60, 480, 1000000) == 7200 ? 0 : 1;
}

static int test_90d_480_picks_21600(void)
{
    /* 7776000/21600 = 360, |360-480|=120 minimal */
    return pick_bucket(7776000, 60, 480, 100000000) == 21600 ? 0 : 1;
}

static int test_small_points_picks_900(void)
{
    /* 1d at points=100: 86400/900 = 96, |96-100|=4 minimal */
    return pick_bucket(86400, 60, 100, 100000) == 900 ? 0 : 1;
}

/* --- Interval divisibility --- */

static int test_bucket_divisible_by_interval(void)
{
    /* interval 300: only buckets that are multiples of 300 are eligible */
    int b = pick_bucket(2592000, 300, 480, 1000000);
    return (b == 7200 && b % 300 == 0) ? 0 : 1;
}

static int test_fallback_to_interval_when_no_divisor(void)
{
    /* interval 7 divides no bucket -> fall back to interval_sec */
    return pick_bucket(86400, 7, 480, 100000) == 7 ? 0 : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    /* raw cases */
    T(raw_when_fewer_rows),
    T(raw_when_count_equals_points),
    T(raw_when_ideal_le_interval),
    T(raw_zero_range),
    /* default points */
    T(default_points_matches_240),
    T(default_points_negative),
    /* bucket selection */
    T(30d_480_picks_7200),
    T(90d_480_picks_21600),
    T(small_points_picks_900),
    /* interval divisibility */
    T(bucket_divisible_by_interval),
    T(fallback_to_interval_when_no_divisor),
};

int main(void) { return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0])); }
