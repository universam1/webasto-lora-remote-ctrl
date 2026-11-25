// Fake device for ?demo=1: same interface as Ble.create(), cycles through the sample states.
(function (root) {
  'use strict';
  var P = root.Parser;
  var SAMPLES = [
    '01e900000000000000008f0055005a005a0000327a85040000000000000000000000',
    '017a00037d137b0045028f005500c80280025e337a8504001478006100a907000701',
    '0105f30000000000000000800080008000800000ffffffff0000000000000000ff03'
  ];

  function create(cb) {
    var idx = 0, timer = null, connected = false, busy = false, cmdId = 0;
    function push() { cb.onStatus(P.parseStatus(P.hexToBytes(SAMPLES[idx]))); }
    var api = {
      connect: async function () {
        cb.onState('Verbinde …', 'busy');
        await new Promise(function (r) { setTimeout(r, 300); });
        connected = true;
        cb.onInfo({ role: 0, roleText: 'Receiver', transport: 0, transportText: 'CAN', fw: 'demo' }, 'Demo-Gerät');
        cb.onState('Verbunden (Demo)', 'ok');
        push();
        timer = setInterval(function () { idx = (idx + 1) % SAMPLES.length; push(); }, 6000);
        return true;
      },
      disconnect: function () { connected = false; clearInterval(timer); cb.onState('Getrennt', 'off'); },
      isConnected: function () { return connected; },
      isBusy: function () { return busy; },
      send: function () {
        if (busy) return Promise.resolve({ ok: false, text: 'Befehl läuft noch' });
        cmdId = P.nextCmdId(cmdId); busy = true; cb.onBusy(true);
        var stages = ['Eingereiht', 'Gesendet', 'Bestätigt'];
        return new Promise(function (resolve) {
          var i = 0;
          (function step() {
            if (i < stages.length) { cb.onResult({ stage: i }, stages[i]); i++; setTimeout(step, 500); return; }
            busy = false; cb.onBusy(false);
            resolve({ ok: true, text: 'Fertig: OK' });
          }());
        });
      }
    };
    return api;
  }
  root.Demo = { create: create };
}(typeof self !== 'undefined' ? self : this));
