'use strict';
const test = require('node:test');
const assert = require('node:assert');
const U = require('../util.js');

const rx1 = { id: 'a1', name: 'Webasto-RX-AB0C' }, rx2 = { id: 'a2', name: 'Webasto-RX-1234' };
const tx1 = { id: 'b1', name: 'Webasto-TX-AB0C' }, tx2 = { id: 'b2', name: 'Webasto-TX-9999' };
const other = { id: 'c1', name: 'Kopfhoerer' }, noName = { id: 'd1' };

test('pickDevice matches by stored id', () => {
  assert.strictEqual(U.pickDevice([rx2, rx1, tx1], { id: 'a1', role: 'receiver' }), rx1);
  assert.strictEqual(U.pickDevice([rx1, tx2], { id: 'b2', role: 'sender' }), tx2);
});

test('pickDevice falls back to the role prefix when the id is unknown', () => {
  assert.strictEqual(U.pickDevice([other, tx1, rx1], { id: 'zz', role: 'receiver' }), rx1);
  assert.strictEqual(U.pickDevice([other, rx1, tx1], { id: 'zz', role: 'sender' }), tx1);
  assert.strictEqual(U.pickDevice([rx1], { role: 'receiver' }), rx1);
});

test('pickDevice never picks the other role', () => {
  assert.strictEqual(U.pickDevice([tx1, tx2], { id: 'zz', role: 'receiver' }), null);
  assert.strictEqual(U.pickDevice([rx1], { id: 'zz', role: 'sender' }), null);
  // stored id belongs to a device of the other role (role setting changed): not picked
  assert.strictEqual(U.pickDevice([rx1], { id: 'a1', role: 'sender' }), null);
  assert.strictEqual(U.pickDevice([rx1, tx1], { id: 'a1', role: 'sender' }), tx1);
});

test('pickDevice handles empty list and missing saved data', () => {
  assert.strictEqual(U.pickDevice([], { id: 'a1', role: 'receiver' }), null);
  assert.strictEqual(U.pickDevice(null, { id: 'a1', role: 'receiver' }), null);
  assert.strictEqual(U.pickDevice([rx1], null), null);
  assert.strictEqual(U.pickDevice([rx1], {}), null);
  assert.strictEqual(U.pickDevice([rx1], { id: 'a1' }), null);
});

test('pickDevice with several permitted devices prefers id, then stored name, then first', () => {
  const all = [tx1, rx2, other, rx1, noName];
  assert.strictEqual(U.pickDevice(all, { id: 'a1', name: 'Webasto-RX-1234', role: 'receiver' }), rx1);
  assert.strictEqual(U.pickDevice(all, { id: 'zz', name: 'Webasto-RX-AB0C', role: 'receiver' }), rx1);
  assert.strictEqual(U.pickDevice(all, { id: 'zz', role: 'receiver' }), rx2);
  assert.strictEqual(U.pickDevice(all, { id: 'zz', role: 'sender' }), tx1);
});

test('pickDevice keeps a device whose name is unknown when the id matches', () => {
  assert.strictEqual(U.pickDevice([noName], { id: 'd1', role: 'receiver' }), noName);
  assert.strictEqual(U.pickDevice([noName, other], { id: 'zz', role: 'receiver' }), null);
});
