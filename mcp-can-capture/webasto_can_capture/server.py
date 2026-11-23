"""
MCP server: record a car's CAN bus through an ESP32 GVRET sniffer.

Runs over stdio. Exposes connection, live view, capture and analysis tools.
"""

from __future__ import annotations

import asyncio
import errno
import fcntl
import itertools
import json
import logging
import os
import re
import struct
import termios
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, TextIO

import serial
from mcp.server.fastmcp import FastMCP

from . import analysis, gvret
from .analysis import Frame

logging.basicConfig(level=logging.INFO)  # stderr; stdout belongs to the MCP transport
logger = logging.getLogger("webasto-can-capture")

DEFAULT_PORT = "/dev/ttyCANSNIFF"
SERIAL_BAUD = 921_600  # CP2102 cannot do 1 Mbaud; firmware uses 921600
RING_SIZE = 200_000
STATUS_POLL_S = 1.0
REPLY_TIMEOUT_S = 1.0
CAPTURES_DIR = Path(
    os.environ.get("WEBASTO_CAN_CAPTURE_DIR", Path(__file__).resolve().parent.parent / "captures")
)
_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,100}$")

ESPRESSIF_USB_VID = 0x303A

# VW T5.2 heater (Komfort-CAN)
ID_MODE = 0x5D3  # requested mode, sent by the roof control unit every 500 ms
ID_STATUS = 0x3E5  # heater status, every 200 ms
HEATER_MODES = {"off": 0x00, "heat": 0x02, "vent": 0x04}  # 0x5D3 byte 0, bit 7 mirrors the panel
PANEL_FRESH_S = 2.0  # a panel 0x5D3 younger than this gives bit 7 without a wake frame
WAKE_WAIT_S = 0.6  # after the wake frame, wait this long for the panel's first 0x5D3
HEATER_CONFIRM_S = 1.5  # after adoption, watch 0x3E5 this long for confirmation
# radio-start emulation: spoofed 0x3E5 with the Telestart "radio" bits in byte 0
RADIO_BITS = {"off": 0x02, "heat": 0x05, "vent": 0x09}  # b1 off / b0+b2 heat / b0+b3 vent
RADIO_WAKE_BIT = 0x80  # byte 0 b7: wake Komfort-CAN
SPOOF_CONFIRM_S = 1.5  # after our first 0x5D3, wait this long for a real 0x3E5 confirming the target
SPOOF_DELAY_S = 0.1  # a spoof goes this long after a real 0x3E5 (heater period 200 ms)
SPOOF_LATE_S = 0.16  # later than this after the real frame: skip, the next real one is close
TIMELINE_MAX = 60
IMPULSE_WAIT_3E5_S = 1.0  # radio_impulse: wait this long for a real 0x3E5 to copy (after waking)
IMPULSE_OBSERVE_S = 3.0  # radio_impulse: watch the panel and heater this long after the first spoof
BUS_AWAKE_S = 2.0  # no Rx frame for this long: bus asleep
HEATER_MODE_NAMES = {0x00: "off", 0x02: "heat", 0x04: "vent"}


def _is_native_usb(port: str) -> bool:
    """True if `port` is an ESP32-S2/S3 native USB CDC port (Espressif VID)."""
    try:
        from serial.tools import list_ports

        real = os.path.realpath(port)
        for p in list_ports.comports():
            if os.path.realpath(p.device) == real:
                return p.vid == ESPRESSIF_USB_VID
    except Exception:  # pragma: no cover - best effort detection
        pass
    return False


class PortBusyError(RuntimeError):
    """The serial port is already held by another program (MCP server, dashboard, SavvyCAN)."""


def _open_exclusive(ser: serial.Serial, port: str) -> None:
    """Open `ser` so that a second process fails cleanly instead of sharing the GVRET stream.

    pyserial's `exclusive=True` takes an advisory flock(); TIOCEXCL additionally makes the
    kernel refuse further open() calls on the tty with EBUSY (for programs that do not use
    flock, e.g. SavvyCAN). Some ttys (certain pseudo-terminals) reject TIOCEXCL; then only
    the flock protects the port.
    """
    ser.exclusive = True
    try:
        ser.open()
    except serial.SerialException as e:
        busy = e.errno in (errno.EBUSY, errno.EAGAIN, errno.EWOULDBLOCK) or "exclusively lock" in str(e)
        if busy:
            raise PortBusyError(
                f"port {port} is busy (used by another program); disconnect the MCP server, "
                "dashboard or SavvyCAN first"
            ) from e
        raise
    try:
        fcntl.ioctl(ser.fileno(), termios.TIOCEXCL)
    except OSError as e:
        logger.info("TIOCEXCL not supported on %s (%s); relying on flock only", port, e)


def _release_exclusive(ser: serial.Serial) -> None:
    try:
        fcntl.ioctl(ser.fileno(), termios.TIOCNXCL)
    except Exception:  # not open, or TIOCEXCL was never set
        pass


@dataclass
class ActiveCapture:
    name: str
    csv: TextIO
    log: TextIO
    markers: TextIO
    started_host: float
    rx: int = 0
    tx: int = 0
    marks: int = 0

    @property
    def paths(self) -> dict[str, str]:
        return {k: getattr(self, k).name for k in ("csv", "log", "markers")}


@dataclass
class _RxEntry:
    host_time: float
    frame: Frame


@dataclass
class SnifferState:
    device_info: gvret.DeviceInfo | None = None
    num_buses: int | None = None
    bus_params: gvret.BusParams | None = None
    status: gvret.DeviceStatus | None = None
    status_host_time: float | None = None
    last: dict[type, gvret.Message] = field(default_factory=dict)
    replies: dict[type, threading.Event] = field(default_factory=dict)


