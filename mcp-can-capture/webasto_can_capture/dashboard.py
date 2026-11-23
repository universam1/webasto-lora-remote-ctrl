"""
Local web dashboard for the VW T5.2 Webasto parking heater on the Komfort-CAN.

Reuses CanSniffer from server.py (serial port, GVRET, reader/poller threads, TX handling) and
the 0x3E5 decoder from analysis.py. Serves one static page plus a small JSON API on
127.0.0.1 with the stdlib http.server.

    venv/bin/python -m webasto_can_capture.dashboard --port /dev/ttyCANSNIFF [--http 127.0.0.1:8765] [--allow-tx]
        [--allow-status-spoof]
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import logging
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable
from urllib.parse import urlsplit

import serial

from . import analysis
from .server import DEFAULT_PORT, CanSniffer, PortBusyError

logger = logging.getLogger("webasto-dashboard")

ID_STATUS = 0x3E5  # heater status, every 200 ms
ID_MODE = 0x5D3  # requested mode (roof unit), every 500 ms
ID_ACTIVE = 0x3E3  # byte 4: 01 while the heater is active

MODES = {"off": 0x00, "heat": 0x02, "vent": 0x04}
MODE_NAMES = {v: k for k, v in MODES.items()}
# 0x3E5 byte 4 (heating power raw) is linear in the fuel rate (captures: raw 90 -> 217 ml/h,
# 100 -> 243 ml/h, 241 -> 580 ml/h). Watts are an estimate: 5 kW nominal at raw 241.
FUEL_ML_H_PER_STEP = 2.41
HEAT_W_PER_STEP = 20.7

PANEL_FLAG = 0x80  # 0x5D3 byte 0 bit 7: panel-state flag, must be mirrored in commands
COMMAND_MAX_FRAMES = 10
COMMAND_INTERVAL_MS = 500
HINT_VENT_NOT_ADOPTED = "Lüften wurde vom Bedienteil nicht übernommen – am Bedienteil starten."
HINT_NOT_ADOPTED = "Befehl wurde vom Bedienteil nicht übernommen."
HINT_SPOOF_LOCKED = "Funkstart-Emulation gesperrt: Dashboard wurde ohne --allow-status-spoof gestartet."
HINT_IMPULSE_IGNORED = "Bedienteil hat auf den Funk-Impuls nicht wie erwartet reagiert."
IMPULSE_FRAMES = 5

ERROR_TEXT_DE = {
    0: "kein Fehler",
    1: "Batteriespannung zu niedrig",
    2: "Kraftstoff zu wenig",
    3: "Heizgerät defekt",
    4: "Crash-Abschaltung",
    5: "sonstiger Fehler",
    6: "Überhitzung",
    7: "unbekannter Fehler",
}

AWAKE_TIMEOUT_S = 2.0  # no frame for this long: bus asleep (it sleeps ~20 s after activity)
STALE_S = 2.0  # a per-ID value older than this is shown as "last known"
RATE_WINDOW_S = 5.0
HISTORY_S = 30 * 60
POLL_S = 0.25
LOG_SIZE = 20
MAX_BODY = 1024

STATIC_DIR = Path(__file__).resolve().parent / "static"


# ---------------------------------------------------------------------- decoding


def heater_state(dec: dict[str, Any]) -> str:
    """Derived heater state from a decoded 0x3E5 payload: off, heating, ventilating, run_on.

    In the car the combustion bit (byte 1 b2) stays 0 while heating; the control LED
    (byte 1 b1) and the heating power are the reliable signs. Run-on is the water pump
    running after the heater has stopped.
    """
    if dec["ventilation_active"]:
        return "ventilating"
    if dec["control_led"] or dec["combustion_on"] or dec["heating_power_raw"]:
        return "heating"
    if dec["water_pump_request"]:
        return "run_on"
    return "off"


def power_w_est(raw: int) -> int:
    """Estimated heat output in W from 0x3E5 byte 4."""
    return round(raw * HEAT_W_PER_STEP)


def fuel_rate_l_h_est(raw: int) -> float:
    """Estimated fuel rate in l/h from 0x3E5 byte 4."""
    return round(raw * FUEL_ML_H_PER_STEP / 1000, 2)


def decode_status(payload: bytes) -> dict[str, Any]:
    """0x3E5 payload -> decoded fields plus derived state and German error text."""
    dec = analysis.decode_3e5_payload(payload)
    code = analysis.error_code_3e5(payload)
    coolant = dec["coolant_c"]
    return {
        "state": heater_state(dec),
        "coolant_c": None if coolant == "error" else coolant,
        "coolant_error": coolant == "error",
        "power_raw": dec["heating_power_raw"],
        "power_w_est": power_w_est(dec["heating_power_raw"]),
        "fuel_rate_l_h_est": fuel_rate_l_h_est(dec["heating_power_raw"]),
        "fuel_used_ml": dec["fuel_used_ml"],
        "fuel_pump": bool(dec["fuel_pump_on"]),
        "water_pump": bool(dec["water_pump_request"]),
        "error": {
            "code": code,
            "name": analysis.ERROR_CODES.get(code, "?"),
            "text": ERROR_TEXT_DE.get(code, "unbekannter Fehler"),
            "stored": bool(dec["error_stored"]),
        },
        "fields": dec,
    }


def decode_mode(payload: bytes) -> str:
    """0x5D3 byte 0 (bit 7 ignored) -> off / heat / vent, or 'unknown(0xNN)'."""
    if not payload:
        return "unknown"
    return MODE_NAMES.get(payload[0] & ~PANEL_FLAG, f"unknown(0x{payload[0]:02X})")


def decode_active(payload: bytes) -> bool | None:
    """0x3E3 byte 4: heater active echo. None if the frame is too short."""
    return bool(payload[4]) if len(payload) > 4 else None


# ---------------------------------------------------------------------- monitor


@dataclass
class _Last:
    payload: bytes
    host_time: float


class HeaterMonitor:
    """Follows the sniffer's ring buffer, keeps the latest heater frames, a 1 Hz history and
    the command log, and runs heater commands in a worker thread."""

    def __init__(
        self,
        sniffer: CanSniffer,
        allow_tx: bool,
        clock: Callable[[], float] = time.time,
        allow_status_spoof: bool = False,
    ):
        self.sniffer = sniffer
        self.allow_tx = allow_tx
        # radio-start emulation spoofs the heater's own 0x3E5; only with --allow-status-spoof
        self.allow_status_spoof = allow_tx and allow_status_spoof
        self.clock = clock
        self._lock = threading.Lock()
        self._seen = 0
        self._last: dict[int, _Last] = {}
        self._last_frame_time: float | None = None
        self._recent: deque[float] = deque()
        self._history: deque[tuple[int, float | None, int, int]] = deque(maxlen=HISTORY_S)
        self._commands: deque[dict[str, Any]] = deque(maxlen=LOG_SIZE)
        self._cmd_seq = 0
        self._cmd_busy = threading.Lock()
        self._running = False
        self._thread: threading.Thread | None = None

    # -------------------------------------------------------------- frame intake

    def start(self) -> None:
        self._running = True
        self._thread = threading.Thread(target=self._loop, name="dashboard-monitor", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)

    def _loop(self) -> None:
        while self._running:
            try:
                self.poll()
            except Exception:  # keep following the bus even if one batch fails
                logger.exception("monitor poll failed")
            time.sleep(POLL_S)

    def poll(self) -> None:
        """Process frames that arrived since the last call."""
        total, new = self.sniffer.rx_since(self._seen)
        self._seen = total
        with self._lock:
            for e in new:
                f = e.frame
                self._last_frame_time = e.host_time
                self._recent.append(e.host_time)
                if f.extended or f.can_id not in (ID_STATUS, ID_MODE, ID_ACTIVE):
                    continue
                self._last[f.can_id] = _Last(bytes(f.data), e.host_time)
                if f.can_id == ID_STATUS:
                    self._add_history(e.host_time, bytes(f.data))
            cutoff = self.clock() - RATE_WINDOW_S
            while self._recent and self._recent[0] < cutoff:
                self._recent.popleft()

    def _add_history(self, t: float, payload: bytes) -> None:
        sec = int(t)
        if self._history and self._history[-1][0] == sec:
            return  # 1 sample per second: keep the first 0x3E5 of each second
        s = decode_status(payload)
        self._history.append((sec, s["coolant_c"], s["power_raw"], s["fuel_used_ml"]))

    # -------------------------------------------------------------- state

    def tx_availability(self) -> tuple[bool, str | None]:
        """(commands possible, German reason if not)."""
        if not self.allow_tx:
            return False, "Senden gesperrt: Dashboard wurde ohne --allow-tx gestartet."
        if not self.sniffer.connected:
            return False, "Keine Verbindung zum CAN-Sniffer."
        st = self.sniffer.state.status
        if st is None:
            return False, "Noch kein Gerätestatus empfangen."
        if not st.tx_allowed_by_firmware:
            return False, "Firmware erlaubt kein Senden (Build ohne cansniffer_tx)."
        if self._cmd_busy.locked():
            return False, "Ein Befehl wird gerade gesendet."
        return True, None

    def snapshot(self) -> dict[str, Any]:
        now = self.clock()
        sn = self.sniffer
        with self._lock:
            last = dict(self._last)
            last_frame = self._last_frame_time
            rate = sum(1 for t in self._recent if t >= now - RATE_WINDOW_S) / RATE_WINDOW_S
            hist = list(self._history)
            commands = [dict(c) for c in reversed(self._commands)]

        def age(t: float | None) -> float | None:
            return None if t is None else round(max(0.0, now - t), 2)

        heater = None
        if ID_STATUS in last:
            e = last[ID_STATUS]
            heater = decode_status(e.payload)
            heater.update(raw=e.payload.hex(" ").upper(), age_s=age(e.host_time), stale=now - e.host_time > STALE_S)
        mode = None
        if ID_MODE in last:
            e = last[ID_MODE]
            b0 = e.payload[0] if e.payload else None
            mode = {
                "mode": decode_mode(e.payload),
                "byte0": None if b0 is None else f"0x{b0:02X}",
                "panel_flag": None if b0 is None else bool(b0 & PANEL_FLAG),
                "raw": e.payload.hex(" ").upper(),
                "age_s": age(e.host_time),
                "stale": now - e.host_time > STALE_S,
            }
        active = None
        if ID_ACTIVE in last:
            e = last[ID_ACTIVE]
            active = {
                "active": decode_active(e.payload),
                "raw": e.payload.hex(" ").upper(),
                "age_s": age(e.host_time),
                "stale": now - e.host_time > STALE_S,
            }

        since = age(last_frame)
        with sn._lock:
            distinct = len(sn.ids)
            total = sn.total_rx
        bus = {
            "frames_per_s": round(rate, 1),
            "awake": since is not None and since <= AWAKE_TIMEOUT_S,
            "since_last_frame_s": since,
            "distinct_ids": distinct,
            "total_frames": total,
        }
        st = sn.state.status
        device = None
        if st is not None:
            device = {
                "state": st.state_name,
                "rx_frames": st.rx_frames,
                "rx_missed": st.rx_missed,
                "bus_errors": st.bus_errors,
                "tx_allowed_by_firmware": st.tx_allowed_by_firmware,
                "tx_enabled_now": st.tx_enabled_now,
                "age_s": age(sn.state.status_host_time),
            }
        available, reason = self.tx_availability()
        return {
            "time": now,
            "connection": {"port": sn.port, "connected": sn.connected, "reader_error": sn.reader_error},
            "heater": heater,
            "mode_5d3": mode,
            "active_3e3": active,
            "bus": bus,
            "device": device,
            "tx": {
                "allow_tx_flag": self.allow_tx,
                "allow_status_spoof": self.allow_status_spoof,
                "allowed_by_firmware": bool(st and st.tx_allowed_by_firmware),
                "available": available,
                "reason": reason,
                "busy": self._cmd_busy.locked(),
                "session_armed": sn.tx_session_enabled,
            },
            "history": {
                "t": [h[0] for h in hist],
                "coolant_c": [h[1] for h in hist],
                "power_raw": [h[2] for h in hist],
                "power_kw_est": [round(power_w_est(h[2]) / 1000, 2) for h in hist],
                "fuel_used_ml": [h[3] for h in hist],
            },
            "commands": commands,
        }

    # -------------------------------------------------------------- commands

    def command(self, mode: str, radio_emulation: bool = False) -> tuple[int, dict[str, Any]]:
        """Start a heater command. Returns (HTTP status, log entry or error).

        `radio_emulation` additionally spoofs 0x3E5 (radio-start emulation); refused unless the
        dashboard was started with --allow-status-spoof."""
        if mode not in MODES:
            return HTTPStatus.BAD_REQUEST, {"error": f"unbekannter Modus {mode!r}; erlaubt: heat, vent, off"}
        if radio_emulation and not self.allow_status_spoof:
            logger.warning("command %s with radio emulation refused: no --allow-status-spoof", mode)
            return HTTPStatus.FORBIDDEN, {"error": HINT_SPOOF_LOCKED}
        fields = {
            "frame": f"5D3#{self._expected_byte(mode):02X}00",
            "frames_sent": None,
            "adopted_after_ms": None,
            "target_byte": None,
            "heater_confirmed": None,
            "radio_emulation": bool(radio_emulation),
            "heater_confirmed_before_spoof": None,
        }
        return self._launch(mode, fields, self._run_command)

    def impulse(self, kind: str) -> tuple[int, dict[str, Any]]:
        """Start a radio-remote ON/OFF impulse (spoofed 0x3E5 only, no 0x5D3). Needs --allow-status-spoof."""
        if kind not in ("on", "off"):
            return HTTPStatus.BAD_REQUEST, {"error": f"unbekannter Impuls {kind!r}; erlaubt: on, off"}
        if not self.allow_status_spoof:
            logger.warning("impulse %s refused: no --allow-status-spoof", kind)
            return HTTPStatus.FORBIDDEN, {"error": HINT_SPOOF_LOCKED}
        b0 = (0x01 if kind == "on" else 0x02) | 0x80
        fields = {"frame": f"3E5#{b0:02X}… ×{IMPULSE_FRAMES}", "radio_emulation": True, "heater_before": None, "heater_after": None}
        return self._launch(f"impulse_{kind}", fields, self._run_impulse)

    def _launch(self, mode: str, fields: dict[str, Any], runner: Callable[[dict[str, Any]], None]) -> tuple[int, dict[str, Any]]:
        available, reason = self.tx_availability()
        if not available:
            status = HTTPStatus.CONFLICT if self._cmd_busy.locked() else HTTPStatus.FORBIDDEN
            logger.warning("command %s refused: %s", mode, reason)
            return status, {"error": reason}
        if not self._cmd_busy.acquire(blocking=False):
            return HTTPStatus.CONFLICT, {"error": "Ein Befehl wird gerade gesendet."}
        try:
            now = self.clock()
            with self._lock:
                self._cmd_seq += 1
                entry = {
                    "id": self._cmd_seq,
                    "time": now,
                    "iso": datetime.fromtimestamp(now).isoformat(timespec="seconds"),
                    "mode": mode,
                    "frame": None,
                    "status": "pending",
                    "message": "",
                    "panel_before": None,
                    "panel_after": None,
                    "hint": None,
                    "radio_emulation": False,
                    "spoof_frames_sent": None,
                    "spoof_payload": None,
                    **fields,
                }
                self._commands.append(entry)
            logger.info(
                "command %d: %s (%s)%s", entry["id"], mode, entry["frame"],
                " + 0x3E5 status spoof" if entry["radio_emulation"] else "",
            )
            threading.Thread(target=runner, args=(entry,), name="dashboard-cmd", daemon=True).start()
        except Exception:
            self._cmd_busy.release()
            raise
        return HTTPStatus.ACCEPTED, dict(entry)

    def _expected_byte(self, mode: str) -> int:
        """Byte 0 we expect to send: mode bits plus bit 7 of the last panel 0x5D3 (default 0x80)."""
        with self._lock:
            e = self._last.get(ID_MODE)
        flag = (e.payload[0] & PANEL_FLAG) if e and e.payload else PANEL_FLAG
        return MODES[mode] | flag

    def _arm(self, spoof: bool) -> None:
        sn = self.sniffer
        if not sn.tx_session_enabled:
            msg = sn.enable_tx(True)
            if not sn.tx_session_enabled:
                raise RuntimeError(msg)
        if spoof and not sn.status_spoof_enabled:
            sn.enable_status_spoof(True)  # approved by starting with --allow-status-spoof

    def _finish(self, entry: dict[str, Any], update: dict[str, Any], status: str, message: str) -> None:
        with self._lock:
            entry.update(update, status=status, message=message, done=self.clock())
        (logger.info if status == "ok" else logger.error)("command %d %s: %s", entry["id"], status, message)
        self._cmd_busy.release()

    def _run_command(self, entry: dict[str, Any]) -> None:
        update: dict[str, Any] = {}
        try:
            sn = self.sniffer
            self._arm(entry["radio_emulation"])
            kw: dict[str, Any] = {"radio_emulation": True} if entry["radio_emulation"] else {}
            res = sn.set_heater_mode(entry["mode"], COMMAND_MAX_FRAMES, COMMAND_INTERVAL_MS, **kw)
            status = "ok" if res["ok"] else "failed"
            keys = ["frames_sent", "adopted_after_ms", "target_byte", "panel_before", "panel_after", "heater_confirmed"]
            if entry["radio_emulation"]:
                keys += ["spoof_frames_sent", "spoof_payload", "heater_confirmed_before_spoof"]
            update = {k: res.get(k) for k in keys}
            if res.get("target_byte"):
                update["frame"] = f"5D3#{int(res['target_byte'], 16):02X}00"
            if not res["ok"]:
                update["hint"] = HINT_VENT_NOT_ADOPTED if entry["mode"] == "vent" else HINT_NOT_ADOPTED
            message = res["message"]
        except Exception as e:
            status, message = "error", str(e)
        self._finish(entry, update, status, message)

    def _run_impulse(self, entry: dict[str, Any]) -> None:
        update: dict[str, Any] = {}
        try:
            self._arm(True)
            res = self.sniffer.radio_impulse(entry["mode"].removeprefix("impulse_"), True, IMPULSE_FRAMES)
            status = "ok" if res["ok"] else "failed"
            update = {k: res.get(k) for k in (
                "panel_before", "panel_after", "heater_before", "heater_after", "spoof_frames_sent", "spoof_payload",
            )}
            if res.get("spoof_payload"):
                update["frame"] = f"3E5#{res['spoof_payload'].replace(' ', '')} ×{res['spoof_frames_sent']}"
            if not res["ok"]:
                update["hint"] = HINT_IMPULSE_IGNORED
            message = res["message"]
        except Exception as e:
            status, message = "error", str(e)
        self._finish(entry, update, status, message)

    def wait_idle(self, timeout: float) -> bool:
        """Block until no command is running (for tests and shutdown)."""
        if self._cmd_busy.acquire(timeout=timeout):
            self._cmd_busy.release()
            return True
        return False


# ---------------------------------------------------------------------- HTTP


def _is_loopback(host: str) -> bool:
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


class DashboardServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, addr: tuple[str, int], monitor: HeaterMonitor):
        self.monitor = monitor
        super().__init__(addr, DashboardHandler)
        self.loopback = _is_loopback(addr[0])


class DashboardHandler(BaseHTTPRequestHandler):
    server: DashboardServer
    server_version = "WebastoDashboard/1"

    def log_message(self, fmt: str, *args: Any) -> None:
        logger.debug("%s - " + fmt, self.address_string(), *args)

    # -------------------------------------------------------------- guards

    def _host_ok(self) -> bool:
        """Against DNS rebinding: on a loopback bind, only accept loopback Host headers."""
        if not self.server.loopback:
            return True
        host = urlsplit("//" + (self.headers.get("Host") or "")).hostname or ""
        return _is_loopback(host.strip("[]"))

    def _origin_ok(self) -> bool:
        """Against cross-site POSTs from other pages in the browser: Origin must match Host."""
        origin = self.headers.get("Origin")
        if origin is None:
            return True  # curl, scripts; browsers always send Origin on cross-origin POST
        return urlsplit(origin).netloc == (self.headers.get("Host") or "")

    def _send(self, status: int, body: bytes, ctype: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, status: int, obj: Any) -> None:
        self._send(status, json.dumps(obj).encode(), "application/json; charset=utf-8")

    # -------------------------------------------------------------- routes

    def do_GET(self) -> None:
        if not self._host_ok():
            return self._json(HTTPStatus.FORBIDDEN, {"error": "invalid Host header"})
        path = urlsplit(self.path).path
        if path in ("/", "/index.html"):
            body = (STATIC_DIR / "dashboard.html").read_bytes()
            return self._send(HTTPStatus.OK, body, "text/html; charset=utf-8")
        if path == "/api/state":
            return self._json(HTTPStatus.OK, self.server.monitor.snapshot())
        self._json(HTTPStatus.NOT_FOUND, {"error": "not found"})

    def do_POST(self) -> None:
        if not self._host_ok() or not self._origin_ok():
            return self._json(HTTPStatus.FORBIDDEN, {"error": "cross-origin request refused"})
        if urlsplit(self.path).path != "/api/command":
            return self._json(HTTPStatus.NOT_FOUND, {"error": "not found"})
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        if ctype != "application/json":
            return self._json(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, {"error": "Content-Type must be application/json"})
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = -1
        if not 0 < length <= MAX_BODY:
            return self._json(HTTPStatus.BAD_REQUEST, {"error": "invalid body length"})
        try:
            req = json.loads(self.rfile.read(length))
            if "impulse" in req:
                impulse = req["impulse"]
                if not isinstance(impulse, str):
                    raise TypeError
            else:
                impulse = None
                mode = req["mode"]
                radio = req.get("radio_emulation", False)
                if not isinstance(mode, str) or not isinstance(radio, bool):
                    raise TypeError
        except (ValueError, KeyError, TypeError, AttributeError):
            return self._json(
                HTTPStatus.BAD_REQUEST,
                {"error": 'body must be {"mode": "heat"|"vent"|"off", "radio_emulation": false|true} or {"impulse": "on"|"off"}'},
            )
        mon = self.server.monitor
        status, body = mon.impulse(impulse) if impulse is not None else mon.command(mode, radio)
        self._json(status, body)


# ---------------------------------------------------------------------- main


def _parse_http(spec: str) -> tuple[str, int]:
    host, sep, port = spec.rpartition(":")
    if not sep or not port.isdigit():
        raise argparse.ArgumentTypeError("expected HOST:PORT, e.g. 127.0.0.1:8765")
    return host.strip("[]") or "127.0.0.1", int(port)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Webasto CAN dashboard (local web UI)")
    ap.add_argument("--port", default=DEFAULT_PORT, help=f"serial port of the CAN sniffer (default {DEFAULT_PORT})")
    ap.add_argument("--bitrate", type=int, default=100_000, help="CAN bitrate (default 100000, VW Komfort-CAN)")
    ap.add_argument("--http", type=_parse_http, default=("127.0.0.1", 8765), help="HTTP bind address (default 127.0.0.1:8765)")
    ap.add_argument("--allow-tx", action="store_true", help="allow heater commands (0x5D3) from the web UI")
    ap.add_argument(
        "--allow-status-spoof",
        action="store_true",
        help="with --allow-tx: offer radio-start emulation, which spoofs the heater's own status frame 0x3E5",
    )
    args = ap.parse_args(argv)
    logging.getLogger().setLevel(logging.INFO)

    sniffer = CanSniffer()
    try:
        logger.info(sniffer.connect(args.port, args.bitrate).replace("\n", "; "))
    except PortBusyError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except (serial.SerialException, OSError) as e:
        print(f"error: cannot open {args.port}: {e}", file=sys.stderr)
        return 1

    if args.allow_status_spoof and not args.allow_tx:
        logger.warning("--allow-status-spoof has no effect without --allow-tx")
    monitor = HeaterMonitor(sniffer, args.allow_tx, allow_status_spoof=args.allow_status_spoof)
    monitor.start()
    try:
        httpd = DashboardServer(args.http, monitor)
    except OSError as e:
        print(f"error: cannot listen on {args.http[0]}:{args.http[1]}: {e}", file=sys.stderr)
        monitor.stop()
        sniffer.disconnect()
        return 1
    if not httpd.loopback:
        logger.warning("listening on %s: anyone on that network can open the dashboard", args.http[0])
    host = f"[{args.http[0]}]" if ":" in args.http[0] else args.http[0]
    logger.info(
        "dashboard on http://%s:%d/ (TX %s, 0x3E5 status spoof %s)",
        host,
        httpd.server_port,
        "allowed" if args.allow_tx else "disabled",
        "allowed" if monitor.allow_status_spoof else "disabled",
    )
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
        monitor.wait_idle(10.0)  # never cut a sequence; set_heater_mode restores listen-only itself
        monitor.stop()
        logger.info(sniffer.disconnect())
    return 0


if __name__ == "__main__":
    sys.exit(main())
