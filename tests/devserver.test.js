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

/* Unit tests for the dev-server pure functions, on node's built-in runner
 * (node:test + node:assert, zero external deps). Run via `make test` (node --test). */

'use strict';

const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const dev = path.join(__dirname, '..', 'tools', 'devserver');

const {
  configFields, parseDashboard, dashboardTempCriticalFallback, loadDashboardConfig,
} = require(path.join(dev, 'config'));

const { streamShouldDrop } = require(path.join(dev, 'handler'));
const {
  rangeSeconds, clampPointsParam, currentSnapshot, makePoints,
} = require(path.join(dev, 'mock-data'));

const {
  netConvert, netRefBps, loadConvert, tempConvert, tempRef, toCurrent, unitsFor,
  toPoint,
} = require(path.join(dev, 'units'));

function point(scenario = 'normal') {
  const p = currentSnapshot(scenario);

  p.t = 1700000000;

  return p;
}

/* The same snapshot from a host without swap; the mock always has some. */
function swapless(raw) {
  return { ...raw, swap_total_mb: null, swap_free_mb: null };
}

test('clampPointsParam: valid', () => {
  assert.strictEqual(clampPointsParam('480'), 480);
});

test('clampPointsParam: at cap', () => {
  assert.strictEqual(clampPointsParam('1440'), 1440);
});

test('clampPointsParam: above cap', () => {
  assert.strictEqual(clampPointsParam('99999'), 1440);
});

test('clampPointsParam: zero defaults', () => {
  assert.strictEqual(clampPointsParam('0'), 240);
});

test('clampPointsParam: negative defaults', () => {
  assert.strictEqual(clampPointsParam('-5'), 240);
});

test('clampPointsParam: empty defaults', () => {
  assert.strictEqual(clampPointsParam(''), 240);
});

test('clampPointsParam: non-numeric defaults', () => {
  assert.strictEqual(clampPointsParam('abc'), 240);
});

test('streamShouldDrop: off by default', () => {
  assert.strictEqual(streamShouldDrop(false, 0), false);
  assert.strictEqual(streamShouldDrop(false, 1), false);
});

test('streamShouldDrop: every other when flaky', () => {
  assert.strictEqual(streamShouldDrop(true, 0), false); /* even seq: serve normally */
  assert.strictEqual(streamShouldDrop(true, 1), true); /* odd seq: drop (simulate failure) */
  assert.strictEqual(streamShouldDrop(true, 2), false);
  assert.strictEqual(streamShouldDrop(true, 3), true);
});

test('rangeSeconds: minutes', () => {
  assert.strictEqual(rangeSeconds('30m'), 1800);
});

test('rangeSeconds: hours', () => {
  assert.strictEqual(rangeSeconds('12h'), 43200);
});

test('rangeSeconds: days', () => {
  assert.strictEqual(rangeSeconds('7d'), 604800);
});

test('rangeSeconds: invalid defaults to one day', () => {
  assert.strictEqual(rangeSeconds('bogus'), 86400);
});

test('tempRef: uses critical', () => {
  assert.strictEqual(tempRef(105.0, 85.0), 105.0);
});

test('tempRef: falls back when null', () => {
  assert.strictEqual(tempRef(null, 85.0), 85.0);
});

test('tempConvert: percent at ref', () => {
  assert.strictEqual(tempConvert(85.0, '%', 85.0), 100.0);
});

test('tempConvert: percent half', () => {
  assert.strictEqual(tempConvert(42.5, '%', 85.0), 50.0);
});

test('tempConvert: fahrenheit', () => {
  assert.strictEqual(tempConvert(100.0, 'f', 85.0), 212.0);
});

test('tempConvert: celsius', () => {
  assert.strictEqual(tempConvert(50.0, 'c', 85.0), 50.0);
});

test('netConvert: anything but "%" passes through', () => {
  /* Magnitude is the dashboard's call, so the mock ships base bytes/s too. */
  assert.strictEqual(netConvert(1048576, 'bytes'), 1048576);
  assert.strictEqual(netConvert(1048576, 'bits'), 1048576);
  assert.strictEqual(netConvert(1048576, ''), 1048576);
});

