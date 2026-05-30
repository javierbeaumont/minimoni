/* ── Thresholds & colors ─────────────────────────────────────────── */

/* [warn, critical] boundaries used to color-code card values */
var THRESH = {
  load: [2, 3.5],
  cpu:  [70, 90],
  mem:  [70, 90],
  disk: [75, 90],
  temp: [70, 80],
};

/* CSS custom-property lookup helper (cached — see "CSS variable helper"
 * section below). Declared here, ABOVE CLR, because CLR's initialiser
 * calls cssv() and `var` initialisers run in source order. */
var cssCache = {};

/* Chart series colours — sourced from CSS custom properties so a
 * customiser only needs to change style.css (or override the vars).
 * cssv() is a function declaration and is therefore hoisted. */
var CLR = {
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

/* ── State ───────────────────────────────────────────────────────── */

var curRange     = '1d';    /* currently selected time range */
var pts          = [];      /* array of data points from /api/metrics */
var tempCritical = null;    /* sysfs trip-point; drawn as a red dashed line */
/* Which sub-metric is shown as the primary value in each card.
   0 = first option, 1 = second; toggled by clicking the sub-value. */
var cardPrimary  = { load: 0, cpu: 0, mem: 0, disk: 0, net: 0 };
var lastCurrent  = null;    /* last /current snapshot; replayed on swapCard */

/* Units read from server config; sensible defaults until first /current */
var cfgCardUnits  = { mem: '%',  disk: '%',  temp: 'c', net: 'kb', load: 'abs' };
var cfgChartUnits = { mem: 'mb', disk: 'gb', temp: 'c', net: 'kb', load: 'abs' };
var cfgUptimeUnit = 'auto';
var cfgRanges     = ['1d', '7d', '30d', '90d'];
/* Three-state visibility: null = show all, [] = hide all, [...] = listed only */
var cfgVisCharts  = null;
var cfgVisCards   = null;

/* Per-chart cache: { series, opts } as last drawn — used for hover redraws */
var chartCache = {};

/* Per-chart, per-series hidden state; toggled by clicking a legend item */
var seriesHidden = {
  'g-load': [false, false, false],
  'g-cpu':  [false, false],
  'g-mem':  [false, false],
  'g-disk': [false, false],
  'g-net':  [false, false],
};

/* ── CSS variable helper ─────────────────────────────────────────── */

/* Cached so we don't hit getComputedStyle several times per chart per
   frame (6 charts × ~5 lookups × 60fps = 1800 calls/s otherwise).
   `cssCache` is declared above CLR (so the CLR initialiser can use cssv).
   Theme changes invalidate the cache in toggleTheme(). */
function cssv(v) {
  if (cssCache[v] === undefined)
    cssCache[v] = getComputedStyle(document.documentElement).getPropertyValue(v).trim();
  return cssCache[v];
}
function invalidateCssCache() { cssCache = {}; }

/* ── Canvas chart ────────────────────────────────────────────────── */

function drawChart(id, series, opts) {
  opts = opts || {};
  /* Cache the clean args for hover redraws (skip when this IS a hover redraw) */
  if (opts.hoverIdx == null) chartCache[id] = { series: series, opts: opts };
  var cv = document.getElementById(id);
  if (!cv) return;

  /* Scale the canvas backing store to the device pixel ratio so the
     chart looks sharp on retina / HiDPI screens.
     Height is driven by CSS — JS only sets the width to match the panel. */
  var dpr = devicePixelRatio || 1;
  var w   = cv.parentElement.clientWidth - 24;
  var h   = cv.clientHeight || 160;
  cv.width       = w * dpr;
  cv.height      = h * dpr;
  cv.style.width = w + 'px';

  var ctx = cv.getContext('2d');
  ctx.scale(dpr, dpr);

  /* Chart padding: Top, Right, Bottom, Left */
  var Pt = 6, Pr = 8, Pb = 20, Pl = 38;
  var cw = w - Pl - Pr;  /* drawable width */
  var ch = h - Pt - Pb;  /* drawable height */

  var yMn = opts.yMin != null ? opts.yMin : 0;
  var yMx = opts.yMax != null ? opts.yMax : 0;
  if (opts.yMax == null) {
    /* Auto-scale: find the max value across all series, add 10% headroom */
    series.forEach(function(s) {
      s.v.forEach(function(v) {
        if (v != null && v > yMx) yMx = v;
      });
    });
    yMx = yMx > 0 ? yMx * 1.1 : 1;
  }
  var yr = yMx - yMn || 1;
  var n  = (series[0] ? series[0].v.length : 0) || 1;

  /* Map a data-space index/value to canvas pixel coordinates */
  var tx = function(i) { return Pl + (i / Math.max(n - 1, 1)) * cw; };
  var ty = function(v) { return Pt + (1 - (v - yMn) / yr) * ch; };

  var ts      = opts.ts || [];
  var span    = ts.length > 1 ? ts[ts.length - 1] - ts[0] : 0;
  /* How many X-axis labels fit (~50 px/label, 7 max to avoid clutter).
     Capped to ts.length so few-point charts don't stack duplicate labels
     at the same pixel. */
  var maxLbls = Math.min(7, Math.max(2, Math.floor(cw / 50)));
  var lblCount = Math.min(maxLbls, ts.length);

  /* Grid lines — horizontal */
  ctx.strokeStyle = cssv('--brd');
  ctx.lineWidth   = 0.5;
  for (var gi = 0; gi <= 4; gi++) {
    var gy = Pt + (gi / 4) * ch;
    ctx.beginPath();
    ctx.moveTo(Pl, gy);
    ctx.lineTo(Pl + cw, gy);
    ctx.stroke();
  }

  /* Y-axis labels at bottom, middle, top */
  ctx.fillStyle = cssv('--mut');
  ctx.font      = '10px system-ui';
  ctx.textAlign = 'right';
  for (var yi = 0; yi <= 2; yi++) {
    var yf = yi / 2;
    ctx.fillText(fmtY(yMn + (yMx - yMn) * yf, opts.unit), Pl - 3, Pt + (1 - yf) * ch + 3);
  }

  /* Vertical grid lines + X-axis labels — share the same index ladder,
     so we walk it once and emit both. */
  if (lblCount > 1) {
    for (var xi = 0; xi < lblCount; xi++) {
      var xti = xi === lblCount - 1
        ? ts.length - 1
        : Math.round(xi * (ts.length - 1) / (lblCount - 1));
      var px = tx(xti);
      ctx.strokeStyle = cssv('--brd');
      ctx.beginPath();
      ctx.moveTo(px, Pt);
      ctx.lineTo(px, Pt + ch);
      ctx.stroke();
      ctx.textAlign = xi === 0 ? 'left' : xi === lblCount - 1 ? 'right' : 'center';
      ctx.fillText(fmtX(ts[xti], span), px, h - 4);
    }
  }

  /* Series — stroke only (no fill underneath). `ok` resets on null entries
     so gaps in the data render as breaks instead of straight lines. */
  series.forEach(function(s) {
    if (!s.v.length) return;
    ctx.strokeStyle = s.c;
    ctx.lineWidth   = 1.5;
    ctx.beginPath();
    var ok = false;
    s.v.forEach(function(v, i) {
      if (v == null) { ok = false; return; }
      if (!ok) { ctx.moveTo(tx(i), ty(v)); ok = true; }
      else     { ctx.lineTo(tx(i), ty(v)); }
    });
    ctx.stroke();
  });

  /* Reference lines — combines explicit opts.refLines (e.g. sysfs temp trip
     point, dashed thick) with opts.thresh-derived thin solid lines for the
     warn/crit semaphore thresholds, drawn only when the data actually crossed
     them at some point. */
  var refLines = (opts.refLines || []).slice();
  if (opts.thresh) {
    var thMax = -Infinity;
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
    var ry = ty(rl.v);
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

  /* Hover crosshair + series dots — drawn on top of everything.
     Single visual difference between hover and locked: hover is dashed,
     locked is solid. Same color and weight in both states. */
  if (opts.hoverIdx != null && opts.hoverIdx >= 0 && opts.hoverIdx < n) {
    var hi = opts.hoverIdx;
    var hx = tx(hi);
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
      var v = s.v[hi];
      if (v == null) return;
      ctx.beginPath();
      ctx.arc(hx, ty(v), 3, 0, Math.PI * 2);
      ctx.fillStyle = s.c;
      ctx.fill();
    });
    ctx.restore();
  }
}

