/* ── Thresholds & colors ─────────────────────────────────────────── */

/* [warn, critical] boundaries used to color-code card values */
var THRESH = {
  load: [2, 3.5],
  cpu:  [70, 90],
  mem:  [70, 90],
  disk: [75, 90],
  temp: [70, 80],
};

/* Chart series colours — purely for identification, no semantic meaning.
 * Semantic status (good/warn/critical) uses --grn/--ylw/--red from CSS.
 * C1 sky-400   (#38bdf8): load1, user, mem, disk, tx
 * C2 violet-400 (#a78bfa): load5, sys, memAvail, diskFree, rx
 * C3 slate-400 (#94a3b8): load15, temp */
var CLR = {
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

/* ── State ───────────────────────────────────────────────────────── */

var curRange     = '1d';    /* currently selected time range */
var pts          = [];      /* array of data points from /api/metrics */
var tempCritical = null;    /* sysfs trip-point; drawn as a red dashed line */
/* Which sub-metric is shown as the primary value in each card.
   0 = first option, 1 = second; toggled by clicking the sub-value. */
var cardPrimary  = { load: 0, cpu: 0, mem: 0, disk: 0, net: 0 };
var lastCurrent  = null;    /* last /current snapshot; replayed on swapCard */

/* Units read from server config; sensible defaults until first /current */
var cfgCardUnits  = { mem: '%',  disk: '%',  temp: 'c', net: 'mb', load: 'abs' };
var cfgChartUnits = { mem: 'mb', disk: 'gb', temp: 'c', net: 'mb', load: 'abs' };
var cfgUptimeUnit = 'auto';
var cfgRanges     = ['1d', '7d', '30d', '90d'];
/* Three-state visibility: null = show all, [] = hide all, [...] = listed only */
var cfgVisCharts  = null;
var cfgVisCards   = null;

/* ── Chart legend definitions ────────────────────────────────────── */

/* Entries for each chart's interactive legend (temperature has none) */
var LEGENDS = {
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
     chart looks sharp on retina / HiDPI screens */
  var dpr = devicePixelRatio || 1;
  var w   = cv.parentElement.clientWidth - 24;
  var h   = 110;
  cv.width        = w * dpr;
  cv.height       = h * dpr;
  cv.style.width  = w + 'px';
  cv.style.height = h + 'px';

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

  /* Grid lines */
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
  [[0, yMn], [0.5, (yMn + yMx) / 2], [1, yMx]].forEach(function(pair) {
    var f = pair[0];
    var v = pair[1];
    ctx.textAlign = 'right';
    ctx.fillText(fmtY(v, opts.unit), Pl - 3, Pt + (1 - f) * ch + 3);
  });

  /* X-axis: first and last timestamp */
  var ts = opts.ts || [];
  if (ts.length > 1) {
    ctx.textAlign = 'left';
    ctx.fillText(fmtT(ts[0]), Pl, h - 4);
    ctx.textAlign = 'right';
    ctx.fillText(fmtT(ts[ts.length - 1]), Pl + cw, h - 4);
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

/* Format a Unix timestamp as HH:MM */
function fmtT(t) {
  if (!t) return '';
  var d = new Date(t * 1000);
  return d.getHours().toString().padStart(2, '0') + ':' +
         d.getMinutes().toString().padStart(2, '0');
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

/* Format a network throughput value (MB/s internally) into the
   configured display unit, automatically scaling to KB when small */
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

/* Map a status level to its CSS variable (used for inline sub-value spans) */
var lvlClr = { g: 'var(--grn)', y: 'var(--ylw)', r: 'var(--red)', '': 'inherit' };

/* Swap which sub-metric (0 or 1) is shown as primary in a card.
   Replays lastCurrent so the change is visible immediately. */
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

  /* Card visibility & ordering */
  if (d.cards !== undefined) {
    cfgVisCards = d.cards === null ? null : d.cards;
    var CARD_EL = {
      cpu_load:  'c-load',
      cpu_usage: 'c-cpu',
      memory:    'c-mem',
      disk:      'c-disk',
      temp:      'c-temp',
      net:       'c-net',
      uptime:    'upt',
    };
    Object.keys(CARD_EL).forEach(function(nm) {
      var el       = document.getElementById(CARD_EL[nm]);
      if (!el) return;
      var idx      = cfgVisCards !== null ? cfgVisCards.indexOf(nm) : -1;
      var excluded = cfgVisCards !== null && idx === -1;
      if (excluded)           el.style.display = 'none';
      /* Temperature visibility is driven by data (null sensor → hidden),
         not by the cards config, so we never force it to display:'' here */
      else if (nm !== 'temp') el.style.display = '';
      el.style.order = (!excluded && idx !== -1) ? idx : '';
    });
  }

  /* CPU Load */
  var loadFmt = cfgCardUnits.load === '%'
    ? function(v) { return v.toFixed(1) + '%'; }
    : function(v) { return v.toFixed(2); };
  var loadS = [
    { label: '1m',  c: CLR.load1,  v: d.load_1m,  fmt: loadFmt },
    { label: '5m',  c: CLR.load5,  v: d.load_5m,  fmt: loadFmt },
    { label: '15m', c: CLR.load15, v: d.load_15m, fmt: loadFmt },
  ];
  var lp = cardPrimary.load;
  var lc = document.getElementById('c-load');
  var lcLvl = cardLevel(d.load_1m, THRESH.load);
  lc.className = 'card' + (lcLvl ? ' ' + lcLvl : '');
  lc.querySelector('.clabel').innerHTML =
    'CPU Load <span style="color:' + loadS[lp].c +
    ';font-size:10px">' + loadS[lp].label + '</span>';
  lc.querySelector('.cval').textContent =
    loadS[lp].v != null ? loadS[lp].fmt(loadS[lp].v) : '—';
  var loadSub = '';
  loadS.forEach(function(s, i) {
    if (i === lp || s.v == null) return;
    var vc = lvlClr[cardLevel(s.v, THRESH.load)];
    loadSub += (loadSub ? '&ensp;' : '') +
      subSpan(s.label, s.c,
        '<span style="color:' + vc + '">' + s.fmt(s.v) + '</span>',
        'swapCard(\'load\',' + i + ')');
  });
  lc.querySelector('.csub').innerHTML = loadSub;

  /* CPU Usage (absent on the first collect before a delta is available) */
  if (d.cpu_user_percent != null) {
    var pctFmt = function(v) { return v.toFixed(1) + '%'; };
    var cpuS = [
      { label: 'user', c: CLR.user, v: d.cpu_user_percent,   fmt: pctFmt },
      { label: 'sys',  c: CLR.sys,  v: d.cpu_system_percent, fmt: pctFmt },
    ];
    var cp    = cardPrimary.cpu;
    var cc    = document.getElementById('c-cpu');
    var ccLvl = cardLevel(d.cpu_user_percent, THRESH.cpu);
    cc.className = 'card' + (ccLvl ? ' ' + ccLvl : '');
    cc.querySelector('.clabel').innerHTML =
      'CPU Usage <span style="color:' + cpuS[cp].c +
      ';font-size:10px">' + cpuS[cp].label + '</span>';
    cc.querySelector('.cval').textContent = cpuS[cp].v != null ? cpuS[cp].fmt(cpuS[cp].v) : '—';
    var cpuSub = '';
    cpuS.forEach(function(s, i) {
      if (i === cp || s.v == null) return;
      cpuSub = subSpan(s.label, s.c, s.fmt(s.v), 'swapCard(\'cpu\',' + i + ')');
    });
    cc.querySelector('.csub').innerHTML = cpuSub;
  }

  /* Memory */
  if (d.mem_percent != null) {
    /* When unit is absolute (MB/GB), show raw bytes; otherwise show % */
    var memIsAbs = cfgCardUnits.mem !== '%';
    var memFmt = memIsAbs
      ? function(v) {
          if (v == null) return '—';
          if (cfgCardUnits.mem === 'gb') return (v / 1024).toFixed(2) + ' GB';
          return v.toFixed(0) + ' MB';
        }
      : function(v) { return v.toFixed(1) + '%'; };
    var memS = memIsAbs ? [
      { label: 'used',  c: CLR.mem,      v: d.mem_used,      fmt: memFmt },
      { label: 'avail', c: CLR.memAvail, v: d.mem_available, fmt: memFmt },
    ] : [
      { label: 'used',  c: CLR.mem,      v: d.mem_percent,       fmt: memFmt },
      { label: 'avail', c: CLR.memAvail, v: 100 - d.mem_percent, fmt: memFmt },
    ];
    var mp = cardPrimary.mem;
    var mc = document.getElementById('c-mem');
    var mcLvl = cardLevel(d.mem_percent, THRESH.mem);
    mc.className = 'card' + (mcLvl ? ' ' + mcLvl : '');
    mc.querySelector('.clabel').innerHTML =
      'Memory <span style="color:' + memS[mp].c +
      ';font-size:10px">' + memS[mp].label + '</span>';
    mc.querySelector('.cval').textContent = memS[mp].fmt(memS[mp].v);
    var memSub = '';
    memS.forEach(function(s, i) {
      if (i === mp) return;
      var mvc = lvlClr[cardLevel(d.mem_percent, THRESH.mem)];
      memSub = subSpan(s.label, s.c,
        '<span style="color:' + mvc + '">' + s.fmt(s.v) + '</span>',
        'swapCard(\'mem\',' + i + ')');
    });
    mc.querySelector('.csub').innerHTML = memSub;
  }

  /* Disk */
  if (d.disk_percent != null) {
    var diskIsAbs = cfgCardUnits.disk !== '%';
    var diskFmt = diskIsAbs
      ? function(v) {
          if (v == null) return '—';
          if (cfgCardUnits.disk === 'tb') return (v / 1000).toFixed(2) + ' TB';
          return v.toFixed(1) + ' GB';
        }
      : function(v) { return v.toFixed(1) + '%'; };
    var diskS = diskIsAbs ? [
      { label: 'used', c: CLR.disk,     v: d.disk_used, fmt: diskFmt },
      { label: 'free', c: CLR.diskFree, v: d.disk_free, fmt: diskFmt },
    ] : [
      { label: 'used', c: CLR.disk,     v: d.disk_percent,       fmt: diskFmt },
      { label: 'free', c: CLR.diskFree, v: 100 - d.disk_percent, fmt: diskFmt },
    ];
    var dkp  = cardPrimary.disk;
    var dcel = document.getElementById('c-disk');
    var dcelLvl = cardLevel(d.disk_percent, THRESH.disk);
    dcel.className = 'card' + (dcelLvl ? ' ' + dcelLvl : '');
    dcel.querySelector('.clabel').innerHTML =
      'Disk <span style="color:' + diskS[dkp].c +
      ';font-size:10px">' + diskS[dkp].label + '</span>';
    dcel.querySelector('.cval').textContent = diskS[dkp].fmt(diskS[dkp].v);
    var diskSub = '';
    diskS.forEach(function(s, i) {
      if (i === dkp) return;
      var dvc = lvlClr[cardLevel(d.disk_percent, THRESH.disk)];
      diskSub = subSpan(s.label, s.c,
        '<span style="color:' + dvc + '">' + s.fmt(s.v) + '</span>',
        'swapCard(\'disk\',' + i + ')');
    });
    dcel.querySelector('.csub').innerHTML = diskSub;
  }

  /* Temperature — card is shown only when the server sends a non-null
     value, meaning a real sensor was found at collection time */
  if (d.temp != null && (cfgVisCards === null || cfgVisCards.indexOf('temp') !== -1)) {
    document.getElementById('c-temp').style.display = '';
    var tempLvl = cardLevel(d.temp, THRESH.temp);
    setCard('c-temp', fmtTempVal(d.temp, cfgCardUnits.temp), null, tempLvl);
  }
  /* Store the critical trip-point for the chart reference line */
  if (d.temp_critical != null) tempCritical = d.temp_critical;

  /* Network */
  if (d.net_rx != null) {
    var netS = [
      { label: '↑', c: CLR.tx, v: d.net_tx },
      { label: '↓', c: CLR.rx, v: d.net_rx },
    ];
    var np = cardPrimary.net;
    var nc = document.getElementById('c-net');
    nc.className = 'card';
    nc.querySelector('.clabel').innerHTML =
      'Network <span style="color:' + netS[np].c +
      ';font-size:10px">' + netS[np].label + '</span>';
    nc.querySelector('.cval').textContent = fmtNet(netS[np].v, cfgCardUnits.net);
    var netSub = '';
    netS.forEach(function(s, i) {
      if (i === np) return;
      var oc = 'swapCard(\'net\',' + i + ')';
      netSub = subSpan(s.label, s.c, fmtNet(s.v, cfgCardUnits.net), oc);
    });
    nc.querySelector('.csub').innerHTML = netSub;
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
    'mb': 'MB/s', 'gb': 'GB/s', 'mbps': 'Mbps', 'gbps': 'Gbps',
  }[cfgChartUnits.net] || 'MB/s';

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

/* Inject a legend strip into each chart header at startup */
function buildLegends() {
  Object.keys(LEGENDS).forEach(function(id) {
    var hdr  = document.getElementById(id).parentElement.querySelector('.chdr');
    var wrap = document.createElement('div');
    wrap.className = 'legend';
    wrap.id        = 'leg-' + id;
    LEGENDS[id].forEach(function(item, idx) {
      var s = document.createElement('span');
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
  var items = document.getElementById('leg-' + id).querySelectorAll('.leg-item');
  items[idx].classList.toggle('off', seriesHidden[id][idx]);
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
  /* The default dashboard targets 480 points per chart — 1 point per 4
     backing pixels at 1920×1080 fullscreen, the threshold where the eye no
     longer sees discreteness. A custom dashboard can compute its own value
     from the canvas width and pass it via the points query parameter; the
     server caps it at 5120 to bound query memory. */
  fetch('/api/metrics?range=' + curRange + '&points=480').then(function(r) {
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
