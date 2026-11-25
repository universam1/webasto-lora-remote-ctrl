// Fake device for ?demo=1: same interface as Ble.create(). Simulates a believable 3 h history
// (backfill()) and keeps generating live samples afterwards. Scenarios: &scn=heat|vent|err
(function (root) {
  'use strict';
  var P = root.Parser;
  var MIN = 60000, RUNON = 3 * MIN, RUN_LEN = 30 * MIN, HISTORY = 180 * MIN, STEP = 5000;
  var ODO = 48213;

  function rng(seed) { // mulberry32
    return function () {
      seed |= 0; seed = seed + 0x6D2B79F5 | 0;
      var t = Math.imul(seed ^ seed >>> 15, 1 | seed);
      t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
      return ((t ^ t >>> 14) >>> 0) / 4294967296;
    };
  }

  function scenario() {
    var m = /[?&]scn=(\w+)/.exec(root.location ? root.location.search : '');
    return m ? m[1] : '';
  }

  function mkStatus(o) {
    var phase = o.phase;
    return {
      version: 1, phase: phase, busAwake: true, climateActive: o.climateActive, interiorValid: true, transportHealthy: true, ignitionOn: false,
      errorCode: o.errorCode || 0, errorStored: !!o.errorCode, kl30Lost: false, assumed: false, stale: false,
      climateStatus: o.climateStatus, powerW: Math.round(o.powerW), fuelUsedMl: Math.round(o.fuelUsedMl), fuelRate_mlph: Math.round(o.powerW * 0.104),
      targetC: o.targetC, durationMin: o.durationMin, remainingMin: o.remainingMin, rssi: -71, snr: 8, linkAgeS: 2, linkAgeSaturated: false,
      lastCmdId: 0, lastCmdResult: 1, raw: {},
      interior: Math.round(o.interior * 10) / 10, outside: Math.round(o.outside * 10) / 10, coolant: Math.round(o.coolant * 10) / 10,
      engineCoolant: Math.round(o.coolant * 10) / 10, voltage: Math.round(o.voltage * 100) / 100, odometer: ODO, fuelRateLph: o.powerW * 0.104 / 1000
    };
  }

  function createModel(now, shiftMin) {
    var start = now - HISTORY, rnd = rng(7);
    var sh = shiftMin * MIN;
    var m = {
      t: start, runs: [{ s: start + 45 * MIN - sh, e: start + 75 * MIN - sh }, { s: start + 125 * MIN - sh, e: start + 155 * MIN - sh }, { s: start + 205 * MIN - sh, e: start + 235 * MIN - sh }],
      vents: [{ s: start + 158 * MIN - sh, e: start + 168 * MIN - sh }],
      session: { s: start + 125 * MIN - sh, dur: 240, target: 21 }, errorCode: 0,
      coolant: 14, interior: 7.5, voltage: 12.62, fuel: 0, power: 0, phase: 1, prevPhase: 1
    };
    m.outside = 3.2;
    function phaseAt(t) {
      var i;
      for (i = 0; i < m.runs.length; i++) {
        var r = m.runs[i];
        if (t >= r.s && t < r.e) return 2;
        if (t >= r.e && t < r.e + RUNON) return 3;
      }
      for (i = 0; i < m.vents.length; i++) if (t >= m.vents[i].s && t < m.vents[i].e) return 4;
      return 1;
    }
    function runStart(t) { for (var i = 0; i < m.runs.length; i++) if (t >= m.runs[i].s && t < m.runs[i].e) return m.runs[i].s; return t; }
    m.step = function (to) {
      var dt = to - m.t; if (dt <= 0) return;
      var p = phaseAt(to), dm = dt / MIN, e = (to - runStart(to)) / MIN;
      if (p === 2 && m.prevPhase !== 2) m.fuel = 0;
      m.prevPhase = p; m.phase = p;
      var target = p === 2 ? 4800 * Math.min(1, 0.25 + e / 6) * (e > 26 ? 0.8 : 1) : 0;
      m.power += (target - m.power) * Math.min(1, dm * 1.5);
      if (p !== 2 && m.power < 30) m.power = 0;
      var coolTarget = p === 2 ? 78 : (p === 4 ? m.outside + 4 : m.outside + 2);
      var tau = p === 2 ? 6 : (p === 3 ? 9 : 35);
      m.coolant += (coolTarget - m.coolant) * (1 - Math.exp(-dm / tau));
      var heat = p === 2 ? (m.coolant - m.interior) * 0.0045 * m.power / 4800 * 8 : (p === 3 ? 0.05 : 0);
      m.interior += (heat - (m.interior - m.outside) / 70) * dm;
      m.outside = 3.2 + 0.6 * Math.sin(to / (47 * MIN));
      if (p === 2) m.fuel += m.power * 0.104 / 3600000 * dt;
      var v = p === 2 ? 12.35 : (p === 3 ? 12.45 : 12.62 - Math.min(0.15, (to - start) / HISTORY * 0.1));
      if (p === 2 && e < 0.6) v -= 0.9 * (1 - e / 0.6); // glow plug dip
      m.voltage = v + (rnd() - 0.5) * 0.06;
      if (rnd() < 0.002) m.voltage -= 0.45; // other consumers
      m.t = to;
    };
    m.status = function () {
      var s = m.session, active = !!s && m.t >= s.s && m.t < s.s + s.dur * MIN;
      var rem = active ? Math.ceil((s.s + s.dur * MIN - m.t) / MIN) : 0;
      return mkStatus({
        phase: m.errorCode ? 5 : m.phase, climateActive: active, climateStatus: active ? (m.phase === 2 || m.phase === 3 ? 3 : 4) : 0,
        powerW: m.power, fuelUsedMl: m.fuel, coolant: m.coolant, interior: m.interior, outside: m.outside, voltage: m.voltage,
        targetC: active ? s.target : 21, durationMin: active ? s.dur : 0, remainingMin: rem, errorCode: m.errorCode
      });
    };
    return m;
  }

  function create(cb) {
    var timer = null, connected = false, busy = false, cmdId = 0, model = null;
    var scn = scenario();
    // Shifts the schedule so that "now" lands inside a heating run (heat) or a ventilation phase (vent).
    var shift = scn === 'heat' ? 37 : scn === 'vent' ? -17 : 0;

    function ensure() {
      if (model) return;
      var now = Date.now();
      model = createModel(now, shift);
      if (scn === 'err') model.errorCode = 6;
    }

    function history() {
      ensure();
      var out = [], H = root.Hist, now = Date.now();
      model.t = now - HISTORY; model.phase = 1; model.prevPhase = 1;
      for (var t = now - HISTORY + STEP; t <= now; t += STEP) {
        model.step(t);
        out.push(H.fromStatus(model.status(), t));
      }
      out[0].g = 1;
      return out;
    }

    function push() { model.step(Date.now()); cb.onStatus(model.status()); }

    var api = {
      backfill: history,
      connect: async function () {
        cb.onState('Verbinde …', 'busy');
        await new Promise(function (r) { setTimeout(r, 900); });
        ensure();
        connected = true;
        cb.onInfo({ role: 0, roleText: 'Receiver', transport: 0, transportText: 'CAN', fw: 'demo' }, 'Demo-Gerät');
        cb.onState('Verbunden (Demo)', 'ok');
        push();
        timer = setInterval(push, 2000);
        return true;
      },
      disconnect: function () { connected = false; clearInterval(timer); cb.onState('Getrennt', 'off'); },
      isConnected: function () { return connected; },
      isBusy: function () { return busy; },
      send: function (op, args) {
        if (busy) return Promise.resolve({ ok: false, text: 'Befehl läuft noch' });
        cmdId = P.nextCmdId(cmdId); busy = true; cb.onBusy(true);
        var stages = ['Eingereiht', 'Gesendet', 'Bestätigt'], now = Date.now();
        if (op === P.OP.HEAT) model.runs.push({ s: now, e: now + RUN_LEN });
        else if (op === P.OP.STOP) model.runs.forEach(function (r) { if (now >= r.s && now < r.e) r.e = now; });
        else if (op === P.OP.CLIMATE_SET) model.session = { s: now, dur: args.durationMin, target: args.targetC };
        else if (op === P.OP.CLIMATE_OFF) model.session = null;
        return new Promise(function (resolve) {
          var i = 0;
          (function step() {
            if (i < stages.length) { cb.onResult({ stage: i }, stages[i]); i++; setTimeout(step, 450); return; }
            busy = false; cb.onBusy(false);
            push();
            resolve({ ok: true, text: 'Fertig: OK' });
          }());
        });
      }
    };
    return api;
  }
  root.Demo = { create: create, createModel: createModel };
}(typeof self !== 'undefined' ? self : this));
