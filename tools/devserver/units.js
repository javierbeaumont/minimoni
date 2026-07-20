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

/* Unit conversions + /api serialization, mirroring the C server (src/units.c,
 * src/json.c, src/http.c). */

'use strict';

/* The mock has no real CPU, so it fixes 4 cores for load normalisation + thresholds. */
const CORES = 4;

/* Round to n decimals (mock display precision only). */
function round(x, n) {
  const p = 10 ** n;

  return Math.round(x * p) / p;
}

function netConvert(bps, unit) {
  if (!unit || unit[0] === 'm') {
    if (unit && unit[1] === 'b' && unit[2] === 'p') /* mbps */
      return (bps * 8.0) / 1e6;
    return bps / 1048576.0; /* mb */
  }

  if (unit[0] === 'g') {
    if (unit[1] === 'b' && unit[2] === 'p') /* gbps */
      return (bps * 8.0) / 1e9;
    return bps / 1073741824.0; /* gb */
  }

  if (unit[0] === 'k') {
    if (unit[1] === 'b' && unit[2] === 'p') /* kbps */
      return (bps * 8.0) / 1000.0;
    return bps / 1024.0; /* kb */
  }

  return bps / 1048576.0;
}

function memConvert(mb, unit) {
  return unit && unit[0] === 'g' ? mb / 1024.0 : mb;
}

function diskConvert(gb, unit) {
  return unit && unit[0] === 't' ? gb / 1024.0 : gb;
}

function tempConvert(celsius, unit, ref) {
  if (!unit)
    return celsius;

  if (unit[0] === 'f')
    return (celsius * 9.0) / 5.0 + 32.0;

  if (unit[0] === '%')
    return ref > 0 ? (celsius * 100.0) / ref : celsius;

  return celsius;
}

/* 100% reference for temp percent mode: the sysfs critical trip point when
 * present (non-null), else the configured fallback. Mirrors the C server. */
function tempRef(crit, fallback) {
  return crit != null ? crit : fallback;
}

function loadConvert(load, cores, unit) {
  if (unit && unit[0] === '%' && cores > 0)
    return (load * 100.0) / cores;

  return load;
}

/* Mirror config_has(): absent list => show-all, else must contain the name. */
function configHas(list, name) {
  return list == null || (Array.isArray(list) && list.includes(name));
}

/* /api/current in CARD units. Mirrors json.c json_serialize_current(): a metric
 * group is emitted only when its card is configured, and [warn, crit] thresholds
 * are computed server-side. */
