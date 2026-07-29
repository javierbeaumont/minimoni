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
  netConvert, memConvert, diskConvert, loadConvert, tempConvert, tempRef, toCurrent, toPoint,
} = require(path.join(dev, 'units'));

function point(scenario = 'normal') {
  const p = currentSnapshot(scenario);

  p.t = 1700000000;

  return p;
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

test('netConvert: byte and bit units', () => {
  assert.strictEqual(netConvert(1048576, 'mb'), 1);
  assert.strictEqual(netConvert(1073741824, 'gb'), 1);
  assert.strictEqual(netConvert(1024, 'kb'), 1);
  assert.strictEqual(netConvert(1e6, 'mbps'), 8);
  assert.strictEqual(netConvert(1e9, 'gbps'), 8);
  assert.strictEqual(netConvert(1000, 'kbps'), 8);
  assert.strictEqual(netConvert(1048576, ''), 1); /* empty unit -> mb */
  assert.strictEqual(netConvert(1048576, 'x'), 1); /* unknown unit -> mb */
});

test('memConvert / diskConvert', () => {
  assert.strictEqual(memConvert(1024, 'gb'), 1);
  assert.strictEqual(memConvert(500, 'mb'), 500);
  assert.strictEqual(diskConvert(1024, 'tb'), 1);
  assert.strictEqual(diskConvert(50, 'gb'), 50);
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

test('toCurrent: thresh_temp derives from the sysfs critical point', () => {
  const out = toCurrent(currentSnapshot('normal'), configFields({}), 85.0);

  assert.deepStrictEqual(out.thresh_temp, [85, 95]); /* mock crit 105 -> [crit-20, crit-10] */
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

test('dashboardTempCriticalFallback: valid, non-positive, absent', () => {
  assert.strictEqual(dashboardTempCriticalFallback({ temp_critical_fallback: 90 }), 90);
  assert.strictEqual(dashboardTempCriticalFallback({ temp_critical_fallback: 0 }), 85);
  assert.strictEqual(dashboardTempCriticalFallback({ temp_critical_fallback: -5 }), 85);
  assert.strictEqual(dashboardTempCriticalFallback({}), 85);
});

test('configFields: defaults, memory rename, overrides', () => {
  const def = configFields({});

  assert.strictEqual(def.mem_card_unit, '%'); /* memory_card_unit -> mem_card_unit */
  assert.strictEqual(def.mem_chart_unit, 'mb');
  assert.strictEqual(def.title, 'minimoni');
  assert.strictEqual(def.charts, null); /* absent -> show all */
  assert.strictEqual(def.cards, null);

  const over = configFields({ memory_card_unit: 'gb', title: 'X' });

  assert.strictEqual(over.mem_card_unit, 'gb');
  assert.strictEqual(over.title, 'X');
});
