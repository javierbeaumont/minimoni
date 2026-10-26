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

/* Unit tests for src/metrics.c: the pure rx/tx throughput computation, and the /proc/meminfo reader
 * fed synthetic text. Zero-dependency, no framework; build with `make test`. metrics.c is #included
 * directly so the static helpers are exercisable without reading /proc. */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <time.h>
#include <unistd.h>

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

/* --- Reading /proc/meminfo --- */

/* A FILE reading `text`, in place of the /proc file a reader opens. */
static FILE *reading(char *text) { return fmemopen(text, strlen(text), "r"); }

static int test_meminfo_reads_memory(void)
{
    char      text[] = "MemTotal: 4096 kB\nMemFree: 512 kB\nMemAvailable: 1024 kB\n";
    FILE     *f = reading(text);
    metrics_t m = {.swap_valid = 1};
    int       ok = f && read_meminfo(f, &m) == 0 && approx(m.mem_total_mb, 4.0) &&
                   approx(m.mem_available_mb, 1.0) && approx(m.mem_used_mb, 3.0) &&
                   approx(m.mem_percent, 75.0) && !m.swap_valid;
    if (f)
        fclose(f);

    return ok ? 0 : 1;
}

static int test_meminfo_without_memtotal_fails(void)
{
    char      text[] = "MemAvailable: 1024 kB\nSwapTotal: 2048 kB\nSwapFree: 512 kB\n";
    FILE     *f = reading(text);
    metrics_t m = {0};
    int       ok = f && read_meminfo(f, &m) == -1;
    if (f)
        fclose(f);

    return ok ? 0 : 1;
}

static int test_meminfo_reads_swap(void)
{
    char text[] =
        "MemTotal: 4096 kB\nMemAvailable: 1024 kB\nSwapTotal: 2048 kB\nSwapFree: 512 kB\n";
    FILE     *f = reading(text);
    metrics_t m = {0};
    int       ok = f && read_meminfo(f, &m) == 0 && m.swap_valid && approx(m.swap_total_mb, 2.0) &&
                   approx(m.swap_free_mb, 0.5);
    if (f)
        fclose(f);

    return ok ? 0 : 1;
}

static int test_meminfo_without_swap(void)
{
    char  text[] = "MemTotal: 4096 kB\nMemAvailable: 1024 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n";
    FILE *f = reading(text);
    metrics_t m = {.swap_valid = 1};
    int       ok = f && read_meminfo(f, &m) == 0 && !m.swap_valid;
    if (f)
        fclose(f);

    return ok ? 0 : 1;
}

/* --- Link speed (sysfs `speed` parsing) --- */

static int test_link_speed_parse(void)
{
    /* Virtual devices and down wifi report -1; garbage and empties are 0 too.
     * The 1e6 ceiling rejects nonsense without capping any real NIC. */
    return parse_link_speed("1000\n") == 1000 && parse_link_speed("10") == 10 &&
                   parse_link_speed("-1\n") == 0 && parse_link_speed("0\n") == 0 &&
                   parse_link_speed("") == 0 && parse_link_speed("abc") == 0 &&
                   parse_link_speed("2000000") == 0
               ? 0
               : 1;
}

static int test_link_speed_sum_is_sane(void)
{
    /* Reads the real /sys on the build host: only assert it cannot go negative
     * (containers usually expose veth with speed -1, so 0 is a valid answer). */
    return metrics_link_speed_mbit() >= 0 ? 0 : 1;
}

/* --- metrics_collect against the real /proc (the suite runs on Linux) --- */

static int test_collect_fills_plausible_values(void)
{
    metrics_t m;
    memset(&m, 0, sizeof(m));
    if (metrics_collect(&m, "/") != 0)
        return 1;

    /* Ranges, not exact values: this reads the live host. */
    if (m.load_1m < 0 || m.load_5m < 0 || m.load_15m < 0)
        return 1;

    if (!(m.mem_total_mb > 0) || m.mem_used_mb < 0 || m.mem_used_mb > m.mem_total_mb)
        return 1;

    if (m.mem_percent < 0 || m.mem_percent > 100)
        return 1;

    if (!(m.disk_total_gb > 0) || m.disk_percent < 0 || m.disk_percent > 100)
        return 1;

    if (m.swap_valid &&
        (!(m.swap_total_mb > 0) || m.swap_free_mb < 0 || m.swap_free_mb > m.swap_total_mb))
        return 1;

    return m.uptime_seconds > 0 ? 0 : 1;
}

