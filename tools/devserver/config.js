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

/* Loads the [dashboard] and [collect] tables of a real minimoni config for /api/current
 * (unset keys use src/config.c defaults). Node has no TOML parser, so parseTable() is
 * a deliberate subset reader, not a general one. */

'use strict';

const fs = require('fs');
const path = require('path');

const DEFAULT_CONFIG = path.join(__dirname, '..', '..', 'config.example.toml');

/* Defaults mirror config_defaults() in src/config.c. */
const CONFIG_DEFAULTS = {
  title: 'minimoni',
  theme: 'auto',
  show_footer: true,
  memory_card_unit: '%',
  memory_chart_unit: 'auto',
  swap_card_unit: '%',
  swap_chart_unit: 'auto',
  swap_io_card_unit: 'bytes',
  swap_io_chart_unit: 'bytes',
  disk_card_unit: '%',
  disk_chart_unit: 'auto',
  temp_card_unit: 'degrees',
  temp_chart_unit: 'degrees',
  cpu_load_card_unit: 'abs',
  cpu_load_chart_unit: 'abs',
  net_card_unit: 'bytes',
  net_chart_unit: 'bytes',
  uptime_unit: 'auto',
  ranges: ['1d', '7d', '30d', '90d'],
};

/* Drop a trailing '#' comment, respecting a double-quoted string. */
function stripComment(line) {
  let inStr = false;

  for (let i = 0; i < line.length; i++) {
    const c = line[i];

    if (c === '"' && line[i - 1] !== '\\')
      inStr = !inStr;
    else if (c === '#' && !inStr)
      return line.slice(0, i);
  }

  return line;
}

/* Read a double-quoted basic string, unescaping '\<c>' to '<c>'. */
function parseString(s) {
  let out = '';

  for (let i = 1; i < s.length; i++) {
    const c = s[i];

    if (c === '\\' && i + 1 < s.length) {
      out += s[++i];

      continue;
    }

    if (c === '"')
      break;

    out += c;
  }

  return out;
}

/* Extract the quoted strings of an array literal (dashboard arrays are always
 * arrays of strings: ranges / charts / cards). '[]' yields an empty array. */
function parseArray(s) {
  const items = [];
  const re = /"(?:[^"\\]|\\.)*"/g;

  let m;

  while ((m = re.exec(s)) !== null)
    items.push(parseString(m[0]));

  return items;
}

function parseValue(s) {
  s = s.trim();

  if (!s)
    return undefined;

  if (s[0] === '"')
    return parseString(s);

  if (s === 'true')
    return true;

  if (s === 'false')
    return false;

  if (s[0] === '[')
    return parseArray(s);

  const n = Number(s);

  return Number.isNaN(n) ? undefined : n;
}

/* Parse the table [name] into a plain object (see the file header). */
function parseTable(text, name) {
  const out = {};
  const lines = text.split(/\r?\n/);

  let inTable = false;

  for (let i = 0; i < lines.length; i++) {
    const line = stripComment(lines[i]).trim();

    if (!line)
      continue;

    if (line[0] === '[') {
      inTable = line === `[${name}]`;

      continue;
    }

    if (!inTable)
      continue;

    const eq = line.indexOf('=');

    if (eq < 0)
      continue;

    const key = line.slice(0, eq).trim();

    let valStr = line.slice(eq + 1).trim();

    /* Accumulate a multi-line array until its brackets balance. */
    if (valStr[0] === '[') {
      const open = (str) => (str.match(/\[/g) || []).length;
      const close = (str) => (str.match(/\]/g) || []).length;

      while (open(valStr) > close(valStr) && i + 1 < lines.length)
        valStr += ` ${stripComment(lines[++i]).trim()}`;
    }

    const val = parseValue(valStr);

    if (val !== undefined)
      out[key] = val;
  }

  return out;
}

const parseDashboard = (text) => parseTable(text, 'dashboard');

/* The critical trip point set in [collect], given in temp_scale and returned in °C; null when unset
 * or not above 0 °C, as config.c ignores it then. */
function collectTempCritical(c) {
  const v = Number('temp_critical' in c ? c.temp_critical : NaN);
  const celsius = c.temp_scale === 'fahrenheit' ? ((v - 32) * 5) / 9 : v;

  return Number.isFinite(celsius) && celsius > 0 ? celsius : null;
}

/* Net percent reference in Mbit/s, from [collect]; 0 means unset, so the detected link is used. */
function collectNetMaxSpeed(c) {
  const v = Number('net_max_speed' in c ? c.net_max_speed : 0);

  return Number.isFinite(v) && v > 0 ? v : 0;
}

function loadConfig(filepath) {
  let text;

  try {
    text = fs.readFileSync(filepath, 'utf8');
  } catch (err) {
    if (err.code === 'ENOENT')
      console.error(`config not found: ${filepath}`);
    else
      console.error(`cannot read config ${filepath}: ${err.message}`);

    process.exit(1);
  }

  return { dashboard: parseDashboard(text), collect: parseTable(text, 'collect') };
}

/* Mirrors src/http.c /api/current: config keys -> response fields, including
 * the memory_* -> mem_* rename. charts/cards absent => null (show all). `c` is the [collect] table,
 * for temp_scale. */
function configFields(d, c = {}) {
  const g = (key) => (key in d ? d[key] : CONFIG_DEFAULTS[key]);

  return {
    mem_card_unit: g('memory_card_unit'),
    mem_chart_unit: g('memory_chart_unit'),
    swap_card_unit: g('swap_card_unit'),
    swap_chart_unit: g('swap_chart_unit'),
    swap_io_card_unit: g('swap_io_card_unit'),
    swap_io_chart_unit: g('swap_io_chart_unit'),
    disk_card_unit: g('disk_card_unit'),
    disk_chart_unit: g('disk_chart_unit'),
    temp_card_unit: g('temp_card_unit'),
    temp_chart_unit: g('temp_chart_unit'),
    temp_scale: 'temp_scale' in c ? c.temp_scale : 'celsius',
    net_card_unit: g('net_card_unit'),
    net_chart_unit: g('net_chart_unit'),
    cpu_load_card_unit: g('cpu_load_card_unit'),
    cpu_load_chart_unit: g('cpu_load_chart_unit'),
    title: g('title'),
    theme: g('theme'),
    show_footer: g('show_footer'),
    uptime_unit: g('uptime_unit'),
    ranges: g('ranges'),
    charts: 'charts' in d ? d.charts : null,
    cards: 'cards' in d ? d.cards : null,
  };
}

module.exports = {
  DEFAULT_CONFIG,
  CONFIG_DEFAULTS,
  parseTable,
  parseDashboard,
  collectTempCritical,
  collectNetMaxSpeed,
  loadConfig,
  configFields,
};
