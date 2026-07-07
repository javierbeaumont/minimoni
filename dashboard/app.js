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

/* --- Thresholds & colors --- */

/* [warn, critical] boundaries for colour-coding card values. The server
 * computes these per metric (adapting to core count, the thermal trip point
 * and the configured units) and sends them as thresh_* in /api/current;
 * updateCards overwrites the entries below. The literals here are only the
 * fallback used before the first payload arrives. */
const THRESH = {
  cpu:  [70, 90],
  mem:  [70, 90],
  disk: [80, 90],
  load: [3, 4],    /* abs fallback = [0.75x4, 1.0x4], 4 cores assumed */
  temp: [70, 80],  /* degC fallback when no sysfs trip point */
};

/* cssCache is declared above CLR so the CLR initialiser can use cssv(). */
let cssCache = {};

/* Chart series colours: sourced from CSS custom properties so a customiser
 * only needs to edit style.css (or override the --clr-* vars). cssv() is a
 * function declaration and is therefore hoisted above this point. */
const CLR = {
  load1:    cssv('--clr-load1'),
  load5:    cssv('--clr-load5'),
  load15:   cssv('--clr-load15'),
  user:     cssv('--clr-cpu-user'),
  sys:      cssv('--clr-cpu-sys'),
  mem:      cssv('--clr-mem-used'),
  memAvail: cssv('--clr-mem-avail'),
  disk:     cssv('--clr-disk-used'),
  diskFree: cssv('--clr-disk-free'),
  temp:     cssv('--clr-temp'),
  rx:       cssv('--clr-net-rx'),
  tx:       cssv('--clr-net-tx'),
};

/* --- State --- */

let curRange     = '1d';    /* currently selected time range */
let pts          = [];      /* array of data points from /api/metrics */
let tempCritical = null;    /* sysfs trip-point; drawn as a red dashed line */
/* Which sub-metric is shown as the primary value in each card, as a 0-based
 * index into that card's series (load has three, the others two); toggled by
 * clicking a sub-value. */
const cardPrimary  = { load: 0, cpu: 0, mem: 0, disk: 0, net: 0 };
let lastCurrent  = null;    /* last /current snapshot; replayed on swapCard */

/* Units read from server config; sensible defaults until first /current */
const cfgCardUnits  = { mem: '%',  disk: '%',  temp: 'c', net: 'kb', load: 'abs' };
const cfgChartUnits = { mem: 'mb', disk: 'gb', temp: 'c', net: 'kb', load: 'abs' };
let cfgUptimeUnit = 'auto';
let cfgRanges     = ['1d', '7d', '30d', '90d'];
/* Three-state visibility: null = show all, [] = hide all, [...] = listed only */
let cfgVisCharts  = null;
let cfgVisCards   = null;

/* Cache of the clean args last passed to drawChart per canvas id, so a hover
 * redraw re-renders the chart plus the crosshair without recomputing them. */
const chartCache = {};

/* Per-chart, per-series hidden state; toggled by clicking a legend item */
const seriesHidden = {
  'g-load': [false, false, false],
  'g-cpu':  [false, false],
  'g-mem':  [false, false],
  'g-disk': [false, false],
  'g-net':  [false, false],
};

/* --- CSS variable helper --- */

function cssv(v) {
  /* CLR reads these at load time; the test harness requires this file under
   * node (no document), so degrade to an empty string there. */
  if (typeof document === 'undefined') return '';
  if (cssCache[v] === undefined)
    cssCache[v] = getComputedStyle(document.documentElement).getPropertyValue(v).trim();
  return cssCache[v];
}

function invalidateCssCache() { cssCache = {}; }

/* "Nice" number for axis scaling: round x to 1, 2, 5 or 10 x10^n. round picks the
 * nearest such value; otherwise the smallest one that is >= x. */
function niceNum(x, round) {
  const exp = Math.floor(Math.log10(x));
  const f   = x / Math.pow(10, exp);
  const nf  = round
    ? (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10)
    : (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10);
  return nf * Math.pow(10, exp);
}

/* Y-axis ticks from 0 to a rounded bound, stepped by a nice value, aiming for
 * about maxTicks intervals. niceTicks(1883, 5) -> [0, 500, 1000, 1500, 2000], so
 * labels land on round numbers instead of the raw data max. */
function niceTicks(max, maxTicks) {
  if (!(max > 0)) return [0, 1];
  const step = niceNum(niceNum(max, false) / Math.max(1, maxTicks - 1), true);
  const top  = Math.ceil(max / step) * step;
  const out  = [];
  for (let i = 0; i * step <= top + step * 1e-9; i++) out.push(i * step);
  return out;
}

/* Time-axis steps in seconds: sub-minute, minute, hour and day intervals that
 * read as round clock values (1/2/5/10/15/30 of each unit, plus 4h and 2w). */
const TIME_STEPS = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800,
                    3600, 7200, 10800, 14400, 21600, 43200,
                    86400, 172800, 604800, 1209600];

/* X-axis ticks: round timestamps aligned to local midnight at a nice interval,
 * so labels land on 00:00, 04:00, ... instead of raw sample times. Returns the
 * tick times within [t0, t1], aiming for about maxTicks of them. */
function timeTicks(t0, t1, maxTicks) {
  if (!(t1 > t0)) return [t0];
  const rough = (t1 - t0) / Math.max(1, maxTicks);
  let step = TIME_STEPS[TIME_STEPS.length - 1];
  for (let i = 0; i < TIME_STEPS.length; i++) {
    if (TIME_STEPS[i] >= rough) { step = TIME_STEPS[i]; break; }
  }
  const d = new Date(t0 * 1000);
  d.setHours(0, 0, 0, 0);              /* local midnight at or before t0 */
  let t = Math.floor(d.getTime() / 1000);
  while (t < t0) t += step;
  const out = [];
  for (; t <= t1 && out.length < 200; t += step) out.push(t);
  return out;
}

/* --- Canvas chart --- */

