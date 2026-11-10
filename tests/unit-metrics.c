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

/* Unit tests for src/metrics.c: the pure rate computation, the /proc/stat, /proc/meminfo,
 * /proc/vmstat and /proc/net/dev readers fed synthetic text, and the thermal zone and network
 * interface readers fed a synthetic sysfs tree.
 * Zero-dependency, no framework; build with `make test`. metrics.c is #included directly so the
 * static helpers are exercisable without reading /proc or /sys. */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "runner.h"

#include "../src/metrics.c"

/* --- counter_rates --- */

static counters_t snap(int64_t in, int64_t out, long sec, long nsec)
{
    counters_t s;
    s.in = in;
    s.out = out;
    s.t.tv_sec = sec;
    s.t.tv_nsec = nsec;

    return s;
}

static int approx(double a, double b)
{
    double d = a - b;

    return (d < 0 ? -d : d) < 1e-6;
}

static int test_counter_rates_basic(void)
{
    counters_t prev = snap(0, 0, 0, 0);
    counters_t cur = snap(1000, 2000, 1, 0);
    double     in = 0, out = 0;
    int        ok = counter_rates(&prev, &cur, &in, &out);

    return ok && approx(in, 1000.0) && approx(out, 2000.0) ? 0 : 1;
}

/* 250 ms gap (the one-shot collect interval): dt must use the nanoseconds and not round to 0.
 * 250 B over 0.25 s = 1000 B/s. */
static int test_counter_rates_subsecond(void)
{
    counters_t prev = snap(0, 0, 0, 0);
    counters_t cur = snap(250, 500, 0, 250000000L);
    double     in = 0, out = 0;
    int        ok = counter_rates(&prev, &cur, &in, &out);

    return ok && approx(in, 1000.0) && approx(out, 2000.0) ? 0 : 1;
}

/* 10.8 s -> 11.05 s = 0.25 s, with nsec smaller in cur (crosses a second). */
static int test_counter_rates_subsecond_across_second(void)
{
    counters_t prev = snap(0, 0, 10, 800000000L);
    counters_t cur = snap(250, 500, 11, 50000000L);
    double     in = 0, out = 0;
    int        ok = counter_rates(&prev, &cur, &in, &out);

    return ok && approx(in, 1000.0) && approx(out, 2000.0) ? 0 : 1;
}

/* Same instant: no elapsed time, so a rate is undefined -> gap. */
static int test_counter_rates_zero_dt(void)
{
    counters_t prev = snap(0, 0, 5, 0);
    counters_t cur = snap(1000, 1000, 5, 0);
    double     in = -1, out = -1;

    return counter_rates(&prev, &cur, &in, &out) == 0 ? 0 : 1;
}

/* Clock stepped backwards across the sample -> gap. */
static int test_counter_rates_negative_dt(void)
{
    counters_t prev = snap(0, 0, 5, 0);
    counters_t cur = snap(1000, 1000, 4, 0);
    double     in = -1, out = -1;

    return counter_rates(&prev, &cur, &in, &out) == 0 ? 0 : 1;
}

/* The `in` counter went backwards (an interface or the kernel reset it): gap, not a negative
 * rate. */
static int test_counter_rates_in_reset_is_a_gap(void)
{
    counters_t prev = snap(1000, 1000, 0, 0);
    counters_t cur = snap(500, 2000, 1, 0);
    double     in = -1, out = -1;

    return counter_rates(&prev, &cur, &in, &out) == 0 ? 0 : 1;
}

static int test_counter_rates_out_reset_is_a_gap(void)
{
    counters_t prev = snap(1000, 1000, 0, 0);
    counters_t cur = snap(2000, 500, 1, 0);
    double     in = -1, out = -1;

    return counter_rates(&prev, &cur, &in, &out) == 0 ? 0 : 1;
}

/* --- Reading /proc/meminfo and /proc/vmstat --- */

/* A FILE reading `text`, in place of the /proc file a reader opens. */
static FILE *reading(char *text) { return fmemopen(text, strlen(text), "r"); }