test('netRefBps / netConvert percent mirror the C server', () => {
  assert.strictEqual(netRefBps(100, 1000), 12500000); /* configured max wins over the link */
  assert.strictEqual(netRefBps(0, 1000), 125000000); /* detected link when nothing configured */
  assert.strictEqual(netRefBps(0, 0), 125000000); /* neither: assume 1 GbE */

  const ref = netRefBps(1000, 0);

  assert.strictEqual(netConvert(ref / 2, '%', ref), 50); /* full duplex: whole link per direction */
  assert.strictEqual(netConvert(ref, '%', ref), 100);
  assert.strictEqual(netConvert(1e6, '%', 0), 0); /* no reference -> 0, never NaN */
});

test('toCurrent: net percent end to end', () => {
  const raw = currentSnapshot('normal');

  raw.net_rx_bps = 62500000; /* half of the mock 1000 Mbit/s link */
  raw.net_speed_mbit = 1000;

  const out = toCurrent(raw, configFields({ net_card_unit: '%' }), 85.0);

  assert.strictEqual(out.net_rx, 50);
});

test('loadConvert: percent normalises by cores', () => {
  assert.strictEqual(loadConvert(2, 4, '%'), 50);
  assert.strictEqual(loadConvert(2, 4, 'abs'), 2);
  assert.strictEqual(loadConvert(2, 0, '%'), 2); /* cores 0 -> unchanged */
});

test('toCurrent: emits thresholds', () => {
  const out = toCurrent(currentSnapshot('normal'), configFields({}), 85.0);

  assert.deepStrictEqual(out.thresh_cpu, [70.0, 90.0]);
  assert.deepStrictEqual(out.thresh_mem, [70.0, 90.0]);
  assert.deepStrictEqual(out.thresh_disk, [80.0, 90.0]);
  assert.ok(Array.isArray(out.thresh_load) && out.thresh_load.length === 2);
});

test('toCurrent: gates excluded cards', () => {
  const f = configFields({ cards: ['cpu_load'] });
  const out = toCurrent(currentSnapshot('normal'), f, 85.0);

  assert.ok('load_1m' in out && 'thresh_load' in out);
  assert.ok(!('mem_percent' in out) && !('thresh_mem' in out));
  assert.ok(!('net_rx' in out));
});

test('toCurrent: percent unit omits the raw mem/disk fields', () => {
  const pct = toCurrent(currentSnapshot('normal'), configFields({}), 85.0);

  assert.ok(!('mem_used' in pct) && !('disk_used' in pct)); /* defaults are '%' */
  assert.ok('mem_percent' in pct && 'disk_percent' in pct);

  const f = configFields({ memory_card_unit: 'gb', disk_card_unit: 'gb' });
  const raw = toCurrent(currentSnapshot('normal'), f, 85.0);

  assert.ok('mem_used' in raw && 'mem_total' in raw && 'disk_free' in raw);
});

/* The swap use fields of an /api/current response: not swap I/O's, nor the unit echoes. */
const swapFields = (out) => Object.keys(out).filter((k) => /^swap_(total|free)/.test(k));

test('toCurrent: swap_free in the card unit, and swap_total only in "auto"', () => {
  const raw = { ...currentSnapshot('normal'), swap_total_mb: 1024, swap_free_mb: 768 };
  const pct = toCurrent(raw, configFields({}), 85.0); /* swap_card_unit defaults to '%' */

  assert.deepStrictEqual(swapFields(pct), ['swap_free']);
  assert.strictEqual(pct.swap_free, 75);

  const abs = toCurrent(raw, configFields({ swap_card_unit: 'auto' }), 85.0);

  assert.deepStrictEqual(swapFields(abs), ['swap_free', 'swap_total']);
  assert.strictEqual(abs.swap_total, 1024);
  assert.strictEqual(abs.swap_free, 768);
});

test('toCurrent: without swap the fields are null, so the card shows a dash', () => {
  const raw = swapless(currentSnapshot('normal'));
  const pct = toCurrent(raw, configFields({}), 85.0);
  const abs = toCurrent(raw, configFields({ swap_card_unit: 'auto' }), 85.0);

  assert.strictEqual(pct.swap_free, null);
  assert.strictEqual(abs.swap_total, null);
  assert.strictEqual(abs.swap_free, null);
});

test('toCurrent: thresh_swap is sent, and no swap field when its card is not listed', () => {
  const all = toCurrent(currentSnapshot('normal'), configFields({}), 85.0);

  assert.deepStrictEqual(all.thresh_swap, [80.0, 95.0]);

  const mem = toCurrent(currentSnapshot('normal'), configFields({ cards: ['memory'] }), 85.0);

  assert.ok(!('swap_free' in mem) && !('thresh_swap' in mem) && 'mem_percent' in mem);
});