function drawChart(id, series, opts) {
  opts = opts || {};
  /* Cache the clean args for hover redraws (skip when this IS a hover redraw) */
  if (opts.hoverIdx == null) chartCache[id] = { series: series, opts: opts };
  const cv = document.getElementById(id);
  if (!cv) return;

  /* Scale the canvas backing store to the device pixel ratio so the chart
   * looks sharp on retina / HiDPI screens. Height is CSS-driven (read via
   * clientHeight); JS only sets the width to match the panel. */
  const dpr = devicePixelRatio || 1;
  const w   = cv.parentElement.clientWidth - 24;
  const h   = cv.clientHeight || 160;
  cv.width        = w * dpr;
  cv.height       = h * dpr;
  cv.style.width  = w + 'px';

  const ctx = cv.getContext('2d');
  ctx.scale(dpr, dpr);

  /* Chart padding: Top, Right, Bottom, Left */
  const Pt = 6, Pr = 8, Pb = 20, Pl = 38;
  const cw = w - Pl - Pr;  /* drawable width */
  const ch = h - Pt - Pb;  /* drawable height */

  const yMn = opts.yMin != null ? opts.yMin : 0;
  let dataMax = opts.yMax != null ? opts.yMax : 0;
  if (opts.yMax == null) {
    /* Auto-scale: the largest value across all series drives the axis bound. */
    series.forEach(function(s) {
      s.v.forEach(function(v) {
        if (v != null && v > dataMax) dataMax = v;
      });
    });
  }
  /* Round the axis to nice values (0, 500, 1000, ...), one label per gridline. */
  const yticks    = niceTicks(dataMax, 5);
  const yMx       = yticks[yticks.length - 1];
  const yStep     = yticks.length > 1 ? yticks[1] - yticks[0] : yMx || 1;
  const yDecimals = Math.max(0, -Math.floor(Math.log10(yStep)));
  const yr        = yMx - yMn || 1;
  const n  = (series[0] ? series[0].v.length : 0) || 1;

  /* Map a data-space index/value to canvas pixel coordinates */
  const tx = function(i) { return Pl + (i / Math.max(n - 1, 1)) * cw; };
  const ty = function(v) { return Pt + (1 - (v - yMn) / yr) * ch; };

  const ts   = opts.ts || [];
  const span = ts.length > 1 ? ts[ts.length - 1] - ts[0] : 0;
  /* How many X-axis labels fit (~40 px each, 12 max to avoid clutter). Capped to
   * ts.length so few-point charts do not stack duplicates at the same pixel. */
  const maxLbls  = Math.min(12, Math.max(3, Math.floor(cw / 40)));
  const lblCount = Math.min(maxLbls, ts.length);

  /* Horizontal grid lines with a Y-axis label per nice tick. */
  ctx.strokeStyle = cssv('--brd');
  ctx.lineWidth   = 0.5;
  ctx.fillStyle   = cssv('--mut');
  ctx.font        = '10px system-ui';
  ctx.textAlign   = 'right';
  yticks.forEach(function(tv) {
    const gy = ty(tv);
    ctx.beginPath();
    ctx.moveTo(Pl, gy);
    ctx.lineTo(Pl + cw, gy);
    ctx.stroke();
    ctx.fillText(fmtY(tv, opts.unit, yDecimals), Pl - 3, gy + 3);
  });

  /* Vertical grid lines + X-axis labels at nice, round timestamps (00:00, 04:00,
   * ...) positioned by time within the range, not at raw sample indices. */
  if (ts.length > 1 && span > 0) {
    const t0 = ts[0], t1 = ts[ts.length - 1];
    timeTicks(t0, t1, lblCount).forEach(function(tk) {
      const px = Pl + (tk - t0) / (t1 - t0) * cw;
      ctx.strokeStyle = cssv('--brd');
      ctx.beginPath();
      ctx.moveTo(px, Pt);
      ctx.lineTo(px, Pt + ch);
      ctx.stroke();
      ctx.textAlign = px < Pl + 18 ? 'left' : px > Pl + cw - 18 ? 'right' : 'center';
      ctx.fillText(fmtX(tk, span), px, h - 4);
    });
  }

  /* Series: stroke only (no fill). `ok` resets on null entries so gaps in the
   * data render as breaks instead of straight lines. */
  series.forEach(function(s) {
    if (!s.v.length) return;
    ctx.strokeStyle = s.c;
    ctx.lineWidth   = 1.5;
    ctx.beginPath();
    let ok = false;
    s.v.forEach(function(v, i) {
      if (v == null) { ok = false; return; }
      if (!ok) { ctx.moveTo(tx(i), ty(v)); ok = true; }
      else     { ctx.lineTo(tx(i), ty(v)); }
    });
    ctx.stroke();
  });

  /* Reference lines: explicit opts.refLines (e.g. the sysfs temp trip point,
   * dashed) plus opts.thresh-derived thin solid warn/crit lines, drawn only
   * when the data actually crossed them. */
  const refLines = (opts.refLines || []).slice();
  if (opts.thresh) {
    let thMax = -Infinity;
    series.forEach(function(s) {
      s.v.forEach(function(v) {
        if (v != null && v > thMax) thMax = v;
      });
    });
    if (thMax >= opts.thresh[0])
      refLines.push({ v: opts.thresh[0], c: cssv('--ylw'), dashed: false });
    if (thMax >= opts.thresh[1])
      refLines.push({ v: opts.thresh[1], c: cssv('--red'), dashed: false });
  }
  refLines.forEach(function(rl) {
    const ry = ty(rl.v);
    if (ry < Pt || ry > Pt + ch) return;  /* out of visible range */
    ctx.save();
    ctx.strokeStyle = rl.c || cssv('--red');
    if (rl.dashed === false) {
      ctx.lineWidth = 0.5;
    } else {
      ctx.lineWidth = 1;
      ctx.setLineDash([4, 4]);
    }
    ctx.beginPath();
    ctx.moveTo(Pl, ry);
    ctx.lineTo(Pl + cw, ry);
    ctx.stroke();
    ctx.restore();
  });

  /* Hover crosshair + series dots, on top. The only visual difference between
   * hover and locked: hover is dashed, locked is solid. */
  if (opts.hoverIdx != null && opts.hoverIdx >= 0 && opts.hoverIdx < n) {
    const hi = opts.hoverIdx;
    const hx = tx(hi);
    ctx.save();
    ctx.strokeStyle = cssv('--mut');
    ctx.lineWidth   = 0.5;
    if (!opts.hoverLocked) ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(hx, Pt);
    ctx.lineTo(hx, Pt + ch);
    ctx.stroke();
    ctx.setLineDash([]);
    series.forEach(function(s) {
      const v = s.v[hi];
      if (v == null) return;
      ctx.beginPath();
      ctx.arc(hx, ty(v), 3, 0, Math.PI * 2);
      ctx.fillStyle = s.c;
      ctx.fill();
    });
    ctx.restore();
  }
}

