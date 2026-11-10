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

/* Card subsystem: the numeric cards and the /api/current sink (updateCards, which
 * also applies server config: units, thresholds, visibility, title/theme/footer).
 * Owns the config state (written here, read by chart.js/app.js via globals).
 * bundle.sh inlines format.js, chart.js, this, then app.js (one shared scope). */
/* global fmtUptime, fmtTempVal, cardLevel, pairLevel, fmtUnit, usedOf, srvUnits */
/* global buildTabs */
/* exported updateCards, wireCards, replayCards, renderUptime, THRESH, cfgCardUnits */
/* exported cfgChartUnits, cfgTempScale, cfgVisCharts, cfgRanges */

/* Fallback [warn, critical] card thresholds, used only until the first payload:
 * the server sends per-metric thresh_* in /api/current and updateCards overwrites these. */
const THRESH = {
  cpu:  [70, 90],
  mem:  [70, 90],
  swap: [80, 95],
  disk: [80, 90],
  load: [3, 4],    /* abs fallback = [0.75x4, 1.0x4], 4 cores assumed */
  temp: [68, 76.5],  /* °C: 80 and 90 % of the default critical, 85 °C */
};

/* Units read from server config; the server's defaults until the first /api/current */
const cfgCardUnits = {
  mem: '%', swap: '%', swap_io: 'bytes', disk: '%', temp: 'degrees', net: 'bytes', load: 'abs',
};
const cfgChartUnits = {
  mem: 'auto', swap: 'auto', swap_io: 'bytes', disk: 'auto', temp: 'degrees', net: 'bytes',
  load: 'abs',
};
let cfgTempScale = 'celsius';
let cfgUptimeUnit = 'auto';
let cfgRanges     = ['1d', '7d', '30d', '90d'];
/* When the host booted, in the browser's clock, from the uptime the server sends on connecting */
let bootAt        = null;
/* Three-state visibility: null = show all, [] = hide all, [...] = listed only */
let cfgVisCharts  = null;
let cfgVisCards   = null;

/* Which sub-metric is shown as the primary value in each card, as a 0-based index into that
 * card's series; toggled by clicking a sub-value. */
const cardPrimary  = { load: 0, cpu: 0, mem: 0, swap: 0, swap_io: 0, disk: 0, net: 0 };
let lastCurrent  = null;    /* last /current snapshot; replayed on swapCard */

/* --- Card helpers --- */

/* Leave the uptime out: an old reading would set the count back. */
function replayCards() {
  if (!lastCurrent)
    return;

  const d = { ...lastCurrent };

  delete d.uptime_seconds;
  updateCards(d);
}

function swapCard(id, idx) {
  cardPrimary[id] = idx;
  replayCards();
}

/* Wire click + Enter/Space as a single "activation": shared by card sub-values
 * and legend items, keeping the two wiring sites free of repeated plumbing. */
function activate(el, handler) {
  el.addEventListener('click', handler);
  el.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      handler();
    }
  });
}

/* Wire click + keyboard handlers onto the pre-declared .card-sub elements.
 * tabindex="-1" on the primary (hidden) sub keeps it out of the tab order. */
function wireCards() {
  document.querySelectorAll('.card-sub[data-card]').forEach((el) => {
    const cardId = el.dataset.card;
    const idx    = parseInt(el.dataset.idx, 10);
    el.setAttribute('role', 'button');
    el.setAttribute('tabindex', el.classList.contains('hide') ? '-1' : '0');
    activate(el, () => { swapCard(cardId, idx); });
  });
}

/* Render a numeric card whose DOM has the shape
 * (.clabel .card-unit / .cval / .csub .card-sub[data-idx] .card-sub-val). Args:
 *   cardId:      DOM id of the .card ('c-load', 'c-cpu', ...)
 *   primaryKey:  key into cardPrimary
 *   values:      one numeric value per data-idx; null renders as the dash
 *   fmt:         formatter, only called with non-null values
 *   cardLvl:     'g'|'y'|'r'|'' for the overall .card class
 *   subLvl:      '' (none), a level string for every sub, or a function(value) -> level. Optional
 *   subFmt:      formatter for the subs; defaults to fmt */
