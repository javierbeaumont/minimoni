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
#include <unistd.h>

#include "metrics.h"

int main(void)
{
    metrics_t m;

    /* First call — seeds the CPU delta snapshot; cpu_valid will be 0 */
    if (metrics_collect(&m, "/") != 0) {
        fprintf(stderr, "metrics_collect failed\n");
        return 1;
    }
    printf("--- pass 1 (cpu_valid=%d) ---\n", m.cpu_valid);
    printf("load:    %.2f  %.2f  %.2f\n", m.load_1m, m.load_5m, m.load_15m);
    printf("uptime:  %.0f s\n", m.uptime_seconds);

    sleep(1);

    /* Second call — CPU delta now valid */
    if (metrics_collect(&m, "/") != 0) {
        fprintf(stderr, "metrics_collect failed\n");
        return 1;
    }

    printf("\n--- pass 2 ---\n");
    printf("load:    %.2f  %.2f  %.2f\n", m.load_1m, m.load_5m, m.load_15m);

    if (m.cpu_valid) {
        printf("cpu:     user=%.1f%%  sys=%.1f%%  idle=%.1f%%\n", m.cpu_user_pct, m.cpu_system_pct,
               m.cpu_idle_pct);
    } else {
        printf("cpu:     (not valid)\n");
    }

    printf("mem:     total=%.1fMB  used=%.1fMB  avail=%.1fMB  (%.1f%%)\n", m.mem_total_mb,
           m.mem_used_mb, m.mem_available_mb, m.mem_percent);
    printf("disk:    total=%.2fGB  used=%.2fGB  free=%.2fGB  (%.1f%%)\n", m.disk_total_gb,
           m.disk_used_gb, m.disk_free_gb, m.disk_percent);

    if (m.temp_valid) {
        printf("temp:    %.1f C\n", m.temp_celsius);
    } else {
        printf("temp:    (no sensor)\n");
    }

    printf("net:     rx=%lld bytes  tx=%lld bytes\n", m.net_rx_bytes, m.net_tx_bytes);
    printf("uptime:  %.0f s\n", m.uptime_seconds);

    return 0;
}