/* --- Format helpers --- */

/* Format a Y-axis tick label according to the chart unit */
function fmtY(v, u, decimals) {
  const d = decimals != null ? decimals : (v < 10 ? 1 : 0);
  if (u === '%')              return v.toFixed(d) + '%';
  if (u === 'C' || u === 'F') return v.toFixed(d) + '°';
  return v.toFixed(d);
}

/* Tooltip formatter: one decimal finer than fmtY for crosshair reads (axis
 * labels stay readable at low precision; tooltips give the detail). */
function fmtTip(v, u) {
  if (u === '%')              return v.toFixed(1) + '%';
  if (u === 'C' || u === 'F') return v.toFixed(1) + '°';
  return v < 10 ? v.toFixed(2) : v.toFixed(1);
}

/* Format an X-axis label from the data span (seconds): <= 1h -> MM:SS,
 * <= 2d -> HH:MM, <= 60d -> Mon DD, < 2y -> Mon 'YY, else YYYY. Boundaries
 * are inclusive, so a 1d range (span = 86400 s exactly) lands in HH:MM,
 * not Mon DD. */
function fmtX(t, span) {
  if (!t) return '';
  const d  = new Date(t * 1000);
  const mo = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun',
            'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
  if (span <= 3600)
    return d.getMinutes().toString().padStart(2, '0') + ':' +
           d.getSeconds().toString().padStart(2, '0');
  if (span <= 86400 * 2)
    return d.getHours().toString().padStart(2, '0') + ':' +
           d.getMinutes().toString().padStart(2, '0');
  if (span <= 86400 * 60)
    return mo[d.getMonth()] + ' ' + d.getDate();
  if (span < 86400 * 730)
    return mo[d.getMonth()] + " '" + String(d.getFullYear()).slice(2);
  return String(d.getFullYear());
}

/* Timestamp for the hover tooltip, fuller than the fmtX axis labels:
 *   span <= 1h  -> HH:MM:SS
 *   span <= 2d  -> Mon DD HH:MM
 *   else        -> Mon DD, YYYY */
function fmtXFull(t, span) {
  if (!t) return '';
  const d  = new Date(t * 1000);
  const mo = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun',
            'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
  const hh = d.getHours().toString().padStart(2, '0');
  const mm = d.getMinutes().toString().padStart(2, '0');
  const ss = d.getSeconds().toString().padStart(2, '0');
  if (span <= 3600)      return hh + ':' + mm + ':' + ss;
  if (span <= 86400 * 2) return mo[d.getMonth()] + ' ' + d.getDate() + ' ' + hh + ':' + mm;
  return mo[d.getMonth()] + ' ' + d.getDate() + ', ' + d.getFullYear();
}

/* Format uptime according to the configured unit (auto picks the
 * most readable granularity: days -> hours -> minutes) */
function fmtUptime(s) {
  if (cfgUptimeUnit === 'd') return 'up ' + (s / 86400).toFixed(1) + 'd';
  if (cfgUptimeUnit === 'h') return 'up ' + Math.floor(s / 3600) + 'h';
  const d = Math.floor(s / 86400);
  const h = Math.floor(s % 86400 / 3600);
  const m = Math.floor(s % 3600 / 60);
  if (d > 0) return 'up ' + d + 'd ' + h + 'h';
  if (h > 0) return 'up ' + h + 'h ' + m + 'm';
  return 'up ' + m + 'm';
}

/* Format a network throughput value into the configured display unit.
 * The display unit NEVER changes: if the user picked KB/s they always see
 * KB/s, whether the value is 0.001 or 99999. Precision adapts to keep
 * 4 significant digits at every magnitude:
 *     0.000 -     9.999  -> 3 decimals  ("0.001 KB/s" / "9.999 KB/s")
 *    10.00  -    99.99   -> 2 decimals  ("10.00 KB/s" / "99.99 KB/s")
 *   100.0   -   999.9    -> 1 decimal   ("100.0 KB/s" / "999.9 KB/s")
 *  1000     - 99999      -> 0 decimals  ("1000 KB/s"  / "99999 KB/s") */
function fmtNet(v, unit) {
  if (v == null) return '—';
  let s;
  if      (v >= 1000) s = v.toFixed(0);
  else if (v >= 100)  s = v.toFixed(1);
  else if (v >= 10)   s = v.toFixed(2);
  else                s = v.toFixed(3);
  if (!unit || unit === 'mb') return s + ' MB/s';
  if (unit === 'kb')          return s + ' KB/s';
  if (unit === 'gb')          return s + ' GB/s';
  if (unit === 'kbps')        return s + ' Kbps';
  if (unit === 'mbps')        return s + ' Mbps';
  if (unit === 'gbps')        return s + ' Gbps';
  return s;
}

/* Format a temperature value in the configured unit (C, F, or %) */
function fmtTempVal(v, unit) {
  if (v == null) return '—';
  if (!unit || unit[0] === 'c') return v.toFixed(1) + '°C';
  if (unit[0] === 'f')          return v.toFixed(1) + '°F';
  return v.toFixed(1) + '%';
}

/* --- Card helpers --- */

/* Return 'g', 'y', or 'r' (good / warning / critical) for a value */
function cardLevel(v, thresh) {
  if (v == null) return '';
  return v >= thresh[1] ? 'r' : v >= thresh[0] ? 'y' : 'g';
}

/* Swap which sub-metric (by 0-based index) is shown as primary in a card.
 * Replays lastCurrent so the change is visible immediately. */
function swapCard(id, idx) {
  cardPrimary[id] = idx;
  if (lastCurrent) updateCards(lastCurrent);
}

