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

#include <math.h>

/* 100% reference for net percent mode. A configured speed WINS over the detected
 * link: the NIC is often faster than the uplink behind it (1 GbE card on a
 * 300 Mbit line), so the operator's number is the honest denominator. Falls back
 * to sysfs, then to 1 GbE. Full duplex: rx and tx each get the whole link. */
double net_ref_bps(int max_mbit, int detected_mbit)
{
    int mbit = max_mbit > 0 ? max_mbit : detected_mbit;
    if (mbit <= 0)
        mbit = 1000;
    return (double)mbit * 125000.0; /* Mbit/s -> bytes/s */
}

double net_convert(double bps, const char *unit, double ref_bps)
{
    if (unit && unit[0] == '%')
        return ref_bps > 0 ? bps * 100.0 / ref_bps : 0.0;
    return bps;
}

int unit_step(double max, int steps, double factor)
{
    int i = 0;
    if (!(max > 0))
        return -1;
    while (i < steps - 1 && max / pow(factor, i) > UNIT_DIGITS_MAX)
        i++;
    return i;
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
