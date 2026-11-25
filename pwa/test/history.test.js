'use strict';
const test = require('node:test');
const assert = require('node:assert');
const H = require('../history.js');
const U = require('../util.js');

const MIN = 60000, T0 = Date.UTC(2026, 0, 1, 12, 0, 0);
const mk = (t, o) => Object.assign({ t, phase: 1, powerW: 0, fuelUsedMl: 0, coolant: 20, interior: 18, outside: 5, voltage: 12.6, climateStatus: 0, targetC: null }, o || {});
const status = (o) => Object.assign({ phase: 1, climateActive: false, climateStatus: 0, errorCode: 0, powerW: 0, fuelUsedMl: 0, coolant: 20, interior: 18, outside: 5, voltage: 12.6, targetC: 20 }, o || {});

test('norm turns undefined and NaN into null, never zero', () => {
  const s = H.norm({ t: 1, phase: 2, powerW: undefined, coolant: NaN, interior: null });
  assert.strictEqual(s.powerW, null); assert.strictEqual(s.coolant, null); assert.strictEqual(s.interior, null);
  assert.strictEqual(s.phase, 2);
});

test('ring buffer caps at maxSamples and rejects out-of-order samples', () => {
  const h = H.create({ maxSamples: 5 });
  for (let i = 0; i < 8; i++) assert.ok(h.add(mk(T0 + i * 5000)));
  assert.strictEqual(h.size(), 5);
  assert.strictEqual(h.samples()[0].t, T0 + 3 * 5000);
  assert.strictEqual(h.add(mk(T0)), false);
  assert.strictEqual(h.add({ t: NaN }), false);
});

test('default capacity is 24 h at 5 s', () => { assert.strictEqual(H.DEFAULTS.maxSamples, 17280); });

test('since() returns samples at or after t', () => {
  const h = H.create();
  for (let i = 0; i < 10; i++) h.add(mk(T0 + i * 1000));
  assert.strictEqual(h.since(T0 + 4000).length, 6);
  assert.strictEqual(h.since(T0 + 99999).length, 0);
});

test('compact keeps the last 2 h full and averages older data per minute', () => {
  const h = H.create(), now = T0 + 5 * 3600000;
  for (let t = now - 5 * 3600000; t <= now; t += 5000) h.add(mk(t, { interior: (t % 120000) < 60000 ? 10 : 20 }));
  const before = h.size();
  h.compact(now);
  assert.ok(h.size() < before / 2);
  const old = h.samples().filter((s) => s.t < now - 2 * 3600000);
  const recent = h.samples().filter((s) => s.t >= now - 2 * 3600000);
  assert.strictEqual(recent.length, 2 * 3600000 / 5000 + 1);
  assert.ok(old.length <= 3 * 60 + 1 && old.length >= 170);
  assert.ok(old.every((s) => s.n === 12 || s.n < 12));
  assert.ok(Math.abs(old[5].interior - 10) < 0.01 || Math.abs(old[5].interior - 20) < 0.01);
});

test('compact drops data older than 24 h', () => {
  const h = H.create(), now = T0 + 30 * 3600000;
  h.add(mk(now - 25 * 3600000)); h.add(mk(now - 3 * 3600000)); h.add(mk(now));
  h.compact(now);
  assert.strictEqual(h.size(), 2);
});

test('merge: nulls are ignored for averages, all-null stays null, fuel takes the maximum', () => {
  const a = H.norm(mk(T0, { interior: null, outside: 4, fuelUsedMl: 10, powerW: 1000 }));
  const b = H.norm(mk(T0 + 1000, { interior: null, outside: 6, fuelUsedMl: 40, powerW: 3000 }));
  const m = H.merge([a, b]);
  assert.strictEqual(m.interior, null);
  assert.strictEqual(m.outside, 5);
  assert.strictEqual(m.powerW, 2000);
  assert.strictEqual(m.fuelUsedMl, 40);
  assert.strictEqual(m.n, 2);
});

test('merge keeps the gap flag', () => {
  const m = H.merge([H.norm(mk(T0, { g: 1 })), H.norm(mk(T0 + 1000))]);
  assert.strictEqual(m.g, 1);
});

