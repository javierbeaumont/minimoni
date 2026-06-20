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

/* [warn, critical] boundaries used to color-code card values.
 * cpu/mem/disk compare against the percentage, so they are unit-independent.
 * load and temp are resolved at render time (see updateCards):
 *   load: [70,90] when normalized ("%"); [0.75,1.0]x cores when "abs"
 *         (cores unknown client-side -> 4 assumed; real cores TODO post-0.1).
 *   temp: [0.9*trip, trip] from the sysfs critical point; [70,80] degC fallback. */
const THRESH = {
  cpu:  [70, 90],
  mem:  [70, 90],
  disk: [80, 90],
  load: [3, 4],    /* "abs" fallback = [0.75x4, 1.0x4]; "%" uses [70,90] */
  temp: [70, 80],  /* degC fallback when no sysfs trip point */
};

/* Chart series colours - purely for identification, no semantic meaning.
 * Semantic status (good/warn/critical) uses --grn/--ylw/--red from CSS.
 * C1 sky-400    (#38bdf8): load1, user, mem, disk, tx, temp
 * C2 violet-400 (#a78bfa): load5, sys, memAvail, diskFree, rx
 * C3 slate-400  (#94a3b8): load15 */
const CLR = {
  load1:    '#38bdf8',
  load5:    '#a78bfa',
  load15:   '#94a3b8',
  user:     '#38bdf8',
  sys:      '#a78bfa',
  mem:      '#38bdf8',
  memAvail: '#a78bfa',
  disk:     '#38bdf8',
  diskFree: '#a78bfa',
  temp:     '#38bdf8',
  rx:       '#a78bfa',
  tx:       '#38bdf8',
};

/* --- State --- */

let curRange     = '1d';    /* currently selected time range */
let pts          = [];      /* array of data points from /api/metrics */
let tempCritical = null;    /* sysfs trip-point; drawn as a red dashed line */
/* Which sub-metric is shown as the primary value in each card.
 * 0 = first option, 1 = second; toggled by clicking the sub-value. */
const cardPrimary  = { load: 0, cpu: 0, mem: 0, disk: 0, net: 0 };
let lastCurrent  = null;    /* last /current snapshot; replayed on swapCard */

/* Units read from server config; sensible defaults until first /current */
const cfgCardUnits  = { mem: '%',  disk: '%',  temp: 'c', net: 'mb', load: 'abs' };
const cfgChartUnits = { mem: 'mb', disk: 'gb', temp: 'c', net: 'mb', load: 'abs' };
let cfgUptimeUnit = 'auto';
let cfgRanges     = ['1d', '7d', '30d', '90d'];
/* Three-state visibility: null = show all, [] = hide all, [...] = listed only */
let cfgVisCharts  = null;
let cfgVisCards   = null;

/* --- Chart legend definitions --- */

/* Entries for each chart's interactive legend (temperature has none) */
const LEGENDS = {
  'g-load': [
    { label: '1m',  c: CLR.load1  },
    { label: '5m',  c: CLR.load5  },
    { label: '15m', c: CLR.load15 },
  ],
  'g-cpu': [
    { label: 'user', c: CLR.user },
    { label: 'sys',  c: CLR.sys  },
  ],
  'g-mem': [
    { label: 'used',  c: CLR.mem      },
    { label: 'avail', c: CLR.memAvail },
  ],
  'g-disk': [
    { label: 'used', c: CLR.disk     },
    { label: 'free', c: CLR.diskFree },
  ],
  'g-net': [
    { label: '↓', c: CLR.rx },
    { label: '↑', c: CLR.tx },
  ],
};

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
  return getComputedStyle(document.documentElement).getPropertyValue(v).trim();
}

/* --- Canvas chart --- */