static int test_meminfo_reads_memory(void)
{
    char      text[] = "MemTotal: 4096 kB\nMemFree: 512 kB\nMemAvailable: 1024 kB\n";
    FILE     *f = reading(text);
    metrics_t m = {.swap_valid = 1};
    int       ok = f && read_meminfo(f, &m) == 0 && approx(m.mem_total_mb, 4.0) &&
                   approx(m.mem_available_mb, 1.0) && !m.swap_valid;
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

static int test_vmstat_swap_counters(void)
{
    char    text[] = "pgpgin 5\npswpin 12\npswpout 34\nnr_free_pages 9\n";
    FILE   *f = reading(text);
    int64_t in = 0, out = 0;
    int     ok = f && read_vmstat_swap(f, &in, &out) == 0 && in == 12 && out == 34;
    if (f)
        fclose(f);

    return ok ? 0 : 1;
}

static int test_vmstat_without_swap_counters(void)
{
    char    text[] = "pgpgin 5\npswpin 12\n";
    FILE   *f = reading(text);
    int64_t in = 0, out = 0;
    int     ok = f && read_vmstat_swap(f, &in, &out) == -1;
    if (f)
        fclose(f);

    return ok ? 0 : 1;
}

/* --- Swap I/O, one sample at a time --- */

/* One sample_swap_io over `vmstat` (NULL: the file did not open) at second `sec`. */
static void sample_vmstat(metrics_t *m, const char *vmstat, long sec)
{
    char text[128];
    snprintf(text, sizeof(text), "%s", vmstat ? vmstat : "");
    FILE           *f = vmstat ? reading(text) : NULL;
    struct timespec now = {sec, 0};

    sample_swap_io(f, now, m);

    if (f)
        fclose(f);
}

static int test_swap_io_need_a_previous_sample(void)
{
    metrics_t m = {.swap_valid = 1};
    s_prev_swap_io_valid = 0;

    sample_vmstat(&m, "pswpin 10\npswpout 20\n", 10);

    return m.swap_io_valid ? 1 : 0;
}

static int test_swap_io_in_bytes_per_second(void)
{
    metrics_t m = {.swap_valid = 1};
    double    page = (double)sysconf(_SC_PAGESIZE);
    s_prev_swap_io_valid = 0;

    sample_vmstat(&m, "pswpin 10\npswpout 20\n", 10);
    sample_vmstat(&m, "pswpin 14\npswpout 20\n", 12);

    /* 4 pages read back in 2 s, none written */
    int in = approx(m.swap_in_bps, 2.0 * page);
    int out = approx(m.swap_out_bps, 0.0);

    return m.swap_io_valid && in && out ? 0 : 1;
}

static int test_swap_io_skipped_without_swap(void)
{
    metrics_t m = {.swap_valid = 0};
    s_prev_swap_io_valid = 0;

    sample_vmstat(&m, "pswpin 10\npswpout 20\n", 10);
    sample_vmstat(&m, "pswpin 14\npswpout 22\n", 11);

    return m.swap_io_valid ? 1 : 0;
}

static int test_swap_io_start_over_when_vmstat_cannot_be_opened(void)
{
    metrics_t m = {.swap_valid = 1};
    s_prev_swap_io_valid = 0;

    sample_vmstat(&m, "pswpin 10\npswpout 20\n", 10);
    sample_vmstat(&m, NULL, 11);
    sample_vmstat(&m, "pswpin 12\npswpout 20\n", 12);
    int after_gap = m.swap_io_valid;
    sample_vmstat(&m, "pswpin 14\npswpout 20\n", 13);

    return !after_gap && m.swap_io_valid ? 0 : 1;
}

static int test_swap_io_start_over_after_a_missing_counter(void)
{
    metrics_t m = {.swap_valid = 1};
    s_prev_swap_io_valid = 0;

    sample_vmstat(&m, "pswpin 10\npswpout 20\n", 10);
    sample_vmstat(&m, "pswpin 11\n", 11);
    int missing = m.swap_io_valid;
    sample_vmstat(&m, "pswpin 12\npswpout 20\n", 12);
    int after_gap = m.swap_io_valid;

    return !missing && !after_gap ? 0 : 1;
}

static int test_swap_io_counter_reset_is_a_gap(void)
{
    metrics_t m = {.swap_valid = 1};
    s_prev_swap_io_valid = 0;

    sample_vmstat(&m, "pswpin 10\npswpout 20\n", 10);
    sample_vmstat(&m, "pswpin 5\npswpout 20\n", 11);

    return m.swap_io_valid ? 1 : 0;
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

/* --- Uptime --- */

/* read_uptime over `text`, with `seconds` set to 7 first. */
static int uptime_of(const char *text, long *seconds)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", text);
    FILE *f = reading(buf);
    *seconds = 7;
    int rc = f ? read_uptime(f, seconds) : -2;

    if (f)
        fclose(f);

    return rc;
}

static int test_uptime_reads_whole_seconds(void)
{
    long s;
    int  rc = uptime_of("12345.67 45678.90\n", &s);

    return rc == 0 && s == 12345 ? 0 : 1;
}

/* No number, a negative one or one too big for a long fails and leaves `seconds` as it was. */
static int test_uptime_unreadable_fails(void)
{
    long s;
    int  word = uptime_of("uptime\n", &s) == -1 && s == 7;
    int  negative = uptime_of("-5.00 1.00\n", &s) == -1 && s == 7;
    int  huge = uptime_of("99999999999999999999.00 1.00\n", &s) == -1 && s == 7;

    return word && negative && huge ? 0 : 1;
}

/* No file and a file at its end fail too. */
static int test_uptime_without_a_line_fails(void)
{
    char  ended[] = "1";
    FILE *e = reading(ended);
    long  s = 7;
    int   ok =
        read_uptime(NULL, &s) == -1 && e && fgetc(e) == '1' && read_uptime(e, &s) == -1 && s == 7;

    if (e)
        fclose(e);

    return ok ? 0 : 1;
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

    if (!(m.mem_total_mb > 0) || m.mem_available_mb < 0 || m.mem_available_mb > m.mem_total_mb)
        return 1;

    if (!(m.disk_total_gb > 0) || m.disk_available_gb < 0 || m.disk_available_gb > m.disk_total_gb)
        return 1;

    if (m.swap_valid &&
        (!(m.swap_total_mb > 0) || m.swap_free_mb < 0 || m.swap_free_mb > m.swap_total_mb))
        return 1;

    return 0;
}

static int test_uptime_reads_the_live_host(void)
{
    long s = 0;

    return metrics_uptime(&s) == 0 && s > 0 ? 0 : 1;
}

static int test_collect_second_pass_yields_rates(void)
{
    metrics_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    /* The CPU, network and swap snapshots are static, so priming may already have happened in an
     * earlier test: only the SECOND call of this pair is asserted. */
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

    if (b.swap_io_valid && (!b.swap_valid || b.swap_in_bps < 0 || b.swap_out_bps < 0))
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

/* --- MemFree in place of MemAvailable --- */

/* Read each text in turn as a new process would; true when each gives the available memory
 * expected. What the reader logged lands in `err`. */
static int read_logged(const char *const *texts, const double *available_mb, int n, char *err,
                       size_t cap)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/minimoni-meminfo-err-%d.txt", (int)getpid());

    fflush(stderr);
    if (!freopen(path, "w", stderr))
        return 0;

    s_memfree_noted = 0;
    int ok = 1;
    for (int i = 0; i < n; i++) {
        char text[96];
        snprintf(text, sizeof(text), "%s", texts[i]);
        FILE     *f = reading(text);
        metrics_t m = {0};
        ok = ok && f && read_meminfo(f, &m) == 0 && approx(m.mem_available_mb, available_mb[i]);
        if (f)
            fclose(f);
    }

    fflush(stderr);
    if (!freopen("/dev/null", "w", stderr))
        return 0;

    FILE  *f = fopen(path, "r");
    size_t len = f ? fread(err, 1, cap - 1, f) : 0;
    err[len] = '\0';
    if (f)
        fclose(f);
    unlink(path);

    return ok;
}

/* Kernels before 3.14 give no MemAvailable: MemFree stands in and is logged once; nothing is logged
 * while MemAvailable is there. */
static int test_meminfo_without_available_uses_free_and_logs_once(void)
{
    static const char *const texts[] = {
        "MemTotal: 4096 kB\nMemFree: 512 kB\nMemAvailable: 1024 kB\n",
        "MemTotal: 4096 kB\nMemFree: 512 kB\n",
        "MemTotal: 4096 kB\nMemFree: 512 kB\n",
    };
    static const double available_mb[] = {1.0, 0.5, 0.5};
    char                err[512];
    int                 ok = read_logged(texts, available_mb, 3, err, sizeof(err));

    return ok && count(err, "this kernel gives no MemAvailable") == 1 ? 0 : 1;
}

/* Some containers report more available memory than the total: MemFree stands in and is logged
 * once. */
static int test_meminfo_available_above_total_uses_free_and_logs_once(void)
{
    static const char *const texts[] = {
        "MemTotal: 4096 kB\nMemFree: 512 kB\nMemAvailable: 8192 kB\n",
        "MemTotal: 4096 kB\nMemFree: 512 kB\nMemAvailable: 8192 kB\n",
    };
    static const double available_mb[] = {0.5, 0.5};
    char                err[512];
    int                 ok = read_logged(texts, available_mb, 2, err, sizeof(err));

    return ok && count(err, "MemAvailable is above MemTotal") == 1 ? 0 : 1;
}

/* --- Temperature: the zone read and its critical trip --- */

/* A file of a sysfs tree: its path in the tree and what it holds. */
typedef struct {
    const char *path;
    const char *text;
} sysfs_file_t;

/* The one zone of a Raspberry Pi, as it reports it. */
static const sysfs_file_t PI[] = {
    {"thermal_zone0/type", "cpu-thermal\n"},
    {"thermal_zone0/temp", "55306\n"},
    {"thermal_zone0/trip_point_0_type", "critical\n"},
    {"thermal_zone0/trip_point_0_temp", "110000\n"},
};

/* A board's zone before the CPU's, each with a critical trip. Its first 4 files are the board's
 * zone alone, and its first 8 leave the CPU's zone without its critical trip. */
static const sysfs_file_t BOARD_THEN_CPU[] = {
    {"thermal_zone0/type", "acpitz\n"},
    {"thermal_zone0/temp", "27800\n"},
    {"thermal_zone0/trip_point_0_type", "critical\n"},
    {"thermal_zone0/trip_point_0_temp", "105000\n"},
    {"thermal_zone1/type", "x86_pkg_temp\n"},
    {"thermal_zone1/temp", "48000\n"},
    {"thermal_zone1/trip_point_0_type", "passive\n"},
    {"thermal_zone1/trip_point_0_temp", "0\n"},
    {"thermal_zone1/trip_point_1_type", "critical\n"},
    {"thermal_zone1/trip_point_1_temp", "100000\n"},
};

static char g_tree[64];

/* Writes the first `n` files in a new directory, g_tree. Returns 0, or -1 on a failure. */
static int tree_make(const sysfs_file_t *files, int n)
{
    snprintf(g_tree, sizeof(g_tree), "%s", "/tmp/minimoni-sysfs-XXXXXX");

    if (!mkdtemp(g_tree))
        return -1;

    for (int i = 0; i < n; i++) {
        char path[128];

        snprintf(path, sizeof(path), "%s/%s", g_tree, files[i].path);

        for (char *slash = path + strlen(g_tree) + 1; (slash = strchr(slash, '/')); slash++) {
            *slash = '\0';
            mkdir(path, 0700);
            *slash = '/';
        }

        FILE *f = fopen(path, "w");

        if (!f)
            return -1;

        fputs(files[i].text, f);
        fclose(f);
    }

    return 0;
}

/* Removes what tree_make wrote: each directory goes with its last file. */
static void tree_remove(const sysfs_file_t *files, int n)
{
    for (int i = 0; i < n; i++) {
        char path[128];

        snprintf(path, sizeof(path), "%s/%s", g_tree, files[i].path);
        unlink(path);

        for (char *slash; (slash = strrchr(path, '/')) > path + strlen(g_tree);) {
            *slash = '\0';
            rmdir(path);
        }
    }

    rmdir(g_tree);
}

/* temp_zone_note on g_tree when `note`, else temp_critical; what it logged lands in `err`. */
static int temp_logged(int note, double *crit, char *err, size_t cap)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/minimoni-thermal-err-%d.txt", (int)getpid());

    fflush(stderr);
    if (!freopen(path, "w", stderr))
        return -2;

    int rc = 0;

    if (note)
        temp_zone_note(g_tree);
    else
        rc = temp_critical(g_tree, crit);

    fflush(stderr);
    if (!freopen("/dev/null", "w", stderr))
        return -2;

    FILE  *f = fopen(path, "r");
    size_t len = f ? fread(err, 1, cap - 1, f) : 0;
    err[len] = '\0';

    if (f)
        fclose(f);
    unlink(path);

    return rc;
}

