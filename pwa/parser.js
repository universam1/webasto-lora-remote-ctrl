// Parser for the BLE profile (lib/webasto_core/ble_profile.h). No DOM, usable in browser and Node.
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.Parser = factory();
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var STATUS_VERSION = 1, STATUS_LEN = 34;
  var OP = { HEAT: 1, STOP: 2, CLIMATE_SET: 3, CLIMATE_OFF: 4, QUERY: 5 };
  var STAGE = { QUEUED: 0, SENT: 1, ACKED: 2, DONE: 3, FAILED: 4 };
  var INT16_MIN = -32768, ODO_UNKNOWN = 0xFFFFFFFF;

  var PHASE_TEXT = ['Unbekannt', 'Aus', 'Heizt', 'Nachlauf', 'Lüftet', 'Fehler'];
  var ERROR_TEXT = {
    0: 'Kein Fehler', 1: 'Batterie schwach', 2: 'Kraftstoff niedrig', 3: 'Heizung defekt',
    4: 'Crash-Abschaltung', 5: 'Sonstiger Fehler', 6: 'Überhitzung',
    8: 'Befehl nicht übernommen', 9: 'Nicht unterstützt', 10: 'Kein Bus'
  };
  var CLIMATE_TEXT = ['Aus', 'Bereit', 'Startet', 'Heizung läuft', 'Pause', 'Fühlerfehler', 'Spannung zu niedrig', 'Fehlgeschlagen'];
  var RESULT_TEXT = ['Ausstehend', 'OK', 'OK, unbestätigt', 'Nicht übernommen', 'Nicht unterstützt', 'Belegt', 'Kein Bus', 'Bus-Off', 'Zeitüberschreitung'];
  var STAGE_TEXT = ['Eingereiht', 'Gesendet', 'Bestätigt', 'Fertig', 'Fehlgeschlagen'];
  var ROLE_TEXT = ['Receiver', 'Sender'];
  var TRANSPORT_TEXT = ['CAN', 'W-BUS', 'kein'];

  function incompatible() { var e = new Error('Firmware inkompatibel'); e.code = 'INCOMPATIBLE'; return e; }

  function asView(x) {
    if (x instanceof DataView) return x;
    if (ArrayBuffer.isView(x)) return new DataView(x.buffer, x.byteOffset, x.byteLength);
    return new DataView(x);
  }

  function temp(dC) { return dC === INT16_MIN ? null : dC / 10; }

  function parseStatus(input) {
    var dv = asView(input);
    if (dv.byteLength < STATUS_LEN || dv.getUint8(0) !== STATUS_VERSION) throw incompatible();
    var f = dv.getUint8(1), e = dv.getUint8(2);
    var raw = {
      powerW: dv.getUint16(4, true), fuelUsedMl: dv.getUint16(6, true), fuelRate_mlph: dv.getUint16(8, true),
      interior_dC: dv.getInt16(10, true), outside_dC: dv.getInt16(12, true),
      coolant_dC: dv.getInt16(14, true), engineCoolant_dC: dv.getInt16(16, true),
      voltage_mV: dv.getUint16(18, true), odometer_km: dv.getUint32(20, true),
      targetC: dv.getUint8(24), durationMin: dv.getUint16(25, true), remainingMin: dv.getUint16(27, true),
      rssi: dv.getInt8(29), snr: dv.getInt8(30), linkAgeS: dv.getUint8(31),
      lastCmdId: dv.getUint8(32), lastCmdResult: dv.getUint8(33)
    };
    return {
      version: 1,
      phase: f & 7, busAwake: !!(f & 8), climateActive: !!(f & 16), interiorValid: !!(f & 32),
      transportHealthy: !!(f & 64), ignitionOn: !!(f & 128),
      errorCode: e & 15, errorStored: !!(e & 16), kl30Lost: !!(e & 32), assumed: !!(e & 64), stale: !!(e & 128),
      climateStatus: dv.getUint8(3),
      powerW: raw.powerW, fuelUsedMl: raw.fuelUsedMl, fuelRate_mlph: raw.fuelRate_mlph,
      targetC: raw.targetC, durationMin: raw.durationMin, remainingMin: raw.remainingMin,
      rssi: raw.rssi, snr: raw.snr, linkAgeS: raw.linkAgeS,
      lastCmdId: raw.lastCmdId, lastCmdResult: raw.lastCmdResult,
      raw: raw,
      // display values (null = unknown)
      interior: temp(raw.interior_dC), outside: temp(raw.outside_dC),
      coolant: temp(raw.coolant_dC), engineCoolant: temp(raw.engineCoolant_dC),
      voltage: raw.voltage_mV === 0 ? null : raw.voltage_mV / 1000,
      odometer: raw.odometer_km === ODO_UNKNOWN ? null : raw.odometer_km,
      fuelRateLph: raw.fuelRate_mlph / 1000,
      linkAgeSaturated: raw.linkAgeS === 255
    };
  }

  function parseInfo(input) {
    var dv = asView(input);
    if (dv.byteLength < 3 || dv.getUint8(0) !== 1) throw incompatible();
    var fw = '';
    for (var i = 3; i < dv.byteLength; i++) { var c = dv.getUint8(i); if (c === 0) break; fw += String.fromCharCode(c); }
    var role = dv.getUint8(1), tr = dv.getUint8(2);
    return { version: 1, role: role, roleText: ROLE_TEXT[role] || '?', transport: tr, transportText: TRANSPORT_TEXT[tr] || '?', fw: fw };
  }

  function parseResult(input) {
    var dv = asView(input);
    if (dv.byteLength < 4) throw new Error('Ergebnis zu kurz');
    var stage = dv.getUint8(1), result = dv.getUint8(2);
    return {
      cmdId: dv.getUint8(0), stage: stage, result: result, detail: dv.getUint8(3),
      final: stage === STAGE.DONE || stage === STAGE.FAILED,
      stageText: STAGE_TEXT[stage] || ('Stufe ' + stage), resultText: RESULT_TEXT[result] || ('Ergebnis ' + result)
    };
  }

  function encodeCommand(op, cmdId, args) {
    args = args || {};
    var b;
    if (op === OP.CLIMATE_SET) {
      var t = Math.max(10, Math.min(24, Math.round(args.targetC)));
      var d = Math.max(0, Math.min(65535, Math.round(args.durationMin)));
      b = [op, cmdId, t, d & 255, d >> 8];
    } else if (op >= OP.HEAT && op <= OP.QUERY) {
      b = [op, cmdId];
    } else throw new Error('Unbekannter Befehl ' + op);
    return new Uint8Array(b);
  }

  function nextCmdId(id) { return id >= 255 || id < 1 ? 1 : id + 1; }

  function errorText(code) { return ERROR_TEXT[code] || ('Fehler ' + code); }

  function formatDuration(min) {
    min = Math.max(0, Math.round(min));
    var h = Math.floor(min / 60), m = min % 60;
    return h === 0 ? m + ' min' : h + ' h ' + m + ' min';
  }

  function hexToBytes(hex) {
    var a = new Uint8Array(hex.length / 2);
    for (var i = 0; i < a.length; i++) a[i] = parseInt(hex.substr(i * 2, 2), 16);
    return a;
  }
  function bytesToHex(b) {
    return Array.prototype.map.call(b, function (x) { return ('0' + x.toString(16)).slice(-2); }).join('');
  }

  return {
    STATUS_LEN: STATUS_LEN, OP: OP, STAGE: STAGE, PHASE_TEXT: PHASE_TEXT, CLIMATE_TEXT: CLIMATE_TEXT,
    parseStatus: parseStatus, parseInfo: parseInfo, parseResult: parseResult, encodeCommand: encodeCommand,
    nextCmdId: nextCmdId, errorText: errorText, formatDuration: formatDuration,
    hexToBytes: hexToBytes, bytesToHex: bytesToHex
  };
}));