test('toCurrent: thresh_temp derives from the sysfs critical point', () => {
  const out = toCurrent(currentSnapshot('normal'), configFields({}), 85.0);

  assert.deepStrictEqual(out.thresh_temp, [85, 95]); /* mock crit 105 -> [crit-20, crit-10] */
});

test('toCurrent: thresh_net tracks the link ceiling', () => {
  const pct = toCurrent(currentSnapshot('normal'), configFields({ net_card_unit: '%' }), 85.0);

  assert.deepStrictEqual(pct.thresh_net, [85, 98]);

  /* Outside "%", base bytes/s: 100 Mbit configured (4th arg) = 12500000 B/s. */
  const abs = toCurrent(currentSnapshot('normal'),
    configFields({ net_card_unit: 'bytes' }), 85.0, 100);

  assert.deepStrictEqual(abs.thresh_net, [10625000, 12250000]);
});

test('unitsFor: mirrors the C choice for a window', () => {
  const raws = [
    { mem_used_mb: 300, mem_avail_mb: 1748, disk_used_gb: 20, disk_free_gb: 72,
      net_rx_bps: 5242880, net_tx_bps: 1000 },
  ];
  const u = unitsFor(raws, configFields({}));

  assert.deepStrictEqual(u.mem, { sym: 'MB', mul: 1, div: 1 });
  assert.deepStrictEqual(u.disk, { sym: 'GB', mul: 1, div: 1 });
  assert.deepStrictEqual(u.net, { sym: 'KB/s', mul: 1, div: 1024 });

  /* Nothing on screen: no unit at all, rather than an invented one. */
  assert.deepStrictEqual(unitsFor([{}], configFields({})), {});

  /* A "%" metric has no magnitude. */
  assert.ok(!('mem' in unitsFor(raws, configFields({ memory_chart_unit: '%' }))));

  /* Rows with no delta carry noise, and must not set the magnitude (as in C). */
  const noisy = unitsFor([
    { net_valid: 0, net_tx_bps: 9e9 },
    { net_valid: 1, net_tx_bps: 2048 },
  ], configFields({}));

  assert.deepStrictEqual(noisy.net, { sym: 'KB/s', mul: 1, div: 1024 });

  /* Bits climb in 1000, bytes in 1024, and both ladders start one rung up. */
  const bits = unitsFor([{ net_rx_bps: 125000, net_tx_bps: 0 }],
    configFields({ net_chart_unit: 'bits' }));

  assert.deepStrictEqual(bits.net, { sym: 'Kbps', mul: 8, div: 1000 });
});

test('unitsFor: swap takes the larger of used and free, from rows with swap', () => {
  const free = unitsFor([{ swap_total_mb: 12100, swap_free_mb: 12000 }], configFields({}));

  assert.deepStrictEqual(free.swap, { sym: 'GB', mul: 1, div: 1024 });

  const used = unitsFor([{ swap_total_mb: 12100, swap_free_mb: 100 }], configFields({}));

  assert.deepStrictEqual(used.swap, { sym: 'GB', mul: 1, div: 1024 });

  const small = unitsFor([{ swap_total_mb: 200, swap_free_mb: 100 }], configFields({}));

  assert.deepStrictEqual(small.swap, { sym: 'MB', mul: 1, div: 1 });

  /* No row with swap, or a "%" chart: no unit at all. */
  assert.ok(!('swap' in unitsFor([{ mem_used_mb: 500 }], configFields({}))));
  assert.ok(!('swap' in unitsFor([{ swap_total_mb: 200, swap_free_mb: 100 }],
    configFields({ swap_chart_unit: '%' }))));
});

test('toPoint: emits short keys', () => {
  const out = toPoint(point(), configFields({}), 85.0);

  for (const k of ['t', 'l1', 'cu', 'mp', 'dp', 'nr', 'up'])
    assert.ok(k in out, `missing key ${k}`);
});

test('toPoint: temp gated by charts', () => {
  assert.ok('tp' in toPoint(point(), configFields({}), 85.0));
  assert.ok(!('tp' in toPoint(point(), configFields({ charts: ['cpu_load'] }), 85.0)));
});

