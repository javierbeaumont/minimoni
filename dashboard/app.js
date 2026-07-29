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

/* App shell: data fetch, range tabs, SSE and init. The formatting helpers live in
 * format.js, the chart subsystem in chart.js, the cards + config state in cards.js;
 * bundle.sh inlines them in that order before this file (one shared global scope). */
/* global metricsUrl, clampPoints */
/* global renderAll, buildLegends, attachHover, clearAllHovers, canHover, invalidateCssCache */
/* global updateCards, wireCards, cfgRanges */
/* exported pts */

/* --- State --- */

let curRange = '1d';    /* currently selected time range */
let pts      = [];      /* array of data points from /api/metrics */

/* --- Theme toggle --- */

function toggleTheme() {
  const html = document.documentElement;
  /* Resolve the EFFECTIVE current theme, not just data-theme: on first load
   * data-theme is unset and the theme comes from prefers-color-scheme, so
   * flipping the raw attribute would set it to what is already shown (no visible
   * change on the first click). Fall back to the OS preference when unset. */
  const current = html.dataset.theme
    || (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
  html.dataset.theme = current === 'dark' ? 'light' : 'dark';
  /* The sun/moon icon follows the theme in CSS (see #thm .ico-*); JS only flips
   * data-theme. Theme variables changed: drop the cached values so cssv()
   * re-reads them. */
  invalidateCssCache();
  renderAll();
}

/* --- Data fetchers --- */

function loadCurrent() {
  fetch('api/current').then((r) => {
    if (r.ok) r.json().then(updateCards);
  }).catch(() => {});
}

let metricsRequestId = 0;
function loadMetrics() {
  const myId = ++metricsRequestId;
  const cv     = document.getElementById('g-load');
  const w      = cv ? cv.parentElement.clientWidth - 24 : 800;
  const points = clampPoints(w, devicePixelRatio);
  fetch(metricsUrl(curRange, points)).then((r) => {
    if (!r.ok) return;
    r.json().then((d) => {
      if (myId !== metricsRequestId) return;   /* discard stale responses */
      pts = d.points || [];
      renderAll();
    });
  }).catch(() => {});
}

/* --- Range tabs --- */

function buildTabs() {
  const el = document.getElementById('rngs');
  el.innerHTML = '';
  if (cfgRanges.indexOf(curRange) === -1) curRange = cfgRanges[0];
  const sel = document.createElement('select');
  sel.className = 'range-select';
  sel.setAttribute('aria-label', 'Time range');
  cfgRanges.forEach((r) => {
    const o = document.createElement('option');
    o.value = r;
    o.textContent = r;
    if (r === curRange) o.selected = true;
    sel.appendChild(o);
  });
  sel.onchange = () => {
    curRange = sel.value;
    loadMetrics();
  };
  el.appendChild(sel);
}

/* --- SSE live stream --- */

/* Exponential reconnect backoff: 1s -> 2s -> 4s -> 8s -> 16s -> 32s -> 64s, then
 * stays at 64s. Resets to 1s as soon as a message lands on the newly opened
 * connection, so a quick server hiccup recovers fast but a permanently down
 * server is not hammered. */
let sseBackoffMs = 1000;
const SSE_BACKOFF_MAX = 64000;
let sseCountdownTimer = null;

/* Live-tick the aria-label on the .conn dot so the hover tooltip (rendered via
 * content: attr(aria-label)) shows a per-second countdown to the next reconnect
 * attempt. Without it the user sees a static "in 32s" for 32 seconds with no
 * sign of progress. */
function startReconnectCountdown(targetMs) {
  clearInterval(sseCountdownTimer);
  const c = document.getElementById('conn');
  const tick = () => {
    const remaining = Math.max(0, Math.ceil((targetMs - Date.now()) / 1000));
    c.setAttribute('aria-label', 'Connection lost, reconnecting in ' + remaining + 's');
    if (remaining <= 0) clearInterval(sseCountdownTimer);
  };
  tick();
  sseCountdownTimer = setInterval(tick, 1000);
}

function connectSSE() {
  const c = document.getElementById('conn');
  /* A (re)connect attempt is in flight: stop the countdown and show a generic
   * Connecting label until the first message lands (or we error). */
  clearInterval(sseCountdownTimer);
  c.setAttribute('aria-label', 'Connecting...');

  const es = new EventSource('stream');
  es.onmessage = (e) => {
    clearInterval(sseCountdownTimer);   /* data landed: stop any countdown tick */
    sseBackoffMs = 1000;                /* successful message: reset for next time */
    c.className = 'conn live';
    c.setAttribute('aria-label', 'Live: receiving updates');
    try {
      updateCards(JSON.parse(e.data));
      loadMetrics();
    } catch (e) { /* ignore a malformed SSE frame */ }
  };
  es.onerror = () => {
    es.close();
    c.className = 'conn down';
    startReconnectCountdown(Date.now() + sseBackoffMs);
    setTimeout(connectSSE, sseBackoffMs);
    sseBackoffMs = Math.min(sseBackoffMs * 2, SSE_BACKOFF_MAX);
  };
}

/* --- Init --- */

/* Browser entry point: run the dashboard only inside a real document. Guarded
 * so the test harness (node) can require this file for its pure helpers without
 * a DOM present. */
if (typeof window !== 'undefined' && typeof document !== 'undefined') {
  document.getElementById('thm').addEventListener('click', toggleTheme);
  /* OS-level theme changes (prefers-color-scheme) swap the CSS variables when no
   * explicit data-theme is set: invalidate the cache and redraw. */
  window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
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
  window.addEventListener('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(renderAll, 100);
  });
}
