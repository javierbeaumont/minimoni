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

/* Contract test: tools/devserver/units.js is a hand-written mirror of
 * src/units.c, so it can drift the moment someone edits the C and not the JS
 * (the dashboard would then be developed against wrong numbers). This runs the
 * C reference binary built from tests/contract-units.c and asserts the JS
 * produces the same value for every case. `make test` builds the binary; a bare
 * `node --test` skips this file rather than failing on its absence. */

'use strict';

const { test } = require('node:test');
const assert = require('node:assert');
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const dev = path.join(__dirname, '..', 'tools', 'devserver');
const {
  netConvert, netRefBps, tempConvert, tempRef, loadConvert, unitStep,
} = require(path.join(dev, 'units'));

const REF_BIN = path.join(__dirname, '..', 'build', 'contract-units');

/* Same cases, same order and names as tests/contract-units.c. */
const CASES = [
  ['net bytes passthrough', () => netConvert(1048576, 'bytes', 0)],
  ['net bits passthrough', () => netConvert(1048576, 'bits', 0)],
  ['net null passthrough', () => netConvert(1048576, null, 0)],

  ['net ref configured wins', () => netRefBps(100, 1000)],
  ['net ref detected', () => netRefBps(0, 300)],
  ['net ref default gbe', () => netRefBps(0, 0)],
  ['net pct half', () => netConvert(62500000, '%', netRefBps(1000, 0))],
  ['net pct full', () => netConvert(12500000, '%', netRefBps(100, 0))],
  ['net pct no ref', () => netConvert(1e6, '%', 0)],


  ['unit step nothing', () => unitStep(0, 4, 1024)],
  ['unit step smallest', () => unitStep(900, 4, 1024)],
  ['unit step at the cap', () => unitStep(9999, 4, 1024)],
  ['unit step past the cap', () => unitStep(10000, 4, 1024)],
  ['unit step two up', () => unitStep(5e6, 4, 1024)],
  ['unit step ladder top', () => unitStep(1e15, 4, 1024)],
  ['unit step bits factor', () => unitStep(1e6, 4, 1000)],

  ['temp c', () => tempConvert(50, 'c', 85)],
  ['temp f', () => tempConvert(100, 'f', 85)],
  ['temp f zero', () => tempConvert(0, 'f', 85)],
  ['temp pct at ref', () => tempConvert(85, '%', 85)],
  ['temp pct half', () => tempConvert(42.5, '%', 85)],
  ['temp pct zero ref', () => tempConvert(50, '%', 0)],
  ['temp ref crit', () => tempRef(105, 85)],
  ['temp ref fallback', () => tempRef(null, 85)],

  ['load abs', () => loadConvert(2, 4, 'abs')],
  ['load pct', () => loadConvert(2, 4, '%')],
  ['load pct zero cores', () => loadConvert(2, 0, '%')],
];

test('devserver units.js matches src/units.c case for case', (t) => {
  if (!fs.existsSync(REF_BIN)) {
    t.skip(`no ${path.relative(process.cwd(), REF_BIN)}; run \`make test\` to build it`);
    return;
  }

  let out;
  try {
    out = execFileSync(REF_BIN, { encoding: 'utf8' });
  } catch (err) {
    /* The reference is built for the Alpine container; a macOS host cannot run
     * that ELF. Inside `make test` (the gate) it always executes. */
    if (err.code === 'ENOEXEC') {
      t.skip('reference binary is not native to this host; run `make test`');
      return;
    }
    throw err;
  }

  const reference = new Map(
    out
      .trim()
      .split('\n')
      .map((line) => line.split('|'))
  );

  assert.strictEqual(reference.size, CASES.length,
    'case count differs: the C and JS lists are out of step');

  for (const [name, fn] of CASES) {
    assert.ok(reference.has(name), `case '${name}' missing from the C reference`);
    assert.strictEqual(fn().toFixed(6), reference.get(name), `mirror drift in '${name}'`);
  }
});