class CanSniffer:
    """Owns the serial port, reader/poller threads, ring buffer and capture files."""

    def __init__(self) -> None:
        self.ser: serial.Serial | None = None
        self.port: str | None = None
        self._write_lock = threading.Lock()
        self._lock = threading.Lock()  # ring buffer, state, capture
        self._running = False
        self._threads: list[threading.Thread] = []
        self._parser = gvret.GvretParser()
        self._unwrap = gvret.TimestampUnwrapper()
        self.ring: deque[_RxEntry] = deque(maxlen=RING_SIZE)
        self.total_rx = 0
        self.ids: set[tuple[int, bool]] = set()
        self.state = SnifferState()
        self.capture: ActiveCapture | None = None
        self.tx_session_enabled = False
        self.status_spoof_enabled = False  # separate arm for spoofing the heater's 0x3E5
        self.tx_sent = 0
        # device-us -> host-epoch mapping, fixed at first frame after connect
        self._epoch_offset: float | None = None
        self._last_dev_ts: int | None = None
        self._last_dev_host: float | None = None
        self.reader_error: str | None = None

    # ------------------------------------------------------------------ connection

    @property
    def connected(self) -> bool:
        return self.ser is not None and self.ser.is_open and self._running

    def connect(self, port: str, bitrate: int) -> str:
        if self.connected:
            raise RuntimeError(f"already connected to {self.port}; call disconnect first")
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = SERIAL_BAUD
        ser.timeout = 0.05
        ser.write_timeout = 1.0
        # Native USB CDC (ESP32-S2/S3, Espressif VID 0x303A) has no auto-reset
        # circuit and the Arduino core only transmits while DTR is asserted.
        # USB-UART bridges (CP2102 etc.) need DTR/RTS released so EN/IO0 stay high.
        native = _is_native_usb(port)
        ser.dtr = native
        ser.rts = False
        _open_exclusive(ser, port)
        try:
            ser.dtr = native
            ser.rts = False
        except OSError as e:  # e.g. pseudo-terminals without modem lines
            logger.info("could not set DTR/RTS on %s: %s", port, e)
        time.sleep(0.3 if native else 1.5)
        ser.reset_input_buffer()

        self.ser, self.port = ser, port
        self._parser = gvret.GvretParser()
        self._unwrap = gvret.TimestampUnwrapper()
        self.ring.clear()
        self.total_rx = 0
        self.ids.clear()
        self.state = SnifferState()
        self.tx_session_enabled = False
        self.status_spoof_enabled = False
        self.tx_sent = 0
        self._epoch_offset = None
        self._last_dev_ts = self._last_dev_host = None
        self.reader_error = None
        self._running = True
        self._threads = [
            threading.Thread(target=self._reader, name="gvret-reader", daemon=True),
            threading.Thread(target=self._poller, name="gvret-poller", daemon=True),
        ]
        self._threads[0].start()
        try:
            self._write(gvret.ENTER_BINARY)
            time.sleep(0.05)
            info = self._request(gvret.cmd(gvret.CMD_DEVICE_INFO), gvret.DeviceInfo)
            nb = self._request(gvret.cmd(gvret.CMD_NUM_BUSES), gvret.NumBuses)
            self._write(gvret.encode_set_bus_config(bitrate, True, True))
            time.sleep(0.1)
            bp = self._request(gvret.cmd(gvret.CMD_GET_BUS_PARAMS), gvret.BusParams)
            st = self._request(gvret.cmd(gvret.CMD_STATUS), gvret.DeviceStatus)
        except Exception:
            self.disconnect()
            raise
        self._threads[1].start()
        lines = [f"connected to {port} @ {SERIAL_BAUD} baud, GVRET binary mode"]
        lines.append(f"device info: {info}" if info else "device info: no reply to F1 07")
        lines.append(f"buses: {nb.count}" if nb else "buses: no reply to F1 0C")
        lines.append(_fmt_bus(bp) if bp else "bus params: no reply to F1 06")
        lines.append(_fmt_status(st) if st else "status: no reply to F1 40 (vendor extension missing?)")
        return "\n".join(lines)

    def disconnect(self) -> str:
        msg = []
        if self.capture:
            msg.append(self.stop_capture())
        self._running = False
        for t in self._threads:
            if t.is_alive() and t is not threading.current_thread():
                t.join(timeout=2.0)
        self._threads = []
        if self.ser is not None:
            try:
                # TIOCEXCL sticks to the tty while any fd is open; clear it explicitly
                _release_exclusive(self.ser)
                self.ser.close()
            except Exception as e:  # pragma: no cover
                logger.warning("close failed: %s", e)
        port, self.ser, self.port = self.port, None, None
        self.tx_session_enabled = False
        self.status_spoof_enabled = False
        msg.append(f"disconnected from {port}" if port else "not connected")
        return "\n".join(msg)

    def _require(self) -> None:
        if not self.connected:
            extra = f" (reader error: {self.reader_error})" if self.reader_error else ""
            raise RuntimeError("not connected; call connect first" + extra)

    def _write(self, data: bytes) -> None:
        assert self.ser is not None
        with self._write_lock:
            self.ser.write(data)
            self.ser.flush()

    def _request(self, data: bytes, reply: type, timeout: float = REPLY_TIMEOUT_S):
        ev = self.state.replies.setdefault(reply, threading.Event())
        ev.clear()
        self._write(data)
        if not ev.wait(timeout):
            return None
        return self.state.last.get(reply)

    # ------------------------------------------------------------------ threads

    def _reader(self) -> None:
        ser = self.ser
        assert ser is not None
        while self._running:
            try:
                chunk = ser.read(ser.in_waiting or 1)
            except Exception as e:
                if self._running:
                    self.reader_error = str(e)
                    logger.error("serial read failed: %s", e)
                    self._running = False
                break
            if not chunk:
                continue
            now = time.time()
            for msg in self._parser.feed(chunk):
                try:
                    self._dispatch(msg, now)
                except Exception:  # never kill the reader on one bad message
                    logger.exception("dispatch failed for %r", msg)

    def _poller(self) -> None:
        while self._running:
            time.sleep(STATUS_POLL_S)
            if not self._running:
                break
            try:
                self._write(gvret.cmd(gvret.CMD_STATUS))
                with self._lock:
                    if self.capture:
                        for fh in (self.capture.csv, self.capture.log):
                            fh.flush()
            except Exception as e:
                logger.warning("status poll failed: %s", e)

    def _dispatch(self, msg: gvret.Message, now: float) -> None:
        s = self.state
        if isinstance(msg, gvret.CanFrame):
            ts = self._unwrap(msg.timestamp_us)
            f = Frame(ts, msg.can_id, msg.extended, "Rx", msg.bus, msg.data)
            with self._lock:
                if self._epoch_offset is None:
                    self._epoch_offset = now - ts / 1e6
                self._last_dev_ts, self._last_dev_host = ts, now
                self.ring.append(_RxEntry(now, f))
                self.total_rx += 1
                self.ids.add((f.can_id, f.extended))
                if self.capture:
                    self._write_capture_frame(f)
                    self.capture.rx += 1
            return
        if isinstance(msg, gvret.DeviceInfo):
            s.device_info = msg
        elif isinstance(msg, gvret.NumBuses):
            s.num_buses = msg.count
        elif isinstance(msg, gvret.BusParams):
            s.bus_params = msg
        elif isinstance(msg, gvret.DeviceStatus):
            s.status, s.status_host_time = msg, now
        s.last[type(msg)] = msg
        ev = s.replies.get(type(msg))
        if ev:
            ev.set()

    # ------------------------------------------------------------------ time

    def device_now_us(self) -> tuple[int, str]:
        """Best estimate of the device clock (unwrapped us) and how it was obtained."""
        if self.connected:
            r = self._request(gvret.cmd(gvret.CMD_TIME_SYNC), gvret.TimeSync, timeout=0.5)
            if r:
                with self._lock:
                    ts = self._unwrap(r.timestamp_us)
                return ts, "time_sync"
        with self._lock:
            if self._last_dev_ts is not None and self._last_dev_host is not None:
                return self._last_dev_ts + int((time.time() - self._last_dev_host) * 1e6), "estimated"
        return int(time.time() * 1e6), "host_clock"

    def _epoch_of(self, ts_us: int) -> float:
        if self._epoch_offset is None:
            self._epoch_offset = time.time() - ts_us / 1e6
        return self._epoch_offset + ts_us / 1e6

    # ------------------------------------------------------------------ capture

    def _write_capture_frame(self, f: Frame) -> None:
        cap = self.capture
        assert cap is not None
        cap.csv.write(analysis.format_csv_row(f) + "\n")
        cap.log.write(analysis.format_candump(f, self._epoch_of(f.ts_us)) + "\n")

    def start_capture(self, name: str) -> str:
        if not _NAME_RE.match(name):
            raise ValueError("capture name may contain letters, digits, '.', '_' and '-' only")
        if self.capture:
            raise RuntimeError(f"capture '{self.capture.name}' already active; stop it first")
        CAPTURES_DIR.mkdir(parents=True, exist_ok=True)
        base = CAPTURES_DIR / name
        paths = [base.with_name(name + ext) for ext in (".csv", ".log", ".markers.jsonl")]
        existing = [p.name for p in paths if p.exists()]
        if existing:
            raise FileExistsError(f"capture files already exist: {existing}; choose another name")
        csv_fh = paths[0].open("w", newline="")
        csv_fh.write(analysis.CSV_HEADER + "\n")
        with self._lock:
            self.capture = ActiveCapture(
                name, csv_fh, paths[1].open("w"), paths[2].open("w"), time.time()
            )
        note = "" if self.connected else " (warning: not connected, no frames will arrive yet)"
        return f"capture '{name}' started{note}\n" + "\n".join(f"  {p}" for p in paths)

    def stop_capture(self) -> str:
        with self._lock:
            cap, self.capture = self.capture, None
        if cap is None:
            raise RuntimeError("no active capture")
        for fh in (cap.csv, cap.log, cap.markers):
            fh.close()
        dur = time.time() - cap.started_host
        return (
            f"capture '{cap.name}' stopped after {dur:.1f} s: {cap.rx} Rx frames, {cap.tx} Tx frames, "
            f"{cap.marks} markers\n" + "\n".join(f"  {p}" for p in cap.paths.values())
        )

    def mark_event(self, label: str) -> str:
        if not self.capture:
            raise RuntimeError("no active capture; call start_capture first")
        ts, source = self.device_now_us()
        host = time.time()
        with self._lock:
            cap = self.capture
            if cap is None:
                raise RuntimeError("capture was stopped")
            rec = {
                "label": label,
                "device_ts_us": ts,
                "device_ts_source": source,
                "host_time": round(host, 6),
                "host_iso": datetime.fromtimestamp(host, timezone.utc).isoformat(),
                "frames_before": cap.rx,
            }
            cap.markers.write(json.dumps(rec) + "\n")
            cap.markers.flush()
            cap.csv.flush()
            cap.log.flush()
            cap.marks += 1
        return f"marker '{label}' at device_ts={ts} us ({source}), after {rec['frames_before']} frames"

    def flush_capture(self, name: str) -> None:
        with self._lock:
            if self.capture and self.capture.name == name:
                for fh in (self.capture.csv, self.capture.log, self.capture.markers):
                    fh.flush()

    # ------------------------------------------------------------------ live view

    def stats(self, window_s: float) -> str:
        now = time.time()
        with self._lock:
            recent = 0
            for e in reversed(self.ring):
                if e.host_time < now - window_s:
                    break
                recent += 1
            ring_len, total, n_ids = len(self.ring), self.total_rx, len(self.ids)
            cap = self.capture
        st = self.state.status
        lines = [
            f"connected: {self.connected} ({self.port})"
            + (f", reader error: {self.reader_error}" if self.reader_error else ""),
            f"rx: {recent / window_s:.1f} frames/s over last {window_s:g} s, {total} total since connect, "
            f"{ring_len} in ring buffer, {n_ids} distinct IDs",
            f"parser skipped bytes: {self._parser.skipped_bytes}",
        ]
        if st:
            age = now - (self.state.status_host_time or now)
            lines.append(_fmt_status(st) + f" (age {age:.1f} s)")
        else:
            lines.append("device status: none received")
        if self.state.bus_params:
            lines.append(_fmt_bus(self.state.bus_params))
        lines.append(
            f"tx: session_enabled={self.tx_session_enabled}, status_spoof_enabled={self.status_spoof_enabled}, "
            f"frames_sent={self.tx_sent}"
        )
        lines.append(f"capture: {cap.name} ({cap.rx} rx, {cap.tx} tx, {cap.marks} markers)" if cap else "capture: none")
        return "\n".join(lines)

    def rx_since(self, seen_total: int) -> tuple[int, list[_RxEntry]]:
        """Frames received after the first `seen_total` frames since connect.

        Returns (total_rx, new entries, oldest first). If more frames arrived than the ring
        holds, only the ring's content is returned.
        """
        with self._lock:
            total = self.total_rx
            n = min(max(total - seen_total, 0), len(self.ring))
            new = list(itertools.islice(reversed(self.ring), n))
        new.reverse()
        return total, new

    def tail(self, n: int, id_filter: str | None) -> str:
        ids = _parse_id_filter(id_filter)
        out: list[_RxEntry] = []
        with self._lock:
            for e in reversed(self.ring):
                if ids is None or e.frame.can_id in ids:
                    out.append(e)
                    if len(out) >= n:
                        break
        if not out:
            return "no frames" + (f" matching {id_filter}" if id_filter else "")
        out.reverse()
        lines = []
        for e in out:
            f = e.frame
            t = datetime.fromtimestamp(e.host_time).strftime("%H:%M:%S.%f")[:-3]
            cid = f"{f.can_id:08X}" if f.extended else f"{f.can_id:03X}"
            lines.append(f"{t} dev={f.ts_us:>12} can{f.bus} {cid:>8} [{len(f.data)}] {f.data.hex(' ').upper()}")
        return "\n".join(lines)

    # ------------------------------------------------------------------ config / tx

    def set_bitrate(self, bitrate: int, listen_only: bool) -> str:
        self._require()
        self._write(gvret.encode_set_bus_config(bitrate, True, listen_only))
        time.sleep(0.1)
        bp = self._request(gvret.cmd(gvret.CMD_GET_BUS_PARAMS), gvret.BusParams)
        return _fmt_bus(bp) if bp else "bus config sent, but no reply to F1 06"

    def enable_tx(self, confirm: bool) -> str:
        self._require()
        if not confirm:
            st = self._request(gvret.encode_set_tx(False), gvret.DeviceStatus)
            self.tx_session_enabled = False
            self.status_spoof_enabled = False
            return "TX disabled for this session (status spoof disarmed too). " + (_fmt_status(st) if st else "(no status reply)")
        # Only probe the firmware here; the device stays listen-only. send_frame()
        # switches it to normal mode for the duration of each burst.
        st = self._request(gvret.cmd(gvret.CMD_STATUS), gvret.DeviceStatus)
        if st is None:
            return "no status reply from device; TX stays disabled"
        if not st.tx_allowed_by_firmware:
            return "firmware does not allow TX (tx_allowed_by_firmware=0, flash env cansniffer_tx); TX stays disabled. " + _fmt_status(st)
        self.tx_session_enabled = True
        bp = self._request(gvret.cmd(gvret.CMD_GET_BUS_PARAMS), gvret.BusParams)
        return (
            "TX armed for this session. The device stays listen-only; each send_frame call switches "
            "it to normal mode, sends, and returns it to listen-only.\n"
            + _fmt_status(st)
            + ("\n" + _fmt_bus(bp) if bp else "")
        )

    def enable_status_spoof(self, confirm: bool) -> str:
        """Separate session arm for spoofing the heater's own status frame 0x3E5 (radio emulation).

        Needed in addition to enable_tx. Only call with confirm=True after the user explicitly
        approved transmitting frames under the heater's ID. Sends nothing to the device.
        """
        self._require()
        if not confirm:
            self.status_spoof_enabled = False
            return "0x3E5 status spoof disarmed for this session."
        self.status_spoof_enabled = True
        note = "" if self.tx_session_enabled else " TX itself is not armed yet: enable_tx(confirm=true) is still required."
        return (
            "0x3E5 status spoof armed for this session: set_heater_mode(radio_emulation=true) may now send "
            "frames under the heater's own status ID 0x3E5." + note
        )

    def _set_device_tx(self, on: bool) -> gvret.BusParams | None:
        """Switch the firmware between normal (TX) and listen-only mode and return fresh bus params."""
        st = self._request(gvret.encode_set_tx(on), gvret.DeviceStatus)
        if st is None or st.tx_enabled_now != on:
            raise RuntimeError(f"device did not confirm tx_enabled_now={int(on)}")
        return self._request(gvret.cmd(gvret.CMD_GET_BUS_PARAMS), gvret.BusParams)

    def send_frame(self, can_id: int, data: bytes, extended: bool, count: int, interval_ms: int) -> str:
        self._require()
        st = self.state.status
        if not self.tx_session_enabled:
            raise PermissionError("TX not enabled in this session; call enable_tx(confirm=true) first")
        if not st or not st.tx_allowed_by_firmware:
            raise PermissionError("device status does not report tx_allowed_by_firmware")
        if not 1 <= count <= 1000:
            raise ValueError("count must be 1..1000")
        pkt = gvret.encode_tx_frame(can_id, data, extended)
        bp = self._set_device_tx(True)
        if bp is None or bp.can0_listen_only:
            self._set_device_tx(False)
            raise RuntimeError("device did not leave listen-only mode; nothing sent")
        sent = 0
        try:
            for i in range(count):
                self._write(pkt)
                sent += 1
                self.tx_sent += 1
                if self.capture:
                    ts, _ = self.device_now_us()
                with self._lock:
                    if self.capture:
                        self._write_capture_frame(Frame(ts, can_id, extended, "Tx", 0, bytes(data)))
                        self.capture.tx += 1
                if i + 1 < count:
                    time.sleep(interval_ms / 1000.0)
            time.sleep(0.05)  # let the last frame leave the TX queue
        finally:
            bp_after = self._set_device_tx(False)
        st_after = self._request(gvret.cmd(gvret.CMD_STATUS), gvret.DeviceStatus)
        cid = f"{can_id:08X}" if extended else f"{can_id:03X}"
        lines = [f"sent {sent}x {cid}#{bytes(data).hex().upper()}" + (" (logged to capture)" if self.capture else "")]
        if bp_after:
            lines.append("after burst: " + _fmt_bus(bp_after))
        if st_after:
            lines.append(_fmt_status(st_after))
        return "\n".join(lines)

    # ------------------------------------------------------------------ heater mode (0x5D3)

    def _last_panel_mode(self) -> _RxEntry | None:
        """Newest panel (Rx) 0x5D3 frame in the ring buffer, or None."""
        with self._lock:
            for e in reversed(self.ring):
                f = e.frame
                if f.can_id == ID_MODE and not f.extended and f.direction == "Rx" and f.data:
                    return e
        return None

    def _log_tx(self, can_id: int, extended: bool, data: bytes) -> None:
        """Count a sent frame and write it into the active capture with Dir=Tx (as send_frame does)."""
        self.tx_sent += 1
        ts = None
        if self.capture:
            ts, _ = self.device_now_us()
        with self._lock:
            if self.capture and ts is not None:
                self._write_capture_frame(Frame(ts, can_id, extended, "Tx", 0, bytes(data)))
                self.capture.tx += 1

    def set_heater_mode(
        self,
        mode: str,
        max_frames: int = 10,
        interval_ms: int = 500,
        radio_emulation: bool = False,
        radio_wake_bit: bool = True,
        radio_frames: int = 5,
    ) -> dict[str, Any]:
        """Request off / heat / vent from the roof control unit via 0x5D3 and verify it was adopted.

        The roof unit ("panel") sends 0x5D3 every 500 ms. We send `5D3 <mode | panel bit 7> 00`
        one frame per `interval_ms` until a panel frame carries exactly that byte 0, then stop.
        The device is switched to normal mode once for the whole sequence and back to
        listen-only in `finally`.

        With `radio_emulation` (needs enable_status_spoof in addition to enable_tx) the heater's
        own status frame 0x3E5 is spoofed as a Telestart radio start would report it: after a real
        0x3E5 confirms the target state (within SPOOF_CONFIRM_S of our first 0x5D3), up to
        `radio_frames` frames `3E5 <radio bits [| 0x80]> <bytes 1..7 of that real frame>` are sent,
        each SPOOF_DELAY_S after a confirming real 0x3E5 and at most one per real-frame interval,
        interleaved with the 0x5D3 schedule. Spoofing stops at the panel echo.
        """
        if mode not in HEATER_MODES:
            raise ValueError(f"mode must be one of {', '.join(HEATER_MODES)}")
        if not 1 <= max_frames <= 50:
            raise ValueError("max_frames must be 1..50")
        if not 50 <= interval_ms <= 5000:
            raise ValueError("interval_ms must be 50..5000")
        if radio_emulation and not 1 <= radio_frames <= 20:
            raise ValueError("radio_frames must be 1..20")
        self._require()
        st = self.state.status
        if not self.tx_session_enabled:
            raise PermissionError("TX not enabled in this session; call enable_tx(confirm=true) first")
        if not st or not st.tx_allowed_by_firmware:
            raise PermissionError("device status does not report tx_allowed_by_firmware")
        if radio_emulation and not self.status_spoof_enabled:
            raise PermissionError(
                "radio_emulation spoofs the heater's own status frame 0x3E5 and needs a separate arm: "
                "call enable_status_spoof(confirm=true) first, only after the user explicitly approved it"
            )

        mode_bits = HEATER_MODES[mode]
        interval = interval_ms / 1000.0
        last = self._last_panel_mode()
        fresh = last is not None and time.time() - last.host_time <= PANEL_FRESH_S
        panel_before = last.frame.data[0] if fresh and last else None
        # bit 7 must match what the panel currently sends; otherwise it overwrites our frame
        bit7 = (last.frame.data[0] & 0x80) if last else 0x80
        bit7_known = fresh
        target = mode_bits | bit7
        first_target = target

        frames_sent = 0
        panel_after: int | None = None
        adopted_at: float | None = None
        t_first: float | None = None
        seen = 0
        heater_confirmed: bool | None = None
        tx_log: list[tuple[float, int, bytes]] = []  # (host time, id, data) of everything we sent

        # radio emulation state
        radio_b0 = RADIO_BITS[mode] | (RADIO_WAKE_BIT if radio_wake_bit else 0)
        spoof_confirmed_at: float | None = None  # host time of the first confirming real 0x3E5
        spoof_window_closed = False
        real_3e5_seen = 0
        spoof_due: float | None = None  # when to send the spoof for the latest confirming real frame
        spoof_ref: float | None = None  # host time of that real frame
        spoof_data: bytes | None = None  # payload for it (bytes 1..7 copied from it)
        spoof_last_payload: bytes | None = None
        spoofs_sent = 0

        bp = self._set_device_tx(True)
        if bp is None or bp.can0_listen_only:
            self._set_device_tx(False)
            raise RuntimeError("device did not leave listen-only mode; nothing sent")
        try:
            next_send = time.time()
            while True:
                now = time.time()
                if frames_sent < max_frames and now >= next_send and adopted_at is None:
                    data = bytes([target, 0x00])
                    if t_first is None:
                        seen = self.total_rx  # only frames after our first one count
                    self._write(gvret.encode_tx_frame(ID_MODE, data, False))
                    sent_at = time.time()
                    frames_sent += 1
                    tx_log.append((sent_at, ID_MODE, data))
                    self._log_tx(ID_MODE, False, data)
                    if t_first is None:
                        t_first = sent_at
                        # sleeping bus: the panel's value is only known after our wake frame
                        next_send = sent_at + (interval if bit7_known else max(interval, WAKE_WAIT_S))
                    else:
                        next_send = sent_at + interval
                if t_first is not None:
                    seen, new = self.rx_since(seen)
                    for e in new:
                        f = e.frame
                        if f.extended or f.direction != "Rx" or not f.data:
                            continue
                        if f.can_id == ID_STATUS and radio_emulation and not spoof_window_closed:
                            real_3e5_seen += 1
                            confirms = _status_confirms(mode, f.data)
                            if spoof_confirmed_at is None and confirms and e.host_time <= t_first + SPOOF_CONFIRM_S:
                                spoof_confirmed_at = e.host_time
                            if spoof_confirmed_at is not None and confirms and len(f.data) == 8:
                                # one spoof per real-frame interval, after a confirming real frame only
                                spoof_ref, spoof_due = e.host_time, e.host_time + SPOOF_DELAY_S
                                spoof_data = bytes([radio_b0]) + bytes(f.data[1:8])
                            else:
                                spoof_due = None
                            continue
                        if f.can_id != ID_MODE:
                            continue
                        b0 = f.data[0]
                        panel_after = b0
                        if not bit7_known:
                            bit7_known = True
                            if panel_before is None:
                                panel_before = b0
                            bit7 = b0 & 0x80
                            target = mode_bits | bit7
                            # first panel frame after waking: send the corrected frame right away
                            next_send = min(next_send, max(e.host_time, t_first + interval))
                        if b0 == target:
                            adopted_at = e.host_time
                            break
                if adopted_at is not None:
                    break  # panel echo: stop 0x5D3 and spoofing
                if radio_emulation and t_first is not None and not spoof_window_closed:
                    now = time.time()
                    if spoof_confirmed_at is None and now > t_first + SPOOF_CONFIRM_S:
                        spoof_window_closed = True  # no confirming real 0x3E5: never spoof
                        spoof_due = None
                    if spoof_due is not None and spoof_ref is not None and spoof_data is not None and now >= spoof_due:
                        if spoofs_sent < radio_frames and now - spoof_ref <= SPOOF_LATE_S:
                            self._write(gvret.encode_tx_frame(ID_STATUS, spoof_data, False))
                            tx_log.append((time.time(), ID_STATUS, spoof_data))
                            spoofs_sent += 1
                            spoof_last_payload = spoof_data
                            self._log_tx(ID_STATUS, False, spoof_data)
                        spoof_due = None  # sent or too late: wait for the next real frame
                if frames_sent >= max_frames and time.time() >= next_send:
                    break  # watched one full slot after the last frame
                time.sleep(0.01)
        finally:
            self._set_device_tx(False)

        ok = adopted_at is not None
        if ok:
            heater_confirmed = self._confirm_heater(mode, adopted_at)
        cid_data = f"5D3#{target:02X}00"
        panel_txt = f"0x{panel_after:02X}" if panel_after is not None else "none"
        if ok:
            ms = int(round((adopted_at - t_first) * 1000)) if t_first else 0
            conf = {True: "confirmed by 0x3E5", False: "0x3E5 does not confirm yet", None: "no 0x3E5 seen"}[heater_confirmed]
            message = (
                f"{mode} ({cid_data}) adopted by roof unit after {frames_sent} frame(s), {ms} ms; {conf}. "
                f"Vom Bedienteil übernommen nach {frames_sent} Frame(s)."
            )
        else:
            ms = None
            message = (
                f"{mode} ({cid_data}) not adopted by roof unit after {frames_sent} frame(s); "
                f"panel still sends {panel_txt}. Vom Bedienteil nicht übernommen."
            )
        if first_target != target:
            message += f" Wake frame used 0x{first_target:02X}, panel bit 7 required 0x{target:02X}."
        result: dict[str, Any] = {
            "ok": ok,
            "mode": mode,
            "target_byte": f"0x{target:02X}",
            "panel_before": None if panel_before is None else f"0x{panel_before:02X}",
            "panel_after": None if panel_after is None else f"0x{panel_after:02X}",
            "frames_sent": frames_sent,
            "adopted_after_ms": ms,
            "heater_confirmed": heater_confirmed,
            "message": message,
            "radio_emulation": radio_emulation,
        }
        if radio_emulation:
            confirmed_before = spoof_confirmed_at is not None
            if confirmed_before:
                cms = int(round((spoof_confirmed_at - t_first) * 1000)) if t_first else 0
                message += (
                    f" Radio emulation: heater confirmed {mode} in 0x3E5 after {cms} ms; "
                    f"{spoofs_sent} spoofed 0x3E5 frame(s) sent"
                    + (f" (3E5#{spoof_last_payload.hex().upper()})." if spoof_last_payload else ".")
                )
            else:
                why = "none of them confirmed" if real_3e5_seen else "no 0x3E5 received"
                message += (
                    f" Radio emulation: no confirming 0x3E5 from the heater within "
                    f"{int(SPOOF_CONFIRM_S * 1000)} ms ({why}); no spoof sent."
                )
            result["message"] = message
            result.update(
                spoof_frames_sent=spoofs_sent,
                spoof_payload=spoof_last_payload.hex(" ").upper() if spoof_last_payload else None,
                heater_confirmed_before_spoof=confirmed_before,
            )
        if t_first is not None:
            result["timeline"], dropped = self._timeline(t_first, tx_log)
            if dropped:
                result["timeline_omitted"] = dropped
        return result

    def _last_rx(self, can_id: int) -> _RxEntry | None:
        """Newest Rx frame with `can_id` (standard ID, non-empty) in the ring buffer, or None."""
        with self._lock:
            for e in reversed(self.ring):
                f = e.frame
                if f.can_id == can_id and not f.extended and f.direction == "Rx" and f.data:
                    return e
        return None

    def radio_impulse(
        self, kind: str, wake_bit: bool = True, frames: int = 5, observe_s: float = IMPULSE_OBSERVE_S
    ) -> dict[str, Any]:
        """Emulate a VW radio remote (Telestart hand transmitter) ON or OFF impulse.

        The hand transmitter only sends an impulse; the roof unit picks mode and run time itself
        (menu FERNBED). The heater reports such a start in 0x3E5 byte 0 (b0 on via radio, b1 off
        via radio). No 0x5D3 command is sent: only `frames` spoofed 0x3E5
        `<0x01|0x02 [| 0x80]> <bytes 1..7 of the latest real 0x3E5>`, each SPOOF_DELAY_S after a
        real heater 0x3E5, at most one per real-frame interval. A sleeping bus is first woken with
        one neutral 0x5D3 equal to the panel's last value. Then the device returns to listen-only
        and the bus is observed for `observe_s` seconds.

        Needs enable_tx and enable_status_spoof, like set_heater_mode(radio_emulation=True).
        """
        if kind not in ("on", "off"):
            raise ValueError("kind must be 'on' or 'off'")
        if not 1 <= frames <= 20:
            raise ValueError("frames must be 1..20")
        if not 0.5 <= observe_s <= 10:
            raise ValueError("observe_s must be 0.5..10")
        self._require()
        st = self.state.status
        if not self.tx_session_enabled:
            raise PermissionError("TX not enabled in this session; call enable_tx(confirm=true) first")
        if not st or not st.tx_allowed_by_firmware:
            raise PermissionError("device status does not report tx_allowed_by_firmware")
        if not self.status_spoof_enabled:
            raise PermissionError(
                "radio_impulse spoofs the heater's own status frame 0x3E5 and needs a separate arm: "
                "call enable_status_spoof(confirm=true) first, only after the user explicitly approved it"
            )

        b0 = (0x01 if kind == "on" else 0x02) | (RADIO_WAKE_BIT if wake_bit else 0)
        t0 = time.time()
        with self._lock:
            newest = self.ring[-1].host_time if self.ring else None
        asleep = newest is None or t0 - newest > BUS_AWAKE_S
        last_panel = self._last_panel_mode()
        last_status = self._last_rx(ID_STATUS)
        panel_before = last_panel.frame.data[0] if last_panel and t0 - last_panel.host_time <= PANEL_FRESH_S else None
        heater_before = bytes(last_status.frame.data) if last_status and t0 - last_status.host_time <= PANEL_FRESH_S else None

        tx_log: list[tuple[float, int, bytes]] = []
        wake_frame: bytes | None = None
        seen = self.total_rx
        spoofs_sent = 0
        spoof_last: bytes | None = None
        t_first: float | None = None  # first Tx (wake frame or first spoof)
        t_first_spoof: float | None = None
        real_seen = False

        bp = self._set_device_tx(True)
        if bp is None or bp.can0_listen_only:
            self._set_device_tx(False)
            raise RuntimeError("device did not leave listen-only mode; nothing sent")
        try:
            if asleep:
                # neutral wake frame: the panel's own last value (bit 7 and mode unchanged), else 0x80 = off
                wake_frame = bytes([last_panel.frame.data[0] if last_panel else 0x80, 0x00])
                self._write(gvret.encode_tx_frame(ID_MODE, wake_frame, False))
                t_first = time.time()
                tx_log.append((t_first, ID_MODE, wake_frame))
                self._log_tx(ID_MODE, False, wake_frame)
            start = time.time()
            first_deadline = start + IMPULSE_WAIT_3E5_S
            deadline = first_deadline + frames * 0.2 * 3  # generous: one slot per real frame plus misses
            due: float | None = None
            ref: float | None = None
            payload: bytes | None = None
            while spoofs_sent < frames:
                seen, new = self.rx_since(seen)
                for e in new:
                    f = e.frame
                    if f.extended or f.direction != "Rx" or not f.data:
                        continue
                    if f.can_id == ID_MODE and t_first_spoof is None:
                        panel_before = f.data[0]  # e.g. the panel's first frame after waking
                    if f.can_id == ID_STATUS and len(f.data) == 8:
                        real_seen = True
                        if t_first_spoof is None:
                            heater_before = bytes(f.data)
                        ref, due = e.host_time, e.host_time + SPOOF_DELAY_S
                        payload = bytes([b0]) + bytes(f.data[1:8])
                now = time.time()
                if not real_seen and now > first_deadline:
                    break  # no heater 0x3E5: nothing to copy, no spoof
                if now > deadline:
                    break
                if due is not None and ref is not None and payload is not None and now >= due:
                    if now - ref <= SPOOF_LATE_S:
                        self._write(gvret.encode_tx_frame(ID_STATUS, payload, False))
                        sent_at = time.time()
                        tx_log.append((sent_at, ID_STATUS, payload))
                        self._log_tx(ID_STATUS, False, payload)
                        spoofs_sent += 1
                        spoof_last = payload
                        if t_first is None:
                            t_first = sent_at
                        if t_first_spoof is None:
                            t_first_spoof = sent_at
                    due = None
                time.sleep(0.01)
            time.sleep(0.05)  # let the last frame leave the TX queue
        finally:
            self._set_device_tx(False)

        # observe the reaction (device listen-only again)
        panel_after: int | None = None
        panel_changed_at: float | None = None
        heater_after: bytes | None = None
        heater_changed_at: float | None = None
        if t_first_spoof is not None:
            end = time.time() + observe_s
            while time.time() < end:
                time.sleep(min(0.1, max(0.0, end - time.time())))
            with self._lock:
                recent = list(itertools.takewhile(lambda e: e.host_time >= t_first_spoof, reversed(self.ring)))
            for e in reversed(recent):
                f = e.frame
                if f.extended or f.direction != "Rx" or not f.data:
                    continue
                if f.can_id == ID_MODE:
                    panel_after = f.data[0]
                    if panel_changed_at is None and panel_before is not None and panel_after != panel_before:
                        panel_changed_at = e.host_time
                elif f.can_id == ID_STATUS:
                    heater_after = bytes(f.data)
                    if (
                        heater_changed_at is None
                        and heater_before is not None
                        and _heater_state(heater_after) != _heater_state(heater_before)
                    ):
                        heater_changed_at = e.host_time
        if panel_after is None:
            panel_after = panel_before
        if heater_after is None:
            heater_after = heater_before

        def mode_name(b: int | None) -> str | None:
            return None if b is None else HEATER_MODE_NAMES.get(b & 0x7F, f"unknown(0x{b:02X})")

        def ms(t: float | None) -> int | None:
            return None if t is None or t_first_spoof is None else int(round((t - t_first_spoof) * 1000))

        panel_changed = panel_changed_at is not None
        expected = panel_after is not None and ((panel_after & 0x7F) != 0 if kind == "on" else (panel_after & 0x7F) == 0)
        ok = panel_changed and expected
        hb = _heater_state(heater_before) if heater_before else None
        ha = _heater_state(heater_after) if heater_after else None
        hx = lambda b: None if b is None else f"0x{b:02X}"  # noqa: E731
        if not spoofs_sent:
            message = (
                f"radio impulse {kind}: no real 0x3E5 from the heater within {int(IMPULSE_WAIT_3E5_S * 1000)} ms"
                + (" after the wake frame" if wake_frame else "")
                + "; no spoof sent."
            )
        else:
            message = f"radio impulse {kind}: {spoofs_sent} spoofed 3E5#{spoof_last.hex().upper()} sent. "  # type: ignore[union-attr]
            if panel_changed:
                message += (
                    f"Panel changed 0x5D3 on its own from {hx(panel_before)} to {hx(panel_after)} "
                    f"({mode_name(panel_after)}) after {ms(panel_changed_at)} ms"
                    + ("." if ok else f", not the expected {'non-off mode' if kind == 'on' else 'off'}.")
                )
            else:
                message += f"Panel 0x5D3 unchanged ({hx(panel_after)}) during {observe_s:g} s."
            message += (
                f" Heater state {hb} -> {ha}" + (f" after {ms(heater_changed_at)} ms." if heater_changed_at else " (unchanged).")
            )
        if wake_frame:
            message += f" Bus was asleep: woke it with 5D3#{wake_frame.hex().upper()}."
        result: dict[str, Any] = {
            "ok": ok,
            "kind": kind,
            "panel_before": hx(panel_before),
            "panel_after": hx(panel_after),
            "panel_changed": panel_changed,
            "panel_changed_after_ms": ms(panel_changed_at),
            "heater_before": hb,
            "heater_after": ha,
            "heater_before_raw": heater_before.hex(" ").upper() if heater_before else None,
            "heater_after_raw": heater_after.hex(" ").upper() if heater_after else None,
            "heater_changed_after_ms": ms(heater_changed_at),
            "spoof_frames_sent": spoofs_sent,
            "spoof_payload": spoof_last.hex(" ").upper() if spoof_last else None,
            "wake_frame": wake_frame.hex(" ").upper() if wake_frame else None,
            "message": message,
            "timeline": [],
        }
        if t_first is not None:
            result["timeline"], dropped = self._timeline(t_first, tx_log)
            if dropped:
                result["timeline_omitted"] = dropped
        return result

    def _timeline(self, t_first: float, tx_log: list[tuple[float, int, bytes]]) -> tuple[list[list[Any]], int]:
        """Compact [t_ms, id, dir, hex] list of 0x5D3 and 0x3E5 (Rx from the ring plus our Tx) since
        `t_first`. Above TIMELINE_MAX entries, unchanged Rx repeats not next to a Tx frame are dropped,
        then the middle is cut. Returns (entries, number of entries dropped)."""
        with self._lock:
            rx = list(itertools.takewhile(lambda e: e.host_time >= t_first, reversed(self.ring)))
        ev = [
            (e.host_time, e.frame.can_id, "Rx", bytes(e.frame.data))
            for e in reversed(rx)
            if e.frame.can_id in (ID_MODE, ID_STATUS) and not e.frame.extended and e.frame.direction == "Rx"
        ]
        ev += [(t, cid, "Tx", d) for t, cid, d in tx_log]
        ev.sort(key=lambda x: x[0])
        total = len(ev)
        if total > TIMELINE_MAX:
            keep, prev = [], {}
            for i, x in enumerate(ev):
                key = (x[1], x[2])
                near_tx = any(0 <= j < total and ev[j][2] == "Tx" for j in (i - 1, i + 1))
                if x[2] == "Rx" and prev.get(key) == x[3] and not near_tx:
                    continue
                prev[key] = x[3]
                keep.append(x)
            ev = keep
            if len(ev) > TIMELINE_MAX:
                head = TIMELINE_MAX * 2 // 3
                ev = ev[:head] + ev[-(TIMELINE_MAX - head):]
        out = [[int(round((t - t_first) * 1000)), f"{cid:03X}", d, data.hex().upper()] for t, cid, d, data in ev]
        return out, total - len(out)

    def _confirm_heater(self, mode: str, since: float, timeout_s: float = HEATER_CONFIRM_S) -> bool | None:
        """Watch 0x3E5 after adoption: heat -> control LED (byte1 b1), vent -> byte7 b0, off -> both 0.

        True on the first matching 0x3E5 within `timeout_s`; False if 0x3E5 frames arrived but none
        matched; None if no 0x3E5 arrived (e.g. heater not on the bus).
        """
        deadline = time.time() + timeout_s
        seen_any = False
        while True:
            with self._lock:
                recent = list(itertools.takewhile(lambda e: e.host_time >= since, reversed(self.ring)))
            for e in reversed(recent):
                f = e.frame
                if f.can_id != ID_STATUS or f.extended or f.direction != "Rx":
                    continue
                seen_any = True
                if _status_confirms(mode, f.data):
                    return True
            if time.time() >= deadline:
                return False if seen_any else None
            time.sleep(0.02)


