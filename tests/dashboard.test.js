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
  fmtY, fmtX, fmtNet, fmtTempVal, cardLevel, metricsUrl, clampPoints
} = require(path.join(__dirname, '..', 'dashboard', 'app.js'));

/* UI glyphs the formatters emit, built from code points so this test stays
 * ASCII (the glyphs themselves live only in the dashboard sources). */
const DEG = String.fromCharCode(0x00B0); /* degree sign */
const EMD = String.fromCharCode(0x2014); /* em dash (null placeholder) */

const TESTS = [
  ['fmtY by magnitude and unit', function () {
    assert.strictEqual(fmtY(8, ''), '8.0');
    assert.strictEqual(fmtY(50, ''), '50');
    assert.strictEqual(fmtY(50, '%'), '50%');
    assert.strictEqual(fmtY(45, 'C'), '45' + DEG);
  }],
  ['fmtX picks granularity from span', function () {
    const t = 1700000000;            /* 2023-11-14 22:13:20 UTC */
    assert.strictEqual(fmtX(t, 3600), '13:20');
    assert.strictEqual(fmtX(t, 86400 * 2), '22:13');
    assert.strictEqual(fmtX(t, 86400 * 30), 'Nov 14');
    assert.strictEqual(fmtX(t, 86400 * 365), "Nov '23");
    assert.strictEqual(fmtX(t, 86400 * 800), '2023');
    assert.strictEqual(fmtX(0, 3600), '');
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
    assert.strictEqual(metricsUrl('7d', 480), '/api/metrics?range=7d&points=480');
  }],
  ['metricsUrl other values', function () {
    assert.strictEqual(metricsUrl('1d', 100), '/api/metrics?range=1d&points=100');
  }],
  ['metricsUrl 90d', function () {
    assert.strictEqual(metricsUrl('90d', 1440), '/api/metrics?range=90d&points=1440');
  }],
  ['clampPoints scales by width and dpr', function () {
    assert.strictEqual(clampPoints(800, 1), 200);
    assert.strictEqual(clampPoints(800, 2), 400);
  }],
  ['clampPoints clamps to [120, 1440]', function () {
    assert.strictEqual(clampPoints(100, 1), 120);
    assert.strictEqual(clampPoints(100000, 1), 1440);
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