test('toPoint: percent chart unit omits the raw mem/disk keys', () => {
  const f = configFields({ memory_chart_unit: '%', disk_chart_unit: '%' });
  const pct = toPoint(point(), f, 85.0);

  assert.ok(!('mu' in pct) && !('du' in pct));
  assert.ok('mp' in pct && 'dp' in pct);

  const raw = toPoint(point(), configFields({}), 85.0); /* defaults are mb/gb */

  assert.ok('mu' in raw && 'mt' in raw && 'df' in raw);
});

test('cycle scenario: cards tick with time, charts sweep low-high-low', () => {
  const snap = currentSnapshot('cycle');

  assert.ok(snap.mem_percent >= 0 && snap.mem_percent <= 99);

  const p = makePoints('1d', 'cycle', 9);

  assert.ok(p[4].cpu_user > p[0].cpu_user + 20); /* mid-sweep well above the edges */
  assert.ok(p[4].cpu_user > p[8].cpu_user + 20);
});

test('makePoints: n ascending points spanning the range', () => {
  const p = makePoints('1d', 'normal', 240);

  assert.strictEqual(p.length, 240);

  const ts = p.map((x) => x.t);

  for (let i = 1; i < ts.length; i++)
    assert.ok(ts[i] > ts[i - 1], `t not ascending at ${i}`);

  assert.strictEqual(ts[239] - ts[0], 239 * Math.floor(86400 / 240)); /* (n-1)*step */
});

test('parseDashboard: scalars, comment, # inside string', () => {
  const d = parseDashboard([
    '[server]',
    'listen = "0.0.0.0:8080"',
    '[dashboard]',
    'title = "My # Server"   # trailing comment',
    'theme = "dark"',
    'show_footer = false',
    'temp_critical_fallback = 90',
  ].join('\n'));

  assert.strictEqual(d.title, 'My # Server');
  assert.strictEqual(d.theme, 'dark');
  assert.strictEqual(d.show_footer, false);
  assert.strictEqual(d.temp_critical_fallback, 90);
  assert.ok(!('listen' in d)); /* keys outside [dashboard] are ignored */
});

test('parseDashboard: single, multi-line, and empty arrays', () => {
  const d = parseDashboard([
    '[dashboard]',
    'charts = ["cpu_load", "memory"]',
    'ranges = [',
    '  "6h",',
    '  "1d",',
    ']',
    'cards = []',
  ].join('\n'));

  assert.deepStrictEqual(d.charts, ['cpu_load', 'memory']);
  assert.deepStrictEqual(d.ranges, ['6h', '1d']);
  assert.deepStrictEqual(d.cards, []);
});

test('parseDashboard: unescapes quotes inside strings', () => {
  const d = parseDashboard(['[dashboard]', 'title = "a \\"b\\""'].join('\n'));

  assert.strictEqual(d.title, 'a "b"');
});

test('parseDashboard: ignores non-dashboard tables', () => {
  const d = parseDashboard(['[dashboard]', 'title = "x"', '[[alert]]', 'name = "hot"'].join('\n'));

  assert.strictEqual(d.title, 'x');
  assert.ok(!('name' in d));
});

