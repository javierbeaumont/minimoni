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

#include <ctype.h>
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

/* /proc/stat's counters summed in groups. iowait goes with idle because it can fall while idle
 * rises by at least as much. */
typedef struct {
    unsigned long long user, system, idle, steal;
} cpu_raw_t;

static int       s_prev_cpu_valid = 0;
static cpu_raw_t s_prev_cpu;

/* /proc/stat's first line, every CPU summed. Returns -1 unless it is "cpu" followed by at least 8
 * counters. */
static int parse_cpu_line(const char *line, cpu_raw_t *c)
{
    if (strncmp(line, "cpu ", 4) != 0)
        return -1;

    *c = (cpu_raw_t){0};
    /* user, nice, system, idle, iowait, irq, softirq and steal, in the kernel's order; the guest
     * and guest_nice that follow are already in user and nice */
    unsigned long long *group[] = {&c->user, &c->user,   &c->system, &c->idle,
                                   &c->idle, &c->system, &c->system, &c->steal};
    const char         *p = line + 3; /* past the "cpu" label */
    for (size_t i = 0; i < sizeof(group) / sizeof(group[0]); i++) {
        char *end;
        errno = 0;
        unsigned long long v = strtoull(p, &end, 10);
        if (end == p || errno != 0)
            return -1;
        *group[i] += v;
        p = end;
    }

    return 0;
}

static int read_cpu_raw(cpu_raw_t *c)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return -1;

    char  line[256];
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (!got)
        return -1;

    return parse_cpu_line(line, c);
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

/* User, system and idle in percents of the time from `prev` to `cur`, steal included. Returns 0
 * when a group went back or no time passed. */
static int cpu_percents(const cpu_raw_t *prev, const cpu_raw_t *cur, metrics_t *m)
{
    if (cur->user < prev->user || cur->system < prev->system || cur->idle < prev->idle ||
        cur->steal < prev->steal)
        return 0;

    unsigned long long d_user = cur->user - prev->user;
    unsigned long long d_system = cur->system - prev->system;
    unsigned long long d_idle = cur->idle - prev->idle;
    unsigned long long total = d_user + d_system + d_idle + (cur->steal - prev->steal);

    if (total == 0)
        return 0;

    m->cpu_user_percent = (double)d_user * 100.0 / (double)total;
    m->cpu_system_percent = (double)d_system * 100.0 / (double)total;
    m->cpu_idle_percent = (double)d_idle * 100.0 / (double)total;

    return 1;
}

static void collect_cpu(metrics_t *m)
{
    cpu_raw_t cur;
    if (read_cpu_raw(&cur) != 0) {
        m->cpu_valid = 0;
        return;
    }

    m->cpu_valid = s_prev_cpu_valid && cpu_percents(&s_prev_cpu, &cur, m);
    s_prev_cpu = cur;
    s_prev_cpu_valid = 1;
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

    m->disk_total_gb = (double)total / (1024.0 * 1024.0 * 1024.0);
    m->disk_available_gb = (double)avail / (1024.0 * 1024.0 * 1024.0);

    return 0;
}

/* --- Temperature --- */

#define THERMAL_DIR "/sys/class/thermal"

/* Prefer CPU-specific zones over generic ACPI zones. */
static const char *const TEMP_ZONES[] = {"x86_pkg_temp", "cpu-thermal", "soc_thermal", NULL};

/* The first line of dir/thermal_zoneN/file, without its newline; empty when unreadable.
 * Returns 0, or -1 when the file is missing or its path does not fit. */
static int zone_read(const char *dir, int zone, const char *file, char *buf, size_t cap)
{
    char path[256];
    int  n = snprintf(path, sizeof(path), "%s/thermal_zone%d/%s", dir, zone, file);

    if (n < 0 || (size_t)n >= sizeof(path))
        return -1;

    FILE *f = fopen(path, "r");

    if (!f)
        return -1;

    char *got = fgets(buf, (int)cap, f);
    fclose(f);

    if (!got)
        buf[0] = '\0';

    /* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) strcspn stays within buf */
    buf[strcspn(buf, "\n")] = '\0';

    return 0;
}

