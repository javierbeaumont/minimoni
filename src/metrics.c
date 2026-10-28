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

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "metrics.h"

/* --- CPU helpers --- */

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq;
} cpu_raw_t;

static int       s_prev_cpu_valid = 0;
static cpu_raw_t s_prev_cpu;

static int read_cpu_raw(cpu_raw_t *c)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return -1;

    char  line[256];
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (!got || strncmp(line, "cpu ", 4) != 0)
        return -1;

    unsigned long long *fields[] = {&c->user,   &c->nice, &c->system, &c->idle,
                                    &c->iowait, &c->irq,  &c->softirq};
    char               *p = line + 3; /* past the "cpu" label */
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        char *end;
        errno = 0;
        unsigned long long v = strtoull(p, &end, 10);
        if (end == p || errno != 0)
            return -1;
        *fields[i] = v;
        p = end;
    }
    return 0;
}

/* --- Load average --- */

static int collect_load(metrics_t *m)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f)
        return -1;

    char  line[128];
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (!got)
        return -1;

    double *fields[] = {&m->load_1m, &m->load_5m, &m->load_15m};
    char   *p = line;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        char  *end;
        double v = strtod(p, &end);
        if (end == p)
            return -1;
        *fields[i] = v;
        p = end;
    }
    return 0;
}

/* --- CPU usage --- */

static void collect_cpu(metrics_t *m)
{
    cpu_raw_t cur;
    if (read_cpu_raw(&cur) != 0) {
        m->cpu_valid = 0;
        return;
    }

    if (!s_prev_cpu_valid) {
        s_prev_cpu = cur;
        s_prev_cpu_valid = 1;
        m->cpu_valid = 0;
        return;
    }

    unsigned long long d_user = cur.user - s_prev_cpu.user;
    unsigned long long d_nice = cur.nice - s_prev_cpu.nice;
    unsigned long long d_system = cur.system - s_prev_cpu.system;
    unsigned long long d_idle = cur.idle - s_prev_cpu.idle;
    unsigned long long d_iowait = cur.iowait - s_prev_cpu.iowait;
    unsigned long long d_irq = cur.irq - s_prev_cpu.irq;
    unsigned long long d_softirq = cur.softirq - s_prev_cpu.softirq;

    unsigned long long total = d_user + d_nice + d_system + d_idle + d_iowait + d_irq + d_softirq;

    s_prev_cpu = cur;

    if (total == 0) {
        m->cpu_valid = 0;
        return;
    }

    m->cpu_user_percent = (double)(d_user + d_nice) * 100.0 / (double)total;
    m->cpu_system_percent = (double)(d_system + d_irq + d_softirq) * 100.0 / (double)total;
    m->cpu_idle_percent = (double)(d_idle + d_iowait) * 100.0 / (double)total;
    m->cpu_valid = 1;
}

/* --- Memory and swap --- */

/* Split a /proc line, "Key: value" or "key value", into its first word and the number after it.
 * Returns 0, or -1 when the line has no such pair or the word does not fit `cap`. */
static int split_key_value(const char *line, char *key, size_t cap, unsigned long long *val)
{
    const char *sep = line;
    while (*sep != '\0' && *sep != ' ' && *sep != '\t')
        sep++;

    size_t klen = (size_t)(sep - line);
    if (klen == 0 || klen >= cap)
        return -1;

    memcpy(key, line, klen);
    key[klen] = '\0';

    char *end;
    errno = 0;
    *val = strtoull(sep, &end, 10);

    return end == sep || errno != 0 ? -1 : 0;
}

/* MemFree standing in is logged once per process, not every cycle. */
static int s_memfree_noted = 0;