/* A Raspberry Pi's one zone gives the temperature and the critical trip. */
static int test_temp_reads_a_pi(void)
{
    metrics_t m = {0};
    double    crit = 0;

    if (tree_make(PI, 4) != 0)
        return 1;

    collect_temp(&m, g_tree);

    int ok = m.temp_valid && approx(m.temp_celsius, 55.306) && temp_critical(g_tree, &crit) == 0 &&
             approx(crit, 110.0);

    tree_remove(PI, 4);

    return ok ? 0 : 1;
}

/* The CPU's zone is read rather than zone 0. */
static int test_temp_prefers_the_cpu_zone(void)
{
    metrics_t m = {0};

    if (tree_make(BOARD_THEN_CPU, 10) != 0)
        return 1;

    collect_temp(&m, g_tree);

    int ok = m.temp_valid && approx(m.temp_celsius, 48.0);

    tree_remove(BOARD_THEN_CPU, 10);

    return ok ? 0 : 1;
}

/* The critical trip is the one of the zone read, not zone 0's. */
static int test_temp_critical_of_the_zone_read(void)
{
    double crit = 0;

    if (tree_make(BOARD_THEN_CPU, 10) != 0)
        return 1;

    int ok = temp_critical(g_tree, &crit) == 0 && approx(crit, 100.0);

    tree_remove(BOARD_THEN_CPU, 10);

    return ok ? 0 : 1;
}

