// Web Bluetooth wrapper: connection, reconnect with back-off, commands one at a time.
(function (root) {
  'use strict';
  var P = root.Parser;
  var BASE = '-4b1d-4c5e-9a3f-2f6d0c1b8a10';
  var UUID = {
    service: '7e5a0001' + BASE, info: '7e5a0002' + BASE, status: '7e5a0003' + BASE,
    command: '7e5a0004' + BASE, result: '7e5a0005' + BASE
  };
  var BACKOFF_S = [1, 2, 4, 8, 15];
  var CMD_TIMEOUT_MS = 25000;

  function supported() { return !!(root.navigator && root.navigator.bluetooth); }

  // Callbacks: onState(text, kind), onInfo(info), onStatus(status), onError(msg), onBusy(bool), onResult(res, text)
  function Ble(cb) {
    this.cb = cb || {};
    this.device = null; this.chr = null; this.info = null;
    this.userDisconnect = false; this.attempt = 0; this.timer = null;
    this.cmdId = 0; this.pending = null;
    var self = this;
    this._onStatus = function (ev) { self._handleStatus(ev.target.value); };
    this._onResult = function (ev) { self._handleResult(ev.target.value); };
    this._onDisc = function () { self._handleDisconnect(); };
  }

  Ble.prototype._state = function (t, kind) { if (this.cb.onState) this.cb.onState(t, kind || ''); };

  Ble.prototype._handleStatus = function (dv) {
    try { var s = P.parseStatus(dv); if (this.cb.onStatus) this.cb.onStatus(s); }
    catch (e) { if (this.cb.onError) this.cb.onError(e.message); }
  };

  Ble.prototype._handleResult = function (dv) {
    var r;
    try { r = P.parseResult(dv); } catch (e) { return; }
    var p = this.pending;
    if (!p || r.cmdId !== p.id) return;
    var text = r.stageText + (r.stage >= 3 ? ': ' + r.resultText : '');
    if (this.cb.onResult) this.cb.onResult(r, text);
    if (r.final) this._finish(r.stage === P.STAGE.DONE, text, r);
  };

  Ble.prototype._finish = function (ok, text, r) {
    var p = this.pending; if (!p) return;
    clearTimeout(p.timer); this.pending = null;
    if (this.cb.onBusy) this.cb.onBusy(false);
    p.resolve({ ok: ok, text: text, result: r || null });
  };

  // Must be called from a user gesture.
  Ble.prototype.connect = async function () {
    if (!supported()) throw new Error('Web Bluetooth nicht verfügbar');
    this.userDisconnect = false; this.attempt = 0; clearTimeout(this.timer);
    this._state('Suche Gerät …', 'busy');
    try {
      this.device = await root.navigator.bluetooth.requestDevice({
        filters: [{ services: [UUID.service] }], optionalServices: [UUID.service]
      });
    } catch (e) {
      this._state('Getrennt', 'off');
      if (e && e.name === 'NotFoundError') return false; // user cancelled
      throw e;
    }
    this.device.removeEventListener('gattserverdisconnected', this._onDisc);
    this.device.addEventListener('gattserverdisconnected', this._onDisc);
    return this._open();
  };

  Ble.prototype._open = async function () {
    var self = this;
    this._state('Verbinde …', 'busy');
    try {
      var server = await this.device.gatt.connect();
      var svc = await server.getPrimaryService(UUID.service);
      var c = {
        info: await svc.getCharacteristic(UUID.info), status: await svc.getCharacteristic(UUID.status),
        command: await svc.getCharacteristic(UUID.command), result: await svc.getCharacteristic(UUID.result)
      };
      this.chr = c;
      this.info = P.parseInfo(await c.info.readValue());
      if (this.cb.onInfo) this.cb.onInfo(this.info, this.device.name || '');
      c.status.removeEventListener('characteristicvaluechanged', this._onStatus);
      c.result.removeEventListener('characteristicvaluechanged', this._onResult);
      c.status.addEventListener('characteristicvaluechanged', this._onStatus);
      c.result.addEventListener('characteristicvaluechanged', this._onResult);
      await c.status.startNotifications();
      await c.result.startNotifications();
      this._handleStatus(await c.status.readValue());
      this.attempt = 0;
      this._state('Verbunden', 'ok');
      return true;
    } catch (e) {
      if (e && e.code === 'INCOMPATIBLE') {
        this.userDisconnect = true; // do not retry against incompatible firmware
        if (this.cb.onError) this.cb.onError(e.message);
        this._state('Firmware inkompatibel', 'err');
        try { if (this.device.gatt.connected) this.device.gatt.disconnect(); } catch (x) { /* ignore */ }
        return false;
      }
      if (this.cb.onError) this.cb.onError('Verbindung fehlgeschlagen: ' + (e && e.message ? e.message : e));
      this._schedule();
      return false;
    }
  };

  Ble.prototype._handleDisconnect = function () {
    if (this.pending) this._finish(false, 'Verbindung getrennt', null);
    this.chr = null;
    if (this.userDisconnect) { this._state('Getrennt', 'off'); return; }
    this._schedule();
  };

  Ble.prototype._schedule = function () {
    var self = this;
    if (this.userDisconnect || !this.device) { this._state('Getrennt', 'off'); return; }
    var s = BACKOFF_S[Math.min(this.attempt, BACKOFF_S.length - 1)];
    this.attempt++;
    this._state('Getrennt, neuer Versuch in ' + s + ' s', 'busy');
    clearTimeout(this.timer);
    this.timer = setTimeout(function () { if (!self.userDisconnect) self._open(); }, s * 1000);
  };

  Ble.prototype.disconnect = function () {
    this.userDisconnect = true; clearTimeout(this.timer);
    if (this.pending) this._finish(false, 'Verbindung getrennt', null);
    try { if (this.device && this.device.gatt.connected) this.device.gatt.disconnect(); } catch (e) { /* ignore */ }
    this._state('Getrennt', 'off');
  };

  Ble.prototype.isConnected = function () {
    return !!(this.chr && this.device && this.device.gatt && this.device.gatt.connected);
  };
  Ble.prototype.isBusy = function () { return !!this.pending; };

  // Resolves with {ok, text, result}. Only one command at a time.
  Ble.prototype.send = function (op, args) {
    var self = this;
    if (!this.isConnected()) return Promise.resolve({ ok: false, text: 'Nicht verbunden' });
    if (this.pending) return Promise.resolve({ ok: false, text: 'Befehl läuft noch' });
    this.cmdId = P.nextCmdId(this.cmdId);
    var id = this.cmdId, data = P.encodeCommand(op, id, args);
    return new Promise(function (resolve) {
      self.pending = {
        id: id, resolve: resolve,
        timer: setTimeout(function () { self._finish(false, 'Zeitüberschreitung', null); }, CMD_TIMEOUT_MS)
      };
      if (self.cb.onBusy) self.cb.onBusy(true);
      self.chr.command.writeValueWithResponse(data).catch(function (e) {
        self._finish(false, 'Senden fehlgeschlagen: ' + (e && e.message ? e.message : e), null);
      });
    });
  };

  root.Ble = { UUID: UUID, supported: supported, create: function (cb) { return new Ble(cb); }, BACKOFF_S: BACKOFF_S, CMD_TIMEOUT_MS: CMD_TIMEOUT_MS };
}(typeof self !== 'undefined' ? self : this));
