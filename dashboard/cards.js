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
/* exported updateCards, wireCards, replayCards, THRESH, cfgCardUnits, cfgChartUnits, cfgVisCharts, cfgRanges */

/* Fallback [warn, critical] card thresholds, used only until the first payload:
 * the server sends per-metric thresh_* in /api/current and updateCards overwrites these. */
const THRESH = {
  cpu:  [70, 90],
  mem:  [70, 90],
  swap: [80, 95],
  disk: [80, 90],
  load: [3, 4],    /* abs fallback = [0.75x4, 1.0x4], 4 cores assumed */
  temp: [70, 80],  /* degC fallback when no sysfs trip point */
};

/* Units read from server config; sensible defaults until first /current */
const cfgCardUnits = {
  mem: '%', swap: '%', swap_io: 'bytes', disk: '%', temp: 'c', net: 'kb', load: 'abs',
};
const cfgChartUnits = {
  mem: 'mb', swap: 'auto', swap_io: 'bytes', disk: 'gb', temp: 'c', net: 'kb', load: 'abs',
};
let cfgUptimeUnit = 'auto';
let cfgRanges     = ['1d', '7d', '30d', '90d'];
/* Three-state visibility: null = show all, [] = hide all, [...] = listed only */
let cfgVisCharts  = null;
let cfgVisCards   = null;

/* Which sub-metric is shown as the primary value in each card, as a 0-based
 * index into that card's series (load has three, the others two); toggled by
 * clicking a sub-value. */
const cardPrimary  = { load: 0, cpu: 0, mem: 0, swap: 0, swap_io: 0, disk: 0, net: 0 };
let lastCurrent  = null;    /* last /current snapshot; replayed on swapCard */

/* --- Card helpers --- */