/* The millidegrees in a zone's file, as °C. Returns 0, or -1 when it holds no number. */
static int zone_celsius(const char *dir, int zone, const char *file, double *celsius)
{
    char buf[32];

    if (zone_read(dir, zone, file, buf, sizeof(buf)) != 0)
        return -1;

    char *end;
    long  millidegrees = strtol(buf, &end, 10);

    if (end == buf)
        return -1;

    *celsius = (double)millidegrees / 1000.0;

    return 0;
}

/* The zone under `dir` that collect reads: the one whose type comes first in TEMP_ZONES, else zone
 * 0. Writes its type to `type` and returns its number, or -1 when there is no zone. */
static int temp_zone(const char *dir, char *type, size_t cap)
{
    int best_zone = -1;
    int best_pri = (int)(sizeof(TEMP_ZONES) / sizeof(TEMP_ZONES[0]));

    for (int z = 0; z < 32; z++) {
        char zone_type[32];

        if (zone_read(dir, z, "type", zone_type, sizeof(zone_type)) != 0)
            break;

        int pri = best_pri;

        for (int p = 0; TEMP_ZONES[p] && p < pri; p++) {
            if (strcmp(zone_type, TEMP_ZONES[p]) == 0)
                pri = p;
        }

        if (z == 0 || pri < best_pri) {
            best_zone = z;
            best_pri = pri;
            snprintf(type, cap, "%s", zone_type);
        }
    }

    return best_zone;
}

/* A zone's critical trip point, in °C. Returns 0, or -1 when it has none. */
static int zone_critical(const char *dir, int zone, double *celsius)
{
    for (int i = 0; i < 16; i++) {
        char file[32];
        char trip_type[32];

        snprintf(file, sizeof(file), "trip_point_%d_type", i);

        if (zone_read(dir, zone, file, trip_type, sizeof(trip_type)) != 0)
            return -1;

        if (strcmp(trip_type, "critical") != 0)
            continue;

        double c;

        snprintf(file, sizeof(file), "trip_point_%d_temp", i);

        if (zone_celsius(dir, zone, file, &c) != 0 || c <= 0)
            return -1;

        *celsius = c;
        return 0;
    }

    return -1;
}

static void collect_temp(metrics_t *m, const char *dir)
{
    char type[32];
    int  zone = temp_zone(dir, type, sizeof(type));

    m->temp_valid = zone >= 0 && zone_celsius(dir, zone, "temp", &m->temp_celsius) == 0;
}

static int temp_critical(const char *dir, double *celsius)
{
    char type[32];
    int  zone = temp_zone(dir, type, sizeof(type));

    return zone < 0 ? -1 : zone_critical(dir, zone, celsius);
}

static void temp_zone_note(const char *dir)
{
    char type[32];
    int  zone = temp_zone(dir, type, sizeof(type));

    if (zone >= 0)
        fprintf(stderr, "metrics: temperature read from thermal_zone%d (%s)\n", zone, type);
}

int metrics_temp_critical(double *celsius) { return temp_critical(THERMAL_DIR, celsius); }

void metrics_temp_zone_note(void) { temp_zone_note(THERMAL_DIR); }

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

#define SYS_DIR "/sys"

/* In s_prev_net, `in` is what the interfaces received and `out` what they sent;
 * s_prev_net_physical says whether they were the physical ones. */
static int        s_prev_net_valid = 0;
static int        s_prev_net_physical;
static counters_t s_prev_net;

/* Whether sysfs lists `name` as virtual: a bridge, a veth, a tunnel... */
static int net_is_virtual(const char *sys, const char *name)
{
    char path[320]; /* fits the longest possible name plus SYS_DIR and the fixed parts */
    snprintf(path, sizeof(path), "%s/devices/virtual/net/%s", sys, name);

    return access(path, F_OK) == 0;
}

/* Sum the bytes the physical interfaces listed in `f` (/proc/net/dev) received and sent into
 * cur->in and cur->out; when none is physical, as in a container without the host's network, those
 * of every interface but `lo`. *physical says which sum it is. Returns 0, or -1 when the header
 * lines are missing. */
static int read_net_dev(FILE *f, const char *sys, counters_t *cur, int *physical)
{
    /* skip the two header lines */
    char line[256];
    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f))
        return -1;

    int64_t all_in = 0, all_out = 0, phys_in = 0, phys_out = 0;
    int     phys = 0;
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

        all_in += cols[0];
        all_out += cols[8];
        if (!net_is_virtual(sys, iface)) {
            phys_in += cols[0];
            phys_out += cols[8];
            phys++;
        }
    }

    *physical = phys > 0;
    cur->in = *physical ? phys_in : all_in;
    cur->out = *physical ? phys_out : all_out;

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