/* ── Format helpers ──────────────────────────────────────────────── */

/* Format a Y-axis tick label according to the chart unit. Anything not
   explicitly handled (or u==null) falls through to a generic numeric. */
function fmtY(v, u) {
  if (u === '%')              return v.toFixed(0) + '%';
  if (u === 'C' || u === 'F') return v.toFixed(0) + '°';
  return v < 10 ? v.toFixed(1) : v.toFixed(0);
}

/* Tooltip formatter — one decimal finer than fmtY for crosshair reads
   (axis labels stay readable at low precision, tooltips give the detail). */
function fmtTip(v, u) {
  if (u === '%')              return v.toFixed(1) + '%';
  if (u === 'C' || u === 'F') return v.toFixed(1) + '°';
  return v < 10 ? v.toFixed(2) : v.toFixed(1);
}

/* Format an X-axis label driven by the actual data span (seconds):
 *   ≤ 1 h  → MM:SS     ≤ 2 d  → HH:MM    ≤ 60 d → Mon DD
 *   < 2 y  → Mon 'YY   ≥ 2 y  → YYYY
 * Boundaries are inclusive so a "1d" range (span = 86400 s exactly) lands
 * in HH:MM, not in Mon DD. Slight overlap into 2d gives headroom for
 * custom ranges or bucket-end drift. */
