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

/* Pure formatting + axis helpers, split from app.js so they can be unit-tested
 * without a DOM. bundle.sh inlines this before app.js (shared global scope). */

/* --- Format helpers --- */

/* Y-axis tick label. Unitless: the unit lives in the chart title, so ticks are
 * bare numbers (the tooltip carries the unit, fmtTip). */
function fmtY(v, decimals) {
  const d = decimals != null ? decimals : (v < 10 ? 1 : 0);
  return v.toFixed(d);
}

/* Tooltip value: one decimal finer than fmtY (axis stays coarse, tooltip detailed). */
function fmtTip(v, u) {
  if (u === '%')              return v.toFixed(1) + '%';
  if (u === 'C' || u === 'F') return v.toFixed(1) + '°';
  return v < 10 ? v.toFixed(2) : v.toFixed(1);
}

/* X-axis label by data span (s). Boundaries are inclusive: a span of exactly
 * 86400 s is HH:MM, not Mon DD. */
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

/* Fuller timestamp for the hover tooltip (fmtX is the terse axis form). */
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

/* Uptime string; 'auto' picks days/hours/minutes by magnitude. */
function fmtUptime(s, unit) {
  if (unit === 'd') return 'up ' + (s / 86400).toFixed(1) + 'd';
  if (unit === 'h') return 'up ' + Math.floor(s / 3600) + 'h';
  const d = Math.floor(s / 86400);
  const h = Math.floor(s % 86400 / 3600);
  const m = Math.floor(s % 3600 / 60);
  if (d > 0) return 'up ' + d + 'd ' + h + 'h';
  if (h > 0) return 'up ' + h + 'h ' + m + 'm';
  return 'up ' + m + 'm';
}

/* Throughput in the fixed display unit: the chosen unit NEVER rescales (0.5 mb
 * stays "0.500 MB/s", not "512 KB/s"). Precision adapts to ~4 significant digits. */
function fmtNet(v, unit) {
  if (v == null) return '—';
  if (unit === '%') return v.toFixed(1) + '%'; /* link-speed percent: one decimal like the others */
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

function fmtTempVal(v, unit) {
  if (v == null) return '—';
  if (!unit || unit[0] === 'c') return v.toFixed(1) + '°C';
  if (unit[0] === 'f')          return v.toFixed(1) + '°F';
  return v.toFixed(1) + '%';
}

/* Threshold -> 'g' / 'y' / 'r' (good / warning / critical). */
function cardLevel(v, thresh) {
  if (v == null) return '';
  return v >= thresh[1] ? 'r' : v >= thresh[0] ? 'y' : 'g';
}

/* --- Chart axis helpers --- */

/* Round x to a "nice" 1/2/5/10 x10^n: `round` = nearest, else smallest >= x. */
function niceNum(x, round) {
  const exp = Math.floor(Math.log10(x));
  const f   = x / Math.pow(10, exp);
  const nf  = round
    ? (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10)
    : (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10);
  return nf * Math.pow(10, exp);
}

/* Y-axis ticks 0..nice-bound, ~maxTicks steps (1883,5 -> [0,500,1000,1500,2000]). */
function niceTicks(max, maxTicks) {
  if (!(max > 0)) return [0, 1];
  const step = niceNum(niceNum(max, false) / Math.max(1, maxTicks - 1), true);
  const top  = Math.ceil(max / step) * step;
  const out  = [];
  for (let i = 0; i * step <= top + step * 1e-9; i++) out.push(i * step);
  return out;
}

/* Nice time-axis steps (seconds): round clock intervals from 1 s to 2 weeks. */
const TIME_STEPS = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800,
                    3600, 7200, 10800, 14400, 21600, 43200,
                    86400, 172800, 604800, 1209600];

/* X-axis tick times: a nice step aligned to local midnight (00:00, 04:00, ...). */
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

/* --- URL / points --- */

function metricsUrl(range, points) {
  return 'api/metrics?range=' + range + '&points=' + points;
}

/* 1 point per 4 backing px (finer is invisible), clamped to [120, 1440] (server cap). */
function clampPoints(width, dpr) {
  return Math.min(1440, Math.max(120, Math.round(width * (dpr || 1) / 4)));
}

/* Exports for the test harness (no-op in the browser). */
/* global module */
if (typeof module !== 'undefined' && module.exports) {
  module.exports = {
    fmtY: fmtY,
    fmtTip: fmtTip,
    fmtX: fmtX,
    fmtXFull: fmtXFull,
    fmtUptime: fmtUptime,
    fmtNet: fmtNet,
    fmtTempVal: fmtTempVal,
    cardLevel: cardLevel,
    niceTicks: niceTicks,
    timeTicks: timeTicks,
    metricsUrl: metricsUrl,
    clampPoints: clampPoints
  };
}