/* With no critical trip in the zone read there is none, though zone 0 has one. */
static int test_temp_no_critical_in_the_zone_read(void)
{
    double crit = 0;

    if (tree_make(BOARD_THEN_CPU, 8) != 0)
        return 1;

    int ok = temp_critical(g_tree, &crit) == -1;

    tree_remove(BOARD_THEN_CPU, 8);

    return ok ? 0 : 1;
}

/* Without a CPU zone, zone 0 gives the temperature and the critical trip. */
static int test_temp_without_a_cpu_zone_reads_zone_0(void)
{
    metrics_t m = {0};
    double    crit = 0;

    if (tree_make(BOARD_THEN_CPU, 4) != 0)
        return 1;

    collect_temp(&m, g_tree);

    int ok = m.temp_valid && approx(m.temp_celsius, 27.8) && temp_critical(g_tree, &crit) == 0 &&
             approx(crit, 105.0);

    tree_remove(BOARD_THEN_CPU, 4);

    return ok ? 0 : 1;
}

/* With no zone there is neither a temperature nor a critical trip. */
static int test_temp_without_zones(void)
{
    metrics_t m = {0};
    double    crit = 0;

    if (tree_make(PI, 0) != 0)
        return 1;

    collect_temp(&m, g_tree);

    int ok = !m.temp_valid && temp_critical(g_tree, &crit) == -1;

    tree_remove(PI, 0);

    return ok ? 0 : 1;
}