function fmtX(t, span) {
  if (!t) return '';
  var d  = new Date(t * 1000);
  var mo = ['Jan','Feb','Mar','Apr','May','Jun',
            'Jul','Aug','Sep','Oct','Nov','Dec'];
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

/* Format a timestamp for the hover tooltip — fuller than fmtX axis labels:
 *   ≤ 1 h  → HH:MM:SS    ≤ 2 d  → Mon DD  HH:MM    else → Mon DD, YYYY */
function fmtXFull(t, span) {
  if (!t) return '';
  var d  = new Date(t * 1000);
  var mo = ['Jan','Feb','Mar','Apr','May','Jun',
            'Jul','Aug','Sep','Oct','Nov','Dec'];
  var hh = d.getHours().toString().padStart(2, '0');
  var mm = d.getMinutes().toString().padStart(2, '0');
  var ss = d.getSeconds().toString().padStart(2, '0');
  if (span <= 3600)      return hh + ':' + mm + ':' + ss;
  if (span <= 86400 * 2) return mo[d.getMonth()] + ' ' + d.getDate() + ' ' + hh + ':' + mm;
  return mo[d.getMonth()] + ' ' + d.getDate() + ', ' + d.getFullYear();
}

/* Format uptime according to the configured unit (auto picks the
   most readable granularity: days → hours → minutes) */
function fmtUptime(s) {
  if (cfgUptimeUnit === 'd') return 'up ' + (s / 86400).toFixed(1) + 'd';
  if (cfgUptimeUnit === 'h') return 'up ' + Math.floor(s / 3600) + 'h';
  var d = Math.floor(s / 86400);
  var h = Math.floor(s % 86400 / 3600);
  var m = Math.floor(s % 3600 / 60);
  if (d > 0) return 'up ' + d + 'd ' + h + 'h';
  if (h > 0) return 'up ' + h + 'h ' + m + 'm';
  return 'up ' + m + 'm';
}

/* Format a network throughput value into the configured display unit.
   The display unit NEVER changes — if the user picked Mbps they always see
   Mbps, whether the value is 0.001 or 99999. Precision adapts to keep
   4 significant digits at every magnitude:
       0     – 9.999  → 3 decimals  ("0.001 Mbps" / "9.999 Mbps")
      10.00  – 99.99  → 2 decimals  ("10.00 Mbps" / "99.99 Mbps")
     100.0   – 999.9  → 1 decimal   ("100.0 Mbps" / "999.9 Mbps")
    1000     – 99999  → 0 decimals  ("1000 Mbps"  / "99999 Mbps") */
function fmtNet(v, unit) {
  if (v == null) return '—';
  var s;
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

/* Format a temperature value in the configured unit (°C, °F, or %).
   Returns '—' when v is null (e.g. transient sensor read failure). */
function fmtTempVal(v, unit) {
  if (v == null) return '—';
  if (!unit || unit[0] === 'c') return v.toFixed(1) + '°C';
  if (unit[0] === 'f')          return v.toFixed(1) + '°F';
  return v.toFixed(1) + '%';
}

/* ── Card helpers ────────────────────────────────────────────────── */

/* Return 'g', 'y', or 'r' (good / warning / critical) for a value */
function cardLevel(v, thresh) {
  if (v == null) return '';
  return v >= thresh[1] ? 'r' : v >= thresh[0] ? 'y' : 'g';
}

/* Swap which sub-metric (0 or 1) is shown as primary in a card.
   Replays lastCurrent so the change is visible immediately. */
function swapCard(id, idx) {
  cardPrimary[id] = idx;
  if (lastCurrent) updateCards(lastCurrent);
}

/* Wire click + Enter/Space keyboard handlers as a single "activation" — used
   by both card sub-values and legend items. Keeps the two wiring sites
   (wireCards, buildLegends) identical and free of repeated event plumbing. */
function activate(el, handler) {
  el.addEventListener('click', handler);
  el.addEventListener('keydown', function(e) {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      handler();
    }
  });
}

/* Wire click + keyboard handlers onto the pre-declared .card-sub elements in HTML.
   tabindex="-1" on the primary (hidden) sub keeps it out of the tab order. */
function wireCards() {
  document.querySelectorAll('.card-sub[data-card]').forEach(function(el) {
    var cardId = el.dataset.card;
    var idx    = parseInt(el.dataset.idx, 10);
    el.setAttribute('role', 'button');
    el.setAttribute('tabindex', el.classList.contains('hide') ? '-1' : '0');
    activate(el, function() { swapCard(cardId, idx); });
  });
}

/* Render a numeric card from N values. Used by Load / CPU / Memory /
   Disk / Network — all of which share the same DOM shape:
     .card > .clabel (with .card-unit per series)
           > .cval
           > .csub > .card-sub[data-idx] > .card-sub-val
   Differences between cards are captured in the args:
     cardId     — DOM id of the .card element ('c-load', 'c-cpu', …)
     primaryKey — key into cardPrimary ('load', 'cpu', 'mem', 'disk', 'net')
     values     — array of numeric values, one per data-idx in the HTML.
                  null entries render as '—'.
     fmt        — formatter; only called with non-null values. Helper
                  substitutes '—' for nulls before calling.
     cardLvl    — 'g' | 'y' | 'r' | '' for the overall .card class.
     subLvl     — optional: '' (no per-sub colour), a string applied to
                  every sub-value, or a function(value) → level for
                  per-value colouring. */
function updateNumericCard(cardId, primaryKey, values, fmt, cardLvl, subLvl) {
  var card = document.getElementById(cardId);
  if (!card) return;
  var pi = cardPrimary[primaryKey];
  card.className = 'card' + (cardLvl ? ' ' + cardLvl : '');
  card.querySelectorAll('.card-unit').forEach(function(u, i) {
    u.classList.toggle('hide', i !== pi);
  });
  var pv = values[pi];
  card.querySelector('.cval').textContent = pv != null ? fmt(pv) : '—';
  values.forEach(function(v, i) {
    var sub = card.querySelector('.card-sub[data-idx="' + i + '"]');
    if (!sub) return;
    var valEl = sub.querySelector('.card-sub-val');
    sub.classList.toggle('hide', i === pi);
    sub.setAttribute('tabindex', i === pi ? '-1' : '0');
    valEl.textContent = v != null ? fmt(v) : '—';
    var lvl = typeof subLvl === 'function' ? subLvl(v) : (subLvl || '');
    valEl.className = 'card-sub-val' + (lvl ? ' ' + lvl : '');
  });
}

/* ── Update cards from current snapshot ──────────────────────────── */

function updateCards(d) {
  if (!d) return;
  lastCurrent = d;

  /* Overwrite default units with the values from the server config.
     Each row: [internal key, card-unit JSON field, chart-unit JSON field]. */
  [
    ['mem',  'mem_card_unit',      'mem_chart_unit'],
    ['disk', 'disk_card_unit',     'disk_chart_unit'],
    ['temp', 'temp_card_unit',     'temp_chart_unit'],
    ['net',  'net_card_unit',      'net_chart_unit'],
    ['load', 'cpu_load_card_unit', 'cpu_load_chart_unit'],
  ].forEach(function(u) {
    if (d[u[1]]) cfgCardUnits[u[0]]  = d[u[1]];
    if (d[u[2]]) cfgChartUnits[u[0]] = d[u[2]];
  });

  /* Thresholds — server computes these from core count, trip point, and units */
  Object.keys(THRESH).forEach(function(k) {
    if (d['thresh_' + k]) THRESH[k] = d['thresh_' + k];
  });

  /* Title, footer, theme */
  if (d.title) {
    document.title = d.title;
    document.getElementById('ttl').textContent = d.title;
  }
  if (d.show_footer !== undefined) {
    var ftr = document.getElementById('ftr');
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

  /* Chart ordering — visibility is handled in renderAll() based on pts
     field presence + cfgVisCharts (same field-presence pattern as cards). */
  if (d.charts !== undefined) {
    cfgVisCharts = d.charts === null ? null : d.charts;
    var CHART_ORDER = {
      cpu_load:  'b-load',
      cpu_usage: 'b-cpu',
      memory:    'b-mem',
      disk:      'b-disk',
      temp:      'b-temp',
      net:       'b-net',
    };
    Object.keys(CHART_ORDER).forEach(function(nm) {
      var el  = document.getElementById(CHART_ORDER[nm]);
      if (!el) return;
      var idx = cfgVisCharts !== null ? cfgVisCharts.indexOf(nm) : -1;
      el.style.order = (idx !== -1) ? idx : '';
    });
  }

  /* Card ordering — visibility is handled below per field-presence pass.
     This block runs only when the payload includes `d.cards` (i.e. on every
     /current and /stream tick) and just sets each card's flex `order`. */
  if (d.cards !== undefined) {
    cfgVisCards = d.cards === null ? null : d.cards;
    var CARD_ORDER = {
      cpu_load:  'c-load',
      cpu_usage: 'c-cpu',
      memory:    'c-mem',
      disk:      'c-disk',
      temp:      'c-temp',
      net:       'c-net',
      uptime:    'upt',
    };
    Object.keys(CARD_ORDER).forEach(function(nm) {
      var el  = document.getElementById(CARD_ORDER[nm]);
      if (!el) return;
      var idx = cfgVisCards !== null ? cfgVisCards.indexOf(nm) : -1;
      el.style.order = (idx !== -1) ? idx : '';
    });
  }

  /* Per-card visibility pass — runs every update.
     A card is shown when BOTH:
       (a) it's not excluded by cfgVisCards, AND
       (b) its primary field is present in the payload (null counts as present).
     If either condition fails, the card stays hidden. The HTML initial state
     is `display:none` for every card, so first paint shows nothing until the
     first payload arrives and confirms which cards have data. */
  [
    ['c-load', 'cpu_load',  'load_1m'],
    ['c-cpu',  'cpu_usage', 'cpu_user_percent'],
    ['c-mem',  'memory',    'mem_percent'],
    ['c-disk', 'disk',      'disk_percent'],
    ['c-temp', 'temp',      'temp'],
    ['c-net',  'net',       'net_rx'],
  ].forEach(function(c) {
    var el = document.getElementById(c[0]);
    if (!el) return;
    var excluded = cfgVisCards !== null && cfgVisCards.indexOf(c[1]) === -1;
    var present  = c[2] in d;
    el.style.display = (excluded || !present) ? 'none' : '';
  });

  /* CPU Load — per-sub colour: each load value gets its own threshold level */
  if ('load_1m' in d) {
    var loadFmt = cfgCardUnits.load === '%'
      ? function(v) { return v.toFixed(1) + '%'; }
      : function(v) { return v.toFixed(2); };
    updateNumericCard('c-load', 'load',
      [d.load_1m, d.load_5m, d.load_15m],
      loadFmt,
      cardLevel(d.load_1m, THRESH.load),
      function(v) { return cardLevel(v, THRESH.load); });
  }

  /* CPU Usage — overall card colour only; sub-values stay neutral */
  if ('cpu_user_percent' in d) {
    var pctFmt = function(v) { return v.toFixed(1) + '%'; };
    updateNumericCard('c-cpu', 'cpu',
      [d.cpu_user_percent, d.cpu_system_percent],
      pctFmt,
      cardLevel(d.cpu_user_percent, THRESH.cpu));
  }

  /* Memory — sub-colour shared (every sub takes the overall mem level) */
  if ('mem_percent' in d) {
    var memIsAbs = cfgCardUnits.mem !== '%';
    var memFmt = memIsAbs
      ? function(v) {
          if (cfgCardUnits.mem === 'gb') return (v / 1024).toFixed(2) + ' GB';
          return v.toFixed(0) + ' MB';
        }
      : function(v) { return v.toFixed(1) + '%'; };
    var memV = memIsAbs
      ? [d.mem_used, d.mem_available]
      : [d.mem_percent, d.mem_percent != null ? 100 - d.mem_percent : null];
    var memLvl = cardLevel(d.mem_percent, THRESH.mem);
    updateNumericCard('c-mem', 'mem', memV, memFmt, memLvl, memLvl);
  }

  /* Disk — same shape as memory */
  if ('disk_percent' in d) {
    var diskIsAbs = cfgCardUnits.disk !== '%';
    var diskFmt = diskIsAbs
      ? function(v) {
          if (cfgCardUnits.disk === 'tb') return (v / 1000).toFixed(2) + ' TB';
          return v.toFixed(1) + ' GB';
        }
      : function(v) { return v.toFixed(1) + '%'; };
    var diskV = diskIsAbs
      ? [d.disk_used, d.disk_free]
      : [d.disk_percent, d.disk_percent != null ? 100 - d.disk_percent : null];
    var diskLvl = cardLevel(d.disk_percent, THRESH.disk);
    updateNumericCard('c-disk', 'disk', diskV, diskFmt, diskLvl, diskLvl);
  }

  /* Temperature — show the card whenever the server includes the temp field
     in the payload (the C side only emits it when "temp" is in the cards
     config). A null value (transient sensor read failure) still shows the
     card with "—" rather than hiding it. */
  if ('temp' in d && (cfgVisCards === null || cfgVisCards.indexOf('temp') !== -1)) {
    var tc      = document.getElementById('c-temp');
    var tempLvl = cardLevel(d.temp, THRESH.temp);
    /* Visibility is handled by the per-card pass above. */
    tc.querySelector('.cval').textContent = fmtTempVal(d.temp, cfgCardUnits.temp);
    tc.className = 'card' + (tempLvl ? ' ' + tempLvl : '');
  }
  /* Store the critical trip-point for the chart reference line. Use
     `!== undefined` so an explicit `null` (sensor went offline after a
     prior valid read) clears the stale reference; only an absent field
     preserves the last known value. */
  if (d.temp_critical !== undefined) tempCritical = d.temp_critical;

  /* Network — netV[0]=tx(↑), netV[1]=rx(↓), matching the HTML card-sub
     order. Both values may be null on the very first collect before a
     delta is available — the helper renders '—' for those. No threshold
     levels (network has no semaphore concept). */
  if ('net_rx' in d) {
    var netFmt = function(v) { return fmtNet(v, cfgCardUnits.net); };
    updateNumericCard('c-net', 'net', [d.net_tx, d.net_rx], netFmt, '');
  }

  /* Uptime subtitle */
  if (d.uptime_seconds != null) {
    document.getElementById('upt').textContent = fmtUptime(d.uptime_seconds);
  }
}

/* ── Render all charts ───────────────────────────────────────────── */

function renderAll() {
  var ts = pts.map(function(p) { return p.t; });

  /* If the user pinned a moment (click-to-lock), re-derive the lockedIdx
     against the new pts window so the marker tracks the timestamp, not
     a fixed array position. */
  refreshLockedIdx();

  /* Per-chart visibility — a chart is shown when it's not excluded by the
     cfgVisCharts config AND its primary field is present in the point data
     (the C server omits the field entirely when the chart isn't configured).
     When pts is empty we don't yet know which fields will arrive, so we
     leave the HTML's initial .hide state untouched (b-temp starts hidden,
     others start visible) — that avoids a brief "all charts flash visible"
     at first paint, particularly the temp chart showing empty before the
     first /metrics confirms it should be hidden. */
  function chartVisible(name, fieldKey, boxId) {
    var excluded = cfgVisCharts !== null && cfgVisCharts.indexOf(name) === -1;
    var el = document.getElementById(boxId);
    if (pts.length === 0) {
      /* Without data, only the config can definitively exclude — and only
         then do we override the HTML default. */
      if (excluded) {
        if (el) el.classList.add('hide');
        delete chartCache[boxId.replace('b-', 'g-')];
        return false;
      }
      return !(el && el.classList.contains('hide'));
    }
    var present = fieldKey in pts[0];
    var visible = !excluded && present;
    if (el) el.classList.toggle('hide', !visible);
    if (!visible) delete chartCache[boxId.replace('b-', 'g-')];
    return visible;
  }

  /* Derive display labels and axis unit keys from the configured units */
  var memUL  = { 'mb': 'MB', 'gb': 'GB', '%': '%' }[cfgChartUnits.mem]  || 'MB';
  var dskUL  = { 'gb': 'GB', 'tb': 'TB', '%': '%' }[cfgChartUnits.disk] || 'GB';
  var tmpUK  = cfgChartUnits.temp[0] === '%' ? '%' : 'C';  /* axis key for fmtY */
  var tmpSym = cfgChartUnits.temp[0] === 'f' ? '°F'
             : cfgChartUnits.temp[0] === '%' ? '%' : '°C';
  var netUL  = {
    'kb': 'KB/s', 'mb': 'MB/s', 'gb': 'GB/s',
    'kbps': 'Kbps', 'mbps': 'Mbps', 'gbps': 'Gbps',
  }[cfgChartUnits.net] || 'KB/s';

  var loadOpts = cfgChartUnits.load === '%'
    ? { yMin: 0, unit: '%', ts: ts }
    : { yMin: 0, ts: ts };
  /* Threshold lines only when chart and card units match — otherwise the
     server-computed threshold (in card unit) wouldn't line up with chart values. */
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
    var memIsPct  = cfgChartUnits.mem === '%';
    var memOpts   = { yMin: 0, ts: ts, unit: memIsPct ? '%' : null };
    if (memIsPct) memOpts.thresh = THRESH.mem;
    /* Tooltip suffix so a "512" reading doesn't strand the user wondering
       MB vs GB — the chart title shows the unit but the crosshair shouldn't
       force a glance back at it. */
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
    var diskIsPct = cfgChartUnits.disk === '%';
    var diskOpts  = { yMin: 0, ts: ts, unit: diskIsPct ? '%' : null };
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
    var tempUnitsMatch = (cfgChartUnits.temp || 'c')[0] === (cfgCardUnits.temp || 'c')[0];
    var tempOpts = {
      yMin: 0,
      unit: tmpUK,
      ts:   ts,
      /* Dashed red line at the sysfs critical trip-point (hardware limit) */
      refLines: (tempUnitsMatch && tempCritical != null)
        ? [{ v: tempCritical, c: cssv('--red') }]
        : [],
      /* Tooltip shows the actual unit symbol (°C / °F / %) — fmtTip on its
         own collapses both 'C' and 'F' to a bare degree sign, ambiguous
         when the user picked Fahrenheit. */
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
      { c: CLR.rx, v: pts.map(function(p) { return p.nr; }), label: '↓' },
      { c: CLR.tx, v: pts.map(function(p) { return p.nt; }), label: '↑' },
    ].filter(function(_, i) { return !seriesHidden['g-net'][i]; }),
      { yMin: 0, ts: ts, fmtFn: function(v) { return fmtNet(v, cfgChartUnits.net); } });
  }

  /* Fresh drawChart calls above wrote clean opts to the cache; without
     this the locked crosshair line vanishes after each SSE refresh while
     the lock is still active. */
  if (lockedIdx != null) scheduleHoverDraw();
}

