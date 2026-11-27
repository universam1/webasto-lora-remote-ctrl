"""Serial side against a fake ESP32 on a pseudo-terminal. Never opens a real /dev/tty*."""

import asyncio
import os
import pty
import re
import threading
import time
import tty

import pytest
import serial

from webasto_serial_debug import serial_link as SL
from webasto_serial_debug import server


@pytest.fixture(autouse=True)
def _only_ptys(monkeypatch):
    """Hard guard: any attempt to open something other than a pty fails the test."""
    orig = serial.Serial.open

    def guarded(self):
        if not str(self.port).startswith("/dev/pts/"):
            raise AssertionError(f"test tried to open a real port: {self.port}")
        return orig(self)

    monkeypatch.setattr(serial.Serial, "open", guarded)
    yield
    server.serial_mgr.detach_all()


class FakeEsp(threading.Thread):
    """Prints like the sender firmware, answers commands, prints a boot log on 'reset'."""

    def __init__(self):
        super().__init__(daemon=True)
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        self.port = os.ttyname(slave)
        self._slave = slave  # keep the slave open so the pty stays alive between reopens
        self.received = []
        self.running = True
        self._buf = b""

    def emit(self, *lines):
        os.write(self.master, b"".join(l.encode() + b"\r\n" for l in lines))

    def boot(self):
        self.emit("ets Jun  8 2016 00:22:57", "", "rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)",
                  "WEBASTO LORA RECEIVER (Phase D)", "[WiFi] Connecting to home (PSK)")

    def run(self):
        import select

        while self.running:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if not r:
                continue
            try:
                data = os.read(self.master, 1024)
            except OSError:
                return
            self._buf += data
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                cmd = line.decode().strip()
                self.received.append(cmd)
                if cmd == "query":
                    self.emit("[JOB] start QUERY seq=1", "[LORA] send: 01 05", "[LORA] recv: 81 05 01")
                    time.sleep(0.3)
                    self.emit("[JOB] done seq=1 after 7 tx: ok")

    def stop(self):
        self.running = False
        self.join(1)  # before closing: a reused fd number would otherwise be read by this thread
        for fd in (self.master, self._slave):
            try:
                os.close(fd)
            except OSError:
                pass


@pytest.fixture
def esp():
    d = FakeEsp()
    d.start()
    yield d
    d.stop()


class ResetSpy:
    """Wraps a pyserial object: records dtr/rts at open time and on writes, emulates the EN reset."""

    def __init__(self, esp, fail_reads=0):
        self.esp = esp
        self.opens = []
        self.rts_log = []
        self.fail_reads = fail_reads

    def opener(self, path):
        real = serial.Serial()
        real.port, real.baudrate, real.timeout = path, 115200, 0.1
        real.dtr = False
        real.rts = False
        self.opens.append((real.dtr, real.rts))  # values pyserial applies during open()
        real.open()
        spy = self

        class Wrapped:
            def __getattr__(self, n):
                return getattr(real, n)

            def read(self, n):
                if spy.fail_reads > 0:
                    spy.fail_reads -= 1
                    time.sleep(0.05)
                    raise serial.SerialException(
                        "device reports readiness to read but returned no data "
                        "(device disconnected or multiple access on port?)")
                return real.read(n)

            def __setattr__(self, n, v):
                if n in ("dtr", "rts"):
                    spy.rts_log.append((n, v))
                    if n == "rts" and v is False and ("rts", True) in spy.rts_log[-2:]:
                        spy.esp.boot()  # EN released: the board boots
                    return
                setattr(real, n, v)

        return Wrapped()


def test_open_no_reset_on_pty(esp):
    ser = SL.open_no_reset(esp.port)
    try:
        assert ser.dtr is False and ser.rts is False
        esp.emit("hello")
        time.sleep(0.1)
        assert b"hello" in ser.read(100)
    finally:
        SL.close_port(ser)