def _heater_state(data: bytes) -> str:
    """Derived heater state from 0x3E5 (same rule as the dashboard): off, heating, ventilating, run_on."""
    d = analysis.decode_3e5_payload(data)
    if d["ventilation_active"]:
        return "ventilating"
    if d["control_led"] or d["combustion_on"] or d["heating_power_raw"]:
        return "heating"
    if d["water_pump_request"]:
        return "run_on"
    return "off"


def _status_confirms(mode: str, data: bytes) -> bool:
    """True if a 0x3E5 payload shows `mode`: heat -> control LED (byte1 b1), vent -> ventilation
    active (byte7 b0), off -> both clear."""
    d = analysis.decode_3e5_payload(data)
    led, vent = d["control_led"], d["ventilation_active"]
    return bool((mode == "heat" and led) or (mode == "vent" and vent) or (mode == "off" and not led and not vent))


# ---------------------------------------------------------------------- formatting helpers


def _fmt_bus(bp: gvret.BusParams) -> str:
    def one(n, en, lo, br):
        return f"can{n}: {'enabled' if en else 'disabled'}, {br} bit/s{', listen-only' if lo else ''}"

    return "bus params: " + one(0, bp.can0_enabled, bp.can0_listen_only, bp.can0_bitrate) + "; " + one(
        1, bp.can1_enabled, bp.can1_listen_only, bp.can1_bitrate
    )