/* ── Legend build & toggle ───────────────────────────────────────── */

/* Wire handlers onto the pre-declared .leg-item elements in HTML */
function buildLegends() {
  document.querySelectorAll('.legend[data-chart] .leg-item').forEach(function(s) {
    var id  = s.closest('.legend').dataset.chart;
    var idx = parseInt(s.dataset.series, 10);
    activate(s, function() { toggleSeries(id, idx); });
  });
}

function toggleSeries(id, idx) {
  if (!seriesHidden[id]) return;  /* unknown chart id (e.g. legend-less temp) */
  seriesHidden[id][idx] = !seriesHidden[id][idx];
  var items = document.getElementById('leg-' + id).querySelectorAll('.leg-item');
  var item  = items[idx];
  item.classList.toggle('off', seriesHidden[id][idx]);
  item.setAttribute('aria-checked', seriesHidden[id][idx] ? 'false' : 'true');
  renderAll();
}

/* ── Theme toggle ────────────────────────────────────────────────── */

function toggleTheme() {
  var html    = document.documentElement;
  var goLight = html.dataset.theme !== 'light';
  html.dataset.theme = goLight ? 'light' : 'dark';
  document.getElementById('thm').textContent = goLight ? '🌙 Dark' : '☀ Light';
  /* Theme variables changed — drop the cached values so the next cssv()
     call re-reads from the (new) computed style. */
  invalidateCssCache();
  /* Redraw charts so canvas colors update to the new theme variables */
  renderAll();
}

