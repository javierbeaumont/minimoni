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

/* --- What each chart and card needs, across the files --- */

const read = (f) => fs.readFileSync(path.join(dash, f), 'utf8');
const css = read('style.css');

test('every chart and card the server knows has its element', () => {
  const config = fs.readFileSync(path.join(__dirname, '..', 'src', 'config.c'), 'utf8');
  const charts = [...config.match(/#define CHARTS (.*)/)[1].matchAll(/"(\w+)"/g)].map((m) => m[1]);
  const cards = read('cards.js');
  const ids = (map) => Object.fromEntries(
    [...cards.match(new RegExp(`const ${map} = \\{([^}]*)\\}`))[1]
      .matchAll(/(\w+):\s*'([\w-]+)'/g)].map((m) => [m[1], m[2]]));
  const boxes = ids('CHART_BOX');
  const order = ids('CARD_ORDER');

  assert.deepStrictEqual(Object.keys(boxes), charts);
  assert.deepStrictEqual(Object.keys(order), [...charts, 'uptime']);

  for (const id of [...Object.values(boxes), ...Object.values(order)])
    assert.ok(html.includes(`id="${id}"`), `index.html has no #${id}`);
});

/* Like memory and disk: used and free, each with its legend entry. */
test('the swap chart draws used and free', () => {
  const legend = html.match(/<div class="legend" id="leg-g-swap"[^>]*>([\s\S]*?)<\/div>/);

  assert.ok(legend, 'the swap chart has no legend');
  assert.deepStrictEqual([...legend[1].matchAll(/<\/span>(\w+)<\/span>/g)].map((m) => m[1]),
    ['used', 'free']);
  assert.ok(read('chart.js').includes("k: 'swap-free'"), 'chart.js draws no free swap line');
});

/* Like the network: in and out, each with its legend entry. */
test('the swap I/O chart draws in and out', () => {
  const legend = html.match(/<div class="legend" id="leg-g-swap-io"[^>]*>([\s\S]*?)<\/div>/);

  assert.ok(legend, 'the swap I/O chart has no legend');
  assert.deepStrictEqual([...legend[1].matchAll(/<\/span>(\w+)<\/span>/g)].map((m) => m[1]),
    ['in', 'out']);
  assert.ok(read('chart.js').includes("k: 'swap-out'"), 'chart.js draws no swap out line');
});

test('every chart legend has one hidden flag per series', () => {
  const hidden = vm.runInContext('seriesHidden', ctx);
  const legends = [...html.matchAll(
    /<div class="legend" id="leg-([\w-]+)" data-chart="([\w-]+)">([\s\S]*?)<\/div>/g)];

  assert.ok(legends.length > 0);

  for (const [, id, chart, body] of legends) {
    assert.strictEqual(id, chart);
    assert.ok(Array.isArray(hidden[chart]), `chart.js seriesHidden misses ${chart}`);
    assert.strictEqual(hidden[chart].length, body.split('class="leg-item"').length - 1, chart);
  }

  assert.strictEqual(Object.keys(hidden).length, legends.length,
    'a seriesHidden entry has no legend');
});

test('every chart canvas gets the hover', () => {
  const hover = read('app.js').match(/\[([^\]]*)\]\.forEach\(attachHover\)/)[1];

  for (const [, id] of html.matchAll(/<canvas id="(g-[\w-]+)"/g))
    assert.ok(hover.includes(`'${id}'`), `app.js attaches no hover to ${id}`);
});

test('every series a chart draws has its tooltip dot', () => {
  for (const [, k] of read('chart.js').matchAll(/\bk: '([\w-]+)'/g))
    assert.ok(css.includes(`\n.tt-dot--${k} {`), `style.css misses .tt-dot--${k}`);
});

test('every series colour class in index.html has its rule', () => {
  const classes = [...html.matchAll(/class="([^"]+)"/g)].flatMap((m) => m[1].split(/\s+/));

  for (const c of new Set(classes.filter((n) => n.startsWith('s-'))))
    assert.ok(css.includes(`\n.${c} {`), `style.css misses .${c}`);
});