def _fmt_status(st: gvret.DeviceStatus) -> str:
    return (
        f"device status: state={st.state_name}, rx_frames={st.rx_frames}, rx_missed={st.rx_missed}, "
        f"bus_errors={st.bus_errors}, tx_allowed_by_firmware={int(st.tx_allowed_by_firmware)}, "
        f"tx_enabled_now={int(st.tx_enabled_now)}"
    )


def _parse_int(s: str | int) -> int:
    if isinstance(s, int):
        return s
    s = s.strip()
    return int(s, 0) if s.lower().startswith("0x") else int(s, 16)


def _parse_id_filter(spec: str | None) -> set[int] | None:
    if spec is None or str(spec).strip() == "":
        return None
    return {_parse_int(p) for p in re.split(r"[,\s]+", str(spec).strip()) if p}


def _parse_hex_data(s: str) -> bytes:
    clean = re.sub(r"[\s:.,-]", "", s)
    if clean.lower().startswith("0x"):
        clean = clean[2:]
    if len(clean) % 2:
        raise ValueError("data_hex must have an even number of hex digits")
    return bytes.fromhex(clean)


# ---------------------------------------------------------------------- MCP wiring

sniffer = CanSniffer()
mcp = FastMCP("webasto-can-capture")


async def _run(fn, *args) -> str:
    try:
        return await asyncio.to_thread(fn, *args)
    except Exception as e:
        logger.warning("tool error: %s", e)
        return f"Error: {e}"


