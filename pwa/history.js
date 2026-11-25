// History logic (ring buffer, downsampling, run detection, CSV) and an optional IndexedDB store.
// The logic is pure and runs in Node; the store is the only part that touches the browser.
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.Hist = factory();
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var FIELDS = ['phase', 'powerW', 'fuelUsedMl', 'coolant', 'interior', 'outside', 'voltage', 'climateStatus', 'targetC'];
  var AVG = ['powerW', 'coolant', 'interior', 'outside', 'voltage'];
  var GAP_MS = 150000; // lines break when two samples are further apart than this
  var DEFAULTS = { intervalMs: 5000, fullMs: 2 * 3600000, maxMs: 24 * 3600000, bucketMs: 60000, maxSamples: 17280 };
  var PHASE_HEATING = 2, PHASE_RUNON = 3;

  function num(v) { return typeof v === 'number' && isFinite(v) ? v : null; }

  // Sample: {t, g (1 = first sample after a connection gap), n (weight, >1 for downsampled), ...FIELDS}; null = unknown.
  function norm(s) {
    var o = { t: s.t, g: s.g ? 1 : 0, n: s.n > 0 ? s.n : 1 };
    for (var i = 0; i < FIELDS.length; i++) o[FIELDS[i]] = num(s[FIELDS[i]]);
    return o;
  }

  function fromStatus(s, t) {
    var climate = !!s.climateActive || (s.climateStatus >= 2 && s.climateStatus <= 4);
    var known = s.phase !== 0;
    return norm({
      t: t, phase: s.phase, powerW: known ? s.powerW : null, fuelUsedMl: known ? s.fuelUsedMl : null,
      coolant: s.coolant, interior: s.interior, outside: s.outside, voltage: s.voltage,
      climateStatus: s.climateStatus, targetC: climate && s.targetC > 0 ? s.targetC : null
    });
  }

  function dominant(group, key) {
    var w = {}, best = null, bw = -1;
    group.forEach(function (s) { if (s[key] !== null) w[s[key]] = (w[s[key]] || 0) + s.n; });
    group.forEach(function (s) { if (s[key] !== null && w[s[key]] >= bw) { bw = w[s[key]]; best = s[key]; } });
    return best;
  }

  // Merge consecutive samples into one: weighted mean for gauges, max for the fuel counter, dominant phase.
  function merge(group) {
    if (group.length === 1) return group[0];
    var o = { g: 0, n: 0 }, tw = 0, i, k;
    group.forEach(function (s) { o.n += s.n; tw += s.t * s.n; if (s.g) o.g = 1; });
    o.t = Math.round(tw / o.n);
    for (i = 0; i < AVG.length; i++) {
      k = AVG[i];
      var sum = 0, w = 0;
      group.forEach(function (s) { if (s[k] !== null) { sum += s[k] * s.n; w += s.n; } });
      o[k] = w ? sum / w : null;
    }
    var fuel = null;
    group.forEach(function (s) { if (s.fuelUsedMl !== null && (fuel === null || s.fuelUsedMl > fuel)) fuel = s.fuelUsedMl; });
    o.fuelUsedMl = fuel;
    o.phase = dominant(group, 'phase');
    o.climateStatus = dominant(group, 'climateStatus');
    o.targetC = null;
    group.forEach(function (s) { if (s.targetC !== null) o.targetC = s.targetC; });
    return o;
  }

  // Full resolution for the last fullMs, bucketMs averages up to maxMs, older data dropped.
  function compactList(arr, now, o) {
    var cutoff = now - o.fullMs, minT = now - o.maxMs, out = [], group = [], key = null;
    function flush() { if (group.length) out.push(merge(group)); group = []; key = null; }
    for (var i = 0; i < arr.length; i++) {
      var s = arr[i];
      if (s.t < minT) continue;
      if (s.t >= cutoff) { flush(); out.push(s); continue; }
      var k = Math.floor(s.t / o.bucketMs);
      if (key !== null && k !== key) flush();
      key = k; group.push(s);
    }
    flush();
    return out;
  }

  function pack(s) { return [s.t].concat(FIELDS.map(function (f) { return s[f]; }), [s.g, s.n]); }
  function unpack(r) {
    if (!Array.isArray(r) || typeof r[0] !== 'number' || !isFinite(r[0])) return null;
    var o = { t: r[0], g: r[FIELDS.length + 1], n: r[FIELDS.length + 2] };
    for (var i = 0; i < FIELDS.length; i++) o[FIELDS[i]] = r[i + 1];
    return norm(o);
  }

  function create(opts) {
    var o = {}, k;
    for (k in DEFAULTS) o[k] = opts && opts[k] !== undefined ? opts[k] : DEFAULTS[k];
    var arr = [];
    var api = {
      options: o,
      add: function (s) {
        var n = norm(s);
        if (typeof n.t !== 'number' || !isFinite(n.t)) return false;
        if (arr.length && n.t < arr[arr.length - 1].t) return false;
        arr.push(n);
        if (arr.length > o.maxSamples) arr.splice(0, arr.length - o.maxSamples);
        return true;
      },
      addMany: function (list) { var c = 0; list.forEach(function (s) { if (api.add(s)) c++; }); return c; },
      samples: function () { return arr; },
      size: function () { return arr.length; },
      last: function () { return arr.length ? arr[arr.length - 1] : null; },
      since: function (t) {
        var lo = 0, hi = arr.length;
        while (lo < hi) { var mid = (lo + hi) >> 1; if (arr[mid].t < t) lo = mid + 1; else hi = mid; }
        return arr.slice(lo);
      },
      clear: function () { arr = []; },
      compact: function (now) { arr = compactList(arr, now, o); if (arr.length > o.maxSamples) arr.splice(0, arr.length - o.maxSamples); return arr.length; },
      rows: function () { return arr.map(pack); },
      // Merges stored rows with what was recorded meanwhile.
      load: function (rows) {
        var loaded = [];
        (rows || []).forEach(function (r) { var s = unpack(r); if (s) loaded.push(s); });
        var all = loaded.concat(arr).sort(function (a, b) { return a.t - b.t; });
        arr = all.filter(function (s, i) { return i === 0 || s.t !== all[i - 1].t; });
        if (arr.length > o.maxSamples) arr.splice(0, arr.length - o.maxSamples);
        return arr.length;
      },
      toCsv: function (copts) { return toCsv(arr, copts); }
    };
    return api;
  }

  // Decides when a status becomes a sample: every intervalMs, and immediately when phase/climate/error change.
  function createRecorder(opts) {
    var intervalMs = (opts && opts.intervalMs) || DEFAULTS.intervalMs, last = null, gap = true;
    return {
      markGap: function (v) { gap = v !== false; },
      offer: function (status, now) {
        if (!status) return null;
        var key = status.phase + '|' + status.climateStatus + '|' + status.errorCode;
        if (last && now <= last.t) return null;
        if (last && key === last.key && now - last.t < intervalMs) return null;
        var s = fromStatus(status, now);
        if (gap || (last && now - last.t > GAP_MS)) { s.g = 1; gap = false; }
        last = { t: now, key: key };
        return s;
      }
    };
  }

  // Heating runs. The fuel counter resets with every run, so each run contributes its maximum.
  // A run only counts if heating (or run-on) was seen in it.
  function fuelRuns(samples) {
    var runs = [], cur = null, prevF = null, prevPhase = null;
    function close() { if (cur && cur.active && cur.ml > 0) runs.push({ t0: cur.t0, t1: cur.t1, ml: cur.ml }); cur = null; }
    samples.forEach(function (s) {
      var entering = s.phase === PHASE_HEATING && prevPhase !== PHASE_HEATING && prevPhase !== PHASE_RUNON;
      var f = s.fuelUsedMl;
      if (f !== null) {
        if (cur === null || f < prevF || (entering && f !== prevF)) { close(); cur = { t0: s.t, t1: s.t, ml: f, active: false }; }
        if (f > cur.ml) cur.ml = f;
        cur.t1 = s.t;
        if (s.phase === PHASE_HEATING || s.phase === PHASE_RUNON) cur.active = true;
        prevF = f;
      }
      prevPhase = s.phase;
    });
    close();
    var total = 0;
    runs.forEach(function (r) { total += r.ml; });
    return { runs: runs, total: total };
  }

  // Intervals [t0, t1] where the heater was heating (for the shaded chart bands).
  function bands(samples, maxGap) {
    maxGap = maxGap || GAP_MS;
    var out = [], cur = null, prev = null;
    samples.forEach(function (s) {
      var on = s.phase === PHASE_HEATING;
      if (cur && (s.g || s.t - prev.t > maxGap)) { cur[1] = prev.t; out.push(cur); cur = null; }
      if (on && !cur) cur = [s.t, s.t];
      else if (!on && cur) { cur[1] = s.t; out.push(cur); cur = null; }
      prev = s;
    });
    if (cur) { cur[1] = prev.t; out.push(cur); }
    return out;
  }

  function pad(n) { return ('0' + n).slice(-2); }
  function stamp(t) {
    var d = new Date(t);
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
  }

  // German Excel style: ';' separator, ',' decimal. opts: {decimal, phaseText}
  function toCsv(samples, opts) {
    opts = opts || {};
    var dec = opts.decimal || ',';
    function f(v, d) { return v === null ? '' : v.toFixed(d).replace('.', dec); }
    var lines = [['Zeit', 'Phase', 'Phase_Text', 'Leistung_W', 'Kraftstoff_ml', 'Kuehlmittel_C', 'Innen_C', 'Aussen_C', 'Spannung_V', 'Klima_Status', 'Ziel_C', 'Luecke'].join(';')];
    samples.forEach(function (s) {
      lines.push([
        stamp(s.t), s.phase === null ? '' : s.phase, s.phase !== null && opts.phaseText ? opts.phaseText[s.phase] || '' : '',
        f(s.powerW, 0), f(s.fuelUsedMl, 0), f(s.coolant, 1), f(s.interior, 1), f(s.outside, 1), f(s.voltage, 2),
        s.climateStatus === null ? '' : s.climateStatus, f(s.targetC, 0), s.g ? 1 : 0
      ].join(';'));
    });
    return lines.join('\r\n') + '\r\n';
  }

  // IndexedDB store. Every call resolves (null/false on any failure), so the app works without storage.
  function openStore(name) {
    var dbp = null;
    function db() {
      if (dbp) return dbp;
      dbp = new Promise(function (resolve) {
        try {
          if (typeof indexedDB === 'undefined') return resolve(null);
          var r = indexedDB.open(name, 1);
          r.onupgradeneeded = function () { try { r.result.createObjectStore('kv'); } catch (e) { /* ignore */ } };
          r.onsuccess = function () { resolve(r.result); };
          r.onerror = function () { resolve(null); };
          r.onblocked = function () { resolve(null); };
        } catch (e) { resolve(null); }
      });
      return dbp;
    }
    function tx(mode, fn) {
      return db().then(function (d) {
        if (!d) return null;
        return new Promise(function (resolve) {
          try {
            var t = d.transaction('kv', mode), req = fn(t.objectStore('kv'));
            t.oncomplete = function () { resolve(req && req.result !== undefined ? req.result : true); };
            t.onerror = t.onabort = function () { resolve(null); };
          } catch (e) { resolve(null); }
        });
      }).catch(function () { return null; });
    }
    return {
      load: function () { return tx('readonly', function (s) { return s.get('history'); }).then(function (v) { return Array.isArray(v) ? v : null; }); },
      save: function (rows) { return tx('readwrite', function (s) { return s.put(rows, 'history'); }).then(function (v) { return !!v; }); },
      clear: function () { return tx('readwrite', function (s) { return s.delete('history'); }).then(function (v) { return !!v; }); }
    };
  }

  return {
    FIELDS: FIELDS, GAP_MS: GAP_MS, DEFAULTS: DEFAULTS, fromStatus: fromStatus, norm: norm, merge: merge, pack: pack, unpack: unpack,
    create: create, createRecorder: createRecorder, fuelRuns: fuelRuns, bands: bands, toCsv: toCsv, openStore: openStore
  };
}));