/* ── Canvas tooltip & crosshair ─────────────────────────────────── */

/* Render the floating tooltip at the given viewport coords (mx, my). It
   reads the values at `idx` from the cached chart's series. Flips to the
   opposite side of the cursor if it would clip the viewport edge. */
function showTooltip(idx, mx, my, cached) {
  var tt   = document.getElementById('tt');
  var ts   = (cached.opts.ts || [])[idx];
  var span = cached.opts.ts && cached.opts.ts.length > 1
    ? cached.opts.ts[cached.opts.ts.length - 1] - cached.opts.ts[0]
    : 0;
  var fmt  = cached.opts.fmtFn || function(v) { return fmtTip(v, cached.opts.unit); };
  var html = ts ? '<div class="tt-time">' + fmtXFull(ts, span) + '</div>' : '';
  cached.series.forEach(function(s) {
    var v = s.v[idx];
    if (v == null) return;
    html += '<div class="tt-row">'
      + '<span class="tt-dot" style="background:' + s.c + '"></span>'
      + (s.label ? '<span class="tt-lbl">' + s.label + '</span>' : '')
      + '<span class="tt-val">' + fmt(v) + '</span>'
      + '</div>';
  });
  if (!html) { tt.classList.add('hide'); return; }
  tt.innerHTML = html;
  /* Measure first so we can flip sides if the tip would clip the viewport */
  tt.style.visibility = 'hidden';
  tt.classList.remove('hide');
  var tw = tt.offsetWidth;
  var th = tt.offsetHeight;
  var x  = mx + 14;
  var y  = my - 14;
  if (x + tw > window.innerWidth  - 8) x = mx - tw - 14;
  if (y + th > window.innerHeight - 8) y = my - th - 14;
  tt.style.left       = x + 'px';
  tt.style.top        = y + 'px';
  tt.style.visibility = '';
}