@mcp.tool()
async def connect(port: str = DEFAULT_PORT, bitrate: int = 100000) -> str:
    """Open the ESP32 GVRET CAN sniffer over USB serial (921600 baud), enter binary mode and
    configure can0 at `bitrate` in listen-only mode. Returns device info and bus params."""
    return await _run(sniffer.connect, port, bitrate)


@mcp.tool()
async def disconnect() -> str:
    """Stop any active capture and close the serial port."""
    return await _run(sniffer.disconnect)


@mcp.tool()
async def stats(window_s: float = 5.0) -> str:
    """Live statistics: frames/s over the last window, totals, missed frames, bus errors,
    controller state, TX flags, distinct IDs, capture state."""
    return await _run(sniffer.stats, max(window_s, 0.1))


@mcp.tool()
async def tail(n: int = 20, id_filter: str | None = None) -> str:
    """Show the last n received frames. id_filter: hex IDs, comma separated (e.g. "3E5,5EB")."""
    return await _run(sniffer.tail, max(1, min(n, 2000)), id_filter)


@mcp.tool()
async def set_bitrate(bitrate: int, listen_only: bool = True) -> str:
    """Reconfigure can0 bitrate (e.g. 100000 for VW comfort CAN, 500000 for drivetrain)."""
    return await _run(sniffer.set_bitrate, bitrate, listen_only)


