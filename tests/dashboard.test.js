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

/* Unit tests for the dashboard pure helpers. Zero-dependency (node's stdlib
 * assert + a tiny runner). app.js guards its browser entry point behind a
 * window/document check and exports its pure helpers, so it can be required
 * here without a DOM. One suite for the whole dashboard; add cases as more
 * pure helpers are touched. Run via `make test`. */

'use strict';

/* Pin the zone so fmtX (which formats a local-time Date) is deterministic. */
process.env.TZ = 'UTC';

const assert = require('assert');
const path = require('path');

const {
  fmtY, fmtX, fmtTip, fmtXFull, fmtNet, fmtTempVal, cardLevel, metricsUrl, clampPoints,
  niceTicks, timeTicks
} = require(path.join(__dirname, '..', 'dashboard', 'app.js'));

/* UI glyphs the formatters emit, built from code points so this test stays
 * ASCII (the glyphs themselves live only in the dashboard sources). */
const DEG = String.fromCharCode(0x00B0); /* degree sign */
const EMD = String.fromCharCode(0x2014); /* em dash (null placeholder) */

const TESTS = [
  ['fmtY: bare numeric ticks (unit lives in the title)', function () {
    assert.strictEqual(fmtY(8), '8.0');       /* < 10 -> 1 decimal */
    assert.strictEqual(fmtY(50), '50');       /* >= 10 -> 0 decimals */
    assert.strictEqual(fmtY(1234), '1234');
    assert.strictEqual(fmtY(8, 0), '8');      /* explicit decimals override */
    assert.strictEqual(fmtY(8.25, 2), '8.25');
  }],
  ['fmtTip: precision by unit', function () {
    assert.strictEqual(fmtTip(50.5, '%'), '50.5%');
    assert.strictEqual(fmtTip(45.5, 'C'), '45.5' + DEG);
    assert.strictEqual(fmtTip(45.5, 'F'), '45.5' + DEG);
    /* Unitless: two decimals below 10, one at or above. */
    assert.strictEqual(fmtTip(2.5, ''), '2.50');
    assert.strictEqual(fmtTip(42.3, ''), '42.3');
  }],
  ['fmtX picks granularity from span', function () {
    const t = 1700000000;            /* 2023-11-14 22:13:20 UTC */
    assert.strictEqual(fmtX(t, 3600), '13:20');
    assert.strictEqual(fmtX(t, 86400 * 2), '22:13');
    assert.strictEqual(fmtX(t, 86400 * 30), 'Nov 14');
    assert.strictEqual(fmtX(t, 86400 * 365), "Nov '23");
    assert.strictEqual(fmtX(t, 86400 * 800), '2023');
    /* Inclusive thresholds: one second past a boundary drops to the next granularity. */
    assert.strictEqual(fmtX(t, 3601), '22:13'); /* 3600 is the last MM:SS second */
    assert.strictEqual(fmtX(t, 86400 * 2 + 1), 'Nov 14'); /* 2d is the last HH:MM span */
    assert.strictEqual(fmtX(0, 3600), '');
  }],
  ['fmtXFull: full timestamp by span', function () {
    const t = 1700000000;            /* 2023-11-14 22:13:20 UTC */
    assert.strictEqual(fmtXFull(t, 3600), '22:13:20');
    assert.strictEqual(fmtXFull(t, 86400 * 2), 'Nov 14 22:13');
    assert.strictEqual(fmtXFull(t, 86400 * 30), 'Nov 14, 2023');
    /* Inclusive thresholds, one second past each boundary. */
    assert.strictEqual(fmtXFull(t, 3601), 'Nov 14 22:13'); /* 3600 is the last HH:MM:SS */
    assert.strictEqual(fmtXFull(t, 86400 * 2 + 1), 'Nov 14, 2023'); /* past the 2d window */
    assert.strictEqual(fmtXFull(0, 3600), '');
  }],
  ['fmtNet: fixed unit, adaptive precision', function () {
    assert.strictEqual(fmtNet(null, 'kb'), EMD);
    /* The chosen unit never rescales: a small value keeps its unit
     * (the old behaviour turned 0.5 mb into "512 KB/s"). */
    assert.strictEqual(fmtNet(0.5, 'mb'), '0.500 MB/s');
    assert.strictEqual(fmtNet(0.5, 'mbps'), '0.500 Mbps');
    /* Precision adapts by magnitude to ~4 significant digits. */
    assert.strictEqual(fmtNet(0, 'kb'), '0.000 KB/s');     /* idle: the common case */
    assert.strictEqual(fmtNet(9.999, 'kb'), '9.999 KB/s');
    assert.strictEqual(fmtNet(10, 'kb'), '10.00 KB/s');
    assert.strictEqual(fmtNet(100, 'kb'), '100.0 KB/s');
    assert.strictEqual(fmtNet(1500, 'kb'), '1500 KB/s');
    /* The remaining unit suffixes; mb/kb/mbps are exercised above. */
    assert.strictEqual(fmtNet(2, 'gb'), '2.000 GB/s');
    assert.strictEqual(fmtNet(2, 'kbps'), '2.000 Kbps');
    assert.strictEqual(fmtNet(2, 'gbps'), '2.000 Gbps');
  }],
  ['fmtTempVal by unit', function () {
    assert.strictEqual(fmtTempVal(null, 'c'), EMD);   /* sensor configured, no reading */
    assert.strictEqual(fmtTempVal(45.5, 'c'), '45.5' + DEG + 'C');
    assert.strictEqual(fmtTempVal(113, 'f'), '113.0' + DEG + 'F');
    assert.strictEqual(fmtTempVal(80, '%'), '80.0%');
  }],
  ['cardLevel thresholds', function () {
    assert.strictEqual(cardLevel(null, [70, 90]), '');
    assert.strictEqual(cardLevel(50, [70, 90]), 'g');
    assert.strictEqual(cardLevel(75, [70, 90]), 'y');
    assert.strictEqual(cardLevel(95, [70, 90]), 'r');
  }],
  ['metricsUrl basic', function () {
    assert.strictEqual(metricsUrl('7d', 480), 'api/metrics?range=7d&points=480');
  }],
  ['metricsUrl other values', function () {
    assert.strictEqual(metricsUrl('1d', 100), 'api/metrics?range=1d&points=100');
  }],
  ['metricsUrl 90d', function () {
    assert.strictEqual(metricsUrl('90d', 1440), 'api/metrics?range=90d&points=1440');
  }],
  ['clampPoints scales by width and dpr', function () {
    assert.strictEqual(clampPoints(800, 1), 200);
    assert.strictEqual(clampPoints(800, 2), 400);
  }],
  ['clampPoints clamps to [120, 1440]', function () {
    assert.strictEqual(clampPoints(100, 1), 120);
    assert.strictEqual(clampPoints(100000, 1), 1440);
  }],
  ['niceTicks rounds the axis to round values', function () {
    assert.deepStrictEqual(niceTicks(1883, 5), [0, 500, 1000, 1500, 2000]);
    assert.deepStrictEqual(niceTicks(100, 5), [0, 20, 40, 60, 80, 100]);
    assert.deepStrictEqual(niceTicks(0, 5), [0, 1]);   /* no data yet */
    const load = niceTicks(0.4, 5);                    /* fractional step */
    assert.strictEqual(load[0], 0);
    assert.strictEqual(load[load.length - 1], 0.4);
    assert.strictEqual(load.length, 5);
  }],
  ['timeTicks lands on round, midnight-aligned times', function () {
    const t0 = 1700006400;                       /* 2023-11-15 00:00:00 UTC */
    const ticks = timeTicks(t0, t0 + 86400, 7);  /* 1-day span -> 4h step */
    assert.strictEqual(ticks[0], t0);            /* first tick on local midnight */
    assert.strictEqual(ticks.length >= 5, true);
    ticks.forEach(function(tk) { assert.strictEqual((tk - t0) % 3600, 0); });
  }],
];

let failed = 0;
for (let i = 0; i < TESTS.length; i++) {
  const name = TESTS[i][0];
  try {
    TESTS[i][1]();
    console.log('  ' + name.padEnd(45) + ' ok');
  } catch {
    failed++;
    console.log('  ' + name.padEnd(45) + ' FAIL');
  }
}
console.log('\n  ' + (TESTS.length - failed) + '/' + TESTS.length + ' tests passed');
process.exit(failed ? 1 : 0);