/* Wire click + Enter/Space as a single "activation": shared by card sub-values
 * and legend items, keeping the two wiring sites free of repeated plumbing. */
function activate(el, handler) {
  el.addEventListener('click', handler);
  el.addEventListener('keydown', function(e) {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      handler();
    }
  });
}

/* Wire click + keyboard handlers onto the pre-declared .card-sub elements.
 * tabindex="-1" on the primary (hidden) sub keeps it out of the tab order. */
function wireCards() {
  document.querySelectorAll('.card-sub[data-card]').forEach(function(el) {
    const cardId = el.dataset.card;
    const idx    = parseInt(el.dataset.idx, 10);
    el.setAttribute('role', 'button');
    el.setAttribute('tabindex', el.classList.contains('hide') ? '-1' : '0');
    activate(el, function() { swapCard(cardId, idx); });
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
  card.querySelectorAll('.card-unit').forEach(function(u, i) {
    u.classList.toggle('hide', i !== pi);
  });
  const pv = values[pi];
  card.querySelector('.cval').textContent = pv != null ? fmt(pv) : '—';
  values.forEach(function(v, i) {
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

  /* Overwrite default units with the values from the server config */
  if (d.mem_card_unit)       cfgCardUnits.mem   = d.mem_card_unit;
  if (d.mem_chart_unit)      cfgChartUnits.mem  = d.mem_chart_unit;
  if (d.disk_card_unit)      cfgCardUnits.disk  = d.disk_card_unit;
  if (d.disk_chart_unit)     cfgChartUnits.disk = d.disk_chart_unit;
  if (d.temp_card_unit)      cfgCardUnits.temp  = d.temp_card_unit;
  if (d.temp_chart_unit)     cfgChartUnits.temp = d.temp_chart_unit;
  if (d.net_card_unit)       cfgCardUnits.net   = d.net_card_unit;
  if (d.net_chart_unit)      cfgChartUnits.net  = d.net_chart_unit;
  if (d.cpu_load_card_unit)  cfgCardUnits.load  = d.cpu_load_card_unit;
  if (d.cpu_load_chart_unit) cfgChartUnits.load = d.cpu_load_chart_unit;

  if (d.thresh_load) THRESH.load = d.thresh_load;
  if (d.thresh_cpu)  THRESH.cpu  = d.thresh_cpu;
  if (d.thresh_mem)  THRESH.mem  = d.thresh_mem;
  if (d.thresh_disk) THRESH.disk = d.thresh_disk;
  if (d.thresh_temp) THRESH.temp = d.thresh_temp;

  /* Title, footer, theme */
  if (d.title) {
    document.title = d.title;
    document.getElementById('ttl').textContent = d.title;
  }
  if (d.show_footer !== undefined) {
    const ftr = document.getElementById('ftr');
    ftr.classList.toggle('hide', !d.show_footer);
    if (d.version) document.getElementById('ftr-ver').textContent = d.version;
  }
  /* When theme is fixed server-side, apply it and hide the toggle button */
  if (d.theme && d.theme !== 'auto') {
    document.documentElement.dataset.theme = d.theme;
    document.getElementById('thm').style.display = 'none';
  }

  /* Uptime */
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
      disk:      'b-disk',
      temp:      'b-temp',
      net:       'b-net',
    };
    Object.keys(CHART_BOX).forEach(function(nm) {
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
      disk:      'c-disk',
      temp:      'c-temp',
      net:       'c-net',
      uptime:    'upt',
    };
    Object.keys(CARD_ORDER).forEach(function(nm) {
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
    ['c-load', 'cpu_load',  'load_1m'],
    ['c-cpu',  'cpu_usage', 'cpu_user_percent'],
    ['c-mem',  'memory',    'mem_percent'],
    ['c-disk', 'disk',      'disk_percent'],
    ['c-temp', 'temp',      'temp'],
    ['c-net',  'net',       'net_rx'],
  ].forEach(function(c) {
    const el = document.getElementById(c[0]);
    if (!el) return;
    const excluded = cfgVisCards !== null && cfgVisCards.indexOf(c[1]) === -1;
    const present  = c[2] in d;
    el.classList.toggle('hide', excluded || !present);
  });

  /* CPU Load (per-sub colour): each load value gets its own threshold level */
  if ('load_1m' in d) {
    const loadFmt = cfgCardUnits.load === '%'
      ? function(v) { return v.toFixed(1) + '%'; }
      : function(v) { return v.toFixed(2); };
    updateNumericCard('c-load', 'load',
      [d.load_1m, d.load_5m, d.load_15m], loadFmt,
      cardLevel(d.load_1m, THRESH.load),
      function(v) { return cardLevel(v, THRESH.load); });
  }

  /* CPU Usage: overall card colour only; sub-values stay neutral */
  if ('cpu_user_percent' in d) {
    const pctFmt = function(v) { return v.toFixed(1) + '%'; };
    updateNumericCard('c-cpu', 'cpu',
      [d.cpu_user_percent, d.cpu_system_percent], pctFmt,
      cardLevel(d.cpu_user_percent, THRESH.cpu));
  }

  /* Memory: sub-colour shared (every sub takes the overall mem level) */
  if ('mem_percent' in d) {
    const memIsAbs = cfgCardUnits.mem !== '%';
    const memFmt = memIsAbs
      ? function(v) {
          if (cfgCardUnits.mem === 'gb') return (v / 1024).toFixed(2) + ' GB';
          return v.toFixed(0) + ' MB';
        }
      : function(v) { return v.toFixed(1) + '%'; };
    const memV = memIsAbs
      ? [d.mem_used, d.mem_available]
      : [d.mem_percent, d.mem_percent != null ? 100 - d.mem_percent : null];
    const memLvl = cardLevel(d.mem_percent, THRESH.mem);
    updateNumericCard('c-mem', 'mem', memV, memFmt, memLvl, memLvl);
  }

  /* Disk: same shape as memory */
  if ('disk_percent' in d) {
    const diskIsAbs = cfgCardUnits.disk !== '%';
    const diskFmt = diskIsAbs
      ? function(v) {
          if (cfgCardUnits.disk === 'tb') return (v / 1000).toFixed(2) + ' TB';
          return v.toFixed(1) + ' GB';
        }
      : function(v) { return v.toFixed(1) + '%'; };
    const diskV = diskIsAbs
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
  /* Store the critical trip-point for the chart reference line. `!== undefined`
   * so an explicit null (sensor offline after a valid read) clears the stale
   * reference; only an absent field preserves the last known value. */
  if (d.temp_critical !== undefined) tempCritical = d.temp_critical;

  /* Network: netV[0]=tx, netV[1]=rx, matching the HTML card-sub order. Both
   * may be null on the first collect before a delta exists (rendered as the
   * dash). No threshold levels (network has no semaphore). */
  if ('net_rx' in d) {
    const netFmt = function(v) { return fmtNet(v, cfgCardUnits.net); };
    updateNumericCard('c-net', 'net', [d.net_tx, d.net_rx], netFmt, '');
  }

  /* Uptime subtitle */
  if (d.uptime_seconds != null) {
    document.getElementById('upt').textContent = fmtUptime(d.uptime_seconds);
  }
}

/* --- Render all charts --- */

function renderAll() {
  const ts = pts.map(function(p) { return p.t; });

  /* If the user pinned a moment (click-to-lock), re-derive lockedIdx against
   * the new pts window so the marker tracks the timestamp, not an array slot. */
  refreshLockedIdx();

  /* Per-chart visibility: a chart shows when it is not excluded by cfgVisCharts
   * AND its primary field is present (the server omits the field when the chart
   * is not configured). With no data yet we leave the HTML's initial .hide
   * state untouched, avoiding a first-paint flash (notably the temp chart). */
  function chartVisible(name, fieldKey, boxId) {
    const excluded = cfgVisCharts !== null && cfgVisCharts.indexOf(name) === -1;
    const el = document.getElementById(boxId);
    if (pts.length === 0) {
      if (excluded) {
        if (el) el.classList.add('hide');
        delete chartCache[boxId.replace('b-', 'g-')];
        return false;
      }
      return !(el && el.classList.contains('hide'));
    }
    const present = fieldKey in pts[0];
    const visible = !excluded && present;
    if (el) el.classList.toggle('hide', !visible);
    if (!visible) delete chartCache[boxId.replace('b-', 'g-')];
    return visible;
  }

  /* Derive display labels and axis unit keys from the configured units */
  const memUL  = { 'mb': 'MB', 'gb': 'GB', '%': '%' }[cfgChartUnits.mem]  || 'MB';
  const dskUL  = { 'gb': 'GB', 'tb': 'TB', '%': '%' }[cfgChartUnits.disk] || 'GB';
  const tmpUK  = cfgChartUnits.temp[0] === '%' ? '%' : 'C';  /* axis key for fmtY */
  const tmpSym = cfgChartUnits.temp[0] === 'f' ? '°F'
             : cfgChartUnits.temp[0] === '%' ? '%' : '°C';
  const netUL  = {
    'kb': 'KB/s', 'mb': 'MB/s', 'gb': 'GB/s',
    'kbps': 'Kbps', 'mbps': 'Mbps', 'gbps': 'Gbps',
  }[cfgChartUnits.net] || 'KB/s';

  const loadOpts = cfgChartUnits.load === '%'
    ? { yMin: 0, unit: '%', ts: ts }
    : { yMin: 0, ts: ts };
  /* Threshold lines only when chart and card units match, else the server
   * threshold (in the card unit) would not line up with chart values. */
  if (cfgChartUnits.load === cfgCardUnits.load) loadOpts.thresh = THRESH.load;

  if (chartVisible('cpu_load', 'l1', 'b-load')) {
    drawChart('g-load', [
      { c: CLR.load1,  v: pts.map(function(p) { return p.l1;  }), label: '1m'  },
      { c: CLR.load5,  v: pts.map(function(p) { return p.l5;  }), label: '5m'  },
      { c: CLR.load15, v: pts.map(function(p) { return p.l15; }), label: '15m' },
    ].filter(function(_, i) { return !seriesHidden['g-load'][i]; }), loadOpts);
  }

  if (chartVisible('cpu_usage', 'cu', 'b-cpu')) {
    drawChart('g-cpu', [
      { c: CLR.user, v: pts.map(function(p) { return p.cu; }), label: 'user' },
      { c: CLR.sys,  v: pts.map(function(p) { return p.cs; }), label: 'sys'  },
    ].filter(function(_, i) { return !seriesHidden['g-cpu'][i]; }),
      { yMin: 0, unit: '%', ts: ts, thresh: THRESH.cpu });
  }

  if (chartVisible('memory', 'mp', 'b-mem')) {
    document.getElementById('b-mem').querySelector('.ctitle').textContent =
      'Memory (' + memUL + ')';
    const memIsPct = cfgChartUnits.mem === '%';
    const memOpts  = { yMin: 0, ts: ts, unit: memIsPct ? '%' : null };
    if (memIsPct) memOpts.thresh = THRESH.mem;
    /* Tooltip suffix so a "512" reading does not strand the user on MB vs GB. */
    if (!memIsPct) memOpts.fmtFn = function(v) { return fmtTip(v, null) + ' ' + memUL; };
    drawChart('g-mem', [
      {
        c: CLR.mem, label: 'used',
        v: pts.map(function(p) {
          if (cfgChartUnits.mem === '%')  return p.mp;
          if (cfgChartUnits.mem === 'gb') return p.mu / 1024;
          return p.mu;
        }),
      },
      {
        c: CLR.memAvail, label: 'avail',
        v: pts.map(function(p) {
          if (cfgChartUnits.mem === '%')  return 100 - p.mp;
          if (cfgChartUnits.mem === 'gb') return p.ma / 1024;
          return p.ma;
        }),
      },
    ].filter(function(_, i) { return !seriesHidden['g-mem'][i]; }), memOpts);
  }

  if (chartVisible('disk', 'dp', 'b-disk')) {
    document.getElementById('b-disk').querySelector('.ctitle').textContent =
      'Disk (' + dskUL + ')';
    const diskIsPct = cfgChartUnits.disk === '%';
    const diskOpts  = { yMin: 0, ts: ts, unit: diskIsPct ? '%' : null };
    if (diskIsPct) diskOpts.thresh = THRESH.disk;
    if (!diskIsPct) diskOpts.fmtFn = function(v) { return fmtTip(v, null) + ' ' + dskUL; };
    drawChart('g-disk', [
      {
        c: CLR.disk, label: 'used',
        v: pts.map(function(p) {
          if (cfgChartUnits.disk === '%')  return p.dp;
          if (cfgChartUnits.disk === 'tb') return p.du / 1000;
          return p.du;
        }),
      },
      {
        c: CLR.diskFree, label: 'free',
        v: pts.map(function(p) {
          if (cfgChartUnits.disk === '%')  return 100 - p.dp;
          if (cfgChartUnits.disk === 'tb') return p.df / 1000;
          return p.df;
        }),
      },
    ].filter(function(_, i) { return !seriesHidden['g-disk'][i]; }), diskOpts);
  }

  if (chartVisible('temp', 'tp', 'b-temp')) {
    document.getElementById('b-temp').querySelector('.ctitle').textContent =
      'Temperature (' + tmpSym + ')';
    const tempUnitsMatch = (cfgChartUnits.temp || 'c')[0] === (cfgCardUnits.temp || 'c')[0];
    const tempOpts = {
      yMin: 0,
      unit: tmpUK,
      ts:   ts,
      /* Dashed red line at the sysfs critical trip-point (hardware limit) */
      refLines: (tempUnitsMatch && tempCritical != null)
        ? [{ v: tempCritical, c: cssv('--red') }]
        : [],
      /* Tooltip shows the actual unit symbol; fmtTip alone collapses C and F to
       * a bare degree, ambiguous when the user picked Fahrenheit. */
      fmtFn: function(v) { return v.toFixed(1) + tmpSym; },
    };
    if (tempUnitsMatch) tempOpts.thresh = THRESH.temp;
    drawChart('g-temp',
      [{ c: CLR.temp, v: pts.map(function(p) { return p.tp; }), label: 'temp' }],
      tempOpts);
  }

  if (chartVisible('net', 'nr', 'b-net')) {
    document.getElementById('b-net').querySelector('.ctitle').textContent =
      'Network (' + netUL + ')';
    drawChart('g-net', [
      { c: CLR.rx, v: pts.map(function(p) { return p.nr; }), label: '&#8595;' },
      { c: CLR.tx, v: pts.map(function(p) { return p.nt; }), label: '&#8593;' },
    ].filter(function(_, i) { return !seriesHidden['g-net'][i]; }),
      { yMin: 0, ts: ts, fmtFn: function(v) { return fmtNet(v, cfgChartUnits.net); } });
  }

  /* Fresh drawChart calls above rewrote clean opts to the cache; without this
   * the locked crosshair vanishes after each SSE refresh while lock is active. */
  if (lockedIdx != null) scheduleHoverDraw();
}

/* --- Legend build & toggle --- */

/* Wire handlers onto the pre-declared .leg-item elements in the HTML */
function buildLegends() {
  document.querySelectorAll('.legend[data-chart] .leg-item').forEach(function(s) {
    const id  = s.closest('.legend').dataset.chart;
    const idx = parseInt(s.dataset.series, 10);
    /* IIFE captures idx so each closure refers to its own series index */
    s.onclick = (function(i) { return function() { toggleSeries(id, i); }; })(idx);
    s.addEventListener('keydown', (function(i) {
      return function(e) {
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          toggleSeries(id, i);
        }
      };
    })(idx));
  });
}

function toggleSeries(id, idx) {
  seriesHidden[id][idx] = !seriesHidden[id][idx];
  const items = document.getElementById('leg-' + id).querySelectorAll('.leg-item');
  const item  = items[idx];
  item.classList.toggle('off', seriesHidden[id][idx]);
  item.setAttribute('aria-checked', seriesHidden[id][idx] ? 'false' : 'true');
  renderAll();
}

/* --- Theme toggle --- */

function toggleTheme() {
  const html    = document.documentElement;
  const goLight = html.dataset.theme !== 'light';
  html.dataset.theme = goLight ? 'light' : 'dark';
  document.getElementById('thm').textContent = goLight ? '🌙 Dark' : '☀ Light';
  /* Theme variables changed: drop the cached values so cssv() re-reads them. */
  invalidateCssCache();
  /* Redraw charts so canvas colors update to the new theme variables */
  renderAll();
}

/* --- Hover crosshair + tooltip --- */

/* Position the tooltip near the cursor, reading the values at `idx` from the
 * cached chart's series. Flips to the opposite side if it would clip the edge. */
function showTooltip(idx, mx, my, cached) {
  const tt   = document.getElementById('tt');
  const ts   = (cached.opts.ts || [])[idx];
  const span = cached.opts.ts && cached.opts.ts.length > 1
    ? cached.opts.ts[cached.opts.ts.length - 1] - cached.opts.ts[0]
    : 0;
  const fmt  = cached.opts.fmtFn || function(v) { return fmtTip(v, cached.opts.unit); };
  let html = ts ? '<div class="tt-time">' + fmtXFull(ts, span) + '</div>' : '';
  cached.series.forEach(function(s) {
    const v = s.v[idx];
    /* Show every series, falling back to the em dash placeholder when a point
     * has no value (e.g. a temp chart with no sensor), matching the cards. */
    html += '<div class="tt-row">'
      + '<span class="tt-dot" style="background:' + s.c + '"></span>'
      + (s.label ? '<span class="tt-lbl">' + s.label + '</span>' : '')
      + '<span class="tt-val">' + (v == null ? '—' : fmt(v)) + '</span>'
      + '</div>';
  });
  if (!html) { tt.classList.add('hide'); return; }
  tt.innerHTML = html;
  /* Measure first so we can flip sides if the tip would clip the viewport */
  tt.style.visibility = 'hidden';
  tt.classList.remove('hide');
  const tw = tt.offsetWidth;
  const th = tt.offsetHeight;
  let x = mx + 14;
  let y = my - 14;
  if (x + tw > window.innerWidth  - 8) x = mx - tw - 14;
  if (y + th > window.innerHeight - 8) y = my - th - 14;
  /* Flip is computed in viewport coords; offset by the scroll so the absolutely
   * positioned tip anchors to the page and travels with its point on scroll. */
  tt.style.left       = (x + window.scrollX) + 'px';
  tt.style.top        = (y + window.scrollY) + 'px';
  tt.style.visibility = '';
}

function hideTooltip() { document.getElementById('tt').classList.add('hide'); }

/* Shared-crosshair state.
 *   hoverIdxPending: ephemeral index from the latest mousemove (null when no
 *     chart is hovered).
 *   lockedTs: when set, the crosshair is pinned at that timestamp; the user can
 *     move between charts and the tooltip updates without losing the marker.
 *     Stored as a timestamp (not an index) so SSE window slides do not change
 *     the locked moment.
 *   lockedIdx: derived from lockedTs on each renderAll (closest pts index). */
let hoverIdxPending   = null;
let lockedTs          = null;
let lockedIdx         = null;
let hoverRafScheduled = false;

/* Re-derive lockedIdx from lockedTs against the current pts array. Called at the
 * start of renderAll so the lock survives data updates. */
function refreshLockedIdx() {
  if (lockedTs == null || pts.length === 0) { lockedIdx = null; return; }
  /* If the locked moment fell outside the visible window (it slid forward),
   * auto-unlock rather than snap to the nearest edge point and mislead. */
  if (lockedTs < pts[0].t || lockedTs > pts[pts.length - 1].t) {
    lockedTs = null; lockedIdx = null;
    hideTooltip();
    return;
  }
  let bestIdx = 0, bestDiff = Infinity;
  for (let i = 0; i < pts.length; i++) {
    const diff = Math.abs(pts[i].t - lockedTs);
    if (diff < bestDiff) { bestDiff = diff; bestIdx = i; }
  }
  lockedIdx = bestIdx;
}

function scheduleHoverDraw() {
  if (hoverRafScheduled) return;
  hoverRafScheduled = true;
  requestAnimationFrame(function() {
    hoverRafScheduled = false;
    /* Lock wins over transient hover. */
    const activeIdx = lockedIdx != null ? lockedIdx : hoverIdxPending;
    const locked    = lockedIdx != null;
    Object.keys(chartCache).forEach(function(id) {
      const c = chartCache[id];
      if (!c) return;
      if (activeIdx == null)
        drawChart(id, c.series, c.opts);            /* clean redraw */
      else
        drawChart(id, c.series, Object.assign({}, c.opts,
          { hoverIdx: activeIdx, hoverLocked: locked }));
    });
  });
}

/* Mark all charts to redraw with the shared crosshair at idx. Coalesces into
 * the next animation frame, so a 240Hz mousemove burst still yields ~60 fps. */
function drawAllWithHover(idx) {
  hoverIdxPending = idx;
  scheduleHoverDraw();
}

/* Clear the crosshair on every chart, release any lock, and hide the tooltip. */
function clearAllHovers() {
  hoverIdxPending = null;
  lockedTs        = null;
  lockedIdx       = null;
  scheduleHoverDraw();
  hideTooltip();
}

/* Devices with a mouse can hover and get a mouseleave; touch devices get neither,
 * so the click gesture below differs between them. */
const canHover = typeof window !== 'undefined' && !!window.matchMedia
  && window.matchMedia('(hover: hover)').matches;

/* Attach mousemove, mouseleave and click on a canvas for the shared crosshair
 * and tooltip. mousemove tracks the cursor (the crosshair lives while hovering,
 * the tooltip follows). Click behaviour splits by device: with a mouse a click
 * locks and a second click unlocks back to cursor-following hover; on touch (no
 * hover, no mouseleave) a tap locks or moves the marker, and re-tapping the same
 * point, the axis gutter, or off the charts clears it. */
function attachHover(id) {
  const cv = document.getElementById(id);
  if (!cv) return;
  const Pl = 38, Pr = 8;

  /* Data index for the cursor x, or null over the axis gutter (not data). */
  function idxFromEvent(e) {
    const cached = chartCache[id];
    if (!cached) return null;
    const first = cached.series[0];
    const n = first ? first.v.length : 0;
    if (!n) return null;
    const rect = cv.getBoundingClientRect();
    const w    = cv.parentElement.clientWidth - 24;
    const xPx  = e.clientX - rect.left;
    if (xPx < Pl || xPx > w - Pr) return null;
    const cw = w - Pl - Pr;
    return Math.max(0, Math.min(n - 1, Math.round((xPx - Pl) / cw * (n - 1))));
  }

  /* Viewport X of the crosshair line at lockedIdx for THIS chart, used to
   * anchor the tooltip over the inspected column even when the layout splits
   * charts across columns. */
  function lockedScreenX() {
    const cached = chartCache[id];
    if (!cached) return null;
    const first = cached.series[0];
    const n = first ? first.v.length : 0;
    if (!n || lockedIdx == null) return null;
    const rect = cv.getBoundingClientRect();
    const w    = cv.parentElement.clientWidth - 24;
    const cw   = w - Pl - Pr;
    return rect.left + Pl + (lockedIdx / Math.max(n - 1, 1)) * cw;
  }

  cv.addEventListener('mousemove', function(e) {
    const idx    = idxFromEvent(e);
    const cached = chartCache[id];
    if (idx == null) {
      /* Over the axis gutter: unlocked -> clear; locked -> just hide tooltip. */
      if (lockedIdx == null) clearAllHovers();
      else                   hideTooltip();
      return;
    }
    if (lockedIdx != null) {
      /* Crosshair stays at lockedIdx; tooltip reads THIS chart at lockedIdx,
       * anchored to the crosshair line. Skip if the X resolver returns null
       * (stale cache, lock cleared mid-frame): better no tooltip than NaNpx. */
      const lx = lockedScreenX();
      if (lx != null) showTooltip(lockedIdx, lx, e.clientY, cached);
    } else {
      drawAllWithHover(idx);
      showTooltip(idx, e.clientX, e.clientY, cached);
    }
  });

  cv.addEventListener('mouseleave', function() {
    if (lockedIdx != null) hideTooltip();  /* keep the locked crosshair */
    else                   clearAllHovers();
  });

  cv.addEventListener('click', function(e) {
    /* Keep the document-level clear (see init) from firing for on-chart taps. */
    e.stopPropagation();
    const idx    = idxFromEvent(e);
    const cached = chartCache[id];
    if (canHover) {
      /* Mouse: a click locks; a second click unlocks back to cursor-following
       * hover. Gutter clicks are no-ops. */
      if (idx == null) return;
      if (lockedTs != null) {
        lockedTs        = null;
        lockedIdx       = null;
        hoverIdxPending = idx;
        scheduleHoverDraw();
        showTooltip(idx, e.clientX, e.clientY, cached);
      } else if (pts[idx]) {
        lockedTs        = pts[idx].t;
        lockedIdx       = idx;
        hoverIdxPending = null;
        scheduleHoverDraw();
        const lx = lockedScreenX();
        if (lx != null) showTooltip(idx, lx, e.clientY, cached);
      }
      return;
    }
    /* Touch: a tap on the axis gutter, or re-tapping the already-locked point,
     * clears everything (the only way to dismiss without a mouseleave). */
    if (idx == null || !pts[idx]) { clearAllHovers(); return; }
    if (lockedTs === pts[idx].t) { clearAllHovers(); return; }
    /* Otherwise lock, or move an existing lock, to the tapped point. Pinned by
     * timestamp so the lock survives SSE window slides. */
    lockedTs        = pts[idx].t;
    lockedIdx       = idx;
    hoverIdxPending = null;
    scheduleHoverDraw();
    const lx = lockedScreenX();
    if (lx != null) showTooltip(idx, lx, e.clientY, cached);
  });
}

/* --- Data fetchers --- */

function loadCurrent() {
  fetch('/api/current').then(function(r) {
    if (r.ok) r.json().then(updateCards);
  }).catch(function() {});
}

/* Build the /api/metrics URL. points is computed by the caller from the canvas
 * width (see clampPoints / loadMetrics); the server caps it at 1440. */
function metricsUrl(range, points) {
  return '/api/metrics?range=' + range + '&points=' + points;
}

/* Points the canvas can resolve: 1 point per 4 backing pixels (the threshold
 * where discreteness becomes invisible), clamped to [120, 1440] (the server
 * cap, which is the consolidation ladder design point). */
function clampPoints(width, dpr) {
  return Math.min(1440, Math.max(120, Math.round(width * (dpr || 1) / 4)));
}

let metricsRequestId = 0;
function loadMetrics() {
  const myId = ++metricsRequestId;
  const cv     = document.getElementById('g-load');
  const w      = cv ? cv.parentElement.clientWidth - 24 : 800;
  const points = clampPoints(w, devicePixelRatio);
  fetch(metricsUrl(curRange, points)).then(function(r) {
    if (!r.ok) return;
    r.json().then(function(d) {
      if (myId !== metricsRequestId) return;   /* discard stale responses */
      pts = d.points || [];
      renderAll();
    });
  }).catch(function() {});
}

/* --- Range tabs --- */

function buildTabs() {
  const el = document.getElementById('rngs');
  el.innerHTML = '';
  /* If the previously selected range is no longer in the list, fall back */
  if (cfgRanges.indexOf(curRange) === -1) curRange = cfgRanges[0];
  cfgRanges.forEach(function(r) {
    const b = document.createElement('button');
    b.textContent = r;
    if (r === curRange) b.classList.add('act');
    b.onclick = function() {
      curRange = r;
      document.querySelectorAll('#rngs button').forEach(function(x) {
        x.classList.remove('act');
      });
      b.classList.add('act');
      loadMetrics();
    };
    el.appendChild(b);
  });
}

/* --- SSE live stream --- */

function connectSSE() {
  const es = new EventSource('/stream');
  es.onmessage = function(e) {
    try {
      updateCards(JSON.parse(e.data));
      loadMetrics();
    } catch (e) { /* ignore a malformed SSE frame */ }
  };
  /* On any error (network drop, server restart) close and reconnect
   * after 5 seconds to avoid hammering the server */
  es.onerror = function() {
    es.close();
    setTimeout(connectSSE, 5000);
  };
}

/* --- Init --- */

/* Browser entry point: run the dashboard only inside a real document. Guarded
 * so the test harness (node) can require this file for its pure helpers without
 * a DOM present. */
if (typeof window !== 'undefined' && typeof document !== 'undefined') {
  /* Set the correct initial button label based on the OS preference */
  if (!window.matchMedia('(prefers-color-scheme: dark)').matches) {
    document.getElementById('thm').textContent = '🌙 Dark';
  }
  document.getElementById('thm').addEventListener('click', toggleTheme);
  /* OS-level theme changes (prefers-color-scheme) swap the CSS variables when no
   * explicit data-theme is set: invalidate the cache and redraw. */
  window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function() {
    invalidateCssCache();
    renderAll();
  });

  wireCards();
  buildLegends();
  buildTabs();
  ['g-load', 'g-cpu', 'g-mem', 'g-disk', 'g-temp', 'g-net'].forEach(attachHover);
  /* Touch has no mouseleave: a click off the charts clears the crosshair and
   * tooltip (on-chart clicks stopPropagation, so they set the lock instead).
   * Desktop keeps its mouseleave behaviour, so this listener is touch-only. */
  if (!canHover) document.addEventListener('click', clearAllHovers);
  renderAll();
  loadCurrent();
  loadMetrics();
  connectSSE();

  /* Debounce canvas redraws on window resize to avoid per-pixel storms */
  let resizeTimer;
  window.addEventListener('resize', function() {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(renderAll, 100);
  });
}

/* Export pure helpers for the test harness; a no-op in the browser. */
if (typeof module !== 'undefined' && module.exports) {
  module.exports = {
    fmtY: fmtY,
    fmtX: fmtX,
    fmtTip: fmtTip,
    fmtXFull: fmtXFull,
    fmtNet: fmtNet,
    fmtTempVal: fmtTempVal,
    cardLevel: cardLevel,
    metricsUrl: metricsUrl,
    clampPoints: clampPoints,
    niceTicks: niceTicks,
    timeTicks: timeTicks
  };
}