def test_attach_without_reset_and_read_log(esp):
    spy = ResetSpy(esp)
    mgr = SL.SerialManager(opener=spy.opener)
    link, new = mgr.attach(esp.port)
    try:
        assert new and spy.opens == [(False, False)]
        assert ("rts", True) not in spy.rts_log
        esp.emit("[WiFi] Connected to home, IP 10.0.0.5, RSSI -60 dBm", "[MQTT] Connected to broker")
        assert link.wait_for(re.compile("MQTT"), 0, 3)
        old = link.snapshot()[0]
        old.mono -= 100  # pretend the WiFi line is old
        lines = SL.select_lines(link.snapshot(), 50, since_s=10, grep=None)
        assert [l.text for l in lines] == ["[MQTT] Connected to broker"]
        lines = SL.select_lines(link.snapshot(), 50, since_s=None, grep=r"\[WiFi\]")
        assert len(lines) == 1 and "IP 10.0.0.5" in lines[0].text
        assert "[-" in SL.fmt_lines(lines)
        # attaching again returns the same link
        assert mgr.attach(esp.port) == (link, False)
    finally:
        mgr.detach_all()


def test_partial_lines_are_joined(esp):
    mgr = SL.SerialManager()
    link, _ = mgr.attach(esp.port)
    try:
        os.write(esp.master, b"[LOOP] status pha")
        time.sleep(0.25)
        os.write(esp.master, b"se=0 lastCmdSeq=1 job=1\r\n")
        assert link.wait_for(re.compile("job=1"), 0, 3).text == "[LOOP] status phase=0 lastCmdSeq=1 job=1"
    finally:
        mgr.detach_all()


def test_send_command_wait_for_via_tool(esp):
    out = asyncio.run(server.send_command(esp.port, "query", wait_for=r"\[JOB\] done", timeout_s=5))
    assert "matched: [JOB] done seq=1 after 7 tx: ok" in out
    assert "[LORA] recv: 81 05 01" in out and "[JOB] start QUERY" in out
    assert esp.received == ["query"]
    out = asyncio.run(server.send_command(esp.port, "stop", wait_for="never", timeout_s=0.5))
    assert "TIMEOUT" in out
    out = asyncio.run(server.read_log(esp.port, lines=2, grep="JOB"))
    assert "2 of" in out and "[JOB] done" in out
    assert "Error" in asyncio.run(server.read_log(esp.port, grep="("))
    listing = asyncio.run(server.detach(esp.port))
    assert "released" in listing


def test_reset_pulses_rts_with_dtr_low_and_captures_boot(esp):
    spy = ResetSpy(esp)
    mgr = SL.SerialManager(opener=spy.opener)
    link, _ = mgr.attach(esp.port)
    try:
        start = link.seq
        link.reset()
        assert spy.rts_log[-3:] == [("dtr", False), ("rts", True), ("rts", False)]
        hit = link.wait_for(re.compile("WEBASTO LORA RECEIVER"), start, 3)
        assert hit is not None
        texts = [l.text for l in link.lines_after(start)]
        assert texts[0].startswith("[mcp] reset")
    finally:
        mgr.detach_all()


def test_attach_with_reset(esp):
    spy = ResetSpy(esp)
    mgr = SL.SerialManager(opener=spy.opener)
    link, _ = mgr.attach(esp.port, reset=True)
    try:
        assert link.wait_for(re.compile("rst:0x1"), 0, 3)
        assert link.resets_by_tool == 1
    finally:
        mgr.detach_all()