/* Reading the critical trip logs nothing: collect reads it every run. */
static int test_temp_critical_logs_nothing(void)
{
    double crit = 0;
    char   err[256];

    if (tree_make(BOARD_THEN_CPU, 10) != 0)
        return 1;

    int rc = temp_logged(0, &crit, err, sizeof(err));

    tree_remove(BOARD_THEN_CPU, 10);

    return rc == 0 && err[0] == '\0' ? 0 : 1;
}

/* The zone note logs which zone the temperature comes from, and its type. */
static int test_temp_zone_logged(void)
{
    double crit = 0;
    char   err[256];

    if (tree_make(BOARD_THEN_CPU, 10) != 0)
        return 1;

    int rc = temp_logged(1, &crit, err, sizeof(err));

    tree_remove(BOARD_THEN_CPU, 10);

    return rc == 0 &&
                   count(err, "metrics: temperature read from thermal_zone1 (x86_pkg_temp)\n") == 1
               ? 0
               : 1;
}

/* --- Network: the interfaces summed and their link speed --- */

/* A host running a container: `eth0` is its NIC; `docker0`, the container's veth and `lo` are
 * virtual. */
static const sysfs_file_t HOST_DOCKER[] = {
    {"class/net/eth0/speed", "100\n"},
    {"class/net/docker0/speed", "10000\n"},
    {"class/net/veth1a2b3c/speed", "10000\n"},
    {"devices/virtual/net/docker0/ifindex", "3\n"},
    {"devices/virtual/net/veth1a2b3c/ifindex", "5\n"},
    {"devices/virtual/net/lo/ifindex", "1\n"},
};

/* The same host before its link is up. */
static const sysfs_file_t HOST_DOCKER_LINK_DOWN[] = {
    {"class/net/eth0/speed", "-1\n"},
    {"class/net/docker0/speed", "10000\n"},
    {"class/net/veth1a2b3c/speed", "10000\n"},
    {"devices/virtual/net/docker0/ifindex", "3\n"},
    {"devices/virtual/net/veth1a2b3c/ifindex", "5\n"},
    {"devices/virtual/net/lo/ifindex", "1\n"},
};

/* Inside a container without the host's network: its `eth0` is a veth. */
static const sysfs_file_t CONTAINER[] = {
    {"class/net/eth0/speed", "10000\n"},
    {"devices/virtual/net/eth0/ifindex", "9\n"},
    {"devices/virtual/net/lo/ifindex", "1\n"},
};

/* An interface of /proc/net/dev: its name and the bytes it received and sent. */
typedef struct {
    const char *name;
    long        rx, tx;
} net_dev_t;

/* sample_net on g_tree, at `sec` seconds, of a /proc/net/dev listing the first `n` of `ifs`. */
static void sample_dev(metrics_t *m, const net_dev_t *ifs, int n, long sec)
{
    char  text[1024] = {0};
    FILE *w = fmemopen(text, sizeof(text) - 1, "w");

    if (w) {
        fputs("Inter-|   Receive                |  Transmit\n"
              " face |bytes    packets errs ...|bytes    packets errs ...\n",
              w);
        for (int i = 0; i < n; i++)
            fprintf(w, "%6s: %ld 0 0 0 0 0 0 0 %ld 0 0 0 0 0 0 0\n", ifs[i].name, ifs[i].rx,
                    ifs[i].tx);
        fclose(w);
    }

    FILE           *f = reading(text);
    struct timespec now = {sec, 0};

    sample_net(f, g_tree, now, m);

    if (f)
        fclose(f);
}

/* A 50 MB download in the container: `eth0` receives it, `docker0` and the veth pass it on. Only
 * `eth0` counts. */
