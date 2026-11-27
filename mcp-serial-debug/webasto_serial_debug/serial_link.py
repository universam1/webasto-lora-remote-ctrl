"""Serial side: device aliases, port resolution, background readers with a ring buffer, and
log analysis for the ESP32 boards (CP2102 USB-UART bridges, 115200 baud).

Opening a CP2102 port normally pulses DTR/RTS, which resets the ESP32 (EN on RTS, IO0 on DTR).
Every open here sets dtr=False and rts=False on the pyserial object BEFORE open(), so attaching
does not reset the board. A reset is an explicit RTS pulse with DTR held False.
"""

from __future__ import annotations

import collections
import errno
import fcntl
import glob
import logging
import os
import re
import select
import struct
import termios
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable, Iterable

import serial
from serial.tools import list_ports

logger = logging.getLogger("webasto-serial-debug")

BAUD = 115200
RING_LINES = 5000
REOPEN_DELAY_S = 0.5
MAX_REOPEN_DELAY_S = 5.0

CP2102 = (0x10C4, 0xEA60)
ESP32S2_CDC = (0x303A, 0x80C2)


@dataclass(frozen=True)
class Alias:
    name: str
    paths: tuple[str, ...]  # first existing wins
    description: str
    mac: str = ""
    ble_name: str = ""
    attachable: bool = True
    note: str = ""


ALIASES: dict[str, Alias] = {
    "receiver": Alias(
        "receiver", ("/dev/ttyWEBASTORECV",), "LoRa receiver in the car (TTGO LoRa32, CP2102, USB path 1.4)",
        mac="30:ae:a4:37:89:c8", ble_name="Webasto-RX-89CA",
    ),
    "sender": Alias(
        "sender", ("/dev/ttyWEBASTOSEND", "/dev/ttyUSB0"),
        "LoRa sender / remote (TTGO LoRa32, CP2102, USB path 1.1; fallback /dev/ttyUSB0 if it is a "
        "CP2102 not claimed by another alias)",
        mac="30:ae:a4:37:89:f0", ble_name="Webasto-TX-89F2",
    ),
    "simulator": Alias(
        "simulator", ("/dev/ttyWEBASTOSIM",), "W-BUS simulator (ESP32, CP2102, USB path 1.2)",
    ),
    "cansniffer": Alias(
        "cansniffer", ("/dev/ttyCANSNIFF",), "CAN sniffer (ESP32-S2 native USB CDC, 303a:80c2)",
        attachable=False,
        note="handled by the webasto-can-capture MCP server (needs DTR asserted, GVRET binary); "
        "not attachable here",
    ),
}
ALIAS_SYNONYMS = {"rx": "receiver", "recv": "receiver", "tx": "sender", "send": "sender", "sim": "simulator",
                  "can": "cansniffer", "sniffer": "cansniffer"}


class PortBusyError(RuntimeError):
    pass


# ---------------------------------------------------------------------- port resolution

def _usb_info() -> dict[str, Any]:
    """realpath -> pyserial ListPortInfo (reads sysfs only, does not open ports)."""
    out = {}
    try:
        for p in list_ports.comports():
            out[os.path.realpath(p.device)] = p
    except Exception as e:  # pragma: no cover
        logger.info("comports failed: %s", e)
    return out


def _vidpid(info) -> tuple[int, int] | None:
    if info is None or info.vid is None:
        return None
    return (info.vid, info.pid)


def resolve_alias(alias: Alias, usb: dict[str, Any] | None = None) -> str | None:
    """Return the path to use for an alias, or None if no candidate is present."""
    usb = _usb_info() if usb is None else usb
    primary = alias.paths[0]
    if os.path.exists(primary):
        return primary
    for cand in alias.paths[1:]:
        if not os.path.exists(cand):
            continue
        real = os.path.realpath(cand)
        if _vidpid(usb.get(real)) != CP2102:
            continue
        claimed = {
            os.path.realpath(a.paths[0]) for a in ALIASES.values() if a is not alias and os.path.exists(a.paths[0])
        }
        if real in claimed:
            continue
        return cand
    return None