function updateNumericCard(cardId, primaryKey, values, fmt, cardLvl, subLvl, subFmt) {
  const card = document.getElementById(cardId);
  if (!card) return;

  const pi = cardPrimary[primaryKey];
  card.className = 'card' + (cardLvl ? ' ' + cardLvl : '');
  card.querySelectorAll('.card-unit').forEach((u, i) => {
    u.classList.toggle('hide', i !== pi);
  });

  const pv = values[pi];
  card.querySelector('.cval').textContent = pv != null ? fmt(pv) : '—';

  values.forEach((v, i) => {
    const sub = card.querySelector('.card-sub[data-idx="' + i + '"]');
    if (!sub) return;

    sub.classList.toggle('hide', i === pi);
    sub.setAttribute('tabindex', i === pi ? '-1' : '0');

    const valEl = sub.querySelector('.card-sub-val');
    valEl.textContent = v != null ? (subFmt || fmt)(v) : '—';
    const lvl = typeof subLvl === 'function' ? subLvl(v) : (subLvl || '');
    valEl.className = 'card-sub-val' + (lvl ? ' ' + lvl : '');
  });
}

/* --- Update cards from current snapshot --- */

function updateCards(d) {
  if (!d) return;

  lastCurrent = d;

  if (d.mem_card_unit)       cfgCardUnits.mem      = d.mem_card_unit;
  if (d.mem_chart_unit)      cfgChartUnits.mem     = d.mem_chart_unit;
  if (d.swap_card_unit)      cfgCardUnits.swap     = d.swap_card_unit;
  if (d.swap_chart_unit)     cfgChartUnits.swap    = d.swap_chart_unit;
  if (d.swap_io_card_unit)   cfgCardUnits.swap_io  = d.swap_io_card_unit;
  if (d.swap_io_chart_unit)  cfgChartUnits.swap_io = d.swap_io_chart_unit;
  if (d.disk_card_unit)      cfgCardUnits.disk     = d.disk_card_unit;
  if (d.disk_chart_unit)     cfgChartUnits.disk    = d.disk_chart_unit;
  if (d.temp_card_unit)      cfgCardUnits.temp     = d.temp_card_unit;
  if (d.temp_chart_unit)     cfgChartUnits.temp    = d.temp_chart_unit;
  if (d.temp_scale)          cfgTempScale          = d.temp_scale;
  if (d.net_card_unit)       cfgCardUnits.net      = d.net_card_unit;
  if (d.net_chart_unit)      cfgChartUnits.net     = d.net_chart_unit;
  if (d.cpu_load_card_unit)  cfgCardUnits.load     = d.cpu_load_card_unit;
  if (d.cpu_load_chart_unit) cfgChartUnits.load    = d.cpu_load_chart_unit;

  if (d.thresh_load)    THRESH.load    = d.thresh_load;
  if (d.thresh_cpu)     THRESH.cpu     = d.thresh_cpu;
  if (d.thresh_mem)     THRESH.mem     = d.thresh_mem;
  if (d.thresh_swap)    THRESH.swap    = d.thresh_swap;
  if (d.thresh_swap_io) THRESH.swap_io = d.thresh_swap_io;
  if (d.thresh_disk)    THRESH.disk    = d.thresh_disk;
  if (d.thresh_temp)    THRESH.temp    = d.thresh_temp;
  if (d.thresh_net)     THRESH.net     = d.thresh_net;

  if (d.title) {
    document.title = d.title;
    document.getElementById('ttl').textContent = d.title;
  }

  if (d.show_footer !== undefined) {
    const ftr = document.getElementById('ftr');
    ftr.classList.toggle('hide', !d.show_footer);
  }

  if (d.theme && d.theme !== 'auto') {
    document.documentElement.dataset.theme = d.theme;
    document.getElementById('thm').classList.add('hide');
  }

  if (d.uptime_unit) cfgUptimeUnit = d.uptime_unit;

  if (d.ranges && d.ranges.join(',') !== cfgRanges.join(',')) {
    cfgRanges = d.ranges;
    buildTabs();
  }

  if (d.charts !== undefined) {
    cfgVisCharts = d.charts === null ? null : d.charts;

    const CHART_BOX = {
      cpu_load:  'b-load',
      cpu_usage: 'b-cpu',
      memory:    'b-mem',
      swap:      'b-swap',
      swap_io:   'b-swap-io',
      disk:      'b-disk',
      temp:      'b-temp',
      net:       'b-net',
    };

    Object.keys(CHART_BOX).forEach((nm) => {
      const el  = document.getElementById(CHART_BOX[nm]);
      if (!el) return;

      const idx = cfgVisCharts !== null ? cfgVisCharts.indexOf(nm) : -1;

      /* CSS order gives the configured sequence. */
      el.classList.toggle('hide', cfgVisCharts !== null && idx === -1);
      el.style.order = (cfgVisCharts !== null && idx !== -1) ? idx : '';
    });
  }

  /* Card order only; visibility is the pass below. */
  if (d.cards !== undefined) {
    cfgVisCards = d.cards === null ? null : d.cards;

    const CARD_ORDER = {
      cpu_load:  'c-load',
      cpu_usage: 'c-cpu',
      memory:    'c-mem',
      swap:      'c-swap',
      swap_io:   'c-swap-io',
      disk:      'c-disk',
      temp:      'c-temp',
      net:       'c-net',
      uptime:    'upt',
    };

    Object.keys(CARD_ORDER).forEach((nm) => {
      const el  = document.getElementById(CARD_ORDER[nm]);
      if (!el) return;

      const idx = cfgVisCards !== null ? cfgVisCards.indexOf(nm) : -1;

      el.style.order = (idx !== -1) ? idx : '';
    });
  }

  /* A null field counts as present. Cards start hidden in the HTML, so the first paint shows none
   * until a payload says which have data. */
  [
    ['c-load',    'cpu_load',  'load_1m'],
    ['c-cpu',     'cpu_usage', 'cpu_user_percent'],
    ['c-mem',     'memory',    'mem_available'],
    ['c-swap',    'swap',      'swap_free'],
    ['c-swap-io', 'swap_io',   'swap_in'],
    ['c-disk',    'disk',      'disk_available'],
    ['c-temp',    'temp',      'temp'],
    ['c-net',     'net',       'net_rx'],
  ].forEach((c) => {
    const el = document.getElementById(c[0]);
    if (!el) return;

    const excluded = cfgVisCards !== null && cfgVisCards.indexOf(c[1]) === -1;
    const present  = c[2] in d;

    el.classList.toggle('hide', excluded || !present);
  });

  /* CPU Load: the card takes the level of the value it shows, each sub its own. */
  if ('load_1m' in d) {
    const loads   = [d.load_1m, d.load_5m, d.load_15m];
    const loadFmt = cfgCardUnits.load === '%'
      ? (v) => v.toFixed(1) + '%'
      : (v) => v.toFixed(2);

    updateNumericCard('c-load', 'load', loads, loadFmt,
      cardLevel(loads[cardPrimary.load], THRESH.load),
      (v) => cardLevel(v, THRESH.load));
  }

  /* CPU Usage: the card takes the level of the value it shows, each sub its own. */
  if ('cpu_user_percent' in d) {
    const pctFmt = (v) => v.toFixed(1) + '%';
    const parts = [d.cpu_user_percent, d.cpu_system_percent, d.cpu_steal_percent];
    const total = parts.every((v) => v != null) ? parts[0] + parts[1] + parts[2] : null;
    const cpu = [total, ...parts];

    updateNumericCard('c-cpu', 'cpu', cpu, pctFmt,
      cardLevel(cpu[cardPrimary.cpu], THRESH.cpu),
      (v) => cardLevel(v, THRESH.cpu), (v) => v.toFixed(1));
  }

  /* Memory: used and available, both coloured by the level of the used share. The server sends
   * mem_available in the card unit, % or MB, plus mem_total in MB under "auto"; the used part is
   * computed here. */
  if ('mem_available' in d) {
    const memAuto = cfgCardUnits.mem !== '%';
    const memFmt = memAuto
      ? (v) => fmtUnit(v, srvUnits.mem)
      : (v) => v.toFixed(1) + '%';

    const memTotal = memAuto ? d.mem_total : 100;
    const memUsed = usedOf(memTotal, d.mem_available);
    const memUsedPct = memUsed != null && memTotal > 0 ? (memUsed * 100) / memTotal : null;
    const memLvl = cardLevel(memUsedPct, THRESH.mem);

    updateNumericCard('c-mem', 'mem', [memUsed, d.mem_available], memFmt, memLvl, memLvl);
  }

  /* Swap: like memory, from swap_free and swap_total. */
  if ('swap_free' in d) {
    const swapAuto = cfgCardUnits.swap !== '%';
    const swapFmt = swapAuto
      ? (v) => fmtUnit(v, srvUnits.swap)
      : (v) => v.toFixed(1) + '%';

    const swapTotal = swapAuto ? d.swap_total : 100;
    const swapUsed = usedOf(swapTotal, d.swap_free);
    const swapUsedPct = swapUsed != null && swapTotal > 0 ? (swapUsed * 100) / swapTotal : null;
    const swapLvl = cardLevel(swapUsedPct, THRESH.swap);

    updateNumericCard('c-swap', 'swap', [swapUsed, d.swap_free], swapFmt, swapLvl, swapLvl);
  }

  /* Swap I/O: in and out like the network, the busier one sets the level. Values stay in base
   * bytes/s, or % of the RAM per minute; only the formatter scales. */
  if ('swap_in' in d) {
    const swapIoFmt = cfgCardUnits.swap_io === 'ram'
      ? (v) => v.toFixed(1) + '%'
      : (v) => fmtUnit(v, srvUnits.swap_io);

    updateNumericCard('c-swap-io', 'swap_io', [d.swap_in, d.swap_out], swapIoFmt,
      pairLevel(d.swap_in, d.swap_out, THRESH.swap_io), (v) => cardLevel(v, THRESH.swap_io));
  }

  /* Disk: like memory, from disk_available and disk_total. */
  if ('disk_available' in d) {
    const diskAuto = cfgCardUnits.disk !== '%';
    const diskFmt = diskAuto
      ? (v) => fmtUnit(v, srvUnits.disk)
      : (v) => v.toFixed(1) + '%';

    const diskTotal = diskAuto ? d.disk_total : 100;
    const diskUsed = usedOf(diskTotal, d.disk_available);
    const diskUsedPct = diskUsed != null && diskTotal > 0 ? (diskUsed * 100) / diskTotal : null;
    const diskLvl = cardLevel(diskUsedPct, THRESH.disk);

    updateNumericCard('c-disk', 'disk', [diskUsed, d.disk_available], diskFmt, diskLvl, diskLvl);
  }

  /* Temperature: null (no reading) shows the dash. */
  if ('temp' in d && (cfgVisCards === null || cfgVisCards.indexOf('temp') !== -1)) {
    const tc      = document.getElementById('c-temp');
    const tempLvl = cardLevel(d.temp, THRESH.temp);

    tc.querySelector('.cval').textContent = fmtTempVal(d.temp, cfgCardUnits.temp, cfgTempScale);
    tc.className = 'card' + (tempLvl ? ' ' + tempLvl : '');
  }

  /* Network: tx first, as in the HTML. Both are null without a rate. Values and thresholds share
   * one unit, bytes/s or %; only the formatter scales. */
  if ('net_rx' in d) {
    const netFmt = cfgCardUnits.net === '%'
      ? (v) => v.toFixed(1) + '%'
      : (v) => fmtUnit(v, srvUnits.net);

    updateNumericCard('c-net', 'net', [d.net_tx, d.net_rx], netFmt,
      pairLevel(d.net_tx, d.net_rx, THRESH.net), (v) => cardLevel(v, THRESH.net));
  }

  if ('uptime_seconds' in d) {
    bootAt = d.uptime_seconds == null ? null : Date.now() - d.uptime_seconds * 1000;
    renderUptime();
  }
}

/* Show the uptime from bootAt and the browser's clock; with none read, the text stays as it is. */
function renderUptime() {
  if (bootAt == null)
    return;

  const seconds = (Date.now() - bootAt) / 1000;

  document.getElementById('upt').textContent = fmtUptime(seconds, cfgUptimeUnit);
}
