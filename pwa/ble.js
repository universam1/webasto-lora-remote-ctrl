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

  // Callbacks: onSearch('searching'|'timeout'|'idle'), onState(text, kind), onInfo(info), onStatus(status), onError(msg), onBusy(bool), onResult(res, text)
  function Ble(cb) {
    this.cb = cb || {};
    this.device = null; this.chr = null; this.info = null;
    this.userDisconnect = false; this.attempt = 0; this.timer = null;
    this._watch = null; this._opening = false; this.needsResume = false;
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

  var WATCH_TIMEOUT_MS = 8000;  // then one direct connect (some platforms deliver no advertisements)
  var MAX_ATTEMPTS = 8; // automatic retries after a failed connect, then wait for a visibility change or the button
  var ROLE_NAME = ['receiver', 'sender'];

  function save(k, v) { try { root.localStorage.setItem(k, v); } catch (e) { /* no storage */ } }
  function load(k) { try { return root.localStorage.getItem(k); } catch (e) { return null; } }
  function visible() { return !root.document || root.document.visibilityState !== 'hidden'; }
  function canList() { return supported() && typeof root.navigator.bluetooth.getDevices === 'function'; }

  Ble.prototype._search = function (kind) { if (this.cb.onSearch) this.cb.onSearch(kind); };

  Ble.prototype._attach = function (dev) {
    if (this.device && this.device !== dev) this.device.removeEventListener('gattserverdisconnected', this._onDisc);
    this.device = dev;
    dev.removeEventListener('gattserverdisconnected', this._onDisc);
    dev.addEventListener('gattserverdisconnected', this._onDisc);
  };

  // First pairing or switching the unit. Must be called from a user gesture. The chooser lists every unit with
  // the service; the names (Webasto-RX-XXXX / Webasto-TX-XXXX) tell receiver and sender apart.
  Ble.prototype.connect = async function () {
    if (!supported()) throw new Error('Web Bluetooth nicht verfügbar');
    this.cancelSearch(); this.userDisconnect = false; this.attempt = 0; clearTimeout(this.timer);
    this._state('Suche Gerät …', 'busy');
    var dev;
    try {
      dev = await root.navigator.bluetooth.requestDevice({
        filters: [{ services: [UUID.service] }],
        optionalServices: [UUID.service]
      });
    } catch (e) {
      this._state('Getrennt', 'off');
      if (e && e.name === 'NotFoundError') return false; // user cancelled
      throw e;
    }
    this._attach(dev);
    return this._open();
  };

  // Reconnect without the chooser. Resolves with 'connected' | 'failed' | 'unsupported' | 'none' | 'timeout' | 'busy' | 'cancel'.
  Ble.prototype.autoConnect = async function () {
    if (!canList()) return 'unsupported';
    if (this.isConnected() || this._watch || this._opening) return 'busy';
    if (!visible()) { this.needsResume = true; return 'timeout'; }
    var list;
    try { list = await root.navigator.bluetooth.getDevices(); } catch (e) { return 'unsupported'; }
    var dev = root.Util.pickDevice(list, { id: load('bleDeviceId'), name: load('bleDeviceName') });
    if (!dev) return 'none';
    this.userDisconnect = false; this.attempt = 0; clearTimeout(this.timer);
    return this._watchAndOpen(dev);
  };

  // Continue after the page became visible again (or from the retry button).
  Ble.prototype.resume = function () {
    if (this.userDisconnect || this.isConnected() || this._watch || this._opening || !visible()) return Promise.resolve('busy');
    this.needsResume = false; clearTimeout(this.timer);
    if (this.device) return this._watchAndOpen(this.device);
    return this.autoConnect();
  };

  // Waits for one advertisement of dev (so it is known to be in range), then connects. Never loops.
  Ble.prototype._watchAndOpen = async function (dev) {
    var self = this;
    this._attach(dev);
    if (typeof dev.watchAdvertisements !== 'function') return (await this._open()) ? 'connected' : 'failed';
    var ac = new root.AbortController(), done = false, result = 'timeout', t, onAdv;
    var finished = new Promise(function (resolve) {
      function end(r) {
        if (done) return; done = true; result = r; clearTimeout(t);
        dev.removeEventListener('advertisementreceived', onAdv);
        try { ac.abort(); } catch (e) { /* ignore */ }
        resolve();
      }
      onAdv = function () { end('found'); };
      dev.addEventListener('advertisementreceived', onAdv);
      ac.signal.addEventListener('abort', function () { end('cancel'); });
      t = setTimeout(function () { end('timeout'); }, WATCH_TIMEOUT_MS);
      self._watch = { cancel: function () { end('cancel'); } };
    });
    this._search('searching'); this._state('Suche Gerät …', 'busy');
    try { await dev.watchAdvertisements({ signal: ac.signal }); }
    catch (e) { // not allowed or not supported: plain connect instead
      if (!done) { this._watch = null; done = true; clearTimeout(t); dev.removeEventListener('advertisementreceived', onAdv); this._search('idle'); return (await this._open()) ? 'connected' : 'failed'; }
    }
    await finished;
    this._watch = null; this._search('idle');
    if (result === 'found') return (await this._open()) ? 'connected' : 'failed';
    if (result === 'cancel') { this._state('Getrennt', 'off'); return 'cancel'; }
    // No advertisement seen. On Linux/BlueZ (and macOS) Chrome often delivers none for an already
    // bonded device, although a direct connect works at once (measured 2026-11-27, Chrome 158).
    // Try one direct connect before reporting "not in range".
    if (await this._open(true)) return 'connected';
    this.needsResume = true;
    this._search('timeout'); this._state('Gerät nicht in Reichweite', 'off');
    return 'timeout';
  };

  Ble.prototype.cancelSearch = function () { if (this._watch) this._watch.cancel(); };

  // quiet: a failed attempt neither reports an error nor schedules a reconnect (used for the
  // direct-connect fallback after an advertisement watch that saw nothing).
  Ble.prototype._open = async function (quiet) {
    if (this._opening) return false;
    this._opening = true;
    try { return await this._openInner(!!quiet); } finally { this._opening = false; }
  };

  Ble.prototype._openInner = async function (quiet) {
    var self = this;
    this._state('Verbinde …', 'busy');
    try {
      var server = await this.device.gatt.connect();
      var svc = await server.getPrimaryService(UUID.service);
      var c = {
        info: await svc.getCharacteristic(UUID.info), status: await svc.getCharacteristic(UUID.status),
        command: await svc.getCharacteristic(UUID.command), result: await svc.getCharacteristic(UUID.result)
      };
      var info = P.parseInfo(await c.info.readValue());
      this.chr = c; this.info = info;
      save('bleDeviceId', this.device.id || ''); save('bleDeviceName', this.device.name || ''); save('bleDeviceRole', ROLE_NAME[info.role] || '');
      if (this.cb.onInfo) this.cb.onInfo(this.info, this.device.name || '');
      c.status.removeEventListener('characteristicvaluechanged', this._onStatus);
      c.result.removeEventListener('characteristicvaluechanged', this._onResult);
      c.status.addEventListener('characteristicvaluechanged', this._onStatus);
      c.result.addEventListener('characteristicvaluechanged', this._onResult);
      await c.status.startNotifications();
      await c.result.startNotifications();
      this._handleStatus(await c.status.readValue());
      this.attempt = 0; this.needsResume = false;
      this._state('Verbunden', 'ok');
      return true;
    } catch (e) {
      if (e && e.code === 'INCOMPATIBLE') {
        this.userDisconnect = true; // do not retry against incompatible firmware
        this.chr = null;
        if (this.cb.onError) this.cb.onError(e.message);
        this._state('Firmware inkompatibel', 'err');
        try { if (this.device.gatt.connected) this.device.gatt.disconnect(); } catch (x) { /* ignore */ }
        return false;
      }
      if (quiet) { this.chr = null; return false; }
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

  // After a link drop or a failed connect: wait (back-off), then look for the device's advertisement first
  // (it may be out of range) and connect only if it is seen. Gives up after MAX_ATTEMPTS.
  Ble.prototype._schedule = function () {
    var self = this;
    if (this.userDisconnect || !this.device) { this._state('Getrennt', 'off'); return; }
    if (this.attempt >= MAX_ATTEMPTS) {
      this.needsResume = true; this._search('timeout'); this._state('Verbindung fehlgeschlagen', 'off'); return;
    }
    var s = BACKOFF_S[Math.min(this.attempt, BACKOFF_S.length - 1)];
    this.attempt++;
    this._state('Getrennt, neuer Versuch in ' + s + ' s', 'busy');
    clearTimeout(this.timer);
    this.timer = setTimeout(function () {
      if (self.userDisconnect || self.isConnected() || self._watch || self._opening) return;
      if (!visible()) { self.needsResume = true; self._search('timeout'); self._state('Getrennt', 'off'); return; }
      self._watchAndOpen(self.device);
    }, s * 1000);
  };

  Ble.prototype.disconnect = function () {
    this.userDisconnect = true; clearTimeout(this.timer); this.cancelSearch();
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

  root.Ble = { UUID: UUID, supported: supported, canList: canList, WATCH_TIMEOUT_MS: WATCH_TIMEOUT_MS, create: function (cb) { return new Ble(cb); }, BACKOFF_S: BACKOFF_S, CMD_TIMEOUT_MS: CMD_TIMEOUT_MS };
}(typeof self !== 'undefined' ? self : this));