def resolve_device(device: str) -> tuple[str, str]:
    """(label, path) for an alias or a path. Raises ValueError with a helpful message."""
    d = device.strip()
    key = ALIAS_SYNONYMS.get(d.lower(), d.lower())
    if key in ALIASES:
        a = ALIASES[key]
        if not a.attachable:
            raise ValueError(f"{a.name}: {a.note}")
        path = resolve_alias(a)
        if path is None:
            raise ValueError(f"{a.name} not present (tried {', '.join(a.paths)}); see list_devices")
        return a.name, path
    if d.startswith("/"):
        return d, d
    raise ValueError(f"unknown device {device!r}; use {', '.join(n for n, a in ALIASES.items() if a.attachable)} "
                     "or a /dev path")


# ---------------------------------------------------------------------- open helpers

class RawPort:
    """Minimal serial port on a raw fd, used instead of pyserial's open() for real CP2102 ports.

    Why: pyserial's open() clears DTR and RTS with two separate ioctls after the kernel raised both.
    Between them RTS is high while DTR is low, which is exactly the ESP32 auto-reset pattern
    (EN low), so every pyserial open reset the TTGO boards (verified 2026-11-27: banner + rst).
    Here both lines are cleared with ONE TIOCMBIC right after open; both high -> both low keeps
    EN high. Verified on the sender: no rst, read and write work.

    Offers what SerialLink uses: read(n), write(b), is_open, close(), fileno(), dtr/rts setters."""

    def __init__(self, path: str, baud: int = BAUD, exclusive: bool = True):
        self.port = path
        self._fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            self._set_lines(clear=termios.TIOCM_DTR | termios.TIOCM_RTS)  # one ioctl: no reset pulse
            if exclusive:
                try:
                    fcntl.flock(self._fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except OSError as e:
                    raise PortBusyError(
                        f"{path} is busy (held by another program, e.g. pio device monitor, another MCP "
                        "server or a script); close it there first") from e
            a = termios.tcgetattr(self._fd)
            a[0] = 0                                                   # iflag: raw
            a[1] = 0                                                   # oflag
            a[2] = (termios.CS8 | termios.CREAD | termios.CLOCAL) & ~termios.HUPCL
            a[3] = 0                                                   # lflag: no echo, no canon
            speed = getattr(termios, f"B{baud}", termios.B115200)
            a[4] = a[5] = speed
            a[6][termios.VMIN] = 0
            a[6][termios.VTIME] = 0
            termios.tcsetattr(self._fd, termios.TCSANOW, a)
            self._set_lines(clear=termios.TIOCM_DTR | termios.TIOCM_RTS)
        except BaseException:
            os.close(self._fd)
            raise
        self._dtr = False
        self._rts = False
        self.is_open = True

    def _set_lines(self, set_: int = 0, clear: int = 0) -> None:
        try:
            if clear:
                fcntl.ioctl(self._fd, termios.TIOCMBIC, struct.pack("I", clear))
            if set_:
                fcntl.ioctl(self._fd, termios.TIOCMBIS, struct.pack("I", set_))
        except OSError:
            pass  # a pty has no modem lines

    @property
    def dtr(self) -> bool:
        return self._dtr

    @dtr.setter
    def dtr(self, on: bool) -> None:
        self._dtr = bool(on)
        self._set_lines(set_=termios.TIOCM_DTR if on else 0, clear=0 if on else termios.TIOCM_DTR)

    @property
    def rts(self) -> bool:
        return self._rts

    @rts.setter
    def rts(self, on: bool) -> None:
        self._rts = bool(on)
        self._set_lines(set_=termios.TIOCM_RTS if on else 0, clear=0 if on else termios.TIOCM_RTS)

    def fileno(self) -> int:
        return self._fd

    def read(self, n: int = 1) -> bytes:
        if not self.is_open:
            raise serial.SerialException("port not open")
        r, _, _ = select.select([self._fd], [], [], 0.1)
        if not r:
            return b""
        try:
            data = os.read(self._fd, n)
        except BlockingIOError:
            return b""
        if not data:  # readable but nothing: the USB device went away (same text as pyserial)
            raise serial.SerialException(
                "device reports readiness to read but returned no data "
                "(device disconnected or multiple access on port?)")
        return data

    def write(self, data: bytes) -> int:
        if not self.is_open:
            raise serial.SerialException("port not open")
        view = memoryview(data); sent = 0; deadline = time.monotonic() + 2.0
        while sent < len(view):
            try:
                sent += os.write(self._fd, view[sent:])
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise serial.SerialTimeoutException("write timeout")
                select.select([], [self._fd], [], 0.1)
        return sent

    def reset_input_buffer(self) -> None:
        termios.tcflush(self._fd, termios.TCIFLUSH)

    def flush(self) -> None:
        """Wait until written data left the tty (pyserial compatible)."""
        try:
            termios.tcdrain(self._fd)
        except termios.error:
            pass

    def close(self) -> None:
        if self.is_open:
            self.is_open = False
            try:
                os.close(self._fd)
            except OSError:
                pass


def open_no_reset(path: str, baud: int = BAUD, exclusive: bool = True) -> RawPort:
    """Open without toggling EN/IO0 (see RawPort for why pyserial's open() is not used)."""
    port = RawPort(path, baud, exclusive)
    try:
        fcntl.ioctl(port.fileno(), termios.TIOCEXCL)
    except OSError as e:
        logger.info("TIOCEXCL not supported on %s (%s); flock only", path, e)
    return port


def close_port(ser: serial.Serial | None) -> None:
    if ser is None:
        return
    try:
        fcntl.ioctl(ser.fileno(), termios.TIOCNXCL)
    except Exception:
        pass
    try:
        ser.close()
    except Exception as e:  # pragma: no cover
        logger.info("close failed: %s", e)


def pulse_reset(ser: serial.Serial, hold_s: float = 0.1) -> None:
    """EN low via RTS while DTR stays False (IO0 high => normal boot, not download mode)."""
    ser.dtr = False
    ser.rts = True
    time.sleep(hold_s)
    ser.rts = False


# ---------------------------------------------------------------------- the link

@dataclass
class Line:
    seq: int
    mono: float
    wall: float
    text: str


class SerialLink:
    """One attached port: background reader into a ring buffer of decoded lines."""

    def __init__(self, label: str, path: str, opener: Callable[[str], serial.Serial] = open_no_reset,
                 ring_lines: int = RING_LINES):
        self.label = label
        self.path = path
        self._opener = opener
        self.ring: collections.deque[Line] = collections.deque(maxlen=ring_lines)
        self._cond = threading.Condition()
        self._seq = 0
        self._partial = b""
        self._running = False
        self._thread: threading.Thread | None = None
        self._ser_lock = threading.Lock()  # guards self.ser swaps vs writes/resets
        self.ser: serial.Serial | None = None
        self.attached_at = 0.0
        self.read_errors = 0
        self.reopens = 0
        self.last_error = ""
        self.last_error_at = 0.0
        self.port_lost = False
        self.bytes_rx = 0
        self.resets_by_tool = 0

    # -- lifecycle
    def open(self, reset: bool = False) -> None:
        self.ser = self._opener(self.path)
        self.attached_at = time.monotonic()
        self._running = True
        self._thread = threading.Thread(target=self._reader, name=f"reader-{self.label}", daemon=True)
        self._thread.start()
        if reset:
            self.reset()

    def close(self) -> None:
        self._running = False
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=2.0)
        with self._ser_lock:
            close_port(self.ser)
            self.ser = None
        with self._cond:
            self._cond.notify_all()

    @property
    def alive(self) -> bool:
        return self._running and self._thread is not None and self._thread.is_alive()

    # -- buffer
    def _add(self, text: str) -> None:
        with self._cond:
            self._seq += 1
            self.ring.append(Line(self._seq, time.monotonic(), time.time(), text))
            self._cond.notify_all()

    def note(self, text: str) -> None:
        """Insert a synthetic line (prefixed [mcp]) so the log shows what the tool did."""
        self._add(f"[mcp] {text}")

    @property
    def seq(self) -> int:
        with self._cond:
            return self._seq

    def lines_after(self, seq: int) -> list[Line]:
        with self._cond:
            return [ln for ln in self.ring if ln.seq > seq]

    def snapshot(self) -> list[Line]:
        with self._cond:
            return list(self.ring)

    def wait_for(self, pattern: re.Pattern | None, after_seq: int, timeout_s: float) -> Line | None:
        """Block until a line newer than after_seq matches pattern (None: any real line)."""
        deadline = time.monotonic() + timeout_s
        checked = after_seq
        with self._cond:
            while True:
                for ln in self.ring:
                    if ln.seq <= checked:
                        continue
                    checked = ln.seq
                    if pattern is None:
                        if not ln.text.startswith("[mcp]"):
                            return ln
                    elif pattern.search(ln.text):
                        return ln
                left = deadline - time.monotonic()
                if left <= 0 or not self._running:
                    return None
                self._cond.wait(min(left, 0.2))

    # -- I/O
    def _reader(self) -> None:
        delay = REOPEN_DELAY_S
        while self._running:
            ser = self.ser
            try:
                if ser is None or not ser.is_open:
                    raise serial.SerialException("port not open")
                data = ser.read(4096)
                if data:
                    self.bytes_rx += len(data)
                    self._feed(data)
                    delay = REOPEN_DELAY_S
                    self.port_lost = False
            except (serial.SerialException, OSError, TypeError) as e:
                # Transient CP2102 hiccup ("device reports readiness to read but returned no data")
                # or a USB re-enumeration: reopen without reset after a pause.
                if not self._running:
                    break
                self.read_errors += 1
                self.last_error = str(e)
                self.last_error_at = time.monotonic()
                logger.info("[%s] read error #%d: %s", self.label, self.read_errors, e)
                with self._ser_lock:
                    close_port(self.ser)
                    self.ser = None
                time.sleep(delay)
                if not self._running:
                    break
                try:
                    new = self._opener(self.path)
                except Exception as e2:
                    self.port_lost = True
                    self.last_error = f"reopen failed: {e2}"
                    delay = min(delay * 2, MAX_REOPEN_DELAY_S)
                    continue
                with self._ser_lock:
                    self.ser = new
                self.reopens += 1
                self.port_lost = False
                self.note(f"read error ({e}); reopened {self.path} without reset (#{self.reopens})")
        if self._partial:
            self._flush_partial()

    def _feed(self, data: bytes) -> None:
        buf = self._partial + data
        *complete, self._partial = buf.split(b"\n")
        for raw in complete:
            self._add(raw.rstrip(b"\r").decode("utf-8", errors="replace"))
        if len(self._partial) > 4096:  # binary garbage without newlines
            self._flush_partial()

    def _flush_partial(self) -> None:
        raw, self._partial = self._partial, b""
        self._add(raw.rstrip(b"\r").decode("utf-8", errors="replace"))

    def write_line(self, text: str) -> None:
        with self._ser_lock:
            if self.ser is None:
                raise RuntimeError(f"{self.label}: port currently not open (reconnecting?)")
            self.ser.write(text.rstrip("\r\n").encode() + b"\n")
            self.ser.flush()

    def reset(self) -> None:
        with self._ser_lock:
            if self.ser is None:
                raise RuntimeError(f"{self.label}: port currently not open")
            self.note("reset (RTS pulse, DTR low)")
            pulse_reset(self.ser)
        self.resets_by_tool += 1


