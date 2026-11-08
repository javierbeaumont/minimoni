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

static double temp_in_scale(double celsius, const char *scale)
{
    return scale && scale[0] == 'f' ? celsius * 9.0 / 5.0 + 32.0 : celsius;
}

double temp_convert(double celsius, const char *unit, const char *scale, double ref)
{
    double t = temp_in_scale(celsius, scale);
    double r = temp_in_scale(ref, scale);

    if (unit && unit[0] == '%')
        return r > 0 ? t * 100.0 / r : t;

    return t;
}

double load_convert(double load, int cores, const char *unit)
{
    if (unit && unit[0] == '%' && cores > 0)
        return load * 100.0 / (double)cores;
    return load;
}

double temp_ref(double configured, int crit_valid, double crit)
{
    if (configured > 0)
        return configured;

    return crit_valid ? crit : TEMP_CRITICAL_DEFAULT;
}
