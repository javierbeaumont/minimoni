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

/* Bundle smoke test: evaluates the dashboard scripts in ONE shared vm context,
 * exactly as the browser sees the inlined bundle. Catches what per-file linting
 * cannot: load-order breakage, cross-file name collisions (a duplicate top-level
 * name silently overrides the other in a shared scope), a script listed in
 * index.html but missing its bundle.sh marker, and it exercises the few pure
 * functions of the DOM files (coverage thresholds exclude these files: their
 * bodies need a real browser). */

'use strict';

const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const dash = path.join(__dirname, '..', 'dashboard');
const html = fs.readFileSync(path.join(dash, 'index.html'), 'utf8');
const bundleSh = fs.readFileSync(
  path.join(__dirname, '..', 'tools', 'bundle.sh'), 'utf8');

/* Script load order as declared in index.html. */
const scripts = [...html.matchAll(/<script src="([^"]+)">/g)].map((m) => m[1]);

test('index.html declares the dashboard scripts', () => {
  assert.deepStrictEqual(scripts, ['format.js', 'chart.js', 'hover.js', 'cards.js', 'app.js']);
});

test('bundle.sh has an inline marker for every script', () => {
  for (const s of scripts)
    assert.ok(bundleSh.includes(`/src="${s}"/`), `bundle.sh misses the ${s} marker`);
});

test('no top-level name is defined in two files', () => {
  const owner = {};

  for (const s of scripts) {
    const src = fs.readFileSync(path.join(dash, s), 'utf8');

    for (const m of src.matchAll(/^(?:function|const|let)\s+([A-Za-z_$][\w$]*)/gm)) {
      assert.ok(!(m[1] in owner), `'${m[1]}' defined in both ${owner[m[1]]} and ${s}`);
      owner[m[1]] = s;
    }
  }
});

/* One shared context, loaded in bundle order. No `document` at load time: the
 * files must load cleanly outside a browser (their entry points are guarded). */
const ctx = vm.createContext({});

for (const s of scripts) {
  vm.runInContext(fs.readFileSync(path.join(dash, s), 'utf8'), ctx,
    { filename: path.join('dashboard', s) });
}

test('the bundle loads as one scope and wires the cross-file interface', () => {
  for (const fn of ['fmtY', 'drawChart', 'renderAll', 'attachHover', 'updateCards',
    'wireCards', 'buildTabs', 'connectSSE'])
    assert.strictEqual(typeof ctx[fn], 'function', fn);
});

test('refreshLockedIdx pins the closest point and drops stale locks', () => {
  const run = (code) => vm.runInContext(code, ctx);

  /* hideTooltip (called on auto-unlock) needs a element with classList. */
  ctx.document = { getElementById: () => ({ classList: { add() {} } }) };

  run('pts = [{ t: 100 }, { t: 200 }, { t: 300 }]');
  assert.strictEqual(run('lockedTs = 210; refreshLockedIdx(); lockedIdx'), 1);
  assert.strictEqual(run('lockedTs = 300; refreshLockedIdx(); lockedIdx'), 2);

  /* The locked moment slid out of the window: release, do not snap to an edge. */
  run('lockedTs = 50; refreshLockedIdx()');
  assert.strictEqual(run('lockedTs'), null);
  assert.strictEqual(run('lockedIdx'), null);
  assert.strictEqual(run('lockedTs = null; refreshLockedIdx(); lockedIdx'), null);
});