static int test_net_sums_the_physical_interfaces(void)
{
    const net_dev_t before[] = {
        {"lo", 0, 0}, {"eth0", 1000000, 100000}, {"docker0", 0, 0}, {"veth1a2b3c", 0, 0}};
    const net_dev_t after[] = {{"lo", 2000, 2000},
                               {"eth0", 51000000, 300000},
                               {"docker0", 100000, 48000000},
                               {"veth1a2b3c", 100000, 48000000}};
    metrics_t       m = {0};
    s_prev_net_valid = 0;

    if (tree_make(HOST_DOCKER, 6) != 0)
        return 1;

    sample_dev(&m, before, 4, 10);
    sample_dev(&m, after, 4, 12);

    tree_remove(HOST_DOCKER, 6);

    return m.net_valid && approx(m.net_rx_bps, 25000000.0) && approx(m.net_tx_bps, 100000.0) ? 0
                                                                                             : 1;
}

/* With no physical interface, every one but `lo` counts. */
static int test_net_without_a_physical_sums_all_but_lo(void)
{
    const net_dev_t before[] = {{"lo", 0, 0}, {"eth0", 0, 0}};
    const net_dev_t after[] = {{"lo", 5000, 5000}, {"eth0", 50000000, 200000}};
    metrics_t       m = {0};
    s_prev_net_valid = 0;

    if (tree_make(CONTAINER, 3) != 0)
        return 1;

    sample_dev(&m, before, 2, 10);
    sample_dev(&m, after, 2, 12);

    tree_remove(CONTAINER, 3);

    return m.net_valid && approx(m.net_rx_bps, 25000000.0) && approx(m.net_tx_bps, 100000.0) ? 0
                                                                                             : 1;
}

/* When the only physical interface goes, the sum moves to the virtual ones: one gap, then rates
 * again. */
static int test_net_switching_sets_is_a_gap(void)
{
    const net_dev_t with_eth0[] = {
        {"lo", 0, 0}, {"eth0", 1000, 1000}, {"docker0", 0, 0}, {"veth1a2b3c", 0, 0}};
    const net_dev_t without[] = {{"lo", 0, 0}, {"docker0", 5000, 5000}, {"veth1a2b3c", 5000, 5000}};
    const net_dev_t later[] = {{"lo", 0, 0}, {"docker0", 7000, 7000}, {"veth1a2b3c", 7000, 7000}};
    metrics_t       m = {0};
    s_prev_net_valid = 0;

    if (tree_make(HOST_DOCKER, 6) != 0)
        return 1;

    sample_dev(&m, with_eth0, 4, 10);
    sample_dev(&m, without, 3, 12);
    int gap = !m.net_valid;
    sample_dev(&m, later, 3, 14);

    tree_remove(HOST_DOCKER, 6);

    return gap && m.net_valid && approx(m.net_rx_bps, 2000.0) ? 0 : 1;
}

/* The link speed of `eth0` alone, not that of `docker0` and the veth. */
static int test_link_speed_of_the_physical_interfaces(void)
{
    if (tree_make(HOST_DOCKER, 6) != 0)
        return 1;

    int speed = link_speed(g_tree);

    tree_remove(HOST_DOCKER, 6);

    return speed == 100 ? 0 : 1;
}

/* With the physical link down there is no speed, though the virtual interfaces report one. */
static int test_link_speed_of_a_physical_link_down(void)
{
    if (tree_make(HOST_DOCKER_LINK_DOWN, 6) != 0)
        return 1;

    int speed = link_speed(g_tree);

    tree_remove(HOST_DOCKER_LINK_DOWN, 6);

    return speed == 0 ? 0 : 1;
}

/* With no physical interface, the speed of every one but `lo` counts. */
static int test_link_speed_without_a_physical_sums_all_but_lo(void)
{
    if (tree_make(CONTAINER, 3) != 0)
        return 1;

    int speed = link_speed(g_tree);

    tree_remove(CONTAINER, 3);

    return speed == 10000 ? 0 : 1;
}

/* Without sysfs no interface reads as virtual: every one but `lo` counts. */
static int test_net_without_sysfs_sums_all_but_lo(void)
{
    const net_dev_t before[] = {{"lo", 0, 0}, {"eth0", 0, 0}, {"docker0", 0, 0}};
    const net_dev_t after[] = {{"lo", 5000, 5000}, {"eth0", 4000, 2000}, {"docker0", 0, 6000}};
    metrics_t       m = {0};
    s_prev_net_valid = 0;

    if (tree_make(CONTAINER, 0) != 0)
        return 1;

    sample_dev(&m, before, 3, 10);
    sample_dev(&m, after, 3, 12);

    tree_remove(CONTAINER, 0);

    return m.net_valid && approx(m.net_rx_bps, 2000.0) && approx(m.net_tx_bps, 4000.0) ? 0 : 1;
}

/* Without sysfs there is no speed. */
static int test_link_speed_without_sysfs_is_0(void)
{
    if (tree_make(CONTAINER, 0) != 0)
        return 1;

    int speed = link_speed(g_tree);

    tree_remove(CONTAINER, 0);

    return speed == 0 ? 0 : 1;
}

/* --- CPU usage --- */

