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

/*
 * Unit tests for the dashboard pure helpers. Zero-dependency (node's stdlib
 * assert + a tiny runner). app.js guards its browser entry point behind a
 * window/document check and exports its pure helpers, so it can be required
 * here without a DOM. One suite for the whole dashboard; add cases as more
 * pure helpers (fmtY, fmtNet, fmtX, ...) are touched. Run via `make test`.
 */

'use strict';

const assert = require('assert');
const path = require('path');

const { metricsUrl } = require(path.join(__dirname, '..', 'dashboard', 'app.js'));

const TESTS = [
  ['metricsUrl basic', function () {
    assert.strictEqual(metricsUrl('7d', 480), '/api/metrics?range=7d&points=480');
  }],
  ['metricsUrl other values', function () {
    assert.strictEqual(metricsUrl('1d', 100), '/api/metrics?range=1d&points=100');
  }],
  ['metricsUrl 90d', function () {
    assert.strictEqual(metricsUrl('90d', 1440), '/api/metrics?range=90d&points=1440');
  }],
];

let failed = 0;
for (let i = 0; i < TESTS.length; i++) {
  const name = TESTS[i][0];
  try {
    TESTS[i][1]();
    console.log('  ' + name.padEnd(45) + ' ok');
  } catch (e) {
    failed++;
    console.log('  ' + name.padEnd(45) + ' FAIL');
  }
}
console.log('\n  ' + (TESTS.length - failed) + '/' + TESTS.length + ' tests passed');
process.exit(failed ? 1 : 0);
