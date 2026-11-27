// Small pure helpers (formatting, error help, presets, share text). No DOM, usable in browser and Node.
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.Util = factory();
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var VERSION = '2.0.0';
  var STALE_MS = 30000;

  var PRESETS = [
    { id: 'comfort', label: 'Komfort', targetC: 21, durationMin: 240 },
    { id: 'eco', label: 'Eco', targetC: 16, durationMin: 480 },
    { id: 'frost', label: 'Frostschutz', targetC: 10, durationMin: 480 }
  ];
  var DURATIONS = [60, 120, 240, 480];

  var ERR_HELP = {
    1: { title: 'Batterie leer', help: 'Fahrzeugbatterie laden oder den Motor kurz laufen lassen, danach erneut versuchen.' },
    2: { title: 'Tank leer', help: 'Kraftstoff nachfüllen. Danach das Heizgerät erneut starten.' },
    3: { title: 'Heizgerät defekt', help: 'Heizgerät kurz stromlos machen und neu starten. Bei Wiederholung die Werkstatt aufsuchen.' },
    4: { title: 'Crash erkannt', help: 'Fahrzeug auf Schäden prüfen. Zündung aus und wieder ein, dann den Fehler am Bedienteil quittieren.' },
    5: { title: 'Sonstige Störung', help: 'Heizgerät aus- und wieder einschalten. Bleibt der Fehler, den Fehlerspeicher auslesen lassen.' },
    6: { title: 'Überhitzung', help: 'Heizgerät abkühlen lassen. Luftwege und Kühlmittelstand prüfen, bevor erneut gestartet wird.' },
    8: { title: 'Befehl nicht übernommen', help: 'Zündung und Bus prüfen und den Befehl erneut senden.' },
    9: { title: 'Nicht unterstützt', help: 'Dieser Befehl wird vom Gerät nicht unterstützt.' },
    10: { title: 'Kein Bus', help: 'Fahrzeug aufwecken oder die Verbindung zum Bus prüfen.' }
  };

  function errorInfo(code) {
    return ERR_HELP[code] || { title: code ? 'Fehler ' + code : 'Fehler', help: 'Heizgerät aus- und wieder einschalten. Bleibt der Fehler, die Werkstatt aufsuchen.' };
  }

  function agoText(ms) {
    if (ms === null || ms === undefined || !isFinite(ms)) return 'Noch keine Daten';
    var s = Math.max(0, Math.round(ms / 1000));
    if (s < 60) return 'vor ' + s + ' s';
    var m = Math.floor(s / 60);
    if (m < 60) return 'vor ' + m + ' min';
    return 'vor ' + Math.floor(m / 60) + ' h';
  }

  function isStale(ms) { return ms === null || ms === undefined || ms > STALE_MS; }

  function clock(t, hour12) {
    var d = new Date(t), h = d.getHours(), mm = ('0' + d.getMinutes()).slice(-2);
    if (!hour12) return ('0' + h).slice(-2) + ':' + mm;
    return (h % 12 || 12) + ':' + mm + (h < 12 ? ' AM' : ' PM');
  }

  function fmtNum(v, digits) {
    if (v === null || v === undefined || !isFinite(v)) return '–';
    return v.toFixed(digits === undefined ? 1 : digits).replace('.', ',');
  }

  function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

  // h:mm for the ring label.
  function hm(min) {
    min = Math.max(0, Math.round(min));
    return Math.floor(min / 60) + ':' + ('0' + (min % 60)).slice(-2);
  }

  function presetLabel(p) { return p.label + ' ' + p.targetC + ' °C / ' + Math.round(p.durationMin / 60) + ' h'; }
  function matchPreset(targetC, durationMin) {
    for (var i = 0; i < PRESETS.length; i++) if (PRESETS[i].targetC === targetC && PRESETS[i].durationMin === durationMin) return PRESETS[i].id;
    return null;
  }

  // Status summary for navigator.share. opts: {phaseText, hour12, now}
  function shareText(s, opts) {
    opts = opts || {};
    var parts = ['Standheizung: ' + ((opts.phaseText && opts.phaseText[s.phase]) || 'Unbekannt')];
    if (s.errorCode) parts.push('Fehler: ' + errorInfo(s.errorCode).title);
    if (s.interior !== null) parts.push('Innen ' + fmtNum(s.interior) + ' °C');
    if (s.outside !== null) parts.push('Außen ' + fmtNum(s.outside) + ' °C');
    if (s.coolant !== null) parts.push('Kühlmittel ' + fmtNum(s.coolant) + ' °C');
    if (s.phase === 2) parts.push('Leistung ' + fmtNum(s.powerW / 1000, 1) + ' kW');
    if (s.voltage !== null) parts.push('Spannung ' + fmtNum(s.voltage) + ' V');
    if (s.climateActive && s.remainingMin > 0) parts.push('Klima ' + s.targetC + ' °C, Rest ' + hm(s.remainingMin) + ' h');
    if (opts.now) parts.push('Stand ' + clock(opts.now, opts.hour12));
    return parts.join(' · ');
  }

  // Device selection for the reconnect without chooser. devices: BluetoothDevice-like {id, name} from getDevices().
  // saved: {id, name} of the device the user last chose in the chooser. Picks by stored id, else by the stored
  // name. Without saved data nothing is picked: the user chooses in the chooser, which shows the unique names
  // Webasto-RX-XXXX (receiver) and Webasto-TX-XXXX (sender).
  var ROLE_PREFIX = { receiver: 'Webasto-RX', sender: 'Webasto-TX' };
  function pickDevice(devices, saved) {
    if (!devices || !devices.length || !saved || (!saved.id && !saved.name)) return null;
    var i;
    if (saved.id) for (i = 0; i < devices.length; i++) if (devices[i] && devices[i].id === saved.id) return devices[i];
    if (saved.name) for (i = 0; i < devices.length; i++) if (devices[i] && devices[i].name === saved.name) return devices[i];
    return null;
  }

  return {
    ROLE_PREFIX: ROLE_PREFIX, pickDevice: pickDevice,
    VERSION: VERSION, STALE_MS: STALE_MS, PRESETS: PRESETS, DURATIONS: DURATIONS, ERR_HELP: ERR_HELP,
    errorInfo: errorInfo, agoText: agoText, isStale: isStale, clock: clock, fmtNum: fmtNum, clamp: clamp,
    hm: hm, presetLabel: presetLabel, matchPreset: matchPreset, shareText: shareText
  };
}));