/* The percents from one /proc/stat cpu line to the next. Returns 1 for a sample, 0 for none, -1
 * when a line does not parse. */
static int cpu_between(const char *was, const char *now, metrics_t *m)
{
    cpu_raw_t prev, cur;
    if (parse_cpu_line(was, &prev) != 0 || parse_cpu_line(now, &cur) != 0)
        return -1;

    return cpu_percents(&prev, &cur, m);
}

/* User takes nice, system takes irq and softirq, idle takes iowait; guest and guest_nice are
 * already in user and nice. */
static int test_cpu_groups_the_counters(void)
{
    metrics_t m;

    int got = cpu_between("cpu  0 0 0 0 0 0 0 0 0 0\n", "cpu  10 10 10 30 10 15 15 0 5 5\n", &m);

    return got == 1 && approx(m.cpu_user_percent, 20.0) && approx(m.cpu_system_percent, 40.0) &&
                   approx(m.cpu_idle_percent, 40.0)
               ? 0
               : 1;
}

/* With 300 of 1000 ticks stolen, user's 600 ticks are 60 %. */
static int test_cpu_counts_the_stolen_time(void)
{
    metrics_t m;

    int got = cpu_between("cpu  0 0 0 0 0 0 0 0 0 0\n", "cpu  600 0 50 50 0 0 0 300 0 0\n", &m);

    return got == 1 && approx(m.cpu_user_percent, 60.0) && approx(m.cpu_system_percent, 5.0) &&
                   approx(m.cpu_idle_percent, 5.0)
               ? 0
               : 1;
}

/* One pair per group: user, system, idle and steal. */
static int test_cpu_group_gone_back_is_a_gap(void)
{
    static const char *const lines[][2] = {
        {"cpu  100 0 0 0 0 0 0 0 0 0\n", "cpu  90 0 0 50 0 0 0 0 0 0\n"},
        {"cpu  0 0 100 0 0 0 0 0 0 0\n", "cpu  50 0 90 0 0 0 0 0 0 0\n"},
        {"cpu  0 0 0 1000 0 0 0 0 0 0\n", "cpu  50 0 0 990 0 0 0 0 0 0\n"},
        {"cpu  0 0 0 0 0 0 0 100 0 0\n", "cpu  50 0 0 0 0 0 0 90 0 0\n"},
    };

    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        metrics_t m;
        if (cpu_between(lines[i][0], lines[i][1], &m) != 0)
            return 1;
    }

    return 0;
}

static int test_cpu_iowait_drop_made_up_by_idle(void)
{
    metrics_t m;

    int got = cpu_between("cpu  0 0 0 100 20 0 0 0 0 0\n", "cpu  60 0 0 130 10 0 0 0 0 0\n", &m);

    return got == 1 && approx(m.cpu_user_percent, 75.0) && approx(m.cpu_idle_percent, 25.0) ? 0 : 1;
}

static int test_cpu_no_time_is_a_gap(void)
{
    metrics_t   m;
    const char *line = "cpu  10 0 10 10 0 0 0 0 0 0\n";

    return cpu_between(line, line, &m) == 0 ? 0 : 1;
}

static int test_cpu_line_malformed_refused(void)
{
    static const char *const bad[] = {
        "",
        "intr 1 2 3 4 5 6 7 8\n",
        "cpu0 1 2 3 4 5 6 7 8\n",
        "cpu  1 2 3 4 5 6 7\n",
        "cpu  1 2 3 x 5 6 7 8\n",
        "cpu  1 2 3 99999999999999999999 5 6 7 8\n",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cpu_raw_t c;
        if (parse_cpu_line(bad[i], &c) != -1)
            return 1;
    }

    return 0;
}

/* --- CPU count --- */

/* The CPU count once took only the first span: "0-3,8-11" gave 4 and "0,2" gave 1. */
static int test_cpulist_counts_every_span(void)
{
    return count_cpulist("0-3,8-11\n") == 8 && count_cpulist("0,2\n") == 2 &&
                   count_cpulist("0-3\n") == 4 && count_cpulist("0\n") == 1
               ? 0
               : 1;
}

static int test_cpulist_malformed_refused(void)
{
    static const char *const bad[] = {"", "x\n", "-1\n", "0-\n", "3-1\n", "0,\n", "0-3 \n"};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (count_cpulist(bad[i]) != -1)
            return 1;

    return 0;
}

static const sysfs_file_t CPUS_TWO_SPANS[] = {
    {"devices/system/cpu/online", "0-3,8-11\n"},
};

