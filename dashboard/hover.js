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

/* Chart interaction layer: the shared hover crosshair, tooltip and click-to-lock
 * machinery, split from chart.js. Owns the pointer/lock state; redraws through
 * drawChart and the chartCache that chart.js owns. bundle.sh inlines format.js,
 * chart.js, this, cards.js, then app.js (one shared global scope). */
/* global fmtTip, fmtXFull */
/* global drawChart, chartCache, pts */
/* exported attachHover, clearAllHovers, canHover, refreshLockedIdx, scheduleHoverDraw, lockedIdx */

/* --- Hover crosshair + tooltip --- */

/* Position the tooltip near the cursor, reading the values at `idx` from the
 * cached chart's series. Flips to the opposite side if it would clip the edge. */
function showTooltip(idx, mx, my, cached) {
  const tt   = document.getElementById('tt');
  const ts   = (cached.opts.ts || [])[idx];
  const span = cached.opts.ts && cached.opts.ts.length > 1
    ? cached.opts.ts[cached.opts.ts.length - 1] - cached.opts.ts[0]
    : 0;
  const fmt  = cached.opts.fmtFn || ((v) => fmtTip(v, cached.opts.unit));
  const tpl = document.getElementById('tt-row');
  tt.textContent = '';
  if (ts) {
    const time = document.createElement('div');
    time.className = 'tt-time';
    time.textContent = fmtXFull(ts, span);
    tt.appendChild(time);
  }
  cached.series.forEach((s) => {
    const v = s.v[idx];
    /* Show every series, falling back to the em dash placeholder when a point
     * has no value (e.g. a temp chart with no sensor), matching the cards. */
    const row = tpl.content.cloneNode(true);
    row.querySelector('.tt-dot').className = 'tt-dot tt-dot--' + s.k;
    const lbl = row.querySelector('.tt-lbl');
    if (s.label) lbl.textContent = s.label;
    else lbl.remove();
    row.querySelector('.tt-val').textContent = v == null ? '—' : fmt(v);
    tt.appendChild(row);
  });
  if (!tt.hasChildNodes()) { tt.classList.add('hide'); return; }
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
  requestAnimationFrame(() => {
    hoverRafScheduled = false;
    /* Lock wins over transient hover. */
    const activeIdx = lockedIdx != null ? lockedIdx : hoverIdxPending;
    const locked    = lockedIdx != null;
    Object.keys(chartCache).forEach((id) => {
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

  cv.addEventListener('mousemove', (e) => {
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

  cv.addEventListener('mouseleave', () => {
    if (lockedIdx != null) hideTooltip();  /* keep the locked crosshair */
    else                   clearAllHovers();
  });

  cv.addEventListener('click', (e) => {
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
