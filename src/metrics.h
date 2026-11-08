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

#ifndef MINIMONI_METRICS_H
#define MINIMONI_METRICS_H

typedef struct {
    /* CPU load averages: always valid */
    double load_1m, load_5m, load_15m;

    /* CPU usage: invalid (0) on the first collect call (no previous snapshot) */
    int    cpu_valid;
    double cpu_user_percent, cpu_system_percent, cpu_idle_percent;

    /* Memory, as the kernel reports it */
    double mem_total_mb, mem_available_mb;

    /* Swap, as the kernel reports it: swap_valid=0 when the host has none */
    int    swap_valid;
    double swap_total_mb, swap_free_mb;

    /* Swap I/O in bytes/s, computed like the network's from two samples: swap_io_valid=0 on
     * the first sample, after a failed read or a counter reset, and without swap */
    int    swap_io_valid;
    double swap_in_bps, swap_out_bps;

    /* Disk, as the kernel reports it: the available space leaves out what is reserved for root */
    double disk_total_gb, disk_available_gb;

    /* Temperature: optional (temp_valid=0 if sensor absent) */
    /* NB: the network link speed is NOT here: it is a host property, not a
     * sample. Read on demand with metrics_link_speed_mbit(). */
    int    temp_valid;
    double temp_celsius;

    /* Network throughput in bytes/s, summed over the physical interfaces (every one but `lo` when
     * none is). Computed from the delta between consecutive collect samples (like CPU);
     * net_valid=0 on the first sample, after a counter reset and when the sum moves from one of
     * those sets to the other. */
    int    net_valid;
    double net_rx_bps, net_tx_bps;

    /* Uptime */
    double uptime_seconds;
} metrics_t;

/* Collect all metrics into m. disk_path is the filesystem to measure (e.g. "/").
 *
 * CPU usage, network throughput and swap I/O use a static snapshot for delta computation. On
 * the first call, their valid flags are set to 0 and the snapshot is saved. On subsequent calls the
 * delta is computed and the corresponding valid flag is set to 1.
 *
 * Returns 0 on success. On non-fatal errors (missing temperature sensor, unreadable network stats)
 * the affected fields are zeroed and collection continues. Returns -1 only if a required metric
 * source is unavailable. */
int metrics_collect(metrics_t *m, const char *disk_path);

/* A run of failed collections, reported once rather than every cycle. Zero-initialise it. */
typedef struct {
    int failed; /* cycles in the current run */
} metrics_run_t;

/* Note a metrics_collect result: logs a run's first failure and the recovery that ends it. */
void metrics_run_note(metrics_run_t *run, int rc);

/* Sum of sysfs `speed` (Mbit/s) over the interfaces metrics_collect sums traffic over; the 100%
 * reference for net percent mode. Returns 0 when none reports a usable speed (wifi, a link down),
 * leaving the caller to fall back to config. */
int metrics_link_speed_mbit(void);

/* The CPUs online, for the load's percent and levels. Returns 1 when they cannot be counted, logged
 * the first time. */
int metrics_num_cores(void);

/* The critical trip point, in °C, of the thermal zone collect reads. Returns 0, or -1 when there
 * is no zone or it has no critical trip. */
int metrics_temp_critical(double *celsius);

/* Logs which thermal zone collect reads, and its type. */
void metrics_temp_zone_note(void);

#endif /* MINIMONI_METRICS_H */
