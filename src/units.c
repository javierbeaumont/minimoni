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

#include "units.h"

double net_convert(double bps, const char *unit)
{
    if (!unit || unit[0] == 'm') {
        if (unit && unit[1] == 'b' && unit[2] == 'p') /* mbps */
            return bps * 8.0 / 1e6;
        return bps / 1048576.0; /* mb */
    }
    if (unit[0] == 'g') {
        if (unit[1] == 'b' && unit[2] == 'p') /* gbps */
            return bps * 8.0 / 1e9;
        return bps / 1073741824.0; /* gb */
    }
    if (unit[0] == 'k') {
        if (unit[1] == 'b' && unit[2] == 'p') /* kbps */
            return bps * 8.0 / 1000.0;
        return bps / 1024.0; /* kb */
    }
    return bps / 1048576.0;
}

double mem_convert(double mb, const char *unit)
{
    if (unit && unit[0] == 'g')
        return mb / 1024.0;
    return mb; /* mb (or %; caller uses mem_percent directly) */
}

double disk_convert(double gb, const char *unit)
{
    if (unit && unit[0] == 't')
        return gb / 1024.0;
    return gb; /* gb (or %; caller uses disk_percent directly) */
}

double temp_convert(double celsius, const char *unit, double ref)
{
    if (!unit)
        return celsius;
    if (unit[0] == 'f')
        return celsius * 9.0 / 5.0 + 32.0;
    if (unit[0] == '%')
        return (ref > 0) ? celsius * 100.0 / ref : celsius;
    return celsius;
}

double load_convert(double load, int cores, const char *unit)
{
    if (unit && unit[0] == '%' && cores > 0)
        return load * 100.0 / (double)cores;
    return load;
}

/* The 100% reference for temperature percent mode is the sysfs critical trip
 * point when the kernel exposes it; otherwise the configured fallback. */
double temp_ref(int crit_valid, double crit, double fallback)
{
    return crit_valid ? crit : fallback;
}
