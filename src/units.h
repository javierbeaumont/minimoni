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

#ifndef MINIMONI_UNITS_H
#define MINIMONI_UNITS_H

/* bps -> % of `ref_bps` (see net_ref_bps), or bytes/s unchanged. */
double net_convert(double bps, const char *unit, double ref_bps);
double load_convert(double load, int cores, const char *unit); /* load -> abs/% */

/* Widest reading a status card can show without cropping: the grid reserves
 * room for five integer digits (see the .cards rule in dashboard/style.css).
 * The step is taken an order of magnitude early so a value larger than the one
 * that chose the unit still fits. */
#define UNIT_DIGITS_MAX 9999.0

/* Index into the metric's unit ladder, or -1 when there is nothing to scale. */
int unit_step(double max, int steps, double factor);

/* 100% reference for net percent mode, in bytes/s: the configured maximum when set
 * (it wins: the uplink may be slower than the NIC), else the detected link speed,
 * else 1 GbE. Both inputs in Mbit/s. */
double net_ref_bps(int max_mbit, int detected_mbit);

/* Temperature: °C -> `unit` ("degrees" or "%") in `scale` ("celsius" or "fahrenheit"). In "%" it is
 * a percent of `ref`, the critical trip point in °C, both counted from the scale's zero. */
double temp_convert(double celsius, const char *unit, const char *scale, double ref);

/* The critical trip point, in °C, when none is configured and the sensor reports none. */
#define TEMP_CRITICAL_DEFAULT 85.0

/* The critical trip point the temperature is measured against, in °C: `configured` when above
 * 0, else the sensor's `crit` when `crit_valid`, else TEMP_CRITICAL_DEFAULT. */
double temp_ref(double configured, int crit_valid, double crit);

#endif /* MINIMONI_UNITS_H */