function drawChart(id, series, opts) {
  opts = opts || {};
  const cv = document.getElementById(id);
  if (!cv) return;

  /* Scale the canvas backing store to the device pixel ratio so the
   * chart looks sharp on retina / HiDPI screens */
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
  let yMx = opts.yMax != null ? opts.yMax : 0;
  if (opts.yMax == null) {
    /* Auto-scale: find the max value across all series, add 10% headroom */
    series.forEach(function(s) {
      s.v.forEach(function(v) {
        if (v != null && v > yMx) yMx = v;
      });
    });
    yMx = yMx > 0 ? yMx * 1.1 : 1;
  }
  const yr = yMx - yMn || 1;
  const n  = (series[0] ? series[0].v.length : 0) || 1;

  /* Map a data-space index/value to canvas pixel coordinates */
  const tx = function(i) { return Pl + (i / Math.max(n - 1, 1)) * cw; };
  const ty = function(v) { return Pt + (1 - (v - yMn) / yr) * ch; };

  /* Grid lines */
  ctx.strokeStyle = cssv('--brd');
  ctx.lineWidth   = 0.5;
  for (let gi = 0; gi <= 4; gi++) {
    const gy = Pt + (gi / 4) * ch;
    ctx.beginPath();
    ctx.moveTo(Pl, gy);
    ctx.lineTo(Pl + cw, gy);
    ctx.stroke();
  }

  /* Y-axis labels at bottom, middle, top */
  ctx.fillStyle = cssv('--mut');
  ctx.font      = '10px system-ui';
  [[0, yMn], [0.5, (yMn + yMx) / 2], [1, yMx]].forEach(function(pair) {
    const f = pair[0];
    const v = pair[1];
    ctx.textAlign = 'right';
    ctx.fillText(fmtY(v, opts.unit), Pl - 3, Pt + (1 - f) * ch + 3);
  });

  /* X-axis: first and last timestamp */
  const ts = opts.ts || [];
  if (ts.length > 1) {
    const xspan = ts[ts.length - 1] - ts[0];
    ctx.textAlign = 'left';
    ctx.fillText(fmtX(ts[0], xspan), Pl, h - 4);
    ctx.textAlign = 'right';
    ctx.fillText(fmtX(ts[ts.length - 1], xspan), Pl + cw, h - 4);
  }

  /* Series */
  series.forEach(function(s) {
    if (!s.v.length) return;

    /* Fill area under the line */
    if (s.fill !== false) {
      ctx.beginPath();
      let ok = false;
      s.v.forEach(function(v, i) {
        if (v == null) { ok = false; return; }  /* null = gap in data */
        if (!ok) { ctx.moveTo(tx(i), ty(v)); ok = true; }
        else     { ctx.lineTo(tx(i), ty(v)); }
      });
      ctx.lineTo(tx(n - 1), Pt + ch);
      ctx.lineTo(Pl, Pt + ch);
      ctx.closePath();
      ctx.globalAlpha = 0.12;
      ctx.fillStyle   = s.c;
      ctx.fill();
      ctx.globalAlpha = 1;
    }

    /* Stroke the line; ok2 resets on null so gaps are drawn correctly */
    ctx.strokeStyle = s.c;
    ctx.lineWidth   = 1.5;
    ctx.beginPath();
    let ok2 = false;
    s.v.forEach(function(v, i) {
      if (v == null) { ok2 = false; return; }
      if (!ok2) { ctx.moveTo(tx(i), ty(v)); ok2 = true; }
      else      { ctx.lineTo(tx(i), ty(v)); }
    });
    ctx.stroke();
  });

  /* Reference lines - used to draw the critical temperature threshold */
  if (opts.refLines) {
    opts.refLines.forEach(function(rl) {
      const ry = ty(rl.v);
      if (ry < Pt || ry > Pt + ch) return;  /* out of visible range */
      ctx.save();
      ctx.strokeStyle = rl.c || cssv('--red');
      ctx.lineWidth   = 1;
      ctx.setLineDash([4, 4]);
      ctx.beginPath();
      ctx.moveTo(Pl, ry);
      ctx.lineTo(Pl + cw, ry);
      ctx.stroke();
      ctx.restore();
    });
  }
}

/* --- Format helpers --- */

/* Format a Y-axis tick label according to the chart unit */
function fmtY(v, u) {
  if (!u)                     return v < 10 ? v.toFixed(1) : v.toFixed(0);
  if (u === '%')              return v.toFixed(0) + '%';
  if (u === 'C' || u === 'F') return v.toFixed(0) + '°';
  return v < 10 ? v.toFixed(1) : v.toFixed(0);
}

