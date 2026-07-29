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

/* HTTP request handler bound to an app state via makeHandler() (no module-global state). */

'use strict';

const fs = require('fs');
const path = require('path');

const { clampPointsParam, currentSnapshot, makePoints } = require('./mock-data');
const { toCurrent, toPoint } = require('./units');

const REPO_ROOT = path.join(__dirname, '..', '..');
const DASHBOARD_DIR = path.join(REPO_ROOT, 'dashboard');
const DASHBOARD = path.join(DASHBOARD_DIR, 'index.html');

const STATIC_FILES = {
  'app.js': 'text/javascript',
  'cards.js': 'text/javascript',
  'chart.js': 'text/javascript',
  'format.js': 'text/javascript',
  'hover.js': 'text/javascript',
  'style.css': 'text/css',
  'favicon.svg': 'image/svg+xml',
};

const NOOP_LOG = { debug() {}, info() {}, error() {} };

/* Flaky mode (--flaky): drop every other /stream connection so the SSE indicator
 * can be exercised (live -> reconnecting -> live). Deterministic by seq, off by
 * default. */
function streamShouldDrop(flaky, seq) {
  return flaky && seq % 2 === 1;
}

function send(res, code, ctype, body) {
  const buf = Buffer.isBuffer(body) ? body : Buffer.from(body);

  if (res.writableEnded)
    return;

  res.writeHead(code, {
    'Content-Type': ctype,
    'Content-Length': buf.length,
    'Access-Control-Allow-Origin': '*',
  });

  res.end(buf);
}

function makeHandler(state, log = NOOP_LOG) {
  let streamSeq = 0; /* per-server /stream connection counter */
  let staticBase = null; /* realpath(DASHBOARD_DIR): constant, resolved once on first static hit */

  /* converted mock metrics + real config fields */
  function current() {
    const raw = currentSnapshot(state.scenario);

    return {
      ...toCurrent(raw, state.configFields, state.tempCriticalFallback),
      ...state.configFields,
    };
  }

  function metrics(rangeValue, nPoints) {

    const points = makePoints(rangeValue, state.scenario, nPoints).map((p) =>
      toPoint(p, state.configFields, state.tempCriticalFallback)
    );

    return { range: rangeValue, points };
  }

  function serveDashboard(res) {
    let html;

    try {
      html = fs.readFileSync(DASHBOARD, 'utf8');
    } catch {
      log.error(`dashboard not found: ${DASHBOARD}`);

      send(res, 404, 'text/plain', 'dashboard not found');

      return;
    }

    /* Mirror bundle.sh: fill the {{VERSION}}/{{RELEASE}} placeholders (release
     * strips the git-describe "-N-gHASH" suffix so its link resolves). */
    const release = state.version.replace(/-\d+-g[0-9a-f]+$/, '');

    html = html.replace(/\{\{VERSION\}\}/g, state.version).replace(/\{\{RELEASE\}\}/g, release);

    send(res, 200, 'text/html; charset=utf-8', html);
  }

  function stream(req, res) {
    const seq = streamSeq++;

    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      'Access-Control-Allow-Origin': '*',
    });

    if (streamShouldDrop(state.flaky, seq)) {
      log.debug(`flaky: dropping /stream connection ${seq}`);
      res.end(); /* close with no data: the browser EventSource errors */

      return;
    }

    let n = 0;
    let timer = null;

    const tick = () => {
      if (res.writableEnded)
        return;

      res.write(`data: ${JSON.stringify(current())}\n\n`);

      n += 1;
      timer = n < 3 ? setTimeout(tick, 5000) : setTimeout(() => res.end(), 5000);
    };

    req.on('close', () => {
      if (timer)
        clearTimeout(timer);
    });

    tick();
  }

  /* Serve dashboard/ static files via a strict filename allowlist; the realpath +
   * startsWith containment is defence-in-depth and the path-injection sanitiser
   * CodeQL recognises. */
  function serveStatic(res, urlPath) {
    const filename = path.basename(urlPath);
    const ctype = STATIC_FILES[filename];

    if (ctype) {
      try {
        const fpath = fs.realpathSync(path.join(DASHBOARD_DIR, filename));
        const base = (staticBase ??= fs.realpathSync(DASHBOARD_DIR));

        if (fpath.startsWith(base + path.sep) && fs.statSync(fpath).isFile()) {
          send(res, 200, ctype, fs.readFileSync(fpath));

          return;
        }
      } catch {
        /* fall through to 404 */
      }
    }

    send(res, 404, 'text/plain', 'not found');
  }

  function route(req, res) {
    const url = new URL(req.url, 'http://localhost');
    const p = url.pathname;

    if (p === '/' || p === '/index.html') {
      serveDashboard(res);
    } else if (p === '/api/current') {
      send(res, 200, 'application/json', JSON.stringify(current()));
    } else if (p === '/api/metrics') {
      const r = url.searchParams.get('range') || '1d';
      const nPoints = clampPointsParam(url.searchParams.get('points') || '');

      send(res, 200, 'application/json', JSON.stringify(metrics(r, nPoints)));
    } else if (p === '/stream') {
      stream(req, res);
    } else {
      serveStatic(res, p);
    }
  }

  return function handler(req, res) {
    /* Swallow client-disconnect errors (Node's BrokenPipe/ConnectionReset). */
    req.on('error', () => {});
    res.on('error', () => {});

    if (req.method !== 'GET') {
      send(res, 404, 'text/plain', 'not found');

      return;
    }

    log.debug(`${req.socket.remoteAddress} GET ${req.url}`);

    try {
      route(req, res);
    } catch (err) {
      log.error(`error handling ${req.url}: ${err.stack || err}`);

      send(res, 500, 'text/plain', 'internal error');
    }
  };
}

module.exports = { makeHandler, streamShouldDrop };