function hideTooltip() { document.getElementById('tt').classList.add('hide'); }

/* Shared-crosshair state.
   - hoverIdxPending: ephemeral index from the latest mousemove (null when
     no chart is being hovered).
   - lockedTs: when set, the crosshair is "pinned" at that timestamp. The
     user can move between charts and the tooltip updates without losing
     the marker. Stored as a timestamp (not an index) so SSE updates that
     shift the window don't change the locked moment.
   - lockedIdx: derived from lockedTs on each renderAll — the closest pts
     index to lockedTs in the current data window. */
var hoverIdxPending   = null;
var lockedTs          = null;
var lockedIdx         = null;
var hoverRafScheduled = false;

/* Re-derive lockedIdx from lockedTs against the current pts array.
   Call at the start of renderAll so the lock survives data updates. */
function refreshLockedIdx() {
  if (lockedTs == null || pts.length === 0) { lockedIdx = null; return; }
  /* If the locked moment falls outside the current visible window
     (typically after the window slid forward in time), auto-unlock so
     we don't snap the crosshair to the nearest edge point and mislead
     the user about which timestamp they're inspecting. */
  if (lockedTs < pts[0].t || lockedTs > pts[pts.length - 1].t) {
    lockedTs = null; lockedIdx = null;
    hideTooltip();
    return;
  }
  var bestIdx = 0, bestDiff = Infinity;
  for (var i = 0; i < pts.length; i++) {
    var diff = Math.abs(pts[i].t - lockedTs);
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
    var activeIdx = lockedIdx != null ? lockedIdx : hoverIdxPending;
    var locked    = lockedIdx != null;
    Object.keys(chartCache).forEach(function(id) {
      var c = chartCache[id];
      if (!c) return;
      if (activeIdx == null)
        drawChart(id, c.series, c.opts);            /* clean redraw */
      else
        drawChart(id, c.series, Object.assign({}, c.opts,
          { hoverIdx: activeIdx, hoverLocked: locked }));
    });
  });
}