# ---------------------------------------------------------------------- formatting

def fmt_lines(lines: Iterable[Line], now: float | None = None) -> str:
    now = time.monotonic() if now is None else now
    return "\n".join(f"[-{max(0.0, now - ln.mono):7.2f}s] {ln.text}" for ln in lines)


def select_lines(lines: list[Line], n: int, since_s: float | None, grep: str | None,
                 now: float | None = None) -> list[Line]:
    now = time.monotonic() if now is None else now
    out = lines
    if since_s is not None:
        out = [ln for ln in out if now - ln.mono <= since_s]
    if grep:
        rx = re.compile(grep)
        out = [ln for ln in out if rx.search(ln.text)]
    return out[-n:] if n > 0 else out


# ---------------------------------------------------------------------- health

RE_RST = re.compile(r"rst:0x([0-9a-f]+)\s*\(([^)]*)\)", re.I)
RE_BANNER = re.compile(r"WEBASTO LORA (RECEIVER|SENDER)|^ets [A-Z][a-z]{2} ")
RE_CRASH = re.compile(
    r"Guru Meditation|Backtrace:|abort\(\) was called|assert failed|Stack canary|stack overflow|CORRUPT HEAP|"
    r"Brownout detector|panic'ed|LoadProhibited|StoreProhibited|InstrFetchProhibited|IllegalInstruction",
    re.I,
)
RE_STACK = re.compile(r"\[rt\] stack (\S+): min free (\d+) of (\d+)")
RE_TASK_FAIL = re.compile(r"\[rt\] task (\S+) FAILED")
RE_ERROR = re.compile(r"\berror\b|\bfail(ed|ure)?\b|^E \(\d+\)|\[E\]|timeout|rc=-", re.I)
WIFI_UP = re.compile(r"\[WiFi\] (Connected to|Event: Got IP)")
WIFI_DOWN = re.compile(r"\[WiFi\] (Connection lost|Disconnected|Manual disconnect|No network configured)")
MQTT_UP = re.compile(r"\[MQTT\] Connected to broker")
MQTT_DOWN = re.compile(r"\[MQTT\] (Connection failed|Disconnected|Connection lost)")


