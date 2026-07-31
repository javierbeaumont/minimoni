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

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

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

/* --- Memory --- */

static int collect_mem(metrics_t *m)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;

    unsigned long long total_kb = 0, available_kb = 0;
    char               line[128];
    char               key[64];
    unsigned long long val;

    while (fgets(line, sizeof(line), f)) {
        char *sep = line;
        while (*sep != '\0' && *sep != ' ' && *sep != '\t')
            sep++;
        size_t klen = (size_t)(sep - line);
        if (klen == 0 || klen >= sizeof(key))
            continue;
        memcpy(key, line, klen);
        key[klen] = '\0';

        char *end;
        errno = 0;
        val = strtoull(sep, &end, 10);
        if (end == sep || errno != 0)
            continue;

        if (strcmp(key, "MemTotal:") == 0)
            total_kb = val;
        if (strcmp(key, "MemAvailable:") == 0)
            available_kb = val;
    }
    fclose(f);

    if (total_kb == 0)
        return -1;

    m->mem_total_mb = (double)total_kb / 1024.0;
    m->mem_available_mb = (double)available_kb / 1024.0;
    m->mem_used_mb = m->mem_total_mb - m->mem_available_mb;
    m->mem_percent = m->mem_used_mb / m->mem_total_mb * 100.0;
    return 0;
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
        char type[32] = {0};
        fgets(type, sizeof(type), f);
        fclose(f);
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

/* --- Network --- */

/* Snapshot of cumulative byte counters plus the monotonic instant they were
 * sampled. The bps fields in metrics_t are computed from the delta between
 * consecutive snapshots (same pattern as the CPU delta). The clock is
 * monotonic with sub-second resolution: net throughput is a rate, so unlike
 * the ratio-based CPU percentage it must divide by the real elapsed time, and
 * the short inter-sample gap of one-shot collect mode must not round to zero. */
typedef struct {
    int64_t         rx, tx;
    struct timespec t;
} net_raw_t;

static int       s_prev_net_valid = 0;
static net_raw_t s_prev_net;

/* Sum rx/tx bytes across all non-loopback interfaces in /proc/net/dev into
 * *cur, stamping the sample with a monotonic timestamp. Returns 0 on success,
 * -1 if the file cannot be read. */
static int read_net_raw(net_raw_t *cur)
{
    cur->rx = 0;
    cur->tx = 0;
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

        cur->rx += cols[0];
        cur->tx += cols[8];
    }

    fclose(f);
    return 0;
}

/* Compute rx/tx throughput (bytes/s) from two consecutive snapshots. Returns 1
 * and fills *rx_bps / *tx_bps on success; returns 0 (a gap) when elapsed time is
 * not positive or a counter went backwards (interface reset or wrap). Pure: the
 * sub-second-dt and counter-reset logic is exercised by tests/unit-metrics.c. */
static int net_rate(const net_raw_t *prev, const net_raw_t *cur, double *rx_bps, double *tx_bps)
{
    double dt =
        (double)(cur->t.tv_sec - prev->t.tv_sec) + (double)(cur->t.tv_nsec - prev->t.tv_nsec) / 1e9;
    int64_t drx = cur->rx - prev->rx;
    int64_t dtx = cur->tx - prev->tx;

    if (dt <= 0.0 || drx < 0 || dtx < 0)
        return 0;

    *rx_bps = (double)drx / dt;
    *tx_bps = (double)dtx / dt;
    return 1;
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
    net_raw_t cur;
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

    m->net_valid = net_rate(&s_prev_net, &cur, &m->net_rx_bps, &m->net_tx_bps);
    s_prev_net = cur;
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
    if (collect_mem(m) != 0)
        return -1;
    if (collect_disk(m, disk_path) != 0)
        return -1;
    if (collect_uptime(m) != 0)
        return -1;

    collect_cpu(m);
    collect_temp(m);
    collect_net(m);

    return 0;
}