/* num_cores on g_tree `calls` times in a row, as serve's cycles; what it logged lands in `err`. */
static int cores_logged(int calls, char *err, size_t cap)
{
    char path[64];
    snprintf(path, sizeof(path), "/tmp/minimoni-cores-err-%d.txt", (int)getpid());

    fflush(stderr);
    if (!freopen(path, "w", stderr))
        return -2;

    int noted = 0, n = 0;
    for (int i = 0; i < calls; i++)
        n = num_cores(g_tree, &noted);

    fflush(stderr);
    if (!freopen("/dev/null", "w", stderr))
        return -2;

    FILE  *f = fopen(path, "r");
    size_t len = f ? fread(err, 1, cap - 1, f) : 0;
    err[len] = '\0';

    if (f)
        fclose(f);
    unlink(path);

    return n;
}

static int test_cores_counted_from_the_online_list(void)
{
    char err[256];

    if (tree_make(CPUS_TWO_SPANS, 1) != 0)
        return 1;

    int n = cores_logged(1, err, sizeof(err));

    tree_remove(CPUS_TWO_SPANS, 1);

    return n == 8 && err[0] == '\0' ? 0 : 1;
}

static const char CORES_UNCOUNTED[] = "metrics: cannot count the CPUs online";

/* Without the list, the load's percent and levels assume 1 CPU: logged once, not every cycle. */
static int test_cores_without_the_list_count_one_logged_once(void)
{
    char err[256];

    if (tree_make(CPUS_TWO_SPANS, 0) != 0)
        return 1;

    int n = cores_logged(3, err, sizeof(err));

    tree_remove(CPUS_TWO_SPANS, 0);

    return n == 1 && count(err, CORES_UNCOUNTED) == 1 ? 0 : 1;
}

/* Past the 64-byte buffer only the head would be counted (2 here, of 41): it is refused instead. */
static int test_cores_list_longer_than_the_buffer_counts_one(void)
{
    char list[160] = "0-1";
    for (int cpu = 3; cpu < 80; cpu += 2)
        snprintf(list + strlen(list), sizeof(list) - strlen(list), ",%d", cpu);
    strcat(list, "\n");

    const sysfs_file_t online[] = {{"devices/system/cpu/online", list}};
    char               err[256];

    if (strlen(list) <= 64 || tree_make(online, 1) != 0)
        return 1;

    int n = cores_logged(1, err, sizeof(err));

    tree_remove(online, 1);

    return n == 1 && count(err, CORES_UNCOUNTED) == 1 ? 0 : 1;
}

/* --- Runner --- */

static const test_t ALL_TESTS[] = {
    T(counter_rates_basic),
    T(counter_rates_subsecond),
    T(counter_rates_subsecond_across_second),
    T(counter_rates_zero_dt),
    T(counter_rates_negative_dt),
    T(counter_rates_in_reset_is_a_gap),
    T(counter_rates_out_reset_is_a_gap),
    T(meminfo_reads_memory),
    T(meminfo_without_memtotal_fails),
    T(meminfo_reads_swap),
    T(meminfo_without_swap),
    T(meminfo_without_available_uses_free_and_logs_once),
    T(meminfo_available_above_total_uses_free_and_logs_once),
    T(temp_reads_a_pi),
    T(temp_prefers_the_cpu_zone),
    T(temp_critical_of_the_zone_read),
    T(temp_no_critical_in_the_zone_read),
    T(temp_without_a_cpu_zone_reads_zone_0),
    T(temp_without_zones),
    T(temp_critical_logs_nothing),
    T(temp_zone_logged),
    T(vmstat_swap_counters),
    T(vmstat_without_swap_counters),
    T(swap_io_need_a_previous_sample),
    T(swap_io_in_bytes_per_second),
    T(swap_io_skipped_without_swap),
    T(swap_io_start_over_when_vmstat_cannot_be_opened),
    T(swap_io_start_over_after_a_missing_counter),
    T(swap_io_counter_reset_is_a_gap),
    T(link_speed_parse),
    T(link_speed_sum_is_sane),
    T(net_sums_the_physical_interfaces),
    T(net_without_a_physical_sums_all_but_lo),
    T(net_switching_sets_is_a_gap),
    T(link_speed_of_the_physical_interfaces),
    T(link_speed_of_a_physical_link_down),
    T(link_speed_without_a_physical_sums_all_but_lo),
    T(net_without_sysfs_sums_all_but_lo),
    T(link_speed_without_sysfs_is_0),
    T(cpu_groups_the_counters),
    T(cpu_counts_the_stolen_time),
    T(cpu_group_gone_back_is_a_gap),
    T(cpu_iowait_drop_made_up_by_idle),
    T(cpu_no_time_is_a_gap),
    T(cpu_line_malformed_refused),
    T(cpulist_counts_every_span),
    T(cpulist_malformed_refused),
    T(cores_counted_from_the_online_list),
    T(cores_without_the_list_count_one_logged_once),
    T(cores_list_longer_than_the_buffer_counts_one),
    T(uptime_reads_whole_seconds),
    T(uptime_unreadable_fails),
    T(uptime_without_a_line_fails),
    T(collect_fills_plausible_values),
    T(uptime_reads_the_live_host),
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