def _link_state(lines: list[Line], up: re.Pattern, down: re.Pattern, now: float) -> dict[str, Any]:
    last = None
    for ln in lines:
        if up.search(ln.text):
            last = ("connected", ln)
        elif down.search(ln.text):
            last = ("disconnected", ln)
    if last is None:
        return {"state": "unknown (no line in buffer)"}
    return {"state": last[0], "line": last[1].text, "age_s": round(now - last[1].mono, 1)}


def analyze(lines: list[Line], window_s: float, now: float | None = None) -> dict[str, Any]:
    """Pure analysis of buffered lines. Link states and stack watermarks use the whole buffer
    (last known value); counts use only the window."""
    now = time.monotonic() if now is None else now
    win = [ln for ln in lines if now - ln.mono <= window_s and not ln.text.startswith("[mcp]")]
    rst = [ln for ln in win if RE_RST.search(ln.text)]
    banners = [ln for ln in win if RE_BANNER.search(ln.text)]
    crashes = [ln for ln in win if RE_CRASH.search(ln.text)]
    stacks: dict[str, dict[str, Any]] = {}
    for ln in lines:
        m = RE_STACK.search(ln.text)
        if m:
            free, total = int(m.group(2)), int(m.group(3))
            stacks[m.group(1)] = {"min_free": free, "of": total, "used_pct": round(100 * (1 - free / total), 1)
                                  if total else None, "age_s": round(now - ln.mono, 1)}
    task_fail = sorted({RE_TASK_FAIL.search(ln.text).group(1) for ln in win if RE_TASK_FAIL.search(ln.text)})
    errors = [ln for ln in win if RE_ERROR.search(ln.text) and not RE_CRASH.search(ln.text)]
    return {
        "window_s": window_s,
        "lines_in_window": len(win),
        "boots": len(rst) or len(banners),
        "reset_reasons": [m.group(0) for m in (RE_RST.search(ln.text) for ln in rst)],
        "boot_banners": len(banners),
        "crash_lines": [ln.text for ln in crashes[-10:]],
        "crash_count": len(crashes),
        "wifi": _link_state(lines, WIFI_UP, WIFI_DOWN, now),
        "mqtt": _link_state(lines, MQTT_UP, MQTT_DOWN, now),
        "stack_watermarks": stacks,
        "low_stack_tasks": [t for t, s in stacks.items() if s["min_free"] < 512],
        "failed_tasks": task_fail,
        "last_errors": [f"-{now - ln.mono:.1f}s {ln.text}" for ln in errors[-10:]],
        "last_line_age_s": round(now - lines[-1].mono, 1) if lines else None,
    }


