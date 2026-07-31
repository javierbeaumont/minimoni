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

#ifndef MINIMONI_UNITS_H
#define MINIMONI_UNITS_H

/* Convert a raw collected value (in its stored base unit) to the configured
 * display unit. `unit` is the matching *_unit string from config. */
/* bps -> kb/mb/gb/kbps/mbps/gbps, or % of `ref_bps` (see net_ref_bps). */
double net_convert(double bps, const char *unit, double ref_bps);
double mem_convert(double mb, const char *unit);               /* mb  -> mb/gb */
double disk_convert(double gb, const char *unit);              /* gb  -> gb/tb */
double load_convert(double load, int cores, const char *unit); /* load -> abs/% */

/* 100% reference for net percent mode, in bytes/s: the configured maximum when set
 * (it wins: the uplink may be slower than the NIC), else the detected link speed,
 * else 1 GbE. Both inputs in Mbit/s. */
double net_ref_bps(int max_mbit, int detected_mbit);

/* Temperature: celsius -> celsius/fahrenheit/percent. In percent mode the value
 * is scaled against `ref` (the temperature that maps to 100%). */
double temp_convert(double celsius, const char *unit, double ref);

/* Resolve the 100% reference for temperature percent mode: the sysfs critical
 * trip point when valid, otherwise the configured fallback. */
double temp_ref(int crit_valid, double crit, double fallback);

#endif /* MINIMONI_UNITS_H */
