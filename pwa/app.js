(function () {
  'use strict';
  var P = window.Parser, U = window.Util, H = window.Hist, C = window.Charts;
  var $ = function (id) { return document.getElementById(id); };
  var reduce = window.matchMedia ? window.matchMedia('(prefers-reduced-motion: reduce)') : { matches: false };
  var MIN = 60000;

  function store(k, v) { try { if (v === undefined) return localStorage.getItem(k); localStorage.setItem(k, v); } catch (e) { /* no storage */ } return null; }
  var demo = /[?&]demo=1\b/.test(location.search) || store('demo') === '1';

  var ble = null, connected = false, busy = false, lastStatus = null, lastStatusAt = 0, roleOfDevice = null;
  var hist = H.create(), recorder = H.createRecorder(), db = demo ? null : H.openStore('standheizung'), storageOk = null;
  var settings = {
    theme: (/[?&]theme=(light|dark)\b/.exec(location.search) || [0, store('theme')])[1] || 'auto', fuelUnit: store('fuelUnit') || 'ml', clock24: store('clock24') !== '0', wake: store('wake') === '1'
  };
  var rangeMin = +store('range') || 120, hidden = {}, dirty = false;
  var target = +store('target') || 20, duration = +store('duration') || 120;

  function haptic(p) { try { if (navigator.vibrate) navigator.vibrate(p); } catch (e) { /* unsupported */ } }
  function nf(n) { return n.toLocaleString('de-DE'); }
  function clockFmt(t) { return U.clock(t, !settings.clock24); }

  // ---------- number tween ----------
  function setNum(el, v, fmt) {
    if (el._raf) cancelAnimationFrame(el._raf);
    var from = el._v; el._v = v;
    if (v === null || v === undefined) { el.textContent = '–'; el._v = null; return; }
    if (reduce.matches || from === null || from === undefined || from === v) { el.textContent = fmt(v); return; }
    var t0 = performance.now(), dur = 450;
    (function frame(now) {
      var k = Math.max(0, Math.min(1, (now - t0) / dur)), e = 1 - Math.pow(1 - k, 3);
      el.textContent = fmt(from + (v - from) * e);
      if (k < 1) el._raf = requestAnimationFrame(frame);
    }(t0));
  }

  // ---------- toast, sheet, ripple ----------
  function toast(text, kind) {
    var box = $('toasts');
    while (box.children.length >= 3) box.removeChild(box.firstChild);
    var t = document.createElement('div'); t.className = 'toast'; t.dataset.kind = kind || 'info';
    var icon = kind === 'ok' ? 'i-check' : kind === 'err' ? 'i-x' : 'i-info';
    t.innerHTML = '<svg class="ico" aria-hidden="true"><use href="#' + icon + '"/></svg><span></span>';
    t.lastChild.textContent = text;
    box.appendChild(t);
    setTimeout(function () { t.classList.add('out'); setTimeout(function () { if (t.parentNode) t.parentNode.removeChild(t); }, 320); }, 3400);
  }

  function confirmSheet(o) {
    return new Promise(function (resolve) {
      var root = $('sheetRoot'), prev = document.activeElement;
      $('sheetTitle').textContent = o.title; $('sheetText').textContent = o.text; $('sheetOk').textContent = o.ok;
      root.hidden = false; $('sheetCancel').focus();
      function done(v) {
        root.hidden = true;
        $('sheetOk').removeEventListener('click', yes); $('sheetCancel').removeEventListener('click', no);
        $('sheetBackdrop').removeEventListener('click', no); document.removeEventListener('keydown', key);
        if (prev && prev.focus) prev.focus();
        resolve(v);
      }
      function yes() { done(true); } function no() { done(false); }
      function key(e) {
        if (e.key === 'Escape') no();
        else if (e.key === 'Tab') { var a = [$('sheetCancel'), $('sheetOk')], i = a.indexOf(document.activeElement); e.preventDefault(); a[i === 0 ? 1 : 0].focus(); }
      }
      $('sheetOk').addEventListener('click', yes); $('sheetCancel').addEventListener('click', no);
      $('sheetBackdrop').addEventListener('click', no); document.addEventListener('keydown', key);
    });
  }

  document.addEventListener('pointerdown', function (e) {
    var b = e.target.closest && e.target.closest('button');
    if (!b || b.disabled) return;
    haptic(8);
    if (reduce.matches) return;
    var r = b.getBoundingClientRect(), s = Math.max(r.width, r.height) * 2, d = document.createElement('span');
    d.className = 'rip'; d.style.width = d.style.height = s + 'px';
    d.style.left = (e.clientX - r.left - s / 2) + 'px'; d.style.top = (e.clientY - r.top - s / 2) + 'px';
    b.appendChild(d); setTimeout(function () { if (d.parentNode) d.parentNode.removeChild(d); }, 520);
  });

  // ---------- status rendering ----------
  var PHASE_KIND = ['unknown', 'off', 'heating', 'runon', 'vent', 'error'];
  var fmtC = function (v) { return U.fmtNum(v, 1) + ' °C'; };

  function renderStatus(s) {
    lastStatus = s; lastStatusAt = Date.now();
    $('stateTile').classList.remove('loading'); $('values').classList.remove('loading'); $('conn').classList.remove('loading');
    var kind = PHASE_KIND[s.phase] || 'unknown';
    var hasErr = s.errorCode !== 0 || s.phase === 5;
    $('stateTile').dataset.kind = hasErr ? 'error' : kind;
    $('stateText').textContent = P.PHASE_TEXT[s.phase] || 'Unbekannt';
    var sub = [];
    if (hasErr) sub.push(P.errorText(s.errorCode));
    if (s.assumed) sub.push('angenommen');
    if (s.stale) sub.push('veraltet');
    if (!s.busAwake && !hasErr) sub.push('Bus schläft');
    if (!sub.length && s.climateActive) sub.push('Klima aktiv');
    $('stateSub').textContent = sub.join(' · ');

    setNum($('vInterior'), s.interior, fmtC);
    setNum($('vOutside'), s.outside, fmtC);
    setNum($('vCoolant'), s.coolant, fmtC);
    setNum($('vPower'), s.powerW / 1000, function (v) { return U.fmtNum(v, 1) + ' kW'; });
    setNum($('vVoltage'), s.voltage, function (v) { return U.fmtNum(v, 1) + ' V'; });
    if (settings.fuelUnit === 'lph') {
      $('fuelLbl').textContent = 'Verbrauchsrate';
      setNum($('vFuel'), s.fuelRateLph, function (v) { return U.fmtNum(v, 2) + ' l/h'; });
      $('vFuelSub').textContent = 'Lauf: ' + nf(s.fuelUsedMl) + ' ml';
    } else {
      $('fuelLbl').textContent = 'Verbrauch (Lauf)';
      setNum($('vFuel'), s.fuelUsedMl, function (v) { return nf(Math.round(v)) + ' ml'; });
      $('vFuelSub').textContent = U.fmtNum(s.fuelRateLph, 2) + ' l/h';
    }
    $('vOdo').textContent = s.odometer === null ? '–' : nf(s.odometer) + ' km';

    var eb = $('errBox');
    if (hasErr) {
      var info = U.errorInfo(s.errorCode);
      $('errTitle').textContent = info.title; $('errHelp').textContent = info.help;
      var ex = []; if (s.errorStored) ex.push('Fehler ist gespeichert'); if (s.kl30Lost) ex.push('Klemme 30 verloren');
      $('errExtra').textContent = ex.join(', ');
    }
    eb.hidden = !hasErr;

    $('climState').textContent = P.CLIMATE_TEXT[s.climateStatus] || 'Unbekannt';
    var rem = s.climateActive && s.remainingMin > 0;
    $('climRemain').textContent = rem ? 'Rest ' + P.formatDuration(s.remainingMin) : '';
    var dur = s.durationMin || 0;
    $('ringFg').style.strokeDashoffset = rem && dur > 0 ? 100 - Math.max(0, Math.min(100, 100 * s.remainingMin / dur)) : 100;
    $('ringVal').textContent = rem ? U.hm(s.remainingMin) + ' h' : '–';
    $('ringLbl').textContent = rem ? 'Restzeit' : 'Klima aus';
    $('remainRing').setAttribute('role', 'img');
    $('remainRing').setAttribute('aria-label', rem ? 'Klima Restzeit ' + P.formatDuration(s.remainingMin) : 'Klima aus');

    var la = $('linkAge');
    if (roleOfDevice === 1) {
      la.hidden = false;
      la.textContent = 'LoRa-Status: ' + (s.linkAgeSaturated ? 'älter als 255 s' : 'vor ' + s.linkAgeS + ' s') + ' · RSSI ' + s.rssi + ' dBm · SNR ' + s.snr + ' dB';
    } else la.hidden = true;
    $('btnShare').hidden = !navigator.share;

    var smp = recorder.offer(s, lastStatusAt);
    if (smp && hist.add(smp)) dirty = true;
    updateAge();
  }

  function updateAge() {
    var age = lastStatusAt ? Date.now() - lastStatusAt : null;
    $('ageText').textContent = lastStatusAt ? 'Aktualisiert ' + U.agoText(age) : 'Noch keine Daten';
    var stale = connected && lastStatusAt && U.isStale(age);
    $('staleBanner').hidden = !stale;
    if (stale) $('staleText').textContent = 'Daten veraltet, letzte Aktualisierung ' + U.agoText(age) + '.';
  }

  var connKind = 'off', devLabel = '', searching = false, retryable = false;
  function setConn(text, kind) {
    connKind = kind || 'off';
    $('connState').textContent = text + (connKind === 'ok' && devLabel ? ' · ' + devLabel : '');
    $('connChip').dataset.kind = connKind;
    connected = !!(ble && ble.isConnected());
    if (!connected || connKind !== 'ok') recorder.markGap();
    $('btnConnect').hidden = connKind === 'ok' || connKind === 'busy';
    $('btnDisconnect').hidden = !(connKind === 'ok' || connKind === 'busy');
    $('role').disabled = connKind === 'ok' || connKind === 'busy';
    $('btnRetry').hidden = !(retryable && connKind === 'off');
    $('btnDisconnect').textContent = searching ? 'Abbrechen' : 'Trennen';
    var load = connKind === 'busy' && !lastStatus;
    $('stateTile').classList.toggle('loading', load); $('values').classList.toggle('loading', load); $('conn').classList.toggle('loading', load);
    updateButtons(); updateAge();
  }

  function updateButtons() {
    var reason = '';
    if (!connected) reason = 'Nicht verbunden.';
    else if (busy) reason = 'Befehl läuft …';
    ['btnClimOn', 'btnClimOff', 'btnHeat', 'btnStop', 'btnQuery'].forEach(function (id) { $(id).disabled = !!reason; });
    $('reason').textContent = reason;
  }

  function setCmdLine(t, kind) { var e = $('cmdLine'); e.textContent = t; e.dataset.kind = kind || ''; }

  var callbacks = {
    onState: function (t, k) { setConn(t, k); },
    onSearch: function (kind) { searching = kind === 'searching'; retryable = kind === 'timeout'; },
    onInfo: function (info, name) {
      roleOfDevice = info.role; devLabel = name || '';
      $('devName').textContent = name || info.roleText;
      $('devInfo').textContent = info.roleText + ' · ' + info.transportText + ' · FW ' + (info.fw || '?');
    },
    onStatus: renderStatus,
    onError: function (m) { setCmdLine(m, 'err'); toast(m, 'err'); },
    onBusy: function (b) { busy = b; updateButtons(); },
    onResult: function (r, t) { setCmdLine(t, r.stage === 4 ? 'err' : ''); }
  };

  async function run(op, args, label) {
    setCmdLine(label + ' …', '');
    var r = await ble.send(op, args);
    setCmdLine(label + ': ' + r.text, r.ok ? 'ok' : 'err');
    toast(label + ': ' + r.text, r.ok ? 'ok' : 'err');
    haptic(r.ok ? 25 : [40, 60, 40]);
  }

  // ---------- tabs ----------
  var TABS = ['uebersicht', 'klima', 'verlauf', 'einstellungen'], tab = null;
  function showView(name, first) {
    if (TABS.indexOf(name) < 0) name = 'uebersicht';
    if (name === tab) return;
    var dir = first || tab === null ? 0 : TABS.indexOf(name) > TABS.indexOf(tab) ? 1 : -1;
    tab = name;
    TABS.forEach(function (n) {
      var v = $('view-' + n), on = n === name;
      v.hidden = !on; v.classList.remove('in-right', 'in-left');
      if (on && dir) { void v.offsetWidth; v.classList.add(dir > 0 ? 'in-right' : 'in-left'); }
    });
    Array.prototype.forEach.call(document.querySelectorAll('#tabbar a'), function (a) {
      if (a.dataset.tab === name) a.setAttribute('aria-current', 'page'); else a.removeAttribute('aria-current');
    });
    window.scrollTo(0, 0);
    if (name === 'verlauf') renderCharts(true);
  }
  function goTo(name) { if (location.hash !== '#' + name) location.hash = name; else showView(name); }
  window.addEventListener('hashchange', function () { showView(location.hash.slice(1)); });

  var sx = 0, sy = 0, sTarget = null;
  document.addEventListener('touchstart', function (e) {
    var t = e.touches[0]; sx = t.clientX; sy = t.clientY;
    sTarget = e.touches.length === 1 && !(e.target.closest && e.target.closest('.noswipe, input, select, .sheet, #tabbar')) ? 1 : null;
  }, { passive: true });
  document.addEventListener('touchend', function (e) {
    if (!sTarget) return;
    var t = e.changedTouches[0], dx = t.clientX - sx, dy = t.clientY - sy; sTarget = null;
    if (Math.abs(dx) < 70 || Math.abs(dy) > Math.abs(dx) * 0.6) return;
    var i = TABS.indexOf(tab) + (dx < 0 ? 1 : -1);
    if (i >= 0 && i < TABS.length) { haptic(10); goTo(TABS[i]); }
  }, { passive: true });

  // ---------- climate controls ----------
  var CX = 100, CY = 100, R = 80, A0 = 150, SWEEP = 240;
  function polar(a) { var r = a * Math.PI / 180; return [CX + R * Math.cos(r), CY + R * Math.sin(r)]; }
  function arcPath(a0, a1) {
    var p0 = polar(a0), p1 = polar(a1);
    return 'M' + p0[0].toFixed(2) + ' ' + p0[1].toFixed(2) + 'A' + R + ' ' + R + ' 0 ' + (a1 - a0 > 180 ? 1 : 0) + ' 1 ' + p1[0].toFixed(2) + ' ' + p1[1].toFixed(2);
  }
  function setTarget(v, save) {
    target = U.clamp(Math.round(v), 10, 24);
    var a = A0 + SWEEP * (target - 10) / 14, k = polar(a);
    $('arcBg').setAttribute('d', arcPath(A0, A0 + SWEEP)); $('arcFg').setAttribute('d', arcPath(A0, a));
    $('arcKnob').setAttribute('cx', k[0]); $('arcKnob').setAttribute('cy', k[1]);
    $('arcVal').textContent = target;
    $('arc').setAttribute('aria-valuenow', target); $('arc').setAttribute('aria-valuetext', target + ' °C');
    $('tMinus').disabled = target <= 10; $('tPlus').disabled = target >= 24;
    if (save) store('target', target);
    markPresets();
  }
  function setDuration(m, save) {
    duration = U.clamp(Math.round(m / 10) * 10, 10, 480);
    $('duration').value = duration; $('durationLbl').textContent = P.formatDuration(duration);
    if (save) store('duration', duration);
    markPresets();
  }
  function markPresets() {
    var m = U.matchPreset(target, duration);
    Array.prototype.forEach.call($('presets').children, function (b) { b.setAttribute('aria-pressed', b.dataset.id === m); });
    Array.prototype.forEach.call($('durChips').children, function (b) { b.setAttribute('aria-pressed', +b.dataset.min === duration); });
  }
  function pointerTarget(e) {
    var svg = $('arc'), r = svg.getBoundingClientRect(), sc = 200 / r.width;
    var x = (e.clientX - r.left) * sc - CX, y = (e.clientY - r.top) * sc - CY;
    var d = ((Math.atan2(y, x) * 180 / Math.PI) - A0 + 720) % 360;
    if (d > SWEEP) d = d > SWEEP + (360 - SWEEP) / 2 ? 0 : SWEEP;
    var v = 10 + Math.round(d / SWEEP * 14);
    if (v !== target) { haptic(6); setTarget(v, true); }
  }
  function buildClimate() {
    U.PRESETS.forEach(function (p) {
      var b = document.createElement('button'); b.textContent = U.presetLabel(p); b.dataset.id = p.id;
      b.addEventListener('click', function () { setTarget(p.targetC, true); setDuration(p.durationMin, true); });
      $('presets').appendChild(b);
    });
    U.DURATIONS.forEach(function (m) {
      var b = document.createElement('button'); b.textContent = m / 60 + ' h'; b.dataset.min = m;
      b.addEventListener('click', function () { setDuration(m, true); });
      $('durChips').appendChild(b);
    });
    var drag = false, svg = $('arc');
    svg.addEventListener('pointerdown', function (e) { drag = true; svg.setPointerCapture(e.pointerId); pointerTarget(e); });
    svg.addEventListener('pointermove', function (e) { if (drag) pointerTarget(e); });
    svg.addEventListener('pointerup', function () { drag = false; });
    svg.addEventListener('pointercancel', function () { drag = false; });
    svg.addEventListener('keydown', function (e) {
      var d = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
      if (d) { e.preventDefault(); setTarget(target + d, true); }
      else if (e.key === 'Home') { e.preventDefault(); setTarget(10, true); } else if (e.key === 'End') { e.preventDefault(); setTarget(24, true); }
    });
    $('tMinus').addEventListener('click', function () { setTarget(target - 1, true); });
    $('tPlus').addEventListener('click', function () { setTarget(target + 1, true); });
    $('duration').addEventListener('input', function () { setDuration(+this.value, true); });
    setTarget(target); setDuration(duration);
  }

  // ---------- charts ----------
  var RANGES = [[30, '30 min'], [120, '2 h'], [720, '12 h'], [1440, '24 h']];
  var SERIES = [
    { key: 'interior', name: 'Innen', color: 'var(--s1)' }, { key: 'outside', name: 'Außen', color: 'var(--s3)' },
    { key: 'coolant', name: 'Kühlmittel', color: 'var(--s2)' }, { key: 'targetC', name: 'Ziel', color: 'var(--muted)', dash: true }
  ];

  function pts(samples, key, scale) { return samples.map(function (s) { return { t: s.t, v: s[key] === null ? null : s[key] * (scale || 1), g: s.g }; }); }
  function thin(data, t0, t1, width) { return data.length > width * 1.5 ? C.bucketize(data, t0, t1, Math.round(width)) : data; }

  function buildChartUi() {
    RANGES.forEach(function (r) {
      var b = document.createElement('button'); b.textContent = r[1]; b.dataset.min = r[0];
      b.addEventListener('click', function () { rangeMin = r[0]; store('range', rangeMin); renderCharts(true); });
      $('rangeChips').appendChild(b);
    });
    SERIES.forEach(function (s) {
      var b = document.createElement('button'); b.dataset.key = s.key; b.setAttribute('aria-pressed', 'true');
      b.innerHTML = '<i class="sw' + (s.dash ? ' dashsw' : '') + '"></i><span></span>'; b.lastChild.textContent = s.name;
      if (!s.dash) b.firstChild.style.background = s.color;
      b.addEventListener('click', function () { hidden[s.key] = !hidden[s.key]; b.setAttribute('aria-pressed', !hidden[s.key]); renderCharts(false); });
      $('legTemp').appendChild(b);
    });
  }

  function renderCharts(animate) {
    if (tab !== 'verlauf') return;
    Array.prototype.forEach.call($('rangeChips').children, function (b) { b.setAttribute('aria-pressed', +b.dataset.min === rangeMin); });
    var now = Date.now(), t0 = now - rangeMin * MIN, samples = hist.since(t0), w = Math.max(240, $('chTemp').clientWidth || 320);
    var common = { t0: t0, t1: now, fmtX: clockFmt, animate: animate, emptyText: 'Noch keine Daten in diesem Zeitraum' };
    function opt(o) { for (var k in common) o[k] = common[k]; return o; }
    // keep an open tooltip while the user reads values
    function redraw(id, fn) { var h = $(id); if (!animate && h.querySelector('.tip:not([hidden])')) return; fn(h); }

    redraw('chTemp', function (h) {
      C.line(h, opt({
        height: 210, label: 'Temperaturverlauf', fmtY: String, fmtV: fmtC,
        series: SERIES.map(function (s) { return { key: s.key, name: s.name, color: s.color, dash: s.dash, hidden: !!hidden[s.key], data: thin(pts(samples, s.key), t0, now, w / 2) }; })
      }));
    });
    redraw('chPower', function (h) {
      C.line(h, opt({
        height: 170, label: 'Heizleistung in kW', yMin: 0, yMax: 5, fmtY: function (v) { return U.fmtNum(v, v % 1 ? 1 : 0); },
        fmtV: function (v) { return U.fmtNum(v, 2) + ' kW'; }, bands: H.bands(samples),
        series: [{ key: 'powerW', name: 'Leistung', color: 'var(--s2)', area: true, data: thin(pts(samples, 'powerW', 0.001), t0, now, w / 2) }]
      }));
    });
    redraw('chVolt', function (h) {
      C.line(h, opt({
        height: 170, label: 'Bordspannung in Volt', yMin: 11.5, yMax: 13, fmtY: function (v) { return U.fmtNum(v, 1); },
        fmtV: function (v) { return U.fmtNum(v, 2) + ' V'; },
        series: [{ key: 'voltage', name: 'Spannung', color: 'var(--s1)', data: thin(pts(samples, 'voltage'), t0, now, w / 2) }]
      }));
    });
    var fr = H.fuelRuns(samples);
    $('fuelTotal').textContent = 'Gesamt ' + nf(Math.round(fr.total)) + ' ml';
    redraw('chFuel', function (h) {
      C.bars(h, opt({ height: 160, label: 'Verbrauch je Heizlauf in ml', runs: fr.runs, color: 'var(--s2)', fmtY: String, fmtV: function (v) { return nf(Math.round(v)) + ' ml'; } }));
    });
    var first = hist.samples()[0];
    $('histInfo').textContent = hist.size() ? nf(hist.size()) + ' Messpunkte seit ' + clockFmt(first.t) + (db && storageOk === false ? ' (nur im Arbeitsspeicher)' : '') : 'Noch kein Verlauf aufgezeichnet.';
    $('btnCsv').disabled = !hist.size();
  }

  function updateSparks() {
    var now = Date.now(), t0 = now - 30 * MIN, s = hist.since(t0);
    [['spInterior', 'interior', 1, 'var(--s1)'], ['spPower', 'powerW', 0.001, 'var(--s2)'], ['spVoltage', 'voltage', 1, 'var(--s1)']].forEach(function (d) {
      var host = $(d[0]); host.style.color = d[3];
      C.spark(host, pts(s, d[1], d[2]), { t0: t0, t1: now, color: d[3], w: Math.max(60, host.clientWidth || 100), h: 26 });
    });
  }

  // ---------- history persistence ----------
  function persist() {
    if (!db || !dirty) return;
    dirty = false; hist.compact(Date.now());
    db.save(hist.rows()).then(function (ok) { storageOk = ok; });
  }
  function downloadCsv() {
    var csv = '﻿' + hist.toCsv({ phaseText: P.PHASE_TEXT });
    try {
      var url = URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8' })), a = document.createElement('a');
      a.href = url; a.download = 'standheizung-verlauf.csv'; document.body.appendChild(a); a.click(); document.body.removeChild(a);
      setTimeout(function () { URL.revokeObjectURL(url); }, 2000);
      toast('CSV exportiert', 'ok');
    } catch (e) { toast('Export fehlgeschlagen', 'err'); }
  }
  async function clearHistory() {
    var ok = await confirmSheet({ title: 'Verlauf löschen?', text: 'Alle aufgezeichneten Messwerte werden entfernt. Das kann nicht rückgängig gemacht werden.', ok: 'Löschen' });
    if (!ok) return;
    hist.clear(); dirty = false;
    if (db) await db.clear();
    renderCharts(true); updateSparks(); toast('Verlauf gelöscht', 'ok');
  }

  // ---------- settings ----------
  function applyTheme() { if (settings.theme === 'light' || settings.theme === 'dark') document.documentElement.dataset.theme = settings.theme; else delete document.documentElement.dataset.theme; }

  var wl = null;
  async function applyWake() {
    try {
      if (settings.wake && document.visibilityState === 'visible' && navigator.wakeLock) {
        if (!wl) { wl = await navigator.wakeLock.request('screen'); wl.addEventListener('release', function () { wl = null; }); }
      } else if (wl) { var l = wl; wl = null; await l.release(); }
    } catch (e) { wl = null; }
  }

  var installEvt = null;
  function bindSettings() {
    $('theme').value = settings.theme; $('fuelUnit').value = settings.fuelUnit;
    $('clock24').checked = settings.clock24; $('wake').checked = settings.wake; $('demoToggle').checked = demo;
    $('appVersion').textContent = U.VERSION;
    $('theme').addEventListener('change', function () { settings.theme = this.value; store('theme', this.value); applyTheme(); });
    $('fuelUnit').addEventListener('change', function () { settings.fuelUnit = this.value; store('fuelUnit', this.value); if (lastStatus) { var s = lastStatus; $('vFuel')._v = null; renderStatus(s); } });
    $('clock24').addEventListener('change', function () { settings.clock24 = this.checked; store('clock24', this.checked ? '1' : '0'); renderCharts(true); });
    $('wake').addEventListener('change', function () { settings.wake = this.checked; store('wake', this.checked ? '1' : '0'); applyWake(); });
    if (!navigator.wakeLock) { $('wake').disabled = true; $('wakeHint').textContent = 'Wird von diesem Browser nicht unterstützt.'; }
    else $('wakeHint').textContent = 'Hält den Bildschirm an, solange die App sichtbar ist.';
    $('demoToggle').addEventListener('change', function () {
      store('demo', this.checked ? '1' : '0');
      location.replace(location.pathname + (this.checked ? '?demo=1' : '') + location.hash);
    });
    $('btnClearHist2').addEventListener('click', clearHistory);
    window.addEventListener('beforeinstallprompt', function (e) {
      e.preventDefault(); installEvt = e; $('btnInstall').hidden = false; $('installHint').hidden = true;
    });
    window.addEventListener('appinstalled', function () { installEvt = null; $('btnInstall').hidden = true; toast('App installiert', 'ok'); });
    $('btnInstall').addEventListener('click', function () {
      if (!installEvt) return;
      installEvt.prompt(); installEvt.userChoice.then(function () { installEvt = null; $('btnInstall').hidden = true; });
    });
    if (window.matchMedia && matchMedia('(display-mode: standalone)').matches) $('installHint').textContent = 'Die App ist installiert.';
    document.addEventListener('visibilitychange', function () {
      applyWake();
      if (document.visibilityState === 'hidden') persist();
      else if (!demo && ble && !connected && !ble.userDisconnect) autoConnect(!!ble.device);
    });
    window.addEventListener('pagehide', persist);
  }

  function bind() {
    var role = store('role');
    if (role === 'sender' || role === 'receiver') $('role').value = role;
    $('role').addEventListener('change', function () { store('role', $('role').value); if (ble) ble.role = $('role').value; });
    $('btnRetry').addEventListener('click', function () { retryable = false; autoConnect(true); });

    $('btnConnect').addEventListener('click', function () {
      ble.connect().catch(function (e) { var m = 'Verbindung fehlgeschlagen: ' + (e && e.message ? e.message : e); setCmdLine(m, 'err'); toast(m, 'err'); setConn('Getrennt', 'off'); });
    });
    $('btnDisconnect').addEventListener('click', function () { ble.disconnect(); });
    $('btnHeat').addEventListener('click', function () { run(P.OP.HEAT, null, 'Heizen'); });
    $('btnStop').addEventListener('click', async function () {
      var running = lastStatus && (lastStatus.phase === 2 || lastStatus.phase === 3 || lastStatus.phase === 4);
      if (running && !(await confirmSheet({ title: 'Heizung ausschalten?', text: 'Das Heizgerät läuft gerade (' + P.PHASE_TEXT[lastStatus.phase] + '). Es wird abgeschaltet und läuft kurz nach.', ok: 'Ausschalten' }))) return;
      run(P.OP.STOP, null, 'Aus');
    });
    $('btnQuery').addEventListener('click', function () { run(P.OP.QUERY, null, 'Abfragen'); });
    $('btnClimOff').addEventListener('click', function () { run(P.OP.CLIMATE_OFF, null, 'Klima aus'); });
    $('btnClimOn').addEventListener('click', function () { run(P.OP.CLIMATE_SET, { targetC: target, durationMin: duration }, 'Klima starten'); });
    $('btnShare').addEventListener('click', function () {
      if (!lastStatus || !navigator.share) return;
      navigator.share({ title: 'Standheizung', text: U.shareText(lastStatus, { phaseText: P.PHASE_TEXT, hour12: !settings.clock24, now: lastStatusAt }) }).catch(function () { /* cancelled */ });
    });
    $('btnCsv').addEventListener('click', downloadCsv);
    $('btnClearHist').addEventListener('click', clearHistory);
    Array.prototype.forEach.call(document.querySelectorAll('#tabbar a'), function (a) { a.addEventListener('click', function () { haptic(8); }); });
  }

  // Reconnect without the chooser (only once the device was paired through the chooser).
  var AUTO_TEXT = { unsupported: 'Automatisches Verbinden wird hier nicht unterstützt. Bitte „Verbinden“ wählen.', none: 'Noch kein Gerät gekoppelt. Bitte einmal „Verbinden“ wählen.' };
  function autoConnect(manual) {
    if (demo || !ble.autoConnect) return;
    var p = manual ? ble.resume() : ble.autoConnect();
    p.then(function (r) {
      if (AUTO_TEXT[r] && !connected) { $('devInfo').textContent = AUTO_TEXT[r]; setConn('Getrennt', 'off'); }
    }).catch(function (e) { setConn('Getrennt', 'off'); setCmdLine('Verbindung fehlgeschlagen: ' + (e && e.message ? e.message : e), 'err'); });
  }

  function init() {
    applyTheme(); buildClimate(); buildChartUi(); bind(); bindSettings();
    if (!demo && !window.Ble.supported()) { $('unsupported').hidden = false; $('btnConnect').disabled = true; }
    ble = demo ? window.Demo.create(callbacks) : window.Ble.create(callbacks);
    ble.role = $('role').value;
    showView(location.hash.slice(1), true);
    setConn('Getrennt', 'off');
    if (demo) {
      hist.addMany(ble.backfill()); hist.compact(Date.now());
      $('devName').textContent = 'Demo-Modus'; ble.connect(); renderCharts(true); updateSparks();
    } else {
      db.load().then(function (rows) { storageOk = rows !== null; if (rows) hist.load(rows); renderCharts(true); updateSparks(); });
      autoConnect(false);
      if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(function () { /* offline cache is optional */ });
    }
    var n = 0;
    setInterval(function () {
      updateAge(); n++;
      if (n % 5 === 0) { renderCharts(false); updateSparks(); }
      if (n % 30 === 0) persist();
    }, 1000);
    applyWake();
    var lastW = 0;
    function onResize() { var w = $('chTemp').getBoundingClientRect().width; if (w && Math.abs(w - lastW) > 2) { lastW = w; renderCharts(false); updateSparks(); } }
    if (window.ResizeObserver) new ResizeObserver(onResize).observe(document.body); else window.addEventListener('resize', onResize);
  }
  init();
}());