static int test_collect_second_pass_yields_rates(void)
{
    metrics_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    /* The CPU/net snapshots are static, so priming may already have happened in
     * an earlier test: only the SECOND call of this pair is asserted. */
    if (metrics_collect(&a, "/") != 0)
        return 1;

    struct timespec pause = {0, 50 * 1000 * 1000}; /* 50 ms: enough for a delta */
    nanosleep(&pause, NULL);

    if (metrics_collect(&b, "/") != 0)
        return 1;
    if (!b.cpu_valid) /* /proc/stat always moves */
        return 1;
    if (b.cpu_user_percent < 0 || b.cpu_user_percent > 100)
        return 1;
    /* net_valid can be 0 if the counters did not move; when set, rates are >= 0. */
    return (!b.net_valid || (b.net_rx_bps >= 0 && b.net_tx_bps >= 0)) ? 0 : 1;
}

static int test_collect_bad_disk_path_fails(void)
{
    metrics_t m;
    memset(&m, 0, sizeof(m));
    return metrics_collect(&m, "/no/such/mount/point") == -1 ? 0 : 1;
}

/* --- A run of failed collections --- */

/* Feed `n` results of metrics_collect to one run; what it logged lands in `err`. */
static void note_results(const int *rcs, int n, char *err, size_t cap)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/minimoni-metrics-err-%d.txt", (int)getpid());

    fflush(stderr);
    if (!freopen(path, "w", stderr))
        return;

    metrics_run_t run = {0};
    for (int i = 0; i < n; i++)
        metrics_run_note(&run, rcs[i]);

    fflush(stderr);
    if (!freopen("/dev/null", "w", stderr))
        return;

    FILE  *f = fopen(path, "r");
    size_t len = f ? fread(err, 1, cap - 1, f) : 0;
    err[len] = '\0';

    if (f)
        fclose(f);
    unlink(path);
}

static int count(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p++)
        n++;

    return n;
}

/* A failing collection was once skipped in silence, every cycle: serve ran and stored nothing. */
static int test_failure_run_reported_once(void)
{
    static const int rcs[] = {-1, -1, -1, 0};
    char             err[1024];
    note_results(rcs, 4, err, sizeof(err));

    return count(err, "metrics: collection failed") == 1 &&
                   count(err, "metrics: collection recovered after 3 failed cycles") == 1
               ? 0
               : 1;
}

static int test_each_failure_run_reported(void)
{
    static const int rcs[] = {-1, 0, -1, -1, 0};
    char             err[1024];
    note_results(rcs, 5, err, sizeof(err));

    return count(err, "metrics: collection failed") == 2 &&
                   count(err, "recovered after 1 failed cycles") == 1 &&
                   count(err, "recovered after 2 failed cycles") == 1
               ? 0
               : 1;
}

static int test_success_is_silent(void)
{
    static const int rcs[] = {0, 0, 0};
    char             err[1024];
    note_results(rcs, 3, err, sizeof(err));

    return err[0] == '\0' ? 0 : 1;
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
    T(meminfo_reads_memory),
    T(meminfo_without_memtotal_fails),
    T(meminfo_reads_swap),
    T(meminfo_without_swap),
    T(link_speed_parse),
    T(link_speed_sum_is_sane),
    T(collect_fills_plausible_values),
    T(collect_second_pass_yields_rates),
    T(collect_bad_disk_path_fails),
    T(failure_run_reported_once),
    T(each_failure_run_reported),
    T(success_is_silent),
};

int main(void)
{
    if (!freopen("/dev/null", "w", stderr))
        return 2;

    return run_tests(ALL_TESTS, sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]));
}