# ---------------------------------------------------------------------- manager

class SerialManager:
    def __init__(self, opener: Callable[[str], serial.Serial] = open_no_reset):
        self.links: dict[str, SerialLink] = {}  # key: realpath
        self._lock = threading.Lock()
        self._opener = opener

    def _find(self, device: str) -> SerialLink | None:
        d = device.strip()
        key = ALIAS_SYNONYMS.get(d.lower(), d.lower())
        for link in self.links.values():
            if link.label == key or link.label == d or link.path == d:
                return link
        if d.startswith("/"):
            return self.links.get(os.path.realpath(d))
        return None

    def attach(self, device: str, reset: bool = False) -> tuple[SerialLink, bool]:
        """Returns (link, newly_opened)."""
        with self._lock:
            existing = self._find(device)
            if existing:
                if reset:
                    existing.reset()
                return existing, False
            label, path = resolve_device(device)
            real = os.path.realpath(path)
            if real in self.links:
                return self.links[real], False
            link = SerialLink(label, path, opener=self._opener)
            link.open(reset=reset)
            self.links[real] = link
            return link, True

    def get(self, device: str, auto_attach: bool = True) -> SerialLink:
        link = self._find(device)
        if link:
            return link
        if not auto_attach:
            raise ValueError(f"{device} is not attached; call attach first")
        return self.attach(device)[0]

    def detach(self, device: str) -> str:
        with self._lock:
            link = self._find(device)
            if not link:
                return f"{device}: not attached"
            link.close()
            self.links = {k: v for k, v in self.links.items() if v is not link}
            return f"{link.label}: released {link.path}"

    def detach_all(self) -> list[str]:
        with self._lock:
            out = []
            for link in list(self.links.values()):
                link.close()
                out.append(f"{link.label}: released {link.path}")
            self.links.clear()
            return out or ["nothing attached"]

    def held_realpaths(self) -> set[str]:
        return set(self.links.keys())

    def list_devices(self) -> dict[str, Any]:
        usb = _usb_info()
        held = self.held_realpaths()
        seen = set()
        known = []
        for a in ALIASES.values():
            path = resolve_alias(a, usb)
            real = os.path.realpath(path) if path else None
            info = usb.get(real) if real else None
            if real:
                seen.add(real)
            entry = {
                "alias": a.name,
                "present": path is not None,
                "path": path,
                "real_port": real,
                "usb_path": getattr(info, "location", None),
                "vid_pid": f"{info.vid:04x}:{info.pid:04x}" if info is not None and info.vid is not None else None,
                "held_by_this_server": real in held if real else False,
                "description": a.description,
            }
            if a.mac:
                entry["mac"] = a.mac
                entry["ble_name"] = a.ble_name
            if a.note:
                entry["note"] = a.note
            if path and path != a.paths[0]:
                entry["note"] = f"{a.paths[0]} missing, using fallback {path}"
            known.append(entry)
        others = []
        for p in sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")):
            real = os.path.realpath(p)
            if real in seen:
                continue
            info = usb.get(real)
            others.append({
                "path": p,
                "usb_path": getattr(info, "location", None),
                "vid_pid": f"{info.vid:04x}:{info.pid:04x}" if info is not None and info.vid is not None else None,
                "description": getattr(info, "description", None),
                "held_by_this_server": real in held,
            })
        return {"known": known, "other_ports": others}