def test_health_counts(esp):
    mgr = SL.SerialManager()
    link, _ = mgr.attach(esp.port)
    try:
        esp.boot()
        esp.emit(
            "[WiFi] Connected to home, IP 10.0.0.5, RSSI -60 dBm",
            "[MQTT] Connection failed, rc=-2, retry in 5000 ms",
            "[rt] stack lora: min free 1200 of 4096",
            "[rt] stack ble: min free 300 of 6144",
            "Guru Meditation Error: Core  1 panic'ed (LoadProhibited). Exception was unhandled.",
            "Backtrace: 0x400d1234:0x3ffb1f20 0x400d5678:0x3ffb1f40",
        )
        esp.boot()
        esp.emit("[rt] stack lora: min free 1100 of 4096", "[MQTT] Connected to broker", "[WiFi] Connection lost")
        assert link.wait_for(re.compile("Connection lost"), 0, 3)
        h = SL.analyze(link.snapshot(), 60)
        assert h["boots"] == 2 and len(h["reset_reasons"]) == 2
        assert h["crash_count"] == 2 and "Guru Meditation" in h["crash_lines"][0]
        assert h["wifi"]["state"] == "disconnected" and h["mqtt"]["state"] == "connected"
        assert h["stack_watermarks"]["lora"]["min_free"] == 1100
        assert h["stack_watermarks"]["ble"]["of"] == 6144
        assert h["low_stack_tasks"] == ["ble"]
        assert any("Connection failed" in e for e in h["last_errors"])
        # outside the window nothing is counted, but the last known states remain
        later = time.monotonic() + 1000
        h2 = SL.analyze(link.snapshot(), 60, now=later)
        assert h2["boots"] == 0 and h2["crash_count"] == 0 and h2["mqtt"]["state"] == "connected"
    finally:
        mgr.detach_all()


def test_recovers_after_read_error(esp, monkeypatch):
    monkeypatch.setattr(SL, "REOPEN_DELAY_S", 0.05)
    spy = ResetSpy(esp, fail_reads=2)
    mgr = SL.SerialManager(opener=spy.opener)
    link, _ = mgr.attach(esp.port)
    try:
        deadline = time.monotonic() + 3
        while link.reopens < 2 and time.monotonic() < deadline:
            time.sleep(0.05)
        assert link.read_errors == 2 and link.reopens == 2
        assert all(o == (False, False) for o in spy.opens) and len(spy.opens) == 3
        assert ("rts", True) not in spy.rts_log  # reopening never resets the board
        esp.emit("still alive")
        assert link.wait_for(re.compile("still alive"), 0, 3)
        assert "readiness to read" in link.last_error
        assert any("reopened" in l.text for l in link.snapshot())
        h = SL.analyze(link.snapshot(), 60)
        assert h["boots"] == 0  # [mcp] notes are not device output
    finally:
        mgr.detach_all()


def test_health_tool_reports_read_errors(esp, monkeypatch):
    monkeypatch.setattr(SL, "REOPEN_DELAY_S", 0.05)
    spy = ResetSpy(esp, fail_reads=1)
    monkeypatch.setattr(server, "serial_mgr", SL.SerialManager(opener=spy.opener))
    asyncio.run(server.attach(esp.port))
    time.sleep(0.3)
    import json

    h = json.loads(asyncio.run(server.health(esp.port)))
    assert h["serial"]["read_errors"] == 1 and h["serial"]["reopens"] == 1
    server.serial_mgr.detach_all()


def test_resolve_device_and_listing(monkeypatch):
    with pytest.raises(ValueError, match="webasto-can-capture"):
        SL.resolve_device("cansniffer")
    with pytest.raises(ValueError, match="unknown device"):
        SL.resolve_device("toaster")
    monkeypatch.setattr(SL.os.path, "exists", lambda p: p in ("/dev/ttyUSB0",))
    monkeypatch.setattr(SL.os.path, "realpath", lambda p: p)

    class Info:
        vid, pid, location, description = 0x10C4, 0xEA60, "1-1.3:1.0", "CP2102"

    usb = {"/dev/ttyUSB0": Info()}
    assert SL.resolve_alias(SL.ALIASES["sender"], usb) == "/dev/ttyUSB0"
    assert SL.resolve_alias(SL.ALIASES["receiver"], usb) is None
    Info.vid = 0x303A
    assert SL.resolve_alias(SL.ALIASES["sender"], usb) is None


def test_list_devices_does_not_open():
    # the autouse guard fails the test if any port is opened
    d = SL.SerialManager().list_devices()
    assert {k["alias"] for k in d["known"]} == {"receiver", "sender", "simulator", "cansniffer"}
