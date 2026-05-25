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

#ifndef MINIMONI_METRICS_H
#define MINIMONI_METRICS_H

typedef struct {
    /* CPU load averages — always valid */
    double load_1m, load_5m, load_15m;

    /* CPU usage — invalid (0) on the first collect call (no previous snapshot) */
    int    cpu_valid;
    double cpu_user_percent, cpu_system_percent, cpu_idle_percent;

    /* Memory */
    double mem_total_mb, mem_used_mb, mem_available_mb, mem_percent;

    /* Disk */
    double disk_total_gb, disk_used_gb, disk_free_gb, disk_percent;

    /* Temperature — optional (temp_valid=0 if sensor absent) */
    int    temp_valid;
    double temp_celsius;

    /* Network throughput — bytes/second since the previous collect call,
     * summed across all non-loopback interfaces. net_valid=0 on the first
     * call (no previous snapshot) or after a counter reset. */
    int    net_valid;
    double net_rx_bps, net_tx_bps;

    /* Uptime */
    double uptime_seconds;
} metrics_t;

/*
 * Collect all metrics into m. disk_path is the filesystem to measure (e.g. "/").
 *
 * CPU usage: uses a static snapshot for delta computation. On the first call,
 * cpu_valid is set to 0 and the snapshot is saved. On subsequent calls the
 * delta is computed and cpu_valid is set to 1.
 *
 * Returns 0 on success. On non-fatal errors (missing temperature sensor,
 * unreadable network stats) the affected fields are zeroed and collection
 * continues. Returns -1 only if a required metric source is unavailable.
 */
int metrics_collect(metrics_t *m, const char *disk_path);

#endif /* MINIMONI_METRICS_H */