/* Fill the memory and swap fields from /proc/meminfo. Returns 0, or -1 without MemTotal. */
static int read_meminfo(FILE *f, metrics_t *m)
{
    unsigned long long mem_total_kb = 0, mem_free_kb = 0, mem_available_kb = ULLONG_MAX;
    unsigned long long swap_total_kb = 0, swap_free_kb = 0;
    char               line[128];
    char               key[64];
    unsigned long long val;

    while (fgets(line, sizeof(line), f)) {
        if (split_key_value(line, key, sizeof(key), &val) != 0)
            continue;

        if (strcmp(key, "MemTotal:") == 0)
            mem_total_kb = val;
        else if (strcmp(key, "MemFree:") == 0)
            mem_free_kb = val;
        else if (strcmp(key, "MemAvailable:") == 0)
            mem_available_kb = val;
        else if (strcmp(key, "SwapTotal:") == 0)
            swap_total_kb = val;
        else if (strcmp(key, "SwapFree:") == 0)
            swap_free_kb = val;
    }

    if (mem_total_kb == 0)
        return -1;

    /* MemFree stands in for a MemAvailable that is missing or above the total, as in free(1) */
    const char *why =
        mem_available_kb == ULLONG_MAX    ? "this kernel gives no MemAvailable (Linux before 3.14)"
        : mem_available_kb > mem_total_kb ? "MemAvailable is above MemTotal, as in some containers"
                                          : NULL;

    if (why) {
        mem_available_kb = mem_free_kb;

        if (!s_memfree_noted) {
            fprintf(stderr, "metrics: %s; MemFree stands in, so the cache counts as used memory\n",
                    why);
            s_memfree_noted = 1;
        }
    }

    m->mem_total_mb = (double)mem_total_kb / 1024.0;
    m->mem_available_mb = (double)mem_available_kb / 1024.0;

    m->swap_valid = swap_total_kb > 0;
    if (m->swap_valid) {
        m->swap_total_mb = (double)swap_total_kb / 1024.0;
        m->swap_free_mb = (double)swap_free_kb / 1024.0;
    }

    return 0;
}

static int collect_meminfo(metrics_t *m)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;

    int rc = read_meminfo(f, m);
    fclose(f);

    return rc;
}

/* --- Disk --- */

static int collect_disk(metrics_t *m, const char *disk_path)
{
    struct statvfs sv;
    if (statvfs(disk_path, &sv) != 0)
        return -1;

    unsigned long long block = sv.f_frsize;
    unsigned long long total = (unsigned long long)sv.f_blocks * block;
    unsigned long long avail = (unsigned long long)sv.f_bavail * block;
    unsigned long long used = total - (unsigned long long)sv.f_bfree * block;

    m->disk_total_gb = (double)total / (1024.0 * 1024.0 * 1024.0);
    m->disk_used_gb = (double)used / (1024.0 * 1024.0 * 1024.0);
    m->disk_free_gb = (double)avail / (1024.0 * 1024.0 * 1024.0);
    m->disk_percent = (total > 0) ? (double)used / (double)total * 100.0 : 0.0;
    return 0;
}

/* --- Temperature --- */

static void collect_temp(metrics_t *m)
{
    /* Prefer CPU-specific zones over generic ACPI zones. */
    static const char *PREFERRED[] = {"x86_pkg_temp", "cpu-thermal", "soc_thermal", NULL};

    int best_zone = 0;
    int best_pri = (int)(sizeof(PREFERRED) / sizeof(PREFERRED[0]));

    for (int z = 0; z < 32; z++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", z);
        FILE *f = fopen(path, "r");

        if (!f)
            break;

        char  type[32];
        char *got = fgets(type, sizeof(type), f);
        fclose(f);

        if (!got)
            continue;

        /* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) strcspn stays within type[32] */
        type[strcspn(type, "\n")] = '\0';

        for (int p = 0; PREFERRED[p]; p++) {
            if (strcmp(type, PREFERRED[p]) == 0 && p < best_pri) {
                best_pri = p;
                best_zone = z;
            }
        }
    }

    char path[128];
    snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", best_zone);
    FILE *f = fopen(path, "r");

    if (!f) {
        m->temp_valid = 0;
        return;
    }

    char  buf[32];
    char *got = fgets(buf, sizeof(buf), f);
    fclose(f);

    char *end = buf;
    long  millidegrees = got ? strtol(buf, &end, 10) : 0;

    if (!got || end == buf) {
        m->temp_valid = 0;
        return;
    }

    m->temp_celsius = (double)millidegrees / 1000.0;
    m->temp_valid = 1;
}