test('pack/unpack round trip, null survives', () => {
  const s = H.norm(mk(T0, { coolant: null, g: 1, n: 12 }));
  const u = H.unpack(JSON.parse(JSON.stringify(H.pack(s))));
  assert.deepStrictEqual(u, s);
  assert.strictEqual(H.unpack('x'), null);
  assert.strictEqual(H.unpack([null]), null);
});

test('load merges stored rows with samples recorded meanwhile, sorted and de-duplicated', () => {
  const h = H.create();
  h.add(mk(T0 + 20000));
  const n = h.load([H.pack(H.norm(mk(T0))), H.pack(H.norm(mk(T0 + 10000))), H.pack(H.norm(mk(T0 + 20000))), 'junk']);
  assert.strictEqual(n, 3);
  assert.deepStrictEqual(h.samples().map((s) => s.t), [T0, T0 + 10000, T0 + 20000]);
});

test('recorder samples every 5 s and immediately on state change', () => {
  const r = H.createRecorder();
  assert.ok(r.offer(status(), T0));
  assert.strictEqual(r.offer(status(), T0 + 2000), null);
  assert.ok(r.offer(status({ phase: 2 }), T0 + 2500), 'phase change');
  assert.strictEqual(r.offer(status({ phase: 2 }), T0 + 4000), null);
  assert.ok(r.offer(status({ phase: 2 }), T0 + 7600));
  assert.ok(r.offer(status({ phase: 2, errorCode: 6 }), T0 + 8000), 'error change');
});

test('recorder marks the first sample and the first one after markGap as a gap', () => {
  const r = H.createRecorder();
  assert.strictEqual(r.offer(status(), T0).g, 1);
  assert.strictEqual(r.offer(status(), T0 + 5000).g, 0);
  r.markGap();
  assert.strictEqual(r.offer(status(), T0 + 10000).g, 1);
  assert.strictEqual(r.offer(status(), T0 + 10000 + H.GAP_MS + 5000).g, 1, 'long silence is a gap');
});

test('fromStatus: unknown phase gives null power/fuel, climate target only while climate runs', () => {
  const u = H.fromStatus(status({ phase: 0, powerW: 0, fuelUsedMl: 0, interior: null }), T0);
  assert.strictEqual(u.powerW, null); assert.strictEqual(u.fuelUsedMl, null); assert.strictEqual(u.interior, null);
  assert.strictEqual(H.fromStatus(status(), T0).targetC, null);
  assert.strictEqual(H.fromStatus(status({ climateActive: true, climateStatus: 3, targetC: 21 }), T0).targetC, 21);
});

function series(spec) { // spec: [[phase, fuel], ...] one sample per minute
  return spec.map((p, i) => H.norm(mk(T0 + i * MIN, { phase: p[0], fuelUsedMl: p[1] })));
}

test('fuelRuns sums the maxima of each run (counter resets)', () => {
  const r = H.fuelRuns(series([[1, 0], [2, 10], [2, 120], [2, 300], [3, 310], [1, 310], [1, 310], [2, 5], [2, 90], [2, 200], [1, 200]]));
  assert.deepStrictEqual(r.runs.map((x) => x.ml), [310, 200]);
  assert.strictEqual(r.total, 510);
});

test('fuelRuns: a held counter value without heating does not count as a run', () => {
  const r = H.fuelRuns(series([[1, 250], [1, 250], [1, 250]]));
  assert.strictEqual(r.runs.length, 0);
  assert.strictEqual(r.total, 0);
});

test('fuelRuns: new run with a counter that did not drop is still split at the heating start', () => {
  const r = H.fuelRuns(series([[2, 100], [2, 150], [1, 150], [1, 150], [2, 160], [2, 200], [1, 200]]));
  assert.strictEqual(r.runs.length, 2);
});

test('fuelRuns ignores unknown counters', () => {
  const s = series([[2, 10], [2, 50], [2, 90]]); s[1].fuelUsedMl = null;
  assert.strictEqual(H.fuelRuns(s).total, 90);
  assert.deepStrictEqual(H.fuelRuns([]), { runs: [], total: 0 });
});

