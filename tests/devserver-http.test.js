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

/* HTTP tests for the dev-server handler, run against a real in-process
 * node:http server on an ephemeral port (node:test + fetch, zero external
 * deps). Covers what tests/devserver.test.js (pure functions) cannot: routing,
 * placeholder substitution, static allowlist and the SSE stream. */

'use strict';

const { test } = require('node:test');
const assert = require('node:assert');
const http = require('http');
const path = require('path');

const dev = path.join(__dirname, '..', 'tools', 'devserver');
const { configFields } = require(path.join(dev, 'config'));
const { makeHandler } = require(path.join(dev, 'handler'));

function mkState(over) {
  return {
    version: 'v9.9.9-7-gabcdef0',
    configFields: configFields({}),
    tempCriticalFallback: 85.0,
    scenario: 'normal',
    flaky: false,
    ...over,
  };
}

/* Run fn against a live server, always closing it afterwards. */
async function withServer(state, fn) {
  const server = http.createServer(makeHandler(state));

  await new Promise((r) => server.listen(0, '127.0.0.1', r));

  const base = `http://127.0.0.1:${server.address().port}`;

  try {
    await fn(base);
  } finally {
    await new Promise((r) => server.close(r));
  }
}

test('GET / substitutes the version placeholders', async () => {
  await withServer(mkState(), async (base) => {
    const res = await fetch(base + '/');
    const html = await res.text();

    assert.strictEqual(res.status, 200);
    assert.match(res.headers.get('content-type'), /text\/html/);
    assert.ok(html.includes('v9.9.9-7-gabcdef0')); /* {{VERSION}} */
    assert.ok(html.includes('tag/v9.9.9')); /* {{RELEASE}}: describe suffix stripped */
    assert.ok(!html.includes('{{VERSION}}') && !html.includes('{{RELEASE}}'));
  });
});

test('GET /index.html serves the dashboard too', async () => {
  await withServer(mkState(), async (base) => {
    const res = await fetch(base + '/index.html');

    assert.strictEqual(res.status, 200);
    assert.match(res.headers.get('content-type'), /text\/html/);
  });
});

test('GET /api/current returns the converted snapshot', async () => {
  await withServer(mkState(), async (base) => {
    const res = await fetch(base + '/api/current');
    const d = await res.json();

    assert.strictEqual(res.status, 200);
    assert.match(res.headers.get('content-type'), /application\/json/);
    assert.deepStrictEqual(d.thresh_cpu, [70, 90]);
    assert.strictEqual(d.title, 'minimoni');
  });
});

test('GET /api/metrics honours range and points', async () => {
  await withServer(mkState(), async (base) => {
    const d = await (await fetch(base + '/api/metrics?range=7d&points=50')).json();

    assert.strictEqual(d.range, '7d');
    assert.strictEqual(d.points.length, 50);

    const dflt = await (await fetch(base + '/api/metrics')).json();

    assert.strictEqual(dflt.range, '1d');
    assert.strictEqual(dflt.points.length, 240);
  });
});

test('static allowlist serves each file with its content type', async () => {
  await withServer(mkState(), async (base) => {
    for (const [f, ct] of [
      ['app.js', 'text/javascript'],
      ['chart.js', 'text/javascript'],
      ['style.css', 'text/css'],
      ['favicon.svg', 'image/svg+xml'],
    ]) {
      const res = await fetch(`${base}/${f}`);

      assert.strictEqual(res.status, 200, f);
      assert.strictEqual(res.headers.get('content-type'), ct, f);
      await res.arrayBuffer(); /* drain */
    }
  });
});

test('unknown paths, traversal and non-GET all 404', async () => {
  await withServer(mkState(), async (base) => {
    assert.strictEqual((await fetch(base + '/nope.txt')).status, 404);
    assert.strictEqual((await fetch(base + '/%2e%2e/index.js')).status, 404);
    assert.strictEqual((await fetch(base + '/', { method: 'POST' })).status, 404);
  });
});

test('GET /stream emits SSE frames', async () => {
  await withServer(mkState(), async (base) => {
    const res = await fetch(base + '/stream');

    assert.strictEqual(res.status, 200);
    assert.match(res.headers.get('content-type'), /text\/event-stream/);

    const reader = res.body.getReader();
    const { value } = await reader.read();

    assert.ok(Buffer.from(value).toString().startsWith('data: {'));
    await reader.cancel(); /* close the client side; the handler clears its timer */
  });
});

test('flaky mode drops every other /stream connection', async () => {
  await withServer(mkState({ flaky: true }), async (base) => {
    const first = await fetch(base + '/stream'); /* seq 0: served */
    const reader = first.body.getReader();
    const { value } = await reader.read();

    assert.ok(Buffer.from(value).toString().startsWith('data: {'));
    await reader.cancel();

    const second = await fetch(base + '/stream'); /* seq 1: dropped, no data */

    assert.strictEqual(await second.text(), '');
  });
});
