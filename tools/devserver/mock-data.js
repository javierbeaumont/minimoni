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

/* Fake metrics in the collector's base units, for units.js to convert like the
 * C server. Reaches warn/critical bands; reads no real system. */

'use strict';

const DAY_SECONDS = 86400;
const CYCLE_PERIOD = 60; /* seconds for one good -> critical -> good sweep of the cards */
/* Stress level (0 = healthy, 1 = critical) for each pinned scenario. */
const SCENARIO_STRESS = { normal: 0.1, warn: 0.85, critical: 0.97 };

const MEM_TOTAL_MB = 1959.0;
const DISK_TOTAL_GB = 97.9;
const TEMP_CRITICAL_C = 105.0;
const NET_SPEED_MBIT = 1000; /* mock link: what sysfs would report on GbE */

/* Parse a range like '7d', '12h' or '30m' into seconds; default 1 day. */
function rangeSeconds(value) {
  const units = { d: DAY_SECONDS, h: 3600, m: 60 };
  const mult = units[value[value.length - 1]];
  const n = parseInt(value.slice(0, -1), 10);

  if (mult === undefined || !Number.isInteger(n))
    return DAY_SECONDS;

  return n * mult;
}

/* Parse a points query value: a positive int capped at `cap`, else `default`
 * when missing, non-numeric, or non-positive. Mirrors the server contract. */
function clampPointsParam(raw, cap = 1440, dflt = 240) {
  if (!/^\d+$/.test(raw))
    return dflt;

  const n = parseInt(raw, 10);

  return n > 0 ? Math.min(n, cap) : dflt;
}

/* Stress for the live cards: 'cycle' sweeps over wall-clock time. */
function cardStress(scenario) {
  if (scenario === 'cycle') {
    const now = Date.now() / 1000;

    return (Math.sin((2 * Math.PI * (now % CYCLE_PERIOD)) / CYCLE_PERIOD) + 1) / 2;
  }

  return SCENARIO_STRESS[scenario] ?? SCENARIO_STRESS.normal;
}

/* Stress for chart point i: 'cycle' sweeps low->critical->low across the chart. */
function seriesStress(scenario, i, n) {
  if (scenario === 'cycle')
    return (1 - Math.cos((2 * Math.PI * i) / Math.max(n - 1, 1))) / 2;

  return SCENARIO_STRESS[scenario] ?? SCENARIO_STRESS.normal;
}

/* Value at stress s in [lo, hi], plus a small ripple for texture, clamped >= 0. */
function band(s, ripple, lo, hi, amp = 0.0) {
  return Math.max(0.0, lo + s * (hi - lo) + amp * ripple);
}

/* Base-unit metric values at stress s (0 healthy .. 1 critical). */
function metrics(s, ripple) {
  const memPercent = Math.min(99.0, band(s, ripple, 38.0, 95.0, 2.0));
  const diskPercent = Math.min(99.0, band(s, ripple, 8.0, 95.0, 1.0));
  const load1m = band(s, ripple, 0.35, 4.6, 0.2);
  const cpuUser = Math.min(99.0, band(s, ripple, 12.0, 95.0, 4.0));
  const cpuSystem = band(s, ripple, 2.0, 4.0, 0.5);

  return {
    load_1m: load1m,
    load_5m: load1m * 0.85,
    load_15m: load1m * 0.7,
    cpu_user: cpuUser,
    cpu_system: cpuSystem,
    cpu_idle: Math.max(0.0, 100.0 - cpuUser - cpuSystem),
    mem_used_mb: (MEM_TOTAL_MB * memPercent) / 100.0,
    mem_avail_mb: (MEM_TOTAL_MB * (100.0 - memPercent)) / 100.0,
    mem_total_mb: MEM_TOTAL_MB,
    mem_percent: memPercent,
    disk_used_gb: (DISK_TOTAL_GB * diskPercent) / 100.0,
    disk_total_gb: DISK_TOTAL_GB,
    disk_free_gb: (DISK_TOTAL_GB * (100.0 - diskPercent)) / 100.0,
    disk_percent: diskPercent,
    temp_c: band(s, ripple, 48.0, 108.0, 3.0),
    net_rx_bps: band(s, ripple, 1.26e6, 8.4e6, 2e5),
    net_tx_bps: band(s, ripple, 0.31e6, 3.1e6, 1e5),
    uptime: 72840.0,
  };
}

/* Format a Date as the collector's "YYYY-MM-DD HH:MM:SS" local timestamp. */
function localTimestamp(d) {
  const p = (n) => String(n).padStart(2, '0');

  return (
    `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ` +
    `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
  );
}

/* Live-ish base-unit snapshot; stress reaches warn/critical (see units.js).
 *
 * scenario: 'normal' | 'warn' | 'critical' | 'cycle' (default sweeps over time). */
function currentSnapshot(scenario = 'cycle') {
  const m = metrics(cardStress(scenario), Math.random() - 0.5);

  m.timestamp = localTimestamp(new Date());
  m.temp_critical_c = TEMP_CRITICAL_C;
  m.net_speed_mbit = NET_SPEED_MBIT;

  return m;
}

/* n base-unit metric points spanning the range, scaled by the scenario stress. */
function makePoints(rangeValue = '1d', scenario = 'cycle', n = 300) {
  const step = Math.max(Math.floor(rangeSeconds(rangeValue) / n), 1);
  const start = Math.floor(Date.now() / 1000) - step * (n - 1);
  const points = [];

  for (let i = 0; i < n; i++) {
    const t = start + i * step;
    const ripple = Math.sin((2 * Math.PI * (t % DAY_SECONDS)) / DAY_SECONDS);
    const m = metrics(seriesStress(scenario, i, n), ripple);

    m.t = t;
    m.temp_critical_c = TEMP_CRITICAL_C;
    m.net_speed_mbit = NET_SPEED_MBIT;

    points.push(m);
  }

  return points;
}

module.exports = {
  rangeSeconds,
  clampPointsParam,
  currentSnapshot,
  makePoints,
};
