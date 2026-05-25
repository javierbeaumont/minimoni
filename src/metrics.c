/*
 * minimoni — zero-dependency system monitoring
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

#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

#include "metrics.h"

/* --- CPU helpers --------------------------------------------------------- */

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

    int ok = (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu", &c->user, &c->nice, &c->system,
                     &c->idle, &c->iowait, &c->irq, &c->softirq) == 7);
    fclose(f);
    return ok ? 0 : -1;
}

/* --- Load average -------------------------------------------------------- */

static int collect_load(metrics_t *m)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f)
        return -1;

    int ok = (fscanf(f, "%lf %lf %lf", &m->load_1m, &m->load_5m, &m->load_15m) == 3);
    fclose(f);
    return ok ? 0 : -1;
}

/* --- CPU usage ----------------------------------------------------------- */

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

/* --- Memory -------------------------------------------------------------- */

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
        if (sscanf(line, "%63s %llu", key, &val) == 2) {
            if (strcmp(key, "MemTotal:") == 0)
                total_kb = val;
            if (strcmp(key, "MemAvailable:") == 0)
                available_kb = val;
        }
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

/* --- Disk ---------------------------------------------------------------- */

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

/* --- Temperature --------------------------------------------------------- */

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

    long millidegrees = 0;
    int  ok = (fscanf(f, "%ld", &millidegrees) == 1);
    fclose(f);

    if (!ok) {
        m->temp_valid = 0;
        return;
    }

    m->temp_celsius = (double)millidegrees / 1000.0;
    m->temp_valid = 1;
}

/* --- Network ------------------------------------------------------------- */

/* Snapshot of cumulative byte counters + the wall-clock time they were
 * sampled. The bps fields exposed in metrics_t are computed from the delta
 * between consecutive snapshots (same pattern as the CPU delta above). */
typedef struct {
    long long rx, tx;
    time_t    t;
} net_raw_t;

static int       s_prev_net_valid = 0;
static net_raw_t s_prev_net;

/* Read /proc/net/dev and sum rx/tx bytes across all non-loopback interfaces
 * into *cur. Returns 0 on success, -1 if the file cannot be opened or its
 * header lines are missing. */
static int read_net_raw(net_raw_t *cur)
{
    cur->rx = 0;
    cur->tx = 0;
    cur->t = time(NULL);

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
        char               iface[64];
        long long          rx, tx;
        unsigned long long dummy;

        /* columns: iface rx_bytes rx_pkts rx_errs rx_drop rx_fifo rx_frame
         *                rx_compressed rx_multicast tx_bytes ... */
        int n = sscanf(line, " %63[^:]: %lld %llu %llu %llu %llu %llu %llu %llu %lld", iface, &rx,
                       &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &tx);

        if (n != 10)
            continue;
        if (strcmp(iface, "lo") == 0)
            continue;

        cur->rx += rx;
        cur->tx += tx;
    }

    fclose(f);
    return 0;
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

    time_t    dt = cur.t - s_prev_net.t;
    long long drx = cur.rx - s_prev_net.rx;
    long long dtx = cur.tx - s_prev_net.tx;

    s_prev_net = cur;

    /* Counter reset (negative delta) or zero/negative time delta → invalid. */
    if (dt <= 0 || drx < 0 || dtx < 0) {
        m->net_valid = 0;
        return;
    }

    m->net_rx_bps = (double)drx / (double)dt;
    m->net_tx_bps = (double)dtx / (double)dt;
    m->net_valid = 1;
}

/* --- Uptime -------------------------------------------------------------- */

static int collect_uptime(metrics_t *m)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f)
        return -1;

    int ok = (fscanf(f, "%lf", &m->uptime_seconds) == 1);
    fclose(f);
    return ok ? 0 : -1;
}

/* --- Public API ---------------------------------------------------------- */

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