/* Format a Unix timestamp as HH:MM */
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

/* Format a network throughput value (MB/s internally) into the
 * configured display unit, automatically scaling to KB when small */
function fmtNet(v, unit) {
  if (v == null) return '—';
  if (!unit || unit === 'mb') {
    return v < 1 ? (v * 1024).toFixed(0) + ' KB/s' : v.toFixed(2) + ' MB/s';
  }
  if (unit === 'gb') {
    if (v < 0.001) return (v * 1048576).toFixed(0) + ' KB/s';
    if (v < 1)     return (v * 1024).toFixed(2)    + ' MB/s';
    return v.toFixed(3) + ' GB/s';
  }
  if (unit === 'mbps') {
    return v < 1 ? (v * 1000).toFixed(0) + ' Kbps' : v.toFixed(2) + ' Mbps';
  }
  if (unit === 'gbps') {
    if (v < 0.001) return (v * 1000000).toFixed(0) + ' Kbps';
    if (v < 1)     return (v * 1000).toFixed(2)    + ' Mbps';
    return v.toFixed(3) + ' Gbps';
  }
  return v.toFixed(2);
}

/* Format a temperature value in the configured unit (C, F, or %) */
function fmtTempVal(v, unit) {
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

function setCard(id, val, sub, cls) {
  const el = document.getElementById(id);
  el.querySelector('.cval').textContent = val != null ? val : '—';
  const s = el.querySelector('.csub');
  if (s) s.textContent = sub || '';
  el.className = 'card' + (cls ? ' ' + cls : '');
}

/* Map a status level to its CSS variable (used for inline sub-value spans) */
const lvlClr = { g: 'var(--grn)', y: 'var(--ylw)', r: 'var(--red)', '': 'inherit' };

/* Swap which sub-metric (0 or 1) is shown as primary in a card.
 * Replays lastCurrent so the change is visible immediately. */
/* eslint-disable-next-line no-unused-vars -- called from generated onclick handlers */
function swapCard(id, idx) {
  cardPrimary[id] = idx;
  if (lastCurrent) updateCards(lastCurrent);
}

/* Build the clickable HTML for a secondary metric shown inside .csub */
function subSpan(label, c, valHtml, oc) {
  return '<span style="color:' + c + ';cursor:pointer" onclick="' + oc + '">'
       + label + ' </span>'
       + '<span style="cursor:pointer" onclick="' + oc + '">'
       + valHtml + '</span>';
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

  /* Title, footer, theme */
  if (d.title) {
    document.title = d.title;
    document.getElementById('ttl').textContent = d.title;
  }
  if (d.show_footer !== undefined) {
    const ftr = document.getElementById('ftr');
    ftr.style.display = d.show_footer ? '' : 'none';
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

  /* Card visibility & ordering */
  if (d.cards !== undefined) {
    cfgVisCards = d.cards === null ? null : d.cards;
    const CARD_EL = {
      cpu_load:  'c-load',
      cpu_usage: 'c-cpu',
      memory:    'c-mem',
      disk:      'c-disk',
      temp:      'c-temp',
      net:       'c-net',
      uptime:    'upt',
    };
    Object.keys(CARD_EL).forEach(function(nm) {
      const el       = document.getElementById(CARD_EL[nm]);
      if (!el) return;
      const idx      = cfgVisCards !== null ? cfgVisCards.indexOf(nm) : -1;
      const excluded = cfgVisCards !== null && idx === -1;
      if (excluded)           el.style.display = 'none';
      /* Temperature visibility is driven by data (null sensor -> hidden),
       * not by the cards config, so we never force it to display:'' here */
      else if (nm !== 'temp') el.style.display = '';
      el.style.order = (!excluded && idx !== -1) ? idx : '';
    });
  }

  /* CPU Load */
  const loadFmt = cfgCardUnits.load === '%'
    ? function(v) { return v.toFixed(1) + '%'; }
    : function(v) { return v.toFixed(2); };
  const loadS = [
    { label: '1m',  c: CLR.load1,  v: d.load_1m,  fmt: loadFmt },
    { label: '5m',  c: CLR.load5,  v: d.load_5m,  fmt: loadFmt },
    { label: '15m', c: CLR.load15, v: d.load_15m, fmt: loadFmt },
  ];
  const lp = cardPrimary.load;
  const lc = document.getElementById('c-load');
  /* "%" is normalized by cores (0-100); "abs" is raw load average */
  const loadTh = cfgCardUnits.load === '%' ? [70, 90] : THRESH.load;
  const lcLvl = cardLevel(d.load_1m, loadTh);
  lc.className = 'card' + (lcLvl ? ' ' + lcLvl : '');
  lc.querySelector('.clabel').innerHTML =
    'CPU Load <span style="color:' + loadS[lp].c +
    ';font-size:10px">' + loadS[lp].label + '</span>';
  lc.querySelector('.cval').textContent =
    loadS[lp].v != null ? loadS[lp].fmt(loadS[lp].v) : '—';
  let loadSub = '';
  loadS.forEach(function(s, i) {
    if (i === lp || s.v == null) return;
    const vc = lvlClr[cardLevel(s.v, loadTh)];
    loadSub += (loadSub ? '&ensp;' : '') +
      subSpan(s.label, s.c,
        '<span style="color:' + vc + '">' + s.fmt(s.v) + '</span>',
        'swapCard(\'load\',' + i + ')');
  });
  lc.querySelector('.csub').innerHTML = loadSub;

  /* CPU Usage (absent on the first collect before a delta is available) */
  if (d.cpu_user_percent != null) {
    const pctFmt = function(v) { return v.toFixed(1) + '%'; };
    const cpuS = [
      { label: 'user', c: CLR.user, v: d.cpu_user_percent,   fmt: pctFmt },
      { label: 'sys',  c: CLR.sys,  v: d.cpu_system_percent, fmt: pctFmt },
    ];
    const cp    = cardPrimary.cpu;
    const cc    = document.getElementById('c-cpu');
    const ccLvl = cardLevel(d.cpu_user_percent, THRESH.cpu);
    cc.className = 'card' + (ccLvl ? ' ' + ccLvl : '');
    cc.querySelector('.clabel').innerHTML =
      'CPU Usage <span style="color:' + cpuS[cp].c +
      ';font-size:10px">' + cpuS[cp].label + '</span>';
    cc.querySelector('.cval').textContent = cpuS[cp].v != null ? cpuS[cp].fmt(cpuS[cp].v) : '—';
    let cpuSub = '';
    cpuS.forEach(function(s, i) {
      if (i === cp || s.v == null) return;
      cpuSub = subSpan(s.label, s.c, s.fmt(s.v), 'swapCard(\'cpu\',' + i + ')');
    });
    cc.querySelector('.csub').innerHTML = cpuSub;
  }

  /* Memory */
  if (d.mem_percent != null) {
    /* When unit is absolute (MB/GB), show raw bytes; otherwise show % */
    const memIsAbs = cfgCardUnits.mem !== '%';
    const memFmt = memIsAbs
      ? function(v) {
          if (v == null) return '—';
          if (cfgCardUnits.mem === 'gb') return (v / 1024).toFixed(2) + ' GB';
          return v.toFixed(0) + ' MB';
        }
      : function(v) { return v.toFixed(1) + '%'; };
    const memS = memIsAbs ? [
      { label: 'used',  c: CLR.mem,      v: d.mem_used,      fmt: memFmt },
      { label: 'avail', c: CLR.memAvail, v: d.mem_available, fmt: memFmt },
    ] : [
      { label: 'used',  c: CLR.mem,      v: d.mem_percent,       fmt: memFmt },
      { label: 'avail', c: CLR.memAvail, v: 100 - d.mem_percent, fmt: memFmt },
    ];
    const mp = cardPrimary.mem;
    const mc = document.getElementById('c-mem');
    const mcLvl = cardLevel(d.mem_percent, THRESH.mem);
    mc.className = 'card' + (mcLvl ? ' ' + mcLvl : '');
    mc.querySelector('.clabel').innerHTML =
      'Memory <span style="color:' + memS[mp].c +
      ';font-size:10px">' + memS[mp].label + '</span>';
    mc.querySelector('.cval').textContent = memS[mp].fmt(memS[mp].v);
    let memSub = '';
    memS.forEach(function(s, i) {
      if (i === mp) return;
      const mvc = lvlClr[cardLevel(d.mem_percent, THRESH.mem)];
      memSub = subSpan(s.label, s.c,
        '<span style="color:' + mvc + '">' + s.fmt(s.v) + '</span>',
        'swapCard(\'mem\',' + i + ')');
    });
    mc.querySelector('.csub').innerHTML = memSub;
  }

  /* Disk */
  if (d.disk_percent != null) {
    const diskIsAbs = cfgCardUnits.disk !== '%';
    const diskFmt = diskIsAbs
      ? function(v) {
          if (v == null) return '—';
          if (cfgCardUnits.disk === 'tb') return (v / 1000).toFixed(2) + ' TB';
          return v.toFixed(1) + ' GB';
        }
      : function(v) { return v.toFixed(1) + '%'; };
    const diskS = diskIsAbs ? [
      { label: 'used', c: CLR.disk,     v: d.disk_used, fmt: diskFmt },
      { label: 'free', c: CLR.diskFree, v: d.disk_free, fmt: diskFmt },
    ] : [
      { label: 'used', c: CLR.disk,     v: d.disk_percent,       fmt: diskFmt },
      { label: 'free', c: CLR.diskFree, v: 100 - d.disk_percent, fmt: diskFmt },
    ];
    const dkp  = cardPrimary.disk;
    const dcel = document.getElementById('c-disk');
    const dcelLvl = cardLevel(d.disk_percent, THRESH.disk);
    dcel.className = 'card' + (dcelLvl ? ' ' + dcelLvl : '');
    dcel.querySelector('.clabel').innerHTML =
      'Disk <span style="color:' + diskS[dkp].c +
      ';font-size:10px">' + diskS[dkp].label + '</span>';
    dcel.querySelector('.cval').textContent = diskS[dkp].fmt(diskS[dkp].v);
    let diskSub = '';
    diskS.forEach(function(s, i) {
      if (i === dkp) return;
      const dvc = lvlClr[cardLevel(d.disk_percent, THRESH.disk)];
      diskSub = subSpan(s.label, s.c,
        '<span style="color:' + dvc + '">' + s.fmt(s.v) + '</span>',
        'swapCard(\'disk\',' + i + ')');
    });
    dcel.querySelector('.csub').innerHTML = diskSub;
  }

  /* Temperature - card is shown only when the server sends a non-null
   * value, meaning a real sensor was found at collection time */
  if (d.temp != null && (cfgVisCards === null || cfgVisCards.indexOf('temp') !== -1)) {
    document.getElementById('c-temp').style.display = '';
    /* Prefer the hardware critical trip-point (same unit as d.temp); warn at
     * 90% of it. Fall back to the fixed degC band when no trip point is known. */
    const tempTh = d.temp_critical != null
      ? [0.9 * d.temp_critical, d.temp_critical]
      : THRESH.temp;
    const tempLvl = cardLevel(d.temp, tempTh);
    setCard('c-temp', fmtTempVal(d.temp, cfgCardUnits.temp), null, tempLvl);
  }
  /* Store the critical trip-point for the chart reference line */
  if (d.temp_critical != null) tempCritical = d.temp_critical;

  /* Network */
  if (d.net_rx != null) {
    const netS = [
      { label: '↑', c: CLR.tx, v: d.net_tx },
      { label: '↓', c: CLR.rx, v: d.net_rx },
    ];
    const np = cardPrimary.net;
    const nc = document.getElementById('c-net');
    nc.className = 'card';
    nc.querySelector('.clabel').innerHTML =
      'Network <span style="color:' + netS[np].c +
      ';font-size:10px">' + netS[np].label + '</span>';
    nc.querySelector('.cval').textContent = fmtNet(netS[np].v, cfgCardUnits.net);
    let netSub = '';
    netS.forEach(function(s, i) {
      if (i === np) return;
      const oc = 'swapCard(\'net\',' + i + ')';
      netSub = subSpan(s.label, s.c, fmtNet(s.v, cfgCardUnits.net), oc);
    });
    nc.querySelector('.csub').innerHTML = netSub;
  }

  /* Uptime subtitle */
  if (d.uptime_seconds != null) {
    document.getElementById('upt').textContent = fmtUptime(d.uptime_seconds);
  }
}

/* --- Render all charts --- */

function renderAll() {
  const ts = pts.map(function(p) { return p.t; });

  /* Derive display labels and axis unit keys from the configured units */
  const memUL  = { 'mb': 'MB', 'gb': 'GB', '%': '%' }[cfgChartUnits.mem]  || 'MB';
  const dskUL  = { 'gb': 'GB', 'tb': 'TB', '%': '%' }[cfgChartUnits.disk] || 'GB';
  const tmpUK  = cfgChartUnits.temp[0] === '%' ? '%' : 'C';  /* axis key for fmtY */
  const tmpSym = cfgChartUnits.temp[0] === 'f' ? '°F'
             : cfgChartUnits.temp[0] === '%' ? '%' : '°C';
  const netUL  = {
    'mb': 'MB/s', 'gb': 'GB/s', 'mbps': 'Mbps', 'gbps': 'Gbps',
  }[cfgChartUnits.net] || 'MB/s';

  const loadOpts = cfgChartUnits.load === '%'
    ? { yMin: 0, yMax: 100, unit: '%', ts: ts }
    : { yMin: 0, ts: ts };

  /* Update chart titles with the active unit */
  document.getElementById('b-mem').querySelector('.ctitle').textContent =
    'Memory (' + memUL + ')';
  document.getElementById('b-disk').querySelector('.ctitle').textContent =
    'Disk (' + dskUL + ')';
  document.getElementById('b-temp').querySelector('.ctitle').textContent =
    'Temperature (' + tmpSym + ')';
  document.getElementById('b-net').querySelector('.ctitle').textContent =
    'Network (' + netUL + ')';

  drawChart('g-load', [
    { c: CLR.load1,  v: pts.map(function(p) { return p.l1;  }), fill: false },
    { c: CLR.load5,  v: pts.map(function(p) { return p.l5;  }), fill: false },
    { c: CLR.load15, v: pts.map(function(p) { return p.l15; }), fill: false },
  ].filter(function(_, i) { return !seriesHidden['g-load'][i]; }), loadOpts);

  drawChart('g-cpu', [
    { c: CLR.user, v: pts.map(function(p) { return p.cu; }), fill: false },
    { c: CLR.sys,  v: pts.map(function(p) { return p.cs; }), fill: false },
  ].filter(function(_, i) { return !seriesHidden['g-cpu'][i]; }),
    { yMin: 0, yMax: 100, unit: '%', ts: ts });

  drawChart('g-mem', [
    {
      c: CLR.mem,
      v: pts.map(function(p) {
        if (cfgChartUnits.mem === '%')  return p.mp;
        if (cfgChartUnits.mem === 'gb') return p.mu / 1024;
        return p.mu;
      }),
      fill: false,
    },
    {
      c: CLR.memAvail,
      v: pts.map(function(p) {
        if (cfgChartUnits.mem === '%')  return 100 - p.mp;
        if (cfgChartUnits.mem === 'gb') return p.ma / 1024;
        return p.ma;
      }),
      fill: false,
    },
  ].filter(function(_, i) { return !seriesHidden['g-mem'][i]; }),
    { yMin: 0, ts: ts, unit: cfgChartUnits.mem === '%' ? '%' : null });

  drawChart('g-disk', [
    {
      c: CLR.disk,
      v: pts.map(function(p) {
        if (cfgChartUnits.disk === '%')  return p.dp;
        if (cfgChartUnits.disk === 'tb') return p.du / 1000;
        return p.du;
      }),
      fill: false,
    },
    {
      c: CLR.diskFree,
      v: pts.map(function(p) {
        if (cfgChartUnits.disk === '%')  return 100 - p.dp;
        if (cfgChartUnits.disk === 'tb') return p.df / 1000;
        return p.df;
      }),
      fill: false,
    },
  ].filter(function(_, i) { return !seriesHidden['g-disk'][i]; }),
    { yMin: 0, ts: ts, unit: cfgChartUnits.disk === '%' ? '%' : null });

  /* Temperature chart: only show when at least one point has a real value */
  const tempVisible = pts.some(function(p) { return p.tp != null; }) &&
      (cfgVisCharts === null || cfgVisCharts.indexOf('temp') !== -1);
  document.getElementById('b-temp').classList.toggle('hide', !tempVisible);
  if (tempVisible) {
    drawChart('g-temp',
      [{ c: CLR.temp, v: pts.map(function(p) { return p.tp; }), fill: false }],
      {
        yMin: 0,
        unit: tmpUK,
        ts:   ts,
        /* Draw a dashed red line at the sysfs critical trip-point */
        refLines: tempCritical != null ? [{ v: tempCritical, c: cssv('--red') }] : [],
      }
    );
  }

  drawChart('g-net', [
    { c: CLR.rx, v: pts.map(function(p) { return p.nr; }), fill: false },
    { c: CLR.tx, v: pts.map(function(p) { return p.nt; }), fill: false },
  ].filter(function(_, i) { return !seriesHidden['g-net'][i]; }),
    { yMin: 0, ts: ts });
}

/* --- Legend build & toggle --- */

/* Inject a legend strip into each chart header at startup */
function buildLegends() {
  Object.keys(LEGENDS).forEach(function(id) {
    const hdr  = document.getElementById(id).parentElement.querySelector('.chdr');
    const wrap = document.createElement('div');
    wrap.className = 'legend';
    wrap.id        = 'leg-' + id;
    LEGENDS[id].forEach(function(item, idx) {
      const s = document.createElement('span');
      s.className = 'leg-item';
      s.innerHTML = '<span class="leg-dot" style="background:' + item.c + '"></span>'
                 + item.label;
      /* IIFE captures idx so each closure refers to its own series index */
      s.onclick   = (function(i) { return function() { toggleSeries(id, i); }; })(idx);
      wrap.appendChild(s);
    });
    hdr.appendChild(wrap);
  });
}

function toggleSeries(id, idx) {
  seriesHidden[id][idx] = !seriesHidden[id][idx];
  const items = document.getElementById('leg-' + id).querySelectorAll('.leg-item');
  items[idx].classList.toggle('off', seriesHidden[id][idx]);
  renderAll();
}

/* --- Theme toggle --- */

/* eslint-disable-next-line no-unused-vars -- called from index.html onclick */
function toggleTheme() {
  const html    = document.documentElement;
  const goLight = html.dataset.theme !== 'light';
  html.dataset.theme = goLight ? 'light' : 'dark';
  document.getElementById('thm').textContent = goLight ? '🌙 Dark' : '☀ Light';
  /* Redraw charts so canvas colors update to the new theme variables */
  renderAll();
}

/* --- Data fetchers --- */

function loadCurrent() {
  fetch('/api/current').then(function(r) {
    if (r.ok) r.json().then(updateCards);
  }).catch(function() {});
}

/* Build the /api/metrics URL. The default dashboard targets 480 points per
 * chart - 1 point per 4 backing pixels at 1920x1080 fullscreen, the threshold
 * where the eye no longer sees discreteness. A custom dashboard can compute its
 * own value from canvas width and pass it here; the server caps it at 1440. */
function metricsUrl(range, points) {
  return '/api/metrics?range=' + range + '&points=' + points;
}

let metricsRequestId = 0;
function loadMetrics() {
  const myId = ++metricsRequestId;
  fetch(metricsUrl(curRange, 480)).then(function(r) {
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

  buildLegends();
  buildTabs();
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
  module.exports = { metricsUrl: metricsUrl };
}
