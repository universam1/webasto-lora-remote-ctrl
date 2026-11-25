'use strict';
const test = require('node:test');
const assert = require('node:assert');
const C = require('../charts.js');

test('niceTicks gives round values covering the range', () => {
  const a = C.niceTicks(3.2, 21.7, 4);
  assert.deepStrictEqual(a.ticks, [0, 5, 10, 15, 20, 25]);
  assert.ok(a.min <= 3.2 && a.max >= 21.7);
  const b = C.niceTicks(0, 5.2, 4);
  assert.deepStrictEqual(b.ticks, [0, 1, 2, 3, 4, 5, 6].slice(0, b.ticks.length));
  assert.ok(b.max >= 5.2);
  const c = C.niceTicks(11.8, 12.9, 4);
  assert.ok(c.ticks.every((v) => Math.abs(v * 10 - Math.round(v * 10)) < 1e-9), 'no float noise: ' + c.ticks);
});

test('niceTicks handles a flat range and bad input', () => {
  const a = C.niceTicks(5, 5, 4);
  assert.ok(a.min < 5 && a.max > 5);
  assert.ok(C.niceTicks(NaN, NaN).ticks.length >= 2);
  assert.ok(C.niceTicks(-3, 4, 4).ticks.includes(0));
});

test('timeTicks lands on round local times and respects the max count', () => {
  const t1 = new Date(2026, 0, 1, 14, 7).getTime(), t0 = t1 - 30 * 60000;
  const r = C.timeTicks(t0, t1, 5);
  assert.ok(r.ticks.length >= 2 && r.ticks.length <= 6);
  r.ticks.forEach((t) => { assert.ok(t >= t0 && t <= t1); assert.strictEqual(new Date(t).getMinutes() % r.stepMin, 0); });
  const day = C.timeTicks(t1 - 24 * 3600000, t1, 5);
  assert.ok(day.ticks.length <= 6);
  day.ticks.forEach((t) => assert.strictEqual(new Date(t).getMinutes(), 0));
});

test('segments break at null values, gap flags and long time gaps', () => {
  const d = [{ t: 0, v: 1 }, { t: 5000, v: 2 }, { t: 10000, v: null }, { t: 15000, v: 3 }, { t: 20000, v: 4, g: 1 }, { t: 25000, v: 5 }, { t: 400000, v: 6 }];
  const s = C.segments(d, 150000);
  assert.deepStrictEqual(s.map((x) => x.length), [2, 1, 2, 1]);
});

test('linePath draws nothing for all-null data and a dot for a single point', () => {
  const xf = (t) => t / 1000, yf = (v) => 100 - v;
  assert.strictEqual(C.linePath([{ t: 0, v: null }], xf, yf), '');
  assert.match(C.linePath([{ t: 1000, v: 5 }], xf, yf), /^M1 95h0\.1$/);
  const p = C.linePath([{ t: 0, v: 0 }, { t: 1000, v: 10 }, { t: 2000, v: null }, { t: 3000, v: 20 }, { t: 4000, v: 30 }], xf, yf);
  assert.strictEqual((p.match(/M/g) || []).length, 2, 'null breaks the line');
});

test('areaPath closes to the baseline and skips single points', () => {
  const xf = (t) => t / 1000, yf = (v) => 100 - v;
  const p = C.areaPath([{ t: 0, v: 10 }, { t: 1000, v: 20 }], xf, yf, 100);
  assert.match(p, /^M0 100L0 90L1 80L1 100Z$/);
  assert.strictEqual(C.areaPath([{ t: 0, v: 10 }], xf, yf, 100), '');
});

test('extent only looks at visible, known values', () => {
  const s = [{ data: [{ t: 0, v: 100 }, { t: 10, v: 5 }, { t: 20, v: null }, { t: 30, v: 9 }, { t: 99, v: -50 }] }];
  assert.deepStrictEqual(C.extent(s, 5, 40), { min: 5, max: 9 });
  assert.strictEqual(C.extent([{ data: [{ t: 1, v: null }] }], 0, 10), null);
});

test('nearestIndex', () => {
  const d = [{ t: 0 }, { t: 10 }, { t: 20 }];
  assert.strictEqual(C.nearestIndex(d, -5), 0);
  assert.strictEqual(C.nearestIndex(d, 12), 1);
  assert.strictEqual(C.nearestIndex(d, 16), 2);
  assert.strictEqual(C.nearestIndex(d, 99), 2);
  assert.strictEqual(C.nearestIndex([], 1), -1);
});

test('bucketize averages per bucket, keeps unknown buckets null and gap flags', () => {
  const d = [{ t: 0, v: 10 }, { t: 1000, v: 20 }, { t: 2000, v: null }, { t: 3000, v: null }, { t: 4000, v: 7, g: 1 }];
  const b = C.bucketize(d, 0, 5000, 5);
  const b2 = C.bucketize(d, 0, 5000, 2);
  assert.strictEqual(b.length, 5);
  assert.deepStrictEqual(b2.map((p) => p.v), [15, null, 7]);
  assert.strictEqual(b[2].v, null);
  assert.strictEqual(b[4].g, 1);
});
