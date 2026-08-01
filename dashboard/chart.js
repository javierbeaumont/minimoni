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

/* Canvas chart subsystem: drawChart + render pass, legends and the theme-aware
 * colour cache. Owns CLR, cssCache, chartCache and seriesHidden; reads the data
 * globals app.js owns and the config globals cards.js owns; the pointer layer on
 * top lives in hover.js. bundle.sh inlines format.js, this, hover.js, cards.js,
 * then app.js (one shared global scope). */
/* global fmtY, fmtTip, fmtX, fmtNet, niceTicks, timeTicks */
/* global THRESH, cfgCardUnits, cfgChartUnits, cfgVisCharts, pts */
/* global refreshLockedIdx, scheduleHoverDraw, lockedIdx */
/* exported renderAll, buildLegends, invalidateCssCache, drawChart, chartCache */

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

/* cssCache is declared above CLR so the CLR initialiser can use cssv(). */
let cssCache = {};

/* Chart series colours: sourced from CSS custom properties so a customiser
 * only needs to edit style.css (or override the --clr-* vars). */
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
    series.forEach((s) => {
      s.v.forEach((v) => {
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
  const tx = (i) => Pl + (i / Math.max(n - 1, 1)) * cw;
  const ty = (v) => Pt + (1 - (v - yMn) / yr) * ch;

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
  yticks.forEach((tv) => {
    const gy = ty(tv);
    ctx.beginPath();
    ctx.moveTo(Pl, gy);
    ctx.lineTo(Pl + cw, gy);
    ctx.stroke();
    ctx.fillText(fmtY(tv, yDecimals), Pl - 3, gy + 3);
  });

  /* Vertical grid lines + X-axis labels at nice, round timestamps (00:00, 04:00,
   * ...) positioned by time within the range, not at raw sample indices. */
  if (ts.length > 1 && span > 0) {
    const t0 = ts[0], t1 = ts[ts.length - 1];
    timeTicks(t0, t1, lblCount).forEach((tk) => {
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

  /* Threshold lines, drawn only once the data crossed them. */
  if (opts.thresh) {
    let thMax = -Infinity;
    series.forEach((s) => {
      s.v.forEach((v) => {
        if (v != null && v > thMax) thMax = v;
      });
    });
    /* No save/restore: the series below sets its own stroke and width. */
    const line = (v, colour) => {
      const ry = ty(v);
      if (ry < Pt || ry > Pt + ch) return;  /* out of visible range */
      ctx.strokeStyle = colour;
      ctx.lineWidth = 0.5;
      ctx.beginPath();
      ctx.moveTo(Pl, ry);
      ctx.lineTo(Pl + cw, ry);
      ctx.stroke();
    };
    if (thMax >= opts.thresh[0]) line(opts.thresh[0], cssv('--ylw'));
    if (thMax >= opts.thresh[1]) line(opts.thresh[1], cssv('--red'));
  }

  /* Series: stroke only (no fill). `ok` resets on null entries so gaps in the
   * data render as breaks instead of straight lines. */
  series.forEach((s) => {
    if (!s.v.length) return;
    ctx.strokeStyle = s.c;
    ctx.lineWidth   = 1.5;
    ctx.beginPath();
    let ok = false;
    s.v.forEach((v, i) => {
      if (v == null) { ok = false; return; }
      if (!ok) { ctx.moveTo(tx(i), ty(v)); ok = true; }
      else     { ctx.lineTo(tx(i), ty(v)); }
    });
    ctx.stroke();
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
    series.forEach((s) => {
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

/* --- Render all charts --- */

function renderAll() {
  const ts = pts.map((p) => p.t);

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
  const tmpSym = cfgChartUnits.temp[0] === 'f' ? '°F'
             : cfgChartUnits.temp[0] === '%' ? '%' : '°C';
  const netUL  = {
    'kb': 'KB/s', 'mb': 'MB/s', 'gb': 'GB/s',
    'kbps': 'Kbps', 'mbps': 'Mbps', 'gbps': 'Gbps', '%': '%',
  }[cfgChartUnits.net] || 'KB/s';

  const loadOpts = cfgChartUnits.load === '%'
    ? { yMin: 0, unit: '%', ts: ts }
    : { yMin: 0, ts: ts };
  /* Threshold lines only when chart and card units match, else the server
   * threshold (in the card unit) would not line up with chart values. */
  if (cfgChartUnits.load === cfgCardUnits.load) loadOpts.thresh = THRESH.load;

  if (chartVisible('cpu_load', 'l1', 'b-load')) {
    /* Load average is dimensionless in abs mode, so no unit then; only the
     * percent mode has a unit to show. */
    document.getElementById('b-load').querySelector('.ctitle').textContent =
      cfgChartUnits.load === '%' ? 'CPU Load (%)' : 'CPU Load';
    drawChart('g-load', [
      { c: CLR.load1,  k: 'load1',  v: pts.map((p) => { return p.l1;  }), label: '1m'  },
      { c: CLR.load5,  k: 'load5',  v: pts.map((p) => { return p.l5;  }), label: '5m'  },
      { c: CLR.load15, k: 'load15', v: pts.map((p) => p.l15), label: '15m' },
    ].filter((_, i) => !seriesHidden['g-load'][i]), loadOpts);
  }

  if (chartVisible('cpu_usage', 'cu', 'b-cpu')) {
    document.getElementById('b-cpu').querySelector('.ctitle').textContent = 'CPU Usage (%)';
    drawChart('g-cpu', [
      { c: CLR.user, k: 'cpu-user', v: pts.map((p) => p.cu), label: 'user' },
      { c: CLR.sys,  k: 'cpu-sys',  v: pts.map((p) => p.cs), label: 'system' },
    ].filter((_, i) => !seriesHidden['g-cpu'][i]),
      { yMin: 0, unit: '%', ts: ts, thresh: THRESH.cpu });
  }

  if (chartVisible('memory', 'mp', 'b-mem')) {
    document.getElementById('b-mem').querySelector('.ctitle').textContent =
      'Memory (' + memUL + ')';
    const memIsPct = cfgChartUnits.mem === '%';
    const memOpts  = { yMin: 0, ts: ts, unit: memIsPct ? '%' : null };
    if (memIsPct) memOpts.thresh = THRESH.mem;
    /* Tooltip suffix so a "512" reading does not strand the user on MB vs GB. */
    if (!memIsPct) memOpts.fmtFn = (v) => fmtTip(v, null) + ' ' + memUL;
    drawChart('g-mem', [
      {
        c: CLR.mem, k: 'mem-used', label: 'used',
        v: pts.map((p) => {
          if (cfgChartUnits.mem === '%')  return p.mp;
          if (cfgChartUnits.mem === 'gb') return p.mu / 1024;
          return p.mu;
        }),
      },
      {
        c: CLR.memAvail, k: 'mem-avail', label: 'available',
        v: pts.map((p) => {
          if (cfgChartUnits.mem === '%')  return 100 - p.mp;
          if (cfgChartUnits.mem === 'gb') return p.ma / 1024;
          return p.ma;
        }),
      },
    ].filter((_, i) => !seriesHidden['g-mem'][i]), memOpts);
  }

  if (chartVisible('disk', 'dp', 'b-disk')) {
    document.getElementById('b-disk').querySelector('.ctitle').textContent =
      'Disk (' + dskUL + ')';
    const diskIsPct = cfgChartUnits.disk === '%';
    const diskOpts  = { yMin: 0, ts: ts, unit: diskIsPct ? '%' : null };
    if (diskIsPct) diskOpts.thresh = THRESH.disk;
    if (!diskIsPct) diskOpts.fmtFn = (v) => fmtTip(v, null) + ' ' + dskUL;
    drawChart('g-disk', [
      {
        c: CLR.disk, k: 'disk-used', label: 'used',
        v: pts.map((p) => {
          if (cfgChartUnits.disk === '%')  return p.dp;
          if (cfgChartUnits.disk === 'tb') return p.du / 1000;
          return p.du;
        }),
      },
      {
        c: CLR.diskFree, k: 'disk-free', label: 'free',
        v: pts.map((p) => {
          if (cfgChartUnits.disk === '%')  return 100 - p.dp;
          if (cfgChartUnits.disk === 'tb') return p.df / 1000;
          return p.df;
        }),
      },
    ].filter((_, i) => !seriesHidden['g-disk'][i]), diskOpts);
  }

  if (chartVisible('temp', 'tp', 'b-temp')) {
    document.getElementById('b-temp').querySelector('.ctitle').textContent =
      'Temperature (' + tmpSym + ')';
    const tempUnitsMatch = (cfgChartUnits.temp || 'c')[0] === (cfgCardUnits.temp || 'c')[0];
    const tempOpts = {
      yMin: 0,
      ts:   ts,
      /* Tooltip shows the actual unit symbol; fmtTip alone collapses C and F to
       * a bare degree, ambiguous when the user picked Fahrenheit. */
      fmtFn: (v) => v.toFixed(1) + tmpSym,
    };
    if (tempUnitsMatch) tempOpts.thresh = THRESH.temp;
    drawChart('g-temp',
      [{ c: CLR.temp, k: 'temp', v: pts.map((p) => p.tp), label: 'temperature' }],
      tempOpts);
  }

  if (chartVisible('net', 'nr', 'b-net')) {
    document.getElementById('b-net').querySelector('.ctitle').textContent =
      'Network (' + netUL + ')';
    /* Thresholds arrive in CARD units: they only apply if the chart shares it. */
    const netUnitsMatch = (cfgChartUnits.net || 'kb') === (cfgCardUnits.net || 'kb');
    const netOpts = {
      yMin: 0,
      ts:   ts,
      fmtFn: (v) => fmtNet(v, cfgChartUnits.net),
    };
    if (netUnitsMatch) netOpts.thresh = THRESH.net;
    drawChart('g-net', [
      { c: CLR.tx, k: 'net-tx', v: pts.map((p) => p.nt), label: 'upload' },
      { c: CLR.rx, k: 'net-rx', v: pts.map((p) => p.nr), label: 'download' },
    ].filter((_, i) => !seriesHidden['g-net'][i]), netOpts);
  }

  /* Fresh drawChart calls above rewrote clean opts to the cache; without this
   * the locked crosshair vanishes after each SSE refresh while lock is active. */
  if (lockedIdx != null) scheduleHoverDraw();
}

/* --- Legend build & toggle --- */

/* Wire handlers onto the pre-declared .leg-item elements in the HTML */
function buildLegends() {
  document.querySelectorAll('.legend[data-chart] .leg-item').forEach((s) => {
    const id  = s.closest('.legend').dataset.chart;
    const idx = parseInt(s.dataset.series, 10);
    s.onclick = () => toggleSeries(id, idx);
    s.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        toggleSeries(id, idx);
      }
    });
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