/* --- Rates from cumulative counters: network and swap I/O --- */

/* Two cumulative byte counters, what came in and what went out, and the monotonic instant they were
 * read. Unlike the ratio-based CPU percentage, a rate divides the delta by the real elapsed time,
 * with sub-second resolution, so the short gap of a one-shot collect does not round to zero. */
typedef struct {
    int64_t         in, out;
    struct timespec t;
} counters_t;

/* Bytes/s in and out from two consecutive snapshots. Returns 1 and fills *in_bps / *out_bps, or 0
 * (a gap) when the elapsed time is not positive or a counter went backwards (a reset or a wrap). */
static int counter_rates(const counters_t *prev, const counters_t *cur, double *in_bps,
                         double *out_bps)
{
    double dt =
        (double)(cur->t.tv_sec - prev->t.tv_sec) + (double)(cur->t.tv_nsec - prev->t.tv_nsec) / 1e9;
    int64_t in_delta = cur->in - prev->in;
    int64_t out_delta = cur->out - prev->out;

    if (dt <= 0.0 || in_delta < 0 || out_delta < 0)
        return 0;

    *in_bps = (double)in_delta / dt;
    *out_bps = (double)out_delta / dt;

    return 1;
}

/* --- Network --- */

/* in is what the interfaces received, out what they sent. */
static int        s_prev_net_valid = 0;
static counters_t s_prev_net;

/* Sum the bytes received and sent by every non-loopback interface in /proc/net/dev into *cur,
 * stamped with a monotonic timestamp. Returns 0, or -1 when the file cannot be read. */
static int read_net_raw(counters_t *cur)
{
    cur->in = 0;
    cur->out = 0;
    clock_gettime(CLOCK_MONOTONIC, &cur->t);

    FILE *f = fopen("/proc/net/dev", "r");
    if (!f)
        return -1;

    /* skip the two header lines */
    char line[256];
    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;

        /* interface name is everything up to ':', minus leading spaces */
        char *name = line;
        while (*name == ' ')
            name++;

        char   iface[64];
        size_t nlen = (size_t)(colon - name);
        if (nlen == 0 || nlen >= sizeof(iface))
            continue;

        memcpy(iface, name, nlen);
        iface[nlen] = '\0';
        if (strcmp(iface, "lo") == 0)
            continue;

        /* columns after ':' are rx_bytes rx_packets rx_errs rx_drop rx_fifo
         * rx_frame rx_compressed rx_multicast tx_bytes ...; keep rx_bytes
         * (column 0) and tx_bytes (column 8). */
        char   *p = colon + 1;
        int64_t cols[9];
        int     ok = 1;
        for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
            char *end;
            errno = 0;
            cols[i] = (int64_t)strtoll(p, &end, 10);
            if (end == p || errno != 0) {
                ok = 0;
                break;
            }

            p = end;
        }

        if (!ok)
            continue;

        cur->in += cols[0];
        cur->out += cols[8];
    }

    fclose(f);

    return 0;
}

/* sysfs `speed` in Mbit/s, or 0 when the link has none to report: virtual
 * devices and down/renegotiating wifi give -1, an empty read or garbage. */
static int parse_link_speed(const char *s)
{
    char *end;
    long  v = strtol(s, &end, 10);
    return (end != s && v > 0 && v <= 1000000) ? (int)v : 0;
}