function toCurrent(raw, f, fallback) {
  const m = raw;
  const cards = f.cards;
  const lu = String(f.cpu_load_card_unit);
  const mu = String(f.mem_card_unit);
  const du = String(f.disk_card_unit);
  const nu = String(f.net_card_unit);
  const tu = String(f.temp_card_unit);

  const out = { timestamp: raw.timestamp };

  if (configHas(cards, 'cpu_load')) {
    out.load_1m = round(loadConvert(m.load_1m, CORES, lu), 2);
    out.load_5m = round(loadConvert(m.load_5m, CORES, lu), 2);
    out.load_15m = round(loadConvert(m.load_15m, CORES, lu), 2);
  }

  if (configHas(cards, 'cpu_usage')) {
    out.cpu_user_percent = round(m.cpu_user, 1);
    out.cpu_system_percent = round(m.cpu_system, 1);
    out.cpu_idle_percent = round(m.cpu_idle, 1);
  }

  if (configHas(cards, 'memory')) {
    if (mu[0] !== '%') {
      out.mem_used = round(memConvert(m.mem_used_mb, mu), 2);
      out.mem_available = round(memConvert(m.mem_avail_mb, mu), 2);
      out.mem_total = round(memConvert(m.mem_total_mb, mu), 2);
    }

    out.mem_percent = round(m.mem_percent, 1);
  }

  if (configHas(cards, 'disk')) {
    if (du[0] !== '%') {
      out.disk_used = round(diskConvert(m.disk_used_gb, du), 2);
      out.disk_total = round(diskConvert(m.disk_total_gb, du), 2);
      out.disk_free = round(diskConvert(m.disk_free_gb, du), 2);
    }

    out.disk_percent = round(m.disk_percent, 1);
  }

  const tc = m.temp_c;
  const crit = m.temp_critical_c;
  const ref = tempRef(crit, fallback);

  if (configHas(cards, 'temp')) {
    out.temp = tc != null ? round(tempConvert(tc, tu, ref), 1) : null;
    out.temp_critical = crit != null ? round(tempConvert(crit, tu, ref), 1) : null;
  }

  if (configHas(cards, 'net')) {
    out.net_rx = round(netConvert(m.net_rx_bps, nu), 2);
    out.net_tx = round(netConvert(m.net_tx_bps, nu), 2);
  }

  if (configHas(cards, 'uptime')) {
    out.uptime_seconds = raw.uptime;
  }

  /* Thresholds, computed server-side ([warn, crit]). */
  if (configHas(cards, 'cpu_load')) {
    out.thresh_load = lu[0] === '%' ? [70.0, 90.0] : [0.75 * CORES, 1.0 * CORES];
  }

  if (configHas(cards, 'cpu_usage'))
    out.thresh_cpu = [70.0, 90.0];

  if (configHas(cards, 'memory'))
    out.thresh_mem = [70.0, 90.0];

  if (configHas(cards, 'disk'))
    out.thresh_disk = [80.0, 90.0];

  if (configHas(cards, 'temp')) {
    const base = crit != null ? [crit - 20.0, crit - 10.0] : [70.0, 80.0];

    out.thresh_temp = [
      round(tempConvert(base[0], tu, ref), 4),
      round(tempConvert(base[1], tu, ref), 4),
    ];
  }

  return out;
}

/* Serialize a raw point for /api/metrics using the CHART units (short keys). */
function toPoint(raw, f, fallback) {
  const m = raw;
  const lu = String(f.cpu_load_chart_unit);
  const mu = String(f.mem_chart_unit);
  const du = String(f.disk_chart_unit);
  const nu = String(f.net_chart_unit);
  const tu = String(f.temp_chart_unit);

  const out = {
    t: raw.t,
    l1: round(loadConvert(m.load_1m, CORES, lu), 2),
    l5: round(loadConvert(m.load_5m, CORES, lu), 2),
    l15: round(loadConvert(m.load_15m, CORES, lu), 2),
    cu: round(m.cpu_user, 1),
    cs: round(m.cpu_system, 1),
    ci: round(m.cpu_idle, 1),
  };

  if (mu[0] !== '%') {
    out.mu = round(memConvert(m.mem_used_mb, mu), 2);
    out.ma = round(memConvert(m.mem_avail_mb, mu), 2);
    out.mt = round(memConvert(m.mem_total_mb, mu), 2);
  }

  out.mp = round(m.mem_percent, 1);

  if (du[0] !== '%') {
    out.du = round(diskConvert(m.disk_used_gb, du), 2);
    out.dt = round(diskConvert(m.disk_total_gb, du), 2);
    out.df = round(diskConvert(m.disk_free_gb, du), 2);
  }

  out.dp = round(m.disk_percent, 1);

  const tc = m.temp_c;
  const ref = tempRef(m.temp_critical_c, fallback);

  if (configHas(f.charts, 'temp')) {
    out.tp = tc != null ? round(tempConvert(tc, tu, ref), 1) : null;
  }

  out.nr = round(netConvert(m.net_rx_bps, nu), 2);
  out.nt = round(netConvert(m.net_tx_bps, nu), 2);
  out.up = raw.uptime;

  return out;
}

module.exports = {
  CORES,
  netConvert,
  memConvert,
  diskConvert,
  tempConvert,
  tempRef,
  loadConvert,
  configHas,
  toCurrent,
  toPoint,
};