@mcp.tool()
async def start_capture(name: str) -> str:
    """Start recording to captures/<name>.csv (SavvyCAN GVRET CSV), <name>.log (candump -L)
    and <name>.markers.jsonl."""
    return await _run(sniffer.start_capture, name)


@mcp.tool()
async def stop_capture() -> str:
    """Stop the active capture and report file paths and frame counts."""
    return await _run(sniffer.stop_capture)


@mcp.tool()
async def mark_event(label: str) -> str:
    """Write a labelled marker (device timestamp + host time) into the active capture,
    e.g. right when the user presses a button on the heater panel."""
    return await _run(sniffer.mark_event, label)


def _list_captures() -> str:
    if not CAPTURES_DIR.is_dir():
        return f"no captures directory ({CAPTURES_DIR})"
    rows = []
    for p in sorted(CAPTURES_DIR.glob("*.csv"), key=lambda p: p.stat().st_mtime):
        with p.open() as fh:
            n = sum(1 for _ in fh) - 1
        mk = analysis.load_markers(p.with_name(p.stem + ".markers.jsonl"))
        mtime = datetime.fromtimestamp(p.stat().st_mtime).strftime("%Y-%m-%d %H:%M:%S")
        active = " (active)" if sniffer.capture and sniffer.capture.name == p.stem else ""
        labels = ", ".join(m.label for m in mk[:10]) + (" ..." if len(mk) > 10 else "")
        rows.append(f"{p.stem}{active}: {n} frames, {len(mk)} markers [{labels}], modified {mtime}")
    return f"captures in {CAPTURES_DIR}:\n" + ("\n".join(rows) if rows else "(none)")