int metrics_link_speed_mbit(void)
{
    DIR *d = opendir("/sys/class/net");
    if (!d)
        return 0;

    int total = 0;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.' || strcmp(e->d_name, "lo") == 0)
            continue;
        char path[320]; /* fits the longest possible d_name plus the fixed parts */
        snprintf(path, sizeof(path), "/sys/class/net/%s/speed", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char buf[32] = {0};
        if (fgets(buf, sizeof(buf), f))
            total += parse_link_speed(buf);
        fclose(f);
    }

    closedir(d);
    return total;
}

static void collect_net(metrics_t *m)
{
    counters_t cur;
    if (read_net_raw(&cur) != 0) {
        m->net_valid = 0;
        return;
    }

    if (!s_prev_net_valid) {
        s_prev_net = cur;
        s_prev_net_valid = 1;
        m->net_valid = 0;
        return;
    }

    m->net_valid = counter_rates(&s_prev_net, &cur, &m->net_rx_bps, &m->net_tx_bps);
    s_prev_net = cur;
}

/* --- Swap I/O --- */

/* in is what was read back from swap, out what was written to it. */
static int        s_prev_swap_io_valid = 0;
static counters_t s_prev_swap_io;

/* pswpin and pswpout from /proc/vmstat, in pages. Returns 0, or -1 when either is missing. */
static int read_vmstat_swap(FILE *f, int64_t *in_pages, int64_t *out_pages)
{
    char               line[128];
    char               key[64];
    unsigned long long val;
    int                found = 0;

    while (fgets(line, sizeof(line), f)) {
        if (split_key_value(line, key, sizeof(key), &val) != 0)
            continue;

        if (strcmp(key, "pswpin") == 0) {
            *in_pages = (int64_t)val;
            found |= 1;
        } else if (strcmp(key, "pswpout") == 0) {
            *out_pages = (int64_t)val;
            found |= 2;
        }
    }

    return found == 3 ? 0 : -1;
}

/* One swap I/O sample, taken at `now`: f is /proc/vmstat, or NULL when it did not open. */
static void sample_swap_io(FILE *f, struct timespec now, metrics_t *m)
{
    m->swap_io_valid = 0;

    int64_t in_pages = 0, out_pages = 0;
    if (!f || read_vmstat_swap(f, &in_pages, &out_pages) != 0) {
        s_prev_swap_io_valid = 0;
        return;
    }

    long       page = sysconf(_SC_PAGESIZE);
    counters_t cur = {.in = in_pages * page, .out = out_pages * page, .t = now};

    if (s_prev_swap_io_valid && m->swap_valid)
        m->swap_io_valid = counter_rates(&s_prev_swap_io, &cur, &m->swap_in_bps, &m->swap_out_bps);

    s_prev_swap_io = cur;
    s_prev_swap_io_valid = 1;
}

static void collect_swap_io(metrics_t *m)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    FILE *f = fopen("/proc/vmstat", "r");

    sample_swap_io(f, now, m);

    if (f)
        fclose(f);
}

/* --- Uptime --- */

static int collect_uptime(metrics_t *m)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f)
        return -1;

    char  line[64];
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (!got)
        return -1;

    char *end;
    m->uptime_seconds = strtod(line, &end);
    return (end == line) ? -1 : 0;
}

/* --- Public API --- */

int metrics_collect(metrics_t *m, const char *disk_path)
{
    if (collect_load(m) != 0)
        return -1;

    if (collect_meminfo(m) != 0)
        return -1;

    if (collect_disk(m, disk_path) != 0)
        return -1;

    if (collect_uptime(m) != 0)
        return -1;

    collect_cpu(m);
    collect_temp(m);
    collect_net(m);
    collect_swap_io(m);

    return 0;
}

void metrics_run_note(metrics_run_t *run, int rc)
{
    if (rc != 0) {
        if (run->failed++ == 0)
            fprintf(stderr, "metrics: collection failed, so no sample is stored until it recovers; "
                            "check collect.disk_path and /proc\n");
        return;
    }

    if (run->failed > 0)
        fprintf(stderr, "metrics: collection recovered after %d failed cycles\n", run->failed);
    run->failed = 0;
}
