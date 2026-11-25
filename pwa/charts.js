// Hand-written SVG charts. The data helpers (ticks, scales, paths) are pure and tested in Node;
// line()/bars()/spark() need a DOM.
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.Charts = factory();
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var NS = 'http://www.w3.org/2000/svg';
  var DEFAULT_GAP_MS = 150000;
  var TIME_STEPS_MIN = [1, 2, 5, 10, 15, 30, 60, 120, 180, 360, 720];

  // ---------- pure helpers ----------
  function niceStep(range, count) {
    var raw = range / Math.max(1, count), mag = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10)), n = raw / mag;
    return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
  }

  function round(v, step) { var d = Math.max(0, -Math.floor(Math.log(step) / Math.LN10) + 1); return +v.toFixed(Math.min(d, 8)); }

  // Axis with round tick values that covers [min, max]. Returns {min, max, step, ticks}.
  function niceTicks(min, max, count) {
    count = count || 4;
    if (!isFinite(min) || !isFinite(max)) { min = 0; max = 1; }
    if (min === max) { var pad = Math.abs(min) > 1 ? Math.abs(min) * 0.1 : 1; min -= pad; max += pad; }
    var step = niceStep(max - min, count);
    var lo = Math.floor(min / step + 1e-9) * step, hi = Math.ceil(max / step - 1e-9) * step, ticks = [];
    for (var v = lo; v <= hi + step * 1e-6; v += step) ticks.push(round(v, step));
    return { min: round(lo, step), max: round(hi, step), step: step, ticks: ticks };
  }

  // Tick times on round local clock values (5 min, 15 min, 1 h, 6 h ...). At most maxCount ticks inside [t0, t1].
  function timeTicks(t0, t1, maxCount) {
    maxCount = maxCount || 5;
    var span = (t1 - t0) / 60000, stepMin = TIME_STEPS_MIN[TIME_STEPS_MIN.length - 1], i;
    for (i = 0; i < TIME_STEPS_MIN.length; i++) if (span / TIME_STEPS_MIN[i] <= maxCount) { stepMin = TIME_STEPS_MIN[i]; break; }
    var step = stepMin * 60000, off = new Date(t0).getTimezoneOffset() * 60000, out = [];
    for (var t = Math.ceil((t0 - off) / step) * step + off; t <= t1; t += step) out.push(t);
    return { ticks: out, stepMin: stepMin };
  }

  // Min and max over the given series' visible points in [t0, t1]; null if there are none.
  function extent(seriesList, t0, t1) {
    var lo = Infinity, hi = -Infinity;
    seriesList.forEach(function (s) {
      s.data.forEach(function (p) { if (p.v !== null && p.t >= t0 && p.t <= t1) { if (p.v < lo) lo = p.v; if (p.v > hi) hi = p.v; } });
    });
    return lo === Infinity ? null : { min: lo, max: hi };
  }

  // Split into continuous runs: null values, connection gaps (p.g) and long time gaps break the line.
  function segments(data, gapMs) {
    gapMs = gapMs || DEFAULT_GAP_MS;
    var out = [], cur = null, prev = null;
    data.forEach(function (p) {
      if (p.v === null || p.v === undefined) { cur = null; prev = null; return; }
      if (!cur || p.g || (prev && p.t - prev.t > gapMs)) { cur = []; out.push(cur); }
      cur.push(p); prev = p;
    });
    return out;
  }

  function f1(n) { return (Math.round(n * 10) / 10).toString(); }

  // xf/yf map data to pixels. A single isolated point becomes a dot-sized segment.
  function linePath(data, xf, yf, gapMs) {
    return segments(data, gapMs).map(function (seg) {
      var d = 'M' + f1(xf(seg[0].t)) + ' ' + f1(yf(seg[0].v));
      if (seg.length === 1) return d + 'h0.1';
      for (var i = 1; i < seg.length; i++) d += 'L' + f1(xf(seg[i].t)) + ' ' + f1(yf(seg[i].v));
      return d;
    }).join('');
  }

  function areaPath(data, xf, yf, baseY, gapMs) {
    return segments(data, gapMs).filter(function (s) { return s.length > 1; }).map(function (seg) {
      var d = 'M' + f1(xf(seg[0].t)) + ' ' + f1(baseY);
      seg.forEach(function (p) { d += 'L' + f1(xf(p.t)) + ' ' + f1(yf(p.v)); });
      return d + 'L' + f1(xf(seg[seg.length - 1].t)) + ' ' + f1(baseY) + 'Z';
    }).join('');
  }

  // Index of the sample closest to time t (data sorted by t), or -1.
  function nearestIndex(data, t) {
    if (!data.length) return -1;
    var lo = 0, hi = data.length - 1;
    while (lo < hi) { var mid = (lo + hi) >> 1; if (data[mid].t < t) lo = mid + 1; else hi = mid; }
    if (lo > 0 && Math.abs(data[lo - 1].t - t) <= Math.abs(data[lo].t - t)) lo--;
    return lo;
  }

  // Display decimation for long ranges: mean per time bucket, nulls and gap markers preserved.
  function bucketize(data, t0, t1, buckets) {
    var w = (t1 - t0) / buckets, out = [], cur = null;
    data.forEach(function (p) {
      if (p.t < t0 || p.t > t1) return;
      var k = Math.floor((p.t - t0) / w);
      if (p.g || !cur || cur.k !== k) {
        if (cur) out.push({ t: cur.ts / cur.n, v: cur.n ? cur.vs / cur.nv : null, g: cur.g });
        cur = { k: k, ts: 0, n: 0, vs: 0, nv: 0, g: p.g ? 1 : 0 };
      }
      cur.ts += p.t; cur.n++;
      if (p.v !== null) { cur.vs += p.v; cur.nv++; }
    });
    if (cur) out.push({ t: cur.ts / cur.n, v: cur.nv ? cur.vs / cur.nv : null, g: cur.g });
    return out.map(function (p) { return { t: Math.round(p.t), v: p.v === null || isNaN(p.v) ? null : p.v, g: p.g }; });
  }

  // ---------- DOM ----------
  function el(name, attrs, parent) {
    var e = document.createElementNS(NS, name);
    for (var k in attrs) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  }

  var uid = 0;

  function frame(host, o) {
    host.textContent = '';
    var w = Math.max(240, Math.floor(host.getBoundingClientRect().width || 320)), h = o.height || 190;
    var svg = el('svg', { viewBox: '0 0 ' + w + ' ' + h, width: w, height: h, role: 'img', 'aria-label': o.label || '' }, host);
    svg.setAttribute('class', o.animate === false ? 'ch' : 'ch enter');
    return { svg: svg, w: w, h: h };
  }

  function emptyChart(host, o, text) {
    var f = frame(host, o);
    el('text', { x: f.w / 2, y: f.h / 2, 'text-anchor': 'middle', class: 'ch-empty' }, f.svg).textContent = text || 'Keine Daten';
  }

  function tip(host) {
    var t = host.querySelector('.tip');
    if (!t) { t = document.createElement('div'); t.className = 'tip'; t.hidden = true; t.setAttribute('aria-hidden', 'true'); host.appendChild(t); }
    return t;
  }

  // Crosshair and tooltip. pick(px) -> {x, title, rows:[{name, text, cls}]} or null.
  function interact(host, svg, plot, pick) {
    var cross = el('line', { class: 'cross', y1: plot.top, y2: plot.bottom, x1: 0, x2: 0 }, svg);
    cross.style.display = 'none';
    var tp = tip(host), hideTimer = 0;
    var ov = el('rect', { x: plot.left, y: plot.top, width: plot.right - plot.left, height: plot.bottom - plot.top, fill: 'transparent', class: 'ov' }, svg);
    function hide() { cross.style.display = 'none'; tp.hidden = true; }
    function show(ev) {
      var r = svg.getBoundingClientRect(), px = ev.clientX - r.left, p = pick(px);
      clearTimeout(hideTimer);
      if (!p) { hide(); return; }
      cross.setAttribute('x1', p.x); cross.setAttribute('x2', p.x); cross.style.display = '';
      tp.textContent = '';
      var t = document.createElement('div'); t.className = 'tip-t'; t.textContent = p.title; tp.appendChild(t);
      p.rows.forEach(function (row) {
        var d = document.createElement('div'); d.className = 'tip-r';
        var sw = document.createElement('i'); sw.className = 'sw'; if (row.color) sw.style.background = row.color; d.appendChild(sw);
        d.appendChild(document.createTextNode(row.name + ' '));
        var b = document.createElement('b'); b.textContent = row.text; d.appendChild(b);
        tp.appendChild(d);
      });
      tp.hidden = false;
      var tw = tp.offsetWidth, left = p.x + 12;
      if (left + tw > r.width - 4) left = p.x - tw - 12;
      tp.style.transform = 'translateX(' + Math.max(4, left) + 'px)';
    }
    ov.addEventListener('pointerdown', show);
    ov.addEventListener('pointermove', show);
    ov.addEventListener('pointerleave', function (ev) { if (ev.pointerType === 'mouse') hide(); });
    ov.addEventListener('pointerup', function (ev) { if (ev.pointerType !== 'mouse') hideTimer = setTimeout(hide, 3000); });
    ov.addEventListener('pointercancel', hide);
  }

  function axes(f, plot, yAxis, xTicks, o) {
    var g = el('g', { class: 'axes' }, f.svg);
    yAxis.ticks.forEach(function (v) {
      var y = plot.yf(v);
      el('line', { x1: plot.left, x2: plot.right, y1: y, y2: y, class: 'gl' }, g);
      el('text', { x: plot.left - 6, y: y + 4, 'text-anchor': 'end', class: 'tick' }, g).textContent = (o.fmtY || String)(v);
    });
    xTicks.forEach(function (t) {
      var x = plot.xf(t);
      el('line', { x1: x, x2: x, y1: plot.bottom, y2: plot.bottom + 4, class: 'axis' }, g);
      el('text', { x: x, y: plot.bottom + 17, 'text-anchor': 'middle', class: 'tick' }, g).textContent = o.fmtX(t);
    });
    el('line', { x1: plot.left, x2: plot.right, y1: plot.bottom, y2: plot.bottom, class: 'axis' }, g);
  }

  function makePlot(f, t0, t1, yAxis, leftPad) {
    var p = { left: leftPad || 40, right: f.w - 10, top: 10, bottom: f.h - 24 };
    p.xf = function (t) { return p.left + (t - t0) / (t1 - t0) * (p.right - p.left); };
    p.yf = function (v) { return p.bottom - (v - yAxis.min) / (yAxis.max - yAxis.min) * (p.bottom - p.top); };
    return p;
  }

  // Line/area chart. o: {height, label, t0, t1, series:[{key,name,color,data,dash,area}], bands, yMin, yMax, fmtY, fmtV, fmtX, emptyText, gapMs}
  // Hidden series are filtered out by the caller. data: [{t, v, g}]
  function line(host, o) {
    var vis = o.series.filter(function (s) { return !s.hidden; });
    var ext = extent(vis, o.t0, o.t1);
    if (!ext) { emptyChart(host, o, o.emptyText); return; }
    var lo = o.yMin !== undefined ? Math.min(o.yMin, ext.min) : ext.min, hi = o.yMax !== undefined ? Math.max(o.yMax, ext.max) : ext.max;
    if (o.yMin !== undefined && o.yMinFixed) lo = o.yMin;
    var yAxis = niceTicks(lo, hi, 4), f = frame(host, o);
    var plot = makePlot(f, o.t0, o.t1, yAxis, o.leftPad);
    var id = 'clip' + (++uid);
    var defs = el('defs', {}, f.svg);
    el('rect', { x: plot.left, y: plot.top - 2, width: plot.right - plot.left, height: plot.bottom - plot.top + 4 }, el('clipPath', { id: id }, defs));
    (o.bands || []).forEach(function (b) {
      var x0 = Math.max(plot.left, plot.xf(b[0])), x1 = Math.min(plot.right, plot.xf(b[1]));
      if (x1 > x0 || (b[1] >= o.t0 && b[0] <= o.t1)) el('rect', { x: x0, y: plot.top, width: Math.max(2, x1 - x0), height: plot.bottom - plot.top, class: 'band' }, f.svg);
    });
    axes(f, plot, yAxis, timeTicks(o.t0, o.t1, Math.floor((plot.right - plot.left) / 62)).ticks, { fmtY: o.fmtY, fmtX: o.fmtX });
    var g = el('g', { 'clip-path': 'url(#' + id + ')' }, f.svg);
    vis.forEach(function (s) {
      var st = 'color:' + s.color;
      if (s.area) el('path', { d: areaPath(s.data, plot.xf, plot.yf, plot.bottom, o.gapMs), class: 'ar', style: st }, g);
      el('path', { d: linePath(s.data, plot.xf, plot.yf, o.gapMs), class: 'ln' + (s.dash ? ' dash' : ''), style: st }, g);
    });
    interact(host, f.svg, plot, function (px) {
      var t = o.t0 + (px - plot.left) / (plot.right - plot.left) * (o.t1 - o.t0), rows = [], best = null;
      vis.forEach(function (s) {
        var i = nearestIndex(s.data, t);
        if (i < 0 || Math.abs(s.data[i].t - t) > 120000 + (o.t1 - o.t0) / 200) return;
        var p = s.data[i];
        if (best === null || Math.abs(p.t - t) < Math.abs(best - t)) best = p.t;
        rows.push({ name: s.name, text: p.v === null ? '–' : (o.fmtV || String)(p.v), color: s.color });
      });
      if (!rows.length) return null;
      return { x: plot.xf(best), title: o.fmtX(best), rows: rows };
    });
  }

  // Bars for heating runs. o: {t0, t1, runs:[{t0,t1,ml}], color, fmtX, fmtV, unit}
  function bars(host, o) {
    var runs = o.runs.filter(function (r) { return r.t1 >= o.t0 && r.t0 <= o.t1; });
    if (!runs.length) { emptyChart(host, o, o.emptyText); return; }
    var max = 0; runs.forEach(function (r) { if (r.ml > max) max = r.ml; });
    var yAxis = niceTicks(0, max, 3), f = frame(host, o), plot = makePlot(f, o.t0, o.t1, yAxis, o.leftPad);
    axes(f, plot, yAxis, timeTicks(o.t0, o.t1, Math.floor((plot.right - plot.left) / 62)).ticks, { fmtY: o.fmtY, fmtX: o.fmtX });
    var bw = Math.max(6, Math.min(26, (plot.right - plot.left) / Math.max(8, runs.length * 2.2)));
    var geo = runs.map(function (r) {
      var mid = plot.xf((r.t0 + r.t1) / 2), x = Math.max(plot.left, Math.min(plot.right - bw, mid - bw / 2)), y = plot.yf(r.ml);
      var rr = Math.min(4, bw / 2, plot.bottom - y);
      el('path', { d: 'M' + f1(x) + ' ' + f1(plot.bottom) + 'V' + f1(y + rr) + 'Q' + f1(x) + ' ' + f1(y) + ' ' + f1(x + rr) + ' ' + f1(y) + 'H' + f1(x + bw - rr) + 'Q' + f1(x + bw) + ' ' + f1(y) + ' ' + f1(x + bw) + ' ' + f1(y + rr) + 'V' + f1(plot.bottom) + 'Z', class: 'bar', style: 'color:' + o.color }, f.svg);
      if (runs.length <= 6) el('text', { x: x + bw / 2, y: y - 4, 'text-anchor': 'middle', class: 'tick' }, f.svg).textContent = o.fmtV(r.ml);
      return { x: x + bw / 2, r: r };
    });
    interact(host, f.svg, plot, function (px) {
      var best = null;
      geo.forEach(function (g) { if (best === null || Math.abs(g.x - px) < Math.abs(best.x - px)) best = g; });
      return best ? { x: best.x, title: o.fmtX(best.r.t0), rows: [{ name: 'Lauf', text: o.fmtV(best.r.ml), color: o.color }] } : null;
    });
  }

  // Tiny trend line for the tiles. o: {t0, t1, color, w, h}
  function spark(host, data, o) {
    host.textContent = '';
    var w = o.w || Math.max(60, host.clientWidth || 100), h = o.h || 26;
    var pts = data.filter(function (p) { return p.t >= o.t0 && p.v !== null; });
    if (pts.length < 2) return;
    var ext = extent([{ data: pts }], o.t0, o.t1), yAxis = { min: ext.min, max: ext.max === ext.min ? ext.min + 1 : ext.max };
    var xf = function (t) { return 2 + (t - o.t0) / (o.t1 - o.t0) * (w - 4); };
    var yf = function (v) { return h - 3 - (v - yAxis.min) / (yAxis.max - yAxis.min) * (h - 6); };
    var svg = el('svg', { viewBox: '0 0 ' + w + ' ' + h, width: w, height: h, 'aria-hidden': 'true', class: 'spark' }, host);
    el('path', { d: linePath(pts, xf, yf, o.gapMs), class: 'ln', style: 'color:' + o.color }, svg);
    var last = pts[pts.length - 1];
    el('circle', { cx: xf(last.t), cy: yf(last.v), r: 2.5, style: 'fill:' + o.color }, svg);
  }

  return {
    niceStep: niceStep, niceTicks: niceTicks, timeTicks: timeTicks, extent: extent, segments: segments, linePath: linePath,
    areaPath: areaPath, nearestIndex: nearestIndex, bucketize: bucketize, line: line, bars: bars, spark: spark
  };
}));