@mcp.tool()
async def list_captures() -> str:
    """List recorded captures with frame and marker counts."""
    return await _run(_list_captures)


def _load(name: str) -> analysis.Capture:
    sniffer.flush_capture(Path(name).stem)
    return analysis.load_capture(name, CAPTURES_DIR)


@mcp.tool()
async def summarize(capture: str, start_s: float | None = None, end_s: float | None = None) -> str:
    """Per-ID summary of a capture (times relative to first frame): count, mean period,
    DLCs, changing bytes with their values, toggled bits."""
    return await _run(lambda: analysis.summarize(_load(capture), start_s, end_s))


@mcp.tool()
async def diff(capture: str, marker: str | None = None, window_s: float = 5.0, capture_b: str | None = None) -> str:
    """Compare `window_s` before vs after a marker (label, label#n for the n-th occurrence,
    or index) in one capture, or compare `capture` with `capture_b`. Reports IDs present on
    one side only, rate changes >20 %, and differing bytes/bits ranked by relevance."""

    def work() -> str:
        cap = _load(capture)
        if capture_b:
            return analysis.diff_captures(cap, _load(capture_b))
        if marker is None:
            raise ValueError("give either marker or capture_b")
        return analysis.diff_marker(cap, marker, window_s)

    return await _run(work)