/* Sum of the sysfs `speed` of the interfaces read_net_dev sums traffic over. */
static int link_speed(const char *sys)
{
    char path[320]; /* fits the longest possible d_name plus SYS_DIR and the fixed parts */
    snprintf(path, sizeof(path), "%s/class/net", sys);
    DIR *d = opendir(path);
    if (!d)
        return 0;

    int all = 0, phys = 0, phys_speed = 0;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.' || strcmp(e->d_name, "lo") == 0)
            continue;

        int physical = !net_is_virtual(sys, e->d_name);
        phys += physical;

        snprintf(path, sizeof(path), "%s/class/net/%s/speed", sys, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        char buf[32] = {0};
        int  speed = fgets(buf, sizeof(buf), f) ? parse_link_speed(buf) : 0;
        fclose(f);
        all += speed;
        if (physical)
            phys_speed += speed;
    }

    closedir(d);
    return phys > 0 ? phys_speed : all;
}

int metrics_link_speed_mbit(void) { return link_speed(SYS_DIR); }

/* --- CPU count, for load's percent and levels --- */

/* CPUs in a kernel cpulist such as "0-3,8-11", or -1 unless the whole list parses. */
static int count_cpulist(const char *s)
{
    int n = 0;

    for (;;) {
        char *end;

        if (!isdigit((unsigned char)*s))
            return -1;

        unsigned long lo = strtoul(s, &end, 10);
        unsigned long hi = lo;
        if (*end == '-') {
            s = end + 1;
            if (!isdigit((unsigned char)*s))
                return -1;
            hi = strtoul(s, &end, 10);
            if (hi < lo)
                return -1;
        }

        n += (int)(hi - lo + 1);

        if (*end != ',')
            return *end == '\n' || *end == '\0' ? n : -1;
        s = end + 1;
    }
}

/* The CPUs online under `sys`; 1 when their list cannot be read whole, logged once per `noted`. */
static int num_cores(const char *sys, int *noted)
{
    char path[128]; /* fits SYS_DIR or a test's tree, and the fixed part */
    snprintf(path, sizeof(path), "%s/devices/system/cpu/online", sys);

    FILE *f = fopen(path, "r");
    char  buf[64];
    char *got = f ? fgets(buf, sizeof(buf), f) : NULL;
    if (f)
        fclose(f);

    /* A list cut at the buffer would count only its head: refused, like a missing file. */
    int n = got && strchr(buf, '\n') ? count_cpulist(buf) : -1;

    if (n > 0)
        return n;

    if (!*noted)
        fprintf(stderr,
                "metrics: cannot count the CPUs online in %s; "
                "the load's percent and levels assume 1 CPU\n",
                path);
    *noted = 1;

    return 1;
}

int metrics_num_cores(void)
{
    static int noted;

    return num_cores(SYS_DIR, &noted);
}

/* One network sample, taken at `now`: `f` is /proc/net/dev, or NULL when it did not open. A failed
 * read keeps the previous sample for the next rate. */
static void sample_net(FILE *f, const char *sys, struct timespec now, metrics_t *m)
{
    m->net_valid = 0;

    counters_t cur = {.t = now};
    int        physical = 0;
    if (!f || read_net_dev(f, sys, &cur, &physical) != 0)
        return;

    /* Sums over different sets of interfaces give no rate. */
    if (s_prev_net_valid && physical == s_prev_net_physical)
        m->net_valid = counter_rates(&s_prev_net, &cur, &m->net_rx_bps, &m->net_tx_bps);

    s_prev_net = cur;
    s_prev_net_physical = physical;
    s_prev_net_valid = 1;
}

static void collect_net(metrics_t *m)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    FILE *f = fopen("/proc/net/dev", "r");

    sample_net(f, SYS_DIR, now, m);

    if (f)
        fclose(f);
}

/* --- Swap I/O --- */

/* In s_prev_swap_io, `in` is what was read back from swap and `out` what was written to it. */
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

/* One swap I/O sample, taken at `now`: `f` is /proc/vmstat, or NULL when it did not open. */
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
    collect_temp(m, THERMAL_DIR);
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