test('loadDashboardConfig: reads the [dashboard] table from disk', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'minimoni-test-'));
  const file = path.join(dir, 'config.toml');

  try {
    fs.writeFileSync(file, '[dashboard]\ntitle = "from disk"\n');
    assert.strictEqual(loadDashboardConfig(file).title, 'from disk');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('toPoint: sf in the chart unit, and st only in "auto"', () => {
  const raw = { ...point(), swap_total_mb: 1024, swap_free_mb: 768 };
  const abs = toPoint(raw, configFields({}), 85.0); /* swap_chart_unit defaults to 'auto' */

  assert.strictEqual(abs.st, 1024);
  assert.strictEqual(abs.sf, 768);

  const pct = toPoint(raw, configFields({ swap_chart_unit: '%' }), 85.0);

  assert.strictEqual(pct.sf, 75);
  assert.ok(!('st' in pct));
});

test('toPoint: without swap the fields are null; with its chart not listed, absent', () => {
  const none = toPoint(swapless(point()), configFields({}), 85.0);

  assert.strictEqual(none.st, null);
  assert.strictEqual(none.sf, null);

  const mem = toPoint(point(), configFields({ charts: ['memory'] }), 85.0);

  assert.ok(!('st' in mem) && !('sf' in mem));
});

/* Swap I/O on 1000 MB of RAM: 174762.67 B/s moves 1 % of it in a minute. */
const ioRaw = (raw) => ({ ...raw, mem_total_mb: 1000, swap_total_mb: 1024, swap_free_mb: 768,
  swap_in_bps: 174762.67, swap_out_bps: 2 * 174762.67 });

test('toCurrent: swap I/O in bytes/s, or in % of the RAM per minute', () => {
  const raw = ioRaw(currentSnapshot('normal'));
  const bytes = toCurrent(raw, configFields({}), 85.0); /* swap_io_card_unit defaults to 'bytes' */

  assert.strictEqual(bytes.swap_in, 174762.67);
  assert.strictEqual(bytes.swap_out, 349525.34);

  const ram = toCurrent(raw, configFields({ swap_io_card_unit: 'ram' }), 85.0);

  assert.strictEqual(ram.swap_in, 1);
  assert.strictEqual(ram.swap_out, 2);
});

test('toCurrent: thresh_swap_io is 1 and 5 % of the RAM per minute, in the card unit', () => {
  const raw = ioRaw(currentSnapshot('normal'));
  const ram = toCurrent(raw, configFields({ swap_io_card_unit: 'ram' }), 85.0);
  const bytes = toCurrent(raw, configFields({}), 85.0);

  assert.deepStrictEqual(ram.thresh_swap_io, [1, 5]);
  assert.deepStrictEqual(bytes.thresh_swap_io, [174762.6667, 873813.3333]);
});

test('toCurrent: swap I/O is null without swap, and absent with its card not listed', () => {
  const none = toCurrent(swapless(currentSnapshot('normal')), configFields({}), 85.0);

  assert.strictEqual(none.swap_in, null);
  assert.strictEqual(none.swap_out, null);

  const mem = toCurrent(ioRaw(currentSnapshot('normal')),
    configFields({ cards: ['memory'] }), 85.0);

  assert.ok(!('swap_in' in mem) && !('thresh_swap_io' in mem));
});

test('toPoint: swap I/O in bytes/s or in % of the RAM per minute, null without swap', () => {
  const bytes = toPoint(ioRaw(point()), configFields({}), 85.0);

  assert.strictEqual(bytes.si, 174762.67);
  assert.strictEqual(bytes.so, 349525.34);

  const ram = toPoint(ioRaw(point()), configFields({ swap_io_chart_unit: 'ram' }), 85.0);

  assert.strictEqual(ram.si, 1);
  assert.strictEqual(ram.so, 2);

  const none = toPoint(swapless(point()), configFields({}), 85.0);

  assert.strictEqual(none.si, null);
  assert.strictEqual(none.so, null);
});

test('unitsFor: swap I/O in bytes takes the busier direction; in "ram" no unit', () => {
  const io = [{ swap_total_mb: 1024, swap_in_bps: 2.0e6, swap_out_bps: 1.0e3 }];

  assert.deepStrictEqual(unitsFor(io, configFields({})).swap_io,
    { sym: 'KB/s', mul: 1, div: 1024 });
  assert.ok(!('swap_io' in unitsFor(io, configFields({ swap_io_chart_unit: 'ram' }))));
});

test('dashboardTempCriticalFallback: valid, non-positive, absent', () => {
  assert.strictEqual(dashboardTempCriticalFallback({ temp_critical_fallback: 90 }), 90);
  assert.strictEqual(dashboardTempCriticalFallback({ temp_critical_fallback: 0 }), 85);
  assert.strictEqual(dashboardTempCriticalFallback({ temp_critical_fallback: -5 }), 85);
  assert.strictEqual(dashboardTempCriticalFallback({}), 85);
});

test('configFields: defaults, memory rename, overrides', () => {
  const def = configFields({});

  assert.strictEqual(def.mem_card_unit, '%'); /* memory_card_unit -> mem_card_unit */
  assert.strictEqual(def.mem_chart_unit, 'auto');
  assert.strictEqual(def.title, 'minimoni');
  assert.strictEqual(def.charts, null); /* absent -> show all */
  assert.strictEqual(def.cards, null);

  assert.strictEqual(def.swap_card_unit, '%');
  assert.strictEqual(def.swap_chart_unit, 'auto');
  assert.strictEqual(def.swap_io_card_unit, 'bytes');
  assert.strictEqual(def.swap_io_chart_unit, 'bytes');

  const over = configFields({ memory_card_unit: 'auto', swap_chart_unit: '%', title: 'X' });

  assert.strictEqual(over.mem_card_unit, 'auto');
  assert.strictEqual(over.swap_chart_unit, '%');
  assert.strictEqual(over.title, 'X');
});