@mcp.tool()
async def decode_3e5(capture: str, max_rows: int = 50) -> str:
    """Decode 0x3E5 (Webasto/parking-heater status, unverified T5.1 layout) and print only
    rows where a decoded field changes, interleaved with markers."""
    return await _run(lambda: analysis.decode_3e5(_load(capture), max_rows))


@mcp.tool()
async def enable_tx(confirm: bool) -> str:
    """Arm CAN transmit for this session. Only call with confirm=true after the user explicitly
    agreed to inject frames into the car's bus; confirm=false disarms. Arming checks that the
    firmware allows TX (env cansniffer_tx) but keeps the device listen-only."""
    return await _run(sniffer.enable_tx, confirm)


@mcp.tool()
async def send_frame(id: str, data_hex: str, extended: bool = False, count: int = 1, interval_ms: int = 100) -> str:
    """Transmit a CAN frame on can0 (id in hex, e.g. "3E5"; data_hex e.g. "60 80 EE 86").
    Refused unless the firmware allows TX and enable_tx(confirm=true) was called this session.
    Switches the device to normal mode for the burst only and back to listen-only afterwards
    (also on error). Sent frames are logged into the active capture with Dir=Tx.
    Known commands (T5.2 WWZH): 5D3 "02 00" heat, "04 00" ventilate, "00 00" off; count=3,
    interval_ms=500."""

    def work() -> str:
        return sniffer.send_frame(_parse_int(id), _parse_hex_data(data_hex), extended, count, max(0, interval_ms))

    return await _run(work)


@mcp.tool()
async def enable_status_spoof(confirm: bool) -> str:
    """Arm spoofing of the heater's OWN status frame 0x3E5 for this session (needed for
    set_heater_mode(radio_emulation=true), in addition to enable_tx). This transmits frames under
    the Webasto heater's ID, which is a bigger step than sending roof-unit commands: only call with
    confirm=true after the user explicitly approved 0x3E5 spoofing in this conversation.
    confirm=false disarms. Sends nothing to the device by itself."""
    return await _run(sniffer.enable_status_spoof, confirm)


@mcp.tool()
async def set_heater_mode(
    mode: str,
    max_frames: int = 10,
    radio_emulation: bool = False,
    radio_wake_bit: bool = True,
    radio_frames: int = 5,
) -> str:
    """Switch the T5.2 parking heater via the roof control unit (0x5D3) and verify the result.
    mode: "off", "heat" or "vent". Same TX gating as send_frame (enable_tx(confirm=true) first).

    The roof unit sends 0x5D3 every 500 ms; byte 0 is 00/02/04 plus a panel-state flag in
    bit 7 (0x80) that must match what the panel currently sends, otherwise our frame is
    overwritten. This tool takes bit 7 from the last panel 0x5D3 (if younger than 2 s), or
    sends one wake frame and waits up to 600 ms for the panel's value. It then sends
    `5D3 <target> 00` once per 500 ms, normal mode for the whole sequence, until a panel 0x5D3
    carries the target byte (typically after 1-4 frames), and stops. Fails with "not adopted
    by roof unit" after max_frames (known: vent-on with bit 7 set is never adopted). After
    adoption, 0x3E5 is checked (heat: control LED, vent: ventilation bit, off: both clear).

    radio_emulation=true (experimental, Telestart radio-start hypothesis) additionally SPOOFS THE
    HEATER'S OWN STATUS FRAME 0x3E5. It needs explicit user approval and a separate
    enable_status_spoof(confirm=true) besides enable_tx; otherwise it is refused. After a real
    0x3E5 confirms the target state (within 1.5 s of the first 0x5D3), up to radio_frames
    (1..20) frames `3E5 <radio bits> <bytes 1..7 of that real frame>` are sent, each ~100 ms after
    a confirming real 0x3E5, at most one per 200 ms heater interval, interleaved with 0x5D3 and
    stopped at the panel echo. Radio bits in byte 0: vent 0x09, heat 0x05, off 0x02, plus 0x80
    (wake Komfort-CAN) if radio_wake_bit. No confirming real 0x3E5: no spoof, reported.

    Returns JSON: ok, mode, target_byte, panel_before, panel_after, frames_sent,
    adopted_after_ms, heater_confirmed, message, radio_emulation, timeline ([t_ms, id, dir, hex]
    for 0x5D3/0x3E5 from the first Tx, max ~60), and with radio_emulation: spoof_frames_sent,
    spoof_payload, heater_confirmed_before_spoof."""

    def work() -> str:
        return json.dumps(
            sniffer.set_heater_mode(
                mode.strip().lower(),
                max(1, min(max_frames, 50)),
                radio_emulation=radio_emulation,
                radio_wake_bit=radio_wake_bit,
                radio_frames=radio_frames,
            )
        )

    return await _run(work)


@mcp.tool()
async def radio_impulse(kind: str, wake_bit: bool = True, frames: int = 5, observe_s: float = 3.0) -> str:
    """Emulate a VW radio remote (Telestart hand transmitter) ON or OFF impulse by SPOOFING THE
    HEATER'S OWN STATUS FRAME 0x3E5. Needs explicit user approval: enable_tx(confirm=true) and
    enable_status_spoof(confirm=true) first, otherwise refused. kind: "on" or "off".

    The hand transmitter only sends an impulse; the roof unit chooses mode and run time itself
    (menu FERNBED), and the heater reports the start in 0x3E5 byte 0 (b0 on via radio, b1 off via
    radio). No 0x5D3 command is sent. `frames` (default 5) spoofed frames
    `3E5 <01|02, +0x80 if wake_bit> <bytes 1..7 of the latest real 0x3E5>` go out, each ~100 ms
    after a real heater 0x3E5, at most one per 200 ms interval. A sleeping bus is woken first
    with one neutral 0x5D3 equal to the panel's last value; then up to 1 s wait for a real 0x3E5
    (none: no spoof). The device returns to listen-only, and panel and heater are observed for
    `observe_s` s. Returns JSON: ok (panel changed its 0x5D3 on its own to a non-off mode for
    "on", to off for "off"), panel_before, panel_after, panel_changed, panel_changed_after_ms,
    heater_before/after (state) and *_raw, heater_changed_after_ms, spoof_frames_sent,
    spoof_payload, wake_frame, timeline ([t_ms, id, dir, hex]), message."""

    def work() -> str:
        return json.dumps(
            sniffer.radio_impulse(kind.strip().lower(), wake_bit, max(1, min(frames, 20)), max(0.5, min(observe_s, 10.0)))
        )

    return await _run(work)


async def serve() -> None:
    logger.info("Webasto CAN capture MCP server starting (captures: %s)", CAPTURES_DIR)
    try:
        await mcp.run_stdio_async()
    finally:
        if sniffer.connected or sniffer.capture:
            sniffer.disconnect()
