/* ── Thresholds & colors ─────────────────────────────────────────── */

/* [warn, critical] boundaries used to color-code card values */
var THRESH = {
  load: [2, 3.5],
  cpu:  [70, 90],
  mem:  [70, 90],
  disk: [75, 90],
  temp: [70, 80],
};

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

/* Per-chart, per-series hidden state; toggled by clicking a legend item */
var seriesHidden = {
  'g-load': [false, false, false],
  'g-cpu':  [false, false],
  'g-mem':  [false, false],
  'g-disk': [false, false],
  'g-net':  [false, false],
};

/* ── CSS variable helper ─────────────────────────────────────────── */

function cssv(v) {
  return getComputedStyle(document.documentElement).getPropertyValue(v).trim();
}

/* ── Canvas chart ────────────────────────────────────────────────── */

function drawChart(id, series, opts) {
  opts = opts || {};
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
  /* How many X-axis labels fit (~50 px/label, 7 max to avoid clutter) */
  var maxLbls = Math.min(7, Math.max(2, Math.floor(cw / 50)));

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

  /* Grid lines — vertical, aligned with X-axis label positions */
  if (ts.length > 1) {
    for (var vi = 0; vi < maxLbls; vi++) {
      var vti = vi === maxLbls - 1
        ? ts.length - 1
        : Math.round(vi * (ts.length - 1) / (maxLbls - 1));
      ctx.beginPath();
      ctx.moveTo(tx(vti), Pt);
      ctx.lineTo(tx(vti), Pt + ch);
      ctx.stroke();
    }
  }

  /* Y-axis labels at bottom, middle, top */
  ctx.fillStyle = cssv('--mut');
  ctx.font      = '10px system-ui';
  [[0, yMn], [0.5, (yMn + yMx) / 2], [1, yMx]].forEach(function(pair) {
    var f = pair[0];
    var v = pair[1];
    ctx.textAlign = 'right';
    ctx.fillText(fmtY(v, opts.unit), Pl - 3, Pt + (1 - f) * ch + 3);
  });

  /* X-axis labels: evenly distributed across the chart width */
  if (ts.length > 1) {
    for (var li = 0; li < maxLbls; li++) {
      var lti = li === maxLbls - 1
        ? ts.length - 1
        : Math.round(li * (ts.length - 1) / (maxLbls - 1));
      ctx.textAlign = li === 0 ? 'left' : li === maxLbls - 1 ? 'right' : 'center';
      ctx.fillText(fmtX(ts[lti], span), tx(lti), h - 4);
    }
  }

  /* Series */
  series.forEach(function(s) {
    if (!s.v.length) return;

    /* Fill area under the line */
    if (s.fill !== false) {
      ctx.beginPath();
      var ok = false;
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
    var ok2 = false;
    s.v.forEach(function(v, i) {
      if (v == null) { ok2 = false; return; }
      if (!ok2) { ctx.moveTo(tx(i), ty(v)); ok2 = true; }
      else      { ctx.lineTo(tx(i), ty(v)); }
    });
    ctx.stroke();
  });

  /* Reference lines — used to draw the critical temperature threshold */
  if (opts.refLines) {
    opts.refLines.forEach(function(rl) {
      var ry = ty(rl.v);
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

/* ── Format helpers ──────────────────────────────────────────────── */

/* Format a Y-axis tick label according to the chart unit */
function fmtY(v, u) {
  if (!u)                     return v < 10 ? v.toFixed(1) : v.toFixed(0);
  if (u === '%')              return v.toFixed(0) + '%';
  if (u === 'C' || u === 'F') return v.toFixed(0) + '°';
  return v < 10 ? v.toFixed(1) : v.toFixed(0);
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

/* Format a temperature value in the configured unit (°C, °F, or %) */
function fmtTempVal(v, unit) {
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

function setCard(id, val, sub, cls) {
  var el = document.getElementById(id);
  el.querySelector('.cval').textContent = val != null ? val : '—';
  var s = el.querySelector('.csub');
  if (s) s.textContent = sub || '';
  el.className = 'card' + (cls ? ' ' + cls : '');
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

  /* Thresholds — server computes these from core count, trip point, and units */
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

  /* Chart visibility & ordering */
  if (d.charts !== undefined) {
    cfgVisCharts = d.charts === null ? null : d.charts;
    var CHART_BOX = {
      cpu_load:  'b-load',
      cpu_usage: 'b-cpu',
      memory:    'b-mem',
      disk:      'b-disk',
      temp:      'b-temp',
      net:       'b-net',
    };
    Object.keys(CHART_BOX).forEach(function(nm) {
      var el  = document.getElementById(CHART_BOX[nm]);
      if (!el) return;
      var idx = cfgVisCharts !== null ? cfgVisCharts.indexOf(nm) : -1;
      /* Hide when excluded; CSS order drives the configured sequence */
      el.classList.toggle('hide', cfgVisCharts !== null && idx === -1);
      el.style.order = (cfgVisCharts !== null && idx !== -1) ? idx : '';
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
  if (pts.some(function(p) { return p.tp != null; }) &&
      (cfgVisCharts === null || cfgVisCharts.indexOf('temp') !== -1)) {
    document.getElementById('b-temp').classList.remove('hide');
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

/* ── Legend build & toggle ───────────────────────────────────────── */

/* Wire handlers onto the pre-declared .leg-item elements in HTML */
function buildLegends() {
  document.querySelectorAll('.legend[data-chart] .leg-item').forEach(function(s) {
    var id  = s.closest('.legend').dataset.chart;
    var idx = parseInt(s.dataset.series, 10);
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
  /* Redraw charts so canvas colors update to the new theme variables */
  renderAll();
}

/* ── Data fetchers ───────────────────────────────────────────────── */

function loadCurrent() {
  fetch('/api/current').then(function(r) {
    if (r.ok) r.json().then(updateCards);
  }).catch(function() {});
}

function loadMetrics() {
  /* Compute how many points the canvas can actually resolve: 1 point per
     4 backing pixels (the threshold where discreteness becomes invisible).
     The server hard-caps at 1440; we clamp to [120, 1440]. */
  var cv     = document.getElementById('g-load');
  var w      = cv ? cv.parentElement.clientWidth - 24 : 800;
  var points = Math.min(1440, Math.max(120, Math.round(w * (devicePixelRatio || 1) / 4)));
  fetch('/api/metrics?range=' + curRange + '&points=' + points).then(function(r) {
    if (!r.ok) return;
    r.json().then(function(d) {
      pts = d.points || [];
      renderAll();
    });
  }).catch(function() {});
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
      loadMetrics();
    };
    el.appendChild(b);
  });
}

/* ── SSE live stream ─────────────────────────────────────────────── */

function connectSSE() {
  var es = new EventSource('/stream');
  es.onmessage = function(e) {
    try {
      updateCards(JSON.parse(e.data));
      loadMetrics();
    } catch (ex) {}
  };
  /* On any error (network drop, server restart) close and reconnect
     after 5 seconds to avoid hammering the server */
  es.onerror = function() {
    es.close();
    setTimeout(connectSSE, 5000);
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
renderAll();
loadCurrent();
loadMetrics();
connectSSE();

/* Debounce canvas redraws on window resize to avoid per-pixel storms */
var resizeTimer;
window.addEventListener('resize', function() {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(renderAll, 100);
});
