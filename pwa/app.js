(function () {
  'use strict';
  var P = window.Parser;
  var $ = function (id) { return document.getElementById(id); };
  var demo = /[?&]demo=1\b/.test(location.search);

  var ble = null, connected = false, busy = false, lastStatus = null, lastStatusAt = 0, roleOfDevice = null;

  function store(k, v) { try { if (v === undefined) return localStorage.getItem(k); localStorage.setItem(k, v); } catch (e) { /* no storage */ } return null; }

  function fmt(v, unit, digits) { return v === null || v === undefined ? '–' : v.toFixed(digits === undefined ? 1 : digits).replace('.', ',') + ' ' + unit; }
  function nf(n) { return n.toLocaleString('de-DE'); }

  var PHASE_KIND = ['unknown', 'off', 'heating', 'runon', 'vent', 'error'];

  function renderStatus(s) {
    lastStatus = s; lastStatusAt = Date.now();
    var kind = PHASE_KIND[s.phase] || 'unknown';
    var hasErr = s.errorCode !== 0 || s.phase === 5;
    $('stateTile').dataset.kind = hasErr ? 'error' : kind;
    $('stateText').textContent = P.PHASE_TEXT[s.phase] || 'Unbekannt';
    var sub = [];
    if (hasErr) sub.push(P.errorText(s.errorCode));
    if (s.assumed) sub.push('angenommen');
    if (s.stale) sub.push('veraltet');
    if (!s.busAwake && !hasErr) sub.push('Bus schläft');
    $('stateSub').textContent = sub.join(' · ');

    $('vInterior').textContent = fmt(s.interior, '°C');
    $('vOutside').textContent = fmt(s.outside, '°C');
    $('vCoolant').textContent = fmt(s.coolant, '°C');
    $('vPower').textContent = nf(s.powerW) + ' W';
    $('vRate').textContent = fmt(s.fuelRateLph, 'l/h', 2);
    $('vFuel').textContent = nf(s.fuelUsedMl) + ' ml';
    $('vVoltage').textContent = fmt(s.voltage, 'V', 1);
    $('vOdo').textContent = s.odometer === null ? '–' : nf(s.odometer) + ' km';

    var et = P.errorText(s.errorCode);
    if (s.errorStored) et += ' (gespeichert)';
    if (s.kl30Lost) et += ', Klemme 30 verloren';
    $('errText').textContent = et;
    $('errRow').classList.toggle('active', s.errorCode !== 0);

    $('climState').textContent = P.CLIMATE_TEXT[s.climateStatus] || 'Unbekannt';
    $('climRemain').textContent = s.climateActive && s.remainingMin > 0 ? 'Rest ' + P.formatDuration(s.remainingMin) : '';

    var la = $('linkAge');
    if (roleOfDevice === 1) {
      la.hidden = false;
      la.textContent = 'LoRa-Status: ' + (s.linkAgeSaturated ? 'älter als 255 s' : 'vor ' + s.linkAgeS + ' s') +
        ' · RSSI ' + s.rssi + ' dBm · SNR ' + s.snr + ' dB';
    } else la.hidden = true;
  }

  function setConn(text, kind) {
    $('connState').textContent = text;
    $('dot').className = 'dot ' + (kind || 'off');
    connected = ble && ble.isConnected();
    $('btnConnect').hidden = kind === 'ok' || kind === 'busy';
    $('btnDisconnect').hidden = !(kind === 'ok' || kind === 'busy');
    $('role').disabled = kind === 'ok' || kind === 'busy';
    updateButtons();
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
    onInfo: function (info, name) {
      roleOfDevice = info.role;
      $('devName').textContent = name || info.roleText;
      $('devInfo').textContent = info.roleText + ' · ' + info.transportText + ' · FW ' + (info.fw || '?');
    },
    onStatus: renderStatus,
    onError: function (m) { setCmdLine(m, 'err'); },
    onBusy: function (b) { busy = b; updateButtons(); },
    onResult: function (r, t) { setCmdLine(t, r.stage === 4 ? 'err' : ''); }
  };

  async function run(op, args, label) {
    setCmdLine(label + ' …', '');
    var r = await ble.send(op, args);
    setCmdLine(label + ': ' + r.text, r.ok ? 'ok' : 'err');
  }

  function bind() {
    var role = store('role');
    if (role === 'sender' || role === 'receiver') $('role').value = role;
    $('role').addEventListener('change', function () { store('role', $('role').value); });

    var t = $('target'), d = $('duration');
    function lbls() { $('targetLbl').textContent = t.value + ' °C'; $('durationLbl').textContent = P.formatDuration(+d.value); }
    t.addEventListener('input', lbls); d.addEventListener('input', lbls); lbls();

    $('btnConnect').addEventListener('click', function () {
      ble.connect().catch(function (e) { setCmdLine('Verbindung fehlgeschlagen: ' + (e && e.message ? e.message : e), 'err'); setConn('Getrennt', 'off'); });
    });
    $('btnDisconnect').addEventListener('click', function () { ble.disconnect(); });
    $('btnHeat').addEventListener('click', function () { run(P.OP.HEAT, null, 'Heizen'); });
    $('btnStop').addEventListener('click', function () { run(P.OP.STOP, null, 'Aus'); });
    $('btnQuery').addEventListener('click', function () { run(P.OP.QUERY, null, 'Abfragen'); });
    $('btnClimOff').addEventListener('click', function () { run(P.OP.CLIMATE_OFF, null, 'Klima aus'); });
    $('btnClimOn').addEventListener('click', function () {
      run(P.OP.CLIMATE_SET, { targetC: +t.value, durationMin: +d.value }, 'Klima starten');
    });
  }

  function init() {
    if (!demo && !window.Ble.supported()) {
      $('unsupported').hidden = false;
      $('btnConnect').disabled = true;
    }
    ble = demo ? window.Demo.create(callbacks) : window.Ble.create(callbacks);
    bind();
    setConn('Getrennt', 'off');
    if (demo) { $('devName').textContent = 'Demo-Modus'; ble.connect(); }
    if (!demo && 'serviceWorker' in navigator) {
      navigator.serviceWorker.register('sw.js').catch(function () { /* offline cache is optional */ });
    }
  }
  init();
}());
