#!/usr/bin/env node
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

/* Mock HTTP server for iterating on dashboard/index.html without compiling.
 * Usage: node tools/devserver/ [port] [config] [--scenario S] [--flaky] [-v]
 * Metric values are mocked (mock-data.js); presentation settings come from a real
 * minimoni config (config.js); the HTTP serving lives in handler.js.
 * Serves dashboard/index.html at / and data at /api/current, /api/metrics, /stream. */

'use strict';

const http = require('http');
const util = require('util');
const { execFileSync } = require('child_process');

const {
  DEFAULT_CONFIG,
  configFields,
  dashboardNetMaxSpeed,
  dashboardTempCriticalFallback,
  loadDashboardConfig,
} = require('./config');

const { makeHandler } = require('./handler');

const SCENARIOS = ['normal', 'warn', 'critical', 'cycle'];

const USAGE =
  'usage: node tools/devserver/ [port] [config] ' +
  '[--scenario {normal,warn,critical,cycle}] [--flaky] [-v]';

function die(msg) {
  process.stderr.write(`${msg}\n`);
  process.exit(2);
}

function parseArgs(argv) {
  let parsed;

  try {
    parsed = util.parseArgs({
      args: argv,
      allowPositionals: true,
      options: {
        scenario: { type: 'string', default: 'cycle' },
        flaky: { type: 'boolean', default: false },
        verbose: { type: 'boolean', short: 'v', default: false },
        help: { type: 'boolean', short: 'h', default: false },
      },
    });
  } catch (err) {
    /* util.parseArgs messages are verbose; keep just the first sentence. */
    die(`${err.message.split('.')[0]}\n${USAGE}`);
  }

  const { values, positionals } = parsed;

  if (values.help) {
    process.stdout.write(`${USAGE}\n`);
    process.exit(0);
  }

  if (!SCENARIOS.includes(values.scenario))
    die(`invalid --scenario '${values.scenario}' (choose from ${SCENARIOS.join(', ')})`);

  const opts = {
    port: 9090,
    config: DEFAULT_CONFIG,
    scenario: values.scenario,
    flaky: values.flaky,
    verbose: values.verbose,
  };

  if (positionals[0] !== undefined) {
    const port = Number(positionals[0]);

    if (!Number.isInteger(port) || port < 0 || port > 65535)
      die(`invalid port: ${positionals[0]}`);

    opts.port = port;
  }

  if (positionals[1] !== undefined)
    opts.config = positionals[1];

  return opts;
}

/* Two-digit-padded local "YYYY-MM-DD HH:MM:SS" for log lines. */
function timestamp() {
  const d = new Date();
  const p = (n) => String(n).padStart(2, '0');

  return (
    `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ` +
    `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`
  );
}

function makeLog(verbose) {
  const emit = (level, msg) => process.stderr.write(`${timestamp()} ${level} ${msg}\n`);

  return {
    info: (m) => emit('INFO', m),
    error: (m) => emit('ERROR', m),
    debug: (m) => {
      if (verbose)
        emit('DEBUG', m);
    },
  };
}

function gitVersion() {
  try {
    return execFileSync('git', ['describe', '--tags', '--always'], {
      cwd: __dirname,
      stdio: ['ignore', 'pipe', 'ignore'],
    })
      .toString()
      .trim();
  } catch {
    return 'unknown';
  }
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const log = makeLog(opts.verbose);

  const dashboard = loadDashboardConfig(opts.config);
  const state = {
    version: gitVersion(),
    configFields: configFields(dashboard),
    tempCriticalFallback: dashboardTempCriticalFallback(dashboard),
    netMaxSpeed: dashboardNetMaxSpeed(dashboard),
    scenario: opts.scenario,
    flaky: opts.flaky,
  };

  log.info(`config loaded from ${opts.config} (scenario=${opts.scenario})`);

  const server = http.createServer(makeHandler(state, log));

  server.listen(opts.port, '127.0.0.1', () => {
    log.info(`minimoni dev server on http://localhost:${opts.port}`);
  });

  const shutdown = () => server.close(() => process.exit(0));

  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

main();
