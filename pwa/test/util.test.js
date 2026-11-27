'use strict';
const test = require('node:test');
const assert = require('node:assert');
const U = require('../util.js');

const rx1 = { id: 'a1', name: 'Webasto-RX-AB0C' }, rx2 = { id: 'a2', name: 'Webasto-RX-1234' };
const tx1 = { id: 'b1', name: 'Webasto-TX-AB0C' };
const other = { id: 'c1', name: 'Kopfhoerer' }, noName = { id: 'd1' };

test('pickDevice matches by stored id, whatever the role', () => {
  assert.strictEqual(U.pickDevice([rx2, rx1, tx1], { id: 'a1' }), rx1);
  assert.strictEqual(U.pickDevice([rx1, tx1], { id: 'b1' }), tx1);
  assert.strictEqual(U.pickDevice([noName], { id: 'd1' }), noName);
});

test('pickDevice falls back to the stored name when the id changed', () => {
  assert.strictEqual(U.pickDevice([other, tx1, rx1], { id: 'zz', name: 'Webasto-TX-AB0C' }), tx1);
  assert.strictEqual(U.pickDevice([rx1, rx2], { name: 'Webasto-RX-1234' }), rx2);
});

test('pickDevice prefers the id over the name', () => {
  assert.strictEqual(U.pickDevice([rx1, tx1], { id: 'a1', name: 'Webasto-TX-AB0C' }), rx1);
});

test('pickDevice picks nothing without a match or saved data', () => {
  assert.strictEqual(U.pickDevice([rx1, tx1], { id: 'zz', name: 'Webasto-RX-FFFF' }), null);
  assert.strictEqual(U.pickDevice([rx1], {}), null);
  assert.strictEqual(U.pickDevice([rx1], null), null);
  assert.strictEqual(U.pickDevice([], { id: 'a1' }), null);
  assert.strictEqual(U.pickDevice(null, { id: 'a1' }), null);
});

test('ROLE_PREFIX names stay available for display', () => {
  assert.strictEqual(U.ROLE_PREFIX.receiver, 'Webasto-RX');
  assert.strictEqual(U.ROLE_PREFIX.sender, 'Webasto-TX');
});