test('bands cover heating intervals and break at gaps', () => {
  const s = series([[1, 0], [2, 1], [2, 2], [1, 2], [2, 1], [2, 2]]);
  assert.deepStrictEqual(H.bands(s), [[T0 + MIN, T0 + 3 * MIN], [T0 + 4 * MIN, T0 + 5 * MIN]]);
  const g = series([[2, 1], [2, 2], [2, 3]]); g[2].g = 1;
  assert.strictEqual(H.bands(g).length, 2);
});

test('CSV: header, German decimals, empty cells for unknown values, gap flag', () => {
  const csv = H.toCsv([H.norm(mk(T0, { interior: 18.25, coolant: null, g: 1, phase: 2, powerW: 4500, voltage: 12.345 }))], { phaseText: ['?', 'Aus', 'Heizt'] });
  const lines = csv.trim().split('\r\n');
  assert.strictEqual(lines.length, 2);
  assert.match(lines[0], /^Zeit;Phase;Phase_Text;Leistung_W/);
  const c = lines[1].split(';');
  assert.strictEqual(c.length, lines[0].split(';').length);
  assert.match(c[0], /^2026-01-01 \d\d:\d\d:\d\d$/);
  assert.strictEqual(c[2], 'Heizt'); assert.strictEqual(c[3], '4500');
  assert.strictEqual(c[5], '', 'coolant unknown stays empty');
  assert.match(c[6], /^18[,.]\d$/); assert.ok(c[6].includes(','));
  assert.strictEqual(c[8], '12,35'.replace('12,35', (12.345).toFixed(2).replace('.', ',')));
  assert.strictEqual(c[11], '1');
  assert.ok(H.toCsv([], { decimal: '.' }).startsWith('Zeit'));
});

test('openStore resolves to null/false when IndexedDB is missing', async () => {
  const s = H.openStore('x');
  assert.strictEqual(await s.load(), null);
  assert.strictEqual(await s.save([[1]]), false);
  assert.strictEqual(await s.clear(), false);
});

test('util: ago text, stale', () => {
  assert.strictEqual(U.agoText(3200), 'vor 3 s');
  assert.strictEqual(U.agoText(125000), 'vor 2 min');
  assert.strictEqual(U.agoText(3 * 3600000), 'vor 3 h');
  assert.strictEqual(U.agoText(null), 'Noch keine Daten');
  assert.ok(U.isStale(31000)); assert.ok(!U.isStale(5000)); assert.ok(U.isStale(null));
});

test('util: error info covers codes 1..6 with help text', () => {
  for (let c = 1; c <= 6; c++) { const e = U.errorInfo(c); assert.ok(e.title && e.help.length > 20, 'code ' + c); }
  assert.strictEqual(U.errorInfo(1).title, 'Batterie leer');
  assert.strictEqual(U.errorInfo(6).title, 'Überhitzung');
  assert.ok(U.errorInfo(77).help);
});

test('util: presets and matching', () => {
  assert.strictEqual(U.matchPreset(21, 240), 'comfort');
  assert.strictEqual(U.matchPreset(16, 480), 'eco');
  assert.strictEqual(U.matchPreset(10, 480), 'frost');
  assert.strictEqual(U.matchPreset(20, 240), null);
  assert.strictEqual(U.presetLabel(U.PRESETS[0]), 'Komfort 21 °C / 4 h');
});

test('util: clock, hm, share text', () => {
  assert.strictEqual(U.hm(135), '2:15');
  assert.match(U.clock(T0, false), /^\d\d:\d\d$/);
  assert.match(U.clock(T0, true), /^\d+:\d\d (AM|PM)$/);
  const txt = U.shareText({ phase: 2, errorCode: 0, interior: 18.5, outside: null, coolant: 70, powerW: 4200, voltage: 12.4, climateActive: true, remainingMin: 90, targetC: 21 },
    { phaseText: ['Unbekannt', 'Aus', 'Heizt'] });
  assert.match(txt, /Heizt/); assert.match(txt, /Innen 18,5 °C/); assert.match(txt, /4,2 kW/); assert.match(txt, /Rest 1:30/);
  assert.ok(!/Außen/.test(txt));
});