function replayCards() {
  if (lastCurrent) updateCards(lastCurrent);
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

/* Render a numeric card from N values; shared by Load/CPU/Memory/Disk/Network,
 * which share the DOM shape (.clabel .card-unit / .cval / .csub
 * .card-sub[data-idx] .card-sub-val). Args:
 *   cardId:      DOM id of the .card ('c-load', 'c-cpu', ...)
 *   primaryKey:  key into cardPrimary ('load', 'cpu', 'mem', 'disk', 'net')
 *   values:      one numeric value per data-idx; null renders as the dash
 *   fmt:         formatter, only called with non-null values
 *   cardLvl:     'g'|'y'|'r'|'' for the overall .card class
 *   subLvl:      '' (none), a level string for every sub, or a
 *                function(value) -> level for per-value colouring. Optional */
function updateNumericCard(cardId, primaryKey, values, fmt, cardLvl, subLvl) {
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
    const valEl = sub.querySelector('.card-sub-val');
    sub.classList.toggle('hide', i === pi);
    sub.setAttribute('tabindex', i === pi ? '-1' : '0');
    valEl.textContent = v != null ? fmt(v) : '—';
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

  /* Title, footer, theme */
  if (d.title) {
    document.title = d.title;
    document.getElementById('ttl').textContent = d.title;
  }

  if (d.show_footer !== undefined) {
    const ftr = document.getElementById('ftr');
    ftr.classList.toggle('hide', !d.show_footer);
  }

  /* When theme is fixed server-side, apply it and hide the toggle button */
  if (d.theme && d.theme !== 'auto') {
    document.documentElement.dataset.theme = d.theme;
    document.getElementById('thm').classList.add('hide');
  }

  if (d.uptime_unit) cfgUptimeUnit = d.uptime_unit;

  /* Rebuild range tabs only when the server list differs from current */
  if (d.ranges && d.ranges.join(',') !== cfgRanges.join(',')) {
    cfgRanges = d.ranges;
    buildTabs();
  }

  /* Chart visibility & ordering */
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

      /* Hide when excluded; CSS order drives the configured sequence */
      el.classList.toggle('hide', cfgVisCharts !== null && idx === -1);
      el.style.order = (cfgVisCharts !== null && idx !== -1) ? idx : '';
    });
  }

  /* Card ordering: visibility is the per-field-presence pass below. This runs
   * on every tick the payload carries `d.cards`, and only sets flex `order`. */
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

  /* Per-card visibility pass: a card shows when it is not excluded by
   * cfgVisCards AND its primary field is present in the payload (null counts
   * as present). Cards start display:none in the HTML, so first paint shows
   * nothing until the first payload confirms which cards have data. */
  [
    ['c-load',    'cpu_load',  'load_1m'],
    ['c-cpu',     'cpu_usage', 'cpu_user_percent'],
    ['c-mem',     'memory',    'mem_available'],
    ['c-swap',    'swap',      'swap_free'],
    ['c-swap-io', 'swap_io',   'swap_in'],
    ['c-disk',    'disk',      'disk_percent'],
    ['c-temp',    'temp',      'temp'],
    ['c-net',     'net',       'net_rx'],
  ].forEach((c) => {
    const el = document.getElementById(c[0]);
    if (!el) return;

    const excluded = cfgVisCards !== null && cfgVisCards.indexOf(c[1]) === -1;
    const present  = c[2] in d;

    el.classList.toggle('hide', excluded || !present);
  });

  /* CPU Load (per-sub colour): each load value gets its own threshold level */
  if ('load_1m' in d) {
    const loadFmt = cfgCardUnits.load === '%'
      ? (v) => v.toFixed(1) + '%'
      : (v) => v.toFixed(2);

    updateNumericCard('c-load', 'load',
      [d.load_1m, d.load_5m, d.load_15m], loadFmt,
      cardLevel(d.load_1m, THRESH.load),
      (v) => cardLevel(v, THRESH.load));
  }

  /* CPU Usage: card and each sub coloured by its own value vs the cpu threshold */
  if ('cpu_user_percent' in d) {
    const pctFmt = (v) => v.toFixed(1) + '%';

    updateNumericCard('c-cpu', 'cpu',
      [d.cpu_user_percent, d.cpu_system_percent], pctFmt,
      cardLevel(d.cpu_user_percent, THRESH.cpu),
      (v) => cardLevel(v, THRESH.cpu));
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
      ? (v) => (v == null ? '—' : v.toFixed(1) + '%')
      : (v) => fmtUnit(v, srvUnits.swap_io);

    updateNumericCard('c-swap-io', 'swap_io', [d.swap_in, d.swap_out], swapIoFmt,
      pairLevel(d.swap_in, d.swap_out, THRESH.swap_io), (v) => cardLevel(v, THRESH.swap_io));
  }

  /* Disk: same shape as memory */
  if ('disk_percent' in d) {
    const diskFmt = cfgCardUnits.disk !== '%'
      ? (v) => fmtUnit(v, srvUnits.disk)
      : (v) => v.toFixed(1) + '%';

    const diskAuto = cfgCardUnits.disk !== '%';
    const diskV = diskAuto
      ? [d.disk_used, d.disk_free]
      : [d.disk_percent, d.disk_percent != null ? 100 - d.disk_percent : null];
    const diskLvl = cardLevel(d.disk_percent, THRESH.disk);

    updateNumericCard('c-disk', 'disk', diskV, diskFmt, diskLvl, diskLvl);
  }

  /* Temperature: shown whenever the server includes the temp field (it emits
   * it only when "temp" is configured). A null value (transient read failure)
   * still shows the card with the dash rather than hiding it. */
  if ('temp' in d && (cfgVisCards === null || cfgVisCards.indexOf('temp') !== -1)) {
    const tc      = document.getElementById('c-temp');
    const tempLvl = cardLevel(d.temp, THRESH.temp);

    tc.querySelector('.cval').textContent = fmtTempVal(d.temp, cfgCardUnits.temp);
    tc.className = 'card' + (tempLvl ? ' ' + tempLvl : '');
  }

  /* Network: netV[0]=tx, netV[1]=rx, matching the HTML card-sub order. Both
   * may be null on the first collect before a delta exists (rendered as the
   * dash). Values and thresholds stay in base bytes/s here; only the formatter
   * scales, so the semaphore compares like with like. */
  if ('net_rx' in d) {
    const netFmt = cfgCardUnits.net === '%'
      ? (v) => (v == null ? '—' : v.toFixed(1) + '%')
      : (v) => fmtUnit(v, srvUnits.net);

    updateNumericCard('c-net', 'net', [d.net_tx, d.net_rx], netFmt,
      pairLevel(d.net_tx, d.net_rx, THRESH.net), (v) => cardLevel(v, THRESH.net));
  }

  /* Uptime subtitle */
  if (d.uptime_seconds != null) {
    document.getElementById('upt').textContent = fmtUptime(d.uptime_seconds, cfgUptimeUnit);
  }
}
