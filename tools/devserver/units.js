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

/* A configured speed wins over the detected link (the NIC is often faster than
 * the uplink behind it); then sysfs, then 1 GbE. Full duplex: each direction
 * gets the whole link. */
function netRefBps(maxMbit, detectedMbit) {
  let mbit = maxMbit > 0 ? maxMbit : detectedMbit;
  if (!(mbit > 0))
    mbit = 1000;
  return mbit * 125000.0; /* Mbit/s -> bytes/s */
}

function netConvert(bps, unit, refBps) {
  if (unit && unit[0] === '%')
    return refBps > 0 ? (bps * 100.0) / refBps : 0.0;

  return bps;
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
function toCurrent(raw, f, fallback, netMaxSpeed = 0) {
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
      out.mem_used = round(m.mem_used_mb, 2);
      out.mem_available = round(m.mem_avail_mb, 2);
      out.mem_total = round(m.mem_total_mb, 2);
    }

    out.mem_percent = round(m.mem_percent, 1);
  }

  if (configHas(cards, 'disk')) {
    if (du[0] !== '%') {
      out.disk_used = round(m.disk_used_gb, 2);
      out.disk_total = round(m.disk_total_gb, 2);
      out.disk_free = round(m.disk_free_gb, 2);
    }

    out.disk_percent = round(m.disk_percent, 1);
  }

  const tc = m.temp_c;
  const crit = m.temp_critical_c;
  const ref = tempRef(crit, fallback);
  const nref = netRefBps(netMaxSpeed, m.net_speed_mbit);

  if (configHas(cards, 'temp')) {
    out.temp = tc != null ? round(tempConvert(tc, tu, ref), 1) : null;
  }

  if (configHas(cards, 'net')) {
    out.net_rx = round(netConvert(m.net_rx_bps, nu, nref), 2);
    out.net_tx = round(netConvert(m.net_tx_bps, nu, nref), 2);
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

  if (configHas(cards, 'net')) {
    out.thresh_net = [
      round(netConvert(0.85 * nref, nu, nref), 4),
      round(netConvert(0.98 * nref, nu, nref), 4),
    ];
  }

  return out;
}

/* Mirror of unit_step + json_serialize_units in the C server. */
const LADDERS = {
  mem:   { sym: ['MB', 'GB', 'TB'], mul: 1, factor: 1024, base: 0 },
  disk:  { sym: ['GB', 'TB', 'PB'], mul: 1, factor: 1024, base: 0 },
  bytes: { sym: ['KB/s', 'MB/s', 'GB/s'], mul: 1, factor: 1024, base: 1 },
  bits:  { sym: ['Kbps', 'Mbps', 'Gbps'], mul: 8, factor: 1000, base: 1 },
};
const DIGITS_MAX = 9999;

function unitStep(max, steps, factor) {
  if (!(max > 0)) return -1;
  let i = 0;
  while (i < steps - 1 && max / Math.pow(factor, i) > DIGITS_MAX) i++;
  return i;
}

function unitFor(max, kind) {
  const l = LADDERS[kind];
  const i = unitStep((max * l.mul) / Math.pow(l.factor, l.base), l.sym.length, l.factor);
  return i < 0 ? null : { sym: l.sym[i], mul: l.mul, div: Math.pow(l.factor, i + l.base) };
}

function unitsFor(raws, f) {
  const out = {};
  const peak = (rows, ...keys) => rows.reduce((m, r) =>
    keys.reduce((n, k) => (r[k] != null && r[k] > n ? r[k] : n), m), 0);
  /* Like the C server: a row with no delta yet carries noise, not throughput. */
  const withNet = raws.filter((r) => r.net_valid !== 0);

  if (String(f.mem_chart_unit)[0] !== '%')
    out.mem = unitFor(peak(raws, 'mem_used_mb', 'mem_avail_mb'), 'mem');
  if (String(f.disk_chart_unit)[0] !== '%')
    out.disk = unitFor(peak(raws, 'disk_used_gb', 'disk_free_gb'), 'disk');
  if (String(f.net_chart_unit)[0] !== '%') {
    out.net = unitFor(peak(withNet, 'net_rx_bps', 'net_tx_bps'),
      String(f.net_chart_unit) === 'bits' ? 'bits' : 'bytes');
  }
  Object.keys(out).forEach((k) => { if (!out[k]) delete out[k]; });
  return out;
}

/* Serialize a raw point for /api/metrics using the CHART units (short keys). */
function toPoint(raw, f, fallback, netMaxSpeed = 0) {
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
    out.mu = round(m.mem_used_mb, 2);
    out.ma = round(m.mem_avail_mb, 2);
    out.mt = round(m.mem_total_mb, 2);
  }

  out.mp = round(m.mem_percent, 1);

  if (du[0] !== '%') {
    out.du = round(m.disk_used_gb, 2);
    out.dt = round(m.disk_total_gb, 2);
    out.df = round(m.disk_free_gb, 2);
  }

  out.dp = round(m.disk_percent, 1);

  const tc = m.temp_c;
  const ref = tempRef(m.temp_critical_c, fallback);

  if (configHas(f.charts, 'temp')) {
    out.tp = tc != null ? round(tempConvert(tc, tu, ref), 1) : null;
  }

  const nref = netRefBps(netMaxSpeed, m.net_speed_mbit);

  out.nr = round(netConvert(m.net_rx_bps, nu, nref), 2);
  out.nt = round(netConvert(m.net_tx_bps, nu, nref), 2);
  out.up = raw.uptime;

  return out;
}

module.exports = {
  netConvert,
  netRefBps,
  tempConvert,
  tempRef,
  loadConvert,
  toCurrent,
  toPoint,
  unitsFor,
  unitStep,
};