/* Mark all charts to redraw with the shared crosshair at idx. Coalesces
   into the next animation frame, so a single mousemove burst at 240Hz
   still produces only ~60 redraws per second. */
function drawAllWithHover(idx) {
  hoverIdxPending = idx;
  scheduleHoverDraw();
}

/* Clear the crosshair on every chart and hide the tooltip. */
function clearAllHovers() {
  hoverIdxPending = null;
  scheduleHoverDraw();
  hideTooltip();
}

/* Attach mousemove / mouseleave / click handlers to a canvas for the
   shared crosshair + tooltip.

   States:
   - default: mousemove tracks cursor; the crosshair lives only while
     hovering a chart, tooltip follows the cursor.
   - locked  (via click): the crosshair stays pinned at the click moment.
     The tooltip horizontally anchors to the crosshair line of the chart
     you're hovering (so it always sits over the data column you're
     inspecting), and only its vertical position tracks the cursor. Click
     again to unlock. */
function attachHover(id) {
  var cv = document.getElementById(id);
  if (!cv) return;
  var Pl = 38, Pr = 8;

  /* Compute the data index for the cursor's x position, or null if the
     cursor is over the axis gutter (not the data area). */
  function idxFromEvent(e) {
    var cached = chartCache[id];
    if (!cached) return null;
    var first = cached.series[0];
    var n = first ? first.v.length : 0;
    if (!n) return null;
    var rect = cv.getBoundingClientRect();
    var w    = cv.parentElement.clientWidth - 24;
    var xPx  = e.clientX - rect.left;
    if (xPx < Pl || xPx > w - Pr) return null;
    var cw = w - Pl - Pr;
    return Math.max(0, Math.min(n - 1, Math.round((xPx - Pl) / cw * (n - 1))));
  }

  /* Screen X (viewport coords) of the crosshair line at lockedIdx for
     THIS chart — used to anchor the tooltip horizontally while locked,
     so it sits over the actual data column being inspected even when the
     layout splits charts across multiple columns. */
  function lockedScreenX() {
    var cached = chartCache[id];
    if (!cached) return null;
    var first = cached.series[0];
    var n = first ? first.v.length : 0;
    if (!n || lockedIdx == null) return null;
    var rect = cv.getBoundingClientRect();
    var w    = cv.parentElement.clientWidth - 24;
    var cw   = w - Pl - Pr;
    return rect.left + Pl + (lockedIdx / Math.max(n - 1, 1)) * cw;
  }

  cv.addEventListener('mousemove', function(e) {
    var idx    = idxFromEvent(e);
    var cached = chartCache[id];
    if (idx == null) {
      /* Cursor is over the axis gutter. Unlocked → clear everything;
         locked → just hide the tooltip, keep the crosshair line. */
      if (lockedIdx == null) clearAllHovers();
      else                   hideTooltip();
      return;
    }
    if (lockedIdx != null) {
      /* Crosshair stays at lockedIdx; the tooltip reads THIS chart at
         lockedIdx, anchored horizontally to the crosshair line so it
         sits over the data column being inspected. Skip when the X
         resolver can't compute a position (stale cache, lock cleared
         mid-frame) — better no tooltip than one at NaNpx. */
      var lx = lockedScreenX();
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
    var idx = idxFromEvent(e);
    if (idx == null) return;  /* axis gutter clicks are no-ops */
    var cached = chartCache[id];
    if (lockedTs != null) {
      /* Second click — unlock. Smoothly transition back to hover at the
         current cursor position so the marker doesn't visibly jump. */
      lockedTs        = null;
      lockedIdx       = null;
      hoverIdxPending = idx;
      scheduleHoverDraw();
      showTooltip(idx, e.clientX, e.clientY, cached);
    } else if (pts[idx]) {
      /* Pin by timestamp so the lock survives window slides from SSE. */
      lockedTs        = pts[idx].t;
      lockedIdx       = idx;
      hoverIdxPending = null;
      scheduleHoverDraw();
      var lx = lockedScreenX();
      if (lx != null) showTooltip(idx, lx, e.clientY, cached);
    }
  });
}

/* ── Data fetchers ───────────────────────────────────────────────── */

/* Returns a promise so the init sequence can wait for /current to land
   before kicking off /metrics — otherwise the first chart render uses the
   default cfgChartUnits and we get a brief flicker if metrics resolves
   first. The promise always resolves (errors swallowed) so the caller
   doesn't need a catch. */
function loadCurrent() {
  return fetch('/api/current')
    .then(function(r) { return r.ok ? r.json() : null; })
    .then(function(d) { if (d) updateCards(d); })
    .catch(function() {});
}

/* Monotonic request token: each loadMetrics() bumps it, and when the
   response arrives we discard any whose token isn't the most recent one.
   This prevents stale data from overwriting fresh after rapid range
   switches (1d → 7d → 30d) without cancelling in-flight SSE-triggered
   requests, which would otherwise abort one another with sse_keepalive. */
var metricsRequestId = 0;

function loadMetrics() {
  var myId = ++metricsRequestId;
  /* Compute how many points the canvas can actually resolve: 1 point per
     4 backing pixels (the threshold where discreteness becomes invisible).
     The server hard-caps at 1440; we clamp to [120, 1440]. */
  var cv     = document.getElementById('g-load');
  var w      = cv ? cv.parentElement.clientWidth - 24 : 800;
  var points = Math.min(1440, Math.max(120, Math.round(w * (devicePixelRatio || 1) / 4)));
  fetch('/api/metrics?range=' + curRange + '&points=' + points)
    .then(function(r) { return r.ok ? r.json() : null; })
    .then(function(d) {
      /* Drop stale responses — only the most recent request wins. */
      if (!d || myId !== metricsRequestId) return;
      pts = d.points || [];
      renderAll();
    })
    .catch(function() { /* network errors swallowed */ });
}

/* ── Range tabs ──────────────────────────────────────────────────── */

function buildTabs() {
  var el = document.getElementById('rngs');
  el.innerHTML = '';
  /* If the previously selected range is no longer in the list, fall back */
  if (cfgRanges.indexOf(curRange) === -1) curRange = cfgRanges[0];
  cfgRanges.forEach(function(r) {
    var b = document.createElement('button');
    b.textContent = r;
    if (r === curRange) b.classList.add('act');
    b.onclick = function() {
      curRange = r;
      document.querySelectorAll('#rngs button').forEach(function(x) {
        x.classList.remove('act');
      });
      b.classList.add('act');
      /* Range switch redefines the time window — any locked moment from
         the previous window is unlikely to remain visible, so reset.
         scheduleHoverDraw forces a clean (no-crosshair) redraw of the
         existing canvases now; without it the locked crosshair lingers
         on screen for ~200 ms until /api/metrics returns and renderAll
         repaints. */
      lockedTs = null; lockedIdx = null;
      hideTooltip();
      scheduleHoverDraw();
      loadMetrics();
    };
    el.appendChild(b);
  });
}

/* ── SSE live stream ─────────────────────────────────────────────── */

/* Exponential reconnect backoff: 1s → 2s → 4s → 8s → 16s → 32s → 64s,
   then stays at 64s. Resets to 1s as soon as a message lands on the
   newly opened connection — so a quick server hiccup recovers fast, but
   a permanently down server doesn't get hammered. */
var sseBackoffMs = 1000;
var SSE_BACKOFF_MAX = 64000;
var sseCountdownTimer = null;

/* Live-tick the aria-label on the `.conn` dot so the hover tooltip
   (rendered via `content: attr(aria-label)`) shows a per-second countdown
   to the next reconnect attempt. Without this the user sees "in 32s" for
   32 seconds without any feedback that progress is happening. */
function startReconnectCountdown(targetMs) {
  clearInterval(sseCountdownTimer);
  var c = document.getElementById('conn');
  var tick = function() {
    var remaining = Math.max(0, Math.ceil((targetMs - Date.now()) / 1000));
    c.setAttribute('aria-label',
      'Connection lost — reconnecting in ' + remaining + 's');
    if (remaining <= 0) clearInterval(sseCountdownTimer);
  };
  tick();
  sseCountdownTimer = setInterval(tick, 1000);
}

function connectSSE() {
  var c = document.getElementById('conn');
  /* (Re)connect attempt in progress — stop the countdown and switch to a
     generic Connecting label until the first message lands (or we error). */
  clearInterval(sseCountdownTimer);
  c.setAttribute('aria-label', 'Connecting…');

  var es = new EventSource('/stream');
  es.onmessage = function(e) {
    /* Stop any in-flight countdown tick the moment data lands again. */
    clearInterval(sseCountdownTimer);
    sseBackoffMs = 1000;  /* successful message — reset for next failure */
    c.className = 'conn live';
    c.setAttribute('aria-label', 'Live — receiving updates');
    try {
      updateCards(JSON.parse(e.data));
      loadMetrics();
    } catch (ex) {}
  };
  es.onerror = function() {
    es.close();
    c.className = 'conn down';
    startReconnectCountdown(Date.now() + sseBackoffMs);
    setTimeout(connectSSE, sseBackoffMs);
    sseBackoffMs = Math.min(sseBackoffMs * 2, SSE_BACKOFF_MAX);
  };
}

/* ── Init ────────────────────────────────────────────────────────── */

/* Set the correct initial button label based on the OS preference */
if (!window.matchMedia('(prefers-color-scheme: dark)').matches) {
  document.getElementById('thm').textContent = '🌙 Dark';
}

document.getElementById('thm').addEventListener('click', toggleTheme);

wireCards();
buildLegends();
buildTabs();
['g-load', 'g-cpu', 'g-mem', 'g-disk', 'g-temp', 'g-net'].forEach(attachHover);
renderAll();
/* Wait for /current to set units / thresholds before fetching /metrics —
   otherwise charts could briefly render with default cfgChartUnits. */
loadCurrent().then(loadMetrics);
connectSSE();

/* Debounce canvas redraws on window resize to avoid per-pixel storms */
var resizeTimer;
window.addEventListener('resize', function() {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(renderAll, 100);
});

/* OS-level theme changes (`prefers-color-scheme`) swap the CSS variables
   when no explicit `data-theme` attribute is set. Invalidate the cached
   colours and redraw so canvas content picks up the new palette. */
window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function() {
  invalidateCssCache();
  renderAll();
});
