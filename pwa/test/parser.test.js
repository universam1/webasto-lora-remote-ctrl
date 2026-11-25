'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const P = require('../parser.js');
const V = JSON.parse(fs.readFileSync(path.join(__dirname, 'vectors.json'), 'utf8'));
const view = (hex) => new DataView(P.hexToBytes(hex).buffer);

const RAW = ['powerW', 'fuelUsedMl', 'fuelRate_mlph', 'interior_dC', 'outside_dC', 'coolant_dC', 'engineCoolant_dC',
  'voltage_mV', 'odometer_km', 'targetC', 'durationMin', 'remainingMin', 'rssi', 'snr', 'linkAgeS', 'lastCmdId', 'lastCmdResult'];

for (const v of V.bleStatusV1) {
  test('status ' + v.name, () => {
    const s = P.parseStatus(view(v.hex));
    for (const [k, want] of Object.entries(v.expect)) {
      if (RAW.includes(k)) assert.strictEqual(s.raw[k], want, 'raw.' + k);
      else assert.strictEqual(s[k], want, k);
    }
    const e = v.expect;
    const t = (x) => (x === -32768 ? null : x / 10);
    assert.strictEqual(s.interior, t(e.interior_dC));
    assert.strictEqual(s.outside, t(e.outside_dC));
    assert.strictEqual(s.coolant, t(e.coolant_dC));
    assert.strictEqual(s.engineCoolant, t(e.engineCoolant_dC));
    assert.strictEqual(s.voltage, e.voltage_mV === 0 ? null : e.voltage_mV / 1000);
    assert.strictEqual(s.odometer, e.odometer_km === 4294967295 ? null : e.odometer_km);
  });
}

test('sentinels give null in the unknown sample', () => {
  const s = P.parseStatus(view(V.bleStatusV1[2].hex));
  for (const k of ['interior', 'outside', 'coolant', 'engineCoolant', 'voltage', 'odometer']) assert.strictEqual(s[k], null, k);
});

test('accepts Uint8Array and longer buffers', () => {
  const b = P.hexToBytes(V.bleStatusV1[1].hex);
  assert.strictEqual(P.parseStatus(b).powerW, 4989);
  const longer = new Uint8Array(40); longer.set(b);
  assert.strictEqual(P.parseStatus(longer).targetC, 20);
});

test('rejects wrong version and short buffer', () => {
  const b = P.hexToBytes(V.bleStatusV1[0].hex); b[0] = 2;
  assert.throws(() => P.parseStatus(new DataView(b.buffer)), /Firmware inkompatibel/);
  assert.throws(() => P.parseStatus(view(V.bleStatusV1[0].hex.slice(0, 66))), /Firmware inkompatibel/);
  assert.throws(() => P.parseStatus(new DataView(new ArrayBuffer(0))), /Firmware inkompatibel/);
});

for (const c of V.commands) {
  test('command ' + c.name, () => {
    const args = c.op === 3 ? { targetC: 20, durationMin: 120 } : undefined;
    assert.strictEqual(P.bytesToHex(P.encodeCommand(c.op, c.cmdId, args)), c.hex);
  });
}

test('climateSet clamps and encodes LE', () => {
  assert.strictEqual(P.bytesToHex(P.encodeCommand(3, 9, { targetC: 30, durationMin: 480 })), '030918e001');
  assert.strictEqual(P.bytesToHex(P.encodeCommand(3, 9, { targetC: 5, durationMin: 10 })), '03090a0a00');
  assert.throws(() => P.encodeCommand(99, 1));
});

test('result', () => {
  const r = P.parseResult(view(V.result.hex));
  assert.strictEqual(r.cmdId, V.result.cmdId);
  assert.strictEqual(r.stage, V.result.stage);
  assert.strictEqual(r.result, V.result.result);
  assert.strictEqual(r.detail, V.result.detail);
  assert.strictEqual(r.final, true);
  assert.strictEqual(r.resultText, 'OK');
  assert.strictEqual(P.parseResult(view('07010000')).final, false);
  assert.strictEqual(P.parseResult(view('07040800')).final, true);
  assert.throws(() => P.parseResult(view('0703')));
});

test('cmdId never 0 and wraps', () => {
  assert.strictEqual(P.nextCmdId(1), 2);
  assert.strictEqual(P.nextCmdId(254), 255);
  assert.strictEqual(P.nextCmdId(255), 1);
  assert.strictEqual(P.nextCmdId(0), 1);
});

test('error texts', () => {
  assert.strictEqual(P.errorText(0), 'Kein Fehler');
  assert.strictEqual(P.errorText(3), 'Heizung defekt');
  assert.strictEqual(P.errorText(6), 'Überhitzung');
  assert.strictEqual(P.errorText(8), 'Befehl nicht übernommen');
  assert.strictEqual(P.errorText(10), 'Kein Bus');
  assert.strictEqual(P.errorText(7), 'Fehler 7');
});

test('duration label', () => {
  assert.strictEqual(P.formatDuration(120), '2 h 0 min');
  assert.strictEqual(P.formatDuration(90), '1 h 30 min');
  assert.strictEqual(P.formatDuration(45), '45 min');
  assert.strictEqual(P.formatDuration(480), '8 h 0 min');
});

test('LoRa sender fallback fields', () => {
  const b = P.hexToBytes(V.bleStatusV1[1].hex);
  b[31] = 255; b[27] = 0; b[28] = 0; b[25] = 0; b[26] = 0;
  let s = P.parseStatus(new DataView(b.buffer));
  assert.strictEqual(s.linkAgeS, 255);
  assert.strictEqual(s.linkAgeSaturated, true);
  assert.strictEqual(s.durationMin, 0);
  assert.strictEqual(s.remainingMin, 0);
  b[31] = 254;
  s = P.parseStatus(new DataView(b.buffer));
  assert.strictEqual(s.linkAgeSaturated, false);
});

test('info', () => {
  const i = P.parseInfo(new DataView(Uint8Array.from([1, 1, 0, 0x76, 0x35]).buffer));
  assert.strictEqual(i.roleText, 'Sender');
  assert.strictEqual(i.transportText, 'CAN');
  assert.strictEqual(i.fw, 'v5');
});

test('parser.js has no DOM dependency', () => {
  const src = fs.readFileSync(path.join(__dirname, '..', 'parser.js'), 'utf8');
  assert.doesNotMatch(src, /\b(document|navigator|localStorage)\b/);
  assert.strictEqual(typeof globalThis.window, 'undefined');
  assert.strictEqual(typeof globalThis.document, 'undefined');
  assert.ok(P.parseStatus);
});
