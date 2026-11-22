"""End-to-end test of CanSniffer against a fake GVRET device on a pseudo-terminal."""

import os
import pty
import struct
import threading
import time
import tty

import pytest

from webasto_can_capture import analysis, server


class FakePanel:
    """Roof control unit: sends 0x5D3 [byte, 00] every `period_s` and adopts our 0x5D3 value
    after `adopt_after` received frames with that value, but only if bit 7 matches its current
    byte and `accept(value)` allows it. With `asleep=True` it stays silent until the first
    received 0x5D3 (bus wake-up)."""

    def __init__(self, byte=0x80, period_s=0.1, adopt_after=3, accept=None, asleep=False):
        self.byte = byte
        self.period_s = period_s
        self.adopt_after = adopt_after
        self.accept = accept or (lambda v: True)
        self.asleep = asleep
        self.counts = {}
        self.last_sent = 0.0
        self.sent = []  # bytes 0 of the panel frames sent

    def on_tx(self, value):
        """Called for each received TX 0x5D3; returns True if the panel adopted the value now."""
        self.asleep = False
        if value == self.byte or (value & 0x80) != (self.byte & 0x80) or not self.accept(value):
            return False
        self.counts[value] = self.counts.get(value, 0) + 1
        if self.counts[value] >= self.adopt_after:
            self.byte = value
            self.counts.clear()
            return True
        return False


class FakeDevice(threading.Thread):
    def __init__(self, tx_allowed=True, panel=None):
        super().__init__(daemon=True)
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        self.port = os.ttyname(slave)
        self._slave = slave
        self.running = True
        self.binary = False
        self.tx_allowed = tx_allowed
        self.tx_on = False
        self.received_tx = []
        self.tx_mode_at_send = []
        self.bus_cfg = None
        self.ts = 1000
        # (can_id, payload) sent every 10 ms once in binary mode; tests may replace it
        self.frames = [(0x3E5, bytes([0x04, 0x05, 0, 0, 0, 0xA0, 1, 0]))]
        self.lock = threading.Lock()
        self.panel = panel

    def send_frame(self, can_id, data):
        hdr = struct.pack("<IIB", self.ts, can_id, len(data))
        self.send(b"\xF1\x00" + hdr + data + b"\x00")

    def panel_tick(self, force=False):
        p = self.panel
        if p is None or p.asleep or not self.binary:
            return
        if force or time.time() - p.last_sent >= p.period_s:
            p.last_sent = time.time()
            p.sent.append(p.byte)
            self.send_frame(0x5D3, bytes([p.byte, 0]))

    def send(self, b):
        with self.lock:
            os.write(self.master, b)

    def status(self):
        flags = (1 if self.tx_allowed else 0) | (2 if self.tx_on else 0)
        return b"\xF1\x40" + struct.pack("<BIIIB", flags, 42, 1, 0, 1)

    def run(self):
        self.send(b"boot text: CAN sniffer v1\r\n")
        buf = bytearray()
        last_frame = time.time()
        os.set_blocking(self.master, False)
        while self.running:
            try:
                buf += os.read(self.master, 4096)
            except (BlockingIOError, OSError):
                time.sleep(0.005)
            while buf:
                if buf[:2] == b"\xE7\xE7":
                    self.binary = True
                    del buf[:2]
                    continue
                if buf[0] != 0xF1:
                    del buf[0]
                    continue
                if len(buf) < 2:
                    break
                c = buf[1]
                if c == 0x05:
                    if len(buf) < 10:
                        break
                    self.bus_cfg = struct.unpack("<II", buf[2:10])
                    del buf[:10]
                elif c == 0x41:
                    if len(buf) < 3:
                        break
                    self.tx_on = bool(buf[2]) and self.tx_allowed
                    del buf[:3]
                    self.send(self.status())
                elif c == 0x00:
                    if len(buf) < 8 or len(buf) < 8 + buf[7]:
                        break
                    n = 8 + buf[7]
                    pkt = bytes(buf[:n])
                    self.received_tx.append(pkt)
                    self.tx_mode_at_send.append(self.tx_on)
                    del buf[:n]
                    can_id = struct.unpack("<I", pkt[2:6])[0]
                    if self.panel and self.tx_on and can_id == 0x5D3 and n > 8:
                        woke = self.panel.asleep
                        if self.panel.on_tx(pkt[8]):
                            self.panel_tick(force=True)  # the panel sends the adopted value itself
                        elif woke:
                            self.panel_tick(force=True)  # bus wakes: panel's first frame follows
                else:
                    del buf[:2]
                    if c == 0x01:
                        self.send(b"\xF1\x01" + struct.pack("<I", self.ts))
                    elif c == 0x06:
                        v0 = self.bus_cfg[0] if self.bus_cfg else 0
                        # like the firmware: listen-only unless TX is switched on
                        flags = (1 if v0 & 0x40000000 else 0) | (0 if self.tx_on else 0x10)
                        self.send(b"\xF1\x06" + struct.pack("<BIBI", flags, v0 & 0x0FFFFFFF, 0, 0))
                    elif c == 0x07:
                        self.send(b"\xF1\x07" + struct.pack("<HBBBB", 618, 1, 0, 0, 0))
                    elif c == 0x0C:
                        self.send(b"\xF1\x0C\x01")
                    elif c == 0x40:
                        self.send(self.status())
            if self.binary and time.time() - last_frame > 0.01:
                last_frame = time.time()
                self.ts += 10_000
                for can_id, data in list(self.frames):
                    self.send_frame(can_id, data)
            self.panel_tick()

    def stop(self):
        self.running = False
        self.join(timeout=1)
        os.close(self.master)
        os.close(self._slave)


@pytest.fixture
def env(tmp_path, monkeypatch):
    monkeypatch.setattr(server, "CAPTURES_DIR", tmp_path)
    monkeypatch.setattr(server.time, "sleep", _fast_sleep)
    dev = FakeDevice()
    dev.start()
    sn = server.CanSniffer()
    yield sn, dev, tmp_path
    sn.disconnect()
    dev.stop()


_real_sleep = time.sleep


def _fast_sleep(s):
    _real_sleep(min(s, 0.2))


def test_connect_capture_mark_and_tx(env):
    sn, dev, d = env
    out = sn.connect(dev.port, 100000)
    assert "build=618" in out and "100000 bit/s, listen-only" in out and "state=running" in out
    assert dev.bus_cfg[0] == 100000 | 0xE0000000
    sn.start_capture("t1")
    _real_sleep(0.3)
    assert "marker 'heater on'" in sn.mark_event("heater on")
    _real_sleep(0.2)

    with pytest.raises(PermissionError):
        sn.send_frame(0x3E5, b"\x01", False, 1, 0)
    armed = sn.enable_tx(True)
    assert "TX armed" in armed and "listen-only" in armed
    assert not dev.tx_on  # arming alone keeps the device listen-only
    res = sn.send_frame(0x123, b"\xAA\xBB", False, 2, 10)
    assert "sent 2x 123#AABB" in res
    assert "after burst: bus params: can0: enabled, 100000 bit/s, listen-only" in res
    _real_sleep(0.1)
    assert dev.received_tx[0] == b"\xF1\x00\x23\x01\x00\x00\x00\x02\xAA\xBB"
    assert dev.tx_mode_at_send == [True, True]  # normal mode during the burst
    assert not dev.tx_on  # back to listen-only afterwards

    assert "3E5" in sn.tail(3, "3e5")
    assert "distinct IDs" in sn.stats(1.0)
    res = sn.stop_capture()
    assert "2 Tx frames" in res and "1 markers" in res

    csv_lines = (d / "t1.csv").read_text().splitlines()
    assert csv_lines[0] == analysis.CSV_HEADER
    assert any(",Tx,0,2,AA,BB,00," in l for l in csv_lines)
    assert all(l.endswith(",") for l in csv_lines[1:])
    assert (d / "t1.log").read_text().splitlines()[0].endswith("can0 3E5#0405000000A00100")
    cap = analysis.load_capture("t1", d)
    assert cap.markers[0].label == "heater on"
    assert "heating_on=1" in analysis.decode_3e5(cap)


def test_tx_refused_when_firmware_disallows(env):
    sn, dev, _ = env
    dev.tx_allowed = False
    sn.connect(dev.port, 100000)
    assert "does not allow TX" in sn.enable_tx(True)
    with pytest.raises(PermissionError):
        sn.send_frame(0x123, b"", False, 1, 0)


def test_mark_event_without_capture(env):
    sn, _, _ = env
    with pytest.raises(RuntimeError):
        sn.mark_event("x")


# ---------------------------------------------------------------------- set_heater_mode


@pytest.fixture
def panel_env(tmp_path, monkeypatch):
    monkeypatch.setattr(server, "CAPTURES_DIR", tmp_path)
    monkeypatch.setattr(server.time, "sleep", _fast_sleep)
    made = []

    def make(panel, frames=None):
        dev = FakeDevice(panel=panel)
        dev.frames = frames if frames is not None else []
        dev.start()
        sn = server.CanSniffer()
        sn.connect(dev.port, 100000)
        made.append((sn, dev))
        return sn, dev

    yield make
    for sn, dev in made:
        sn.disconnect()
        dev.stop()


def _tx_bytes0(dev):
    """Byte 0 of every 0x5D3 frame the sniffer transmitted."""
    return [p[8] for p in dev.received_tx if struct.unpack("<I", p[2:6])[0] == 0x5D3]


def _wait_panel(sn, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        if sn._last_panel_mode():
            return
        _real_sleep(0.02)
    raise AssertionError("no panel 0x5D3 received")


def test_heater_mode_refused_without_enable_tx(panel_env):
    sn, dev = panel_env(FakePanel(byte=0x80))
    _wait_panel(sn)
    with pytest.raises(PermissionError, match="enable_tx"):
        sn.set_heater_mode("heat")
    assert dev.received_tx == [] and not dev.tx_on
    sn.enable_tx(True)
    with pytest.raises(ValueError):
        sn.set_heater_mode("turbo")


def test_heat_adopted_after_three_frames(panel_env):
    panel = FakePanel(byte=0x80, adopt_after=3)
    sn, dev = panel_env(panel)
    _wait_panel(sn)
    sn.enable_tx(True)
    sn.start_capture("heat")
    dev.frames = [(0x3E5, bytes.fromhex("00 08 00 40 00 70 00 00"))]

    def heater_follows():  # 0x3E5 shows the control LED once the panel requests heat
        while dev.running:
            if panel.byte & 0x7F == 0x02:
                dev.frames = [(0x3E5, bytes.fromhex("00 0A 01 40 44 65 02 00"))]
            _real_sleep(0.02)

    threading.Thread(target=heater_follows, daemon=True).start()
    res = sn.set_heater_mode("heat", interval_ms=200)
    assert res["ok"] is True, res
    assert res["frames_sent"] == 3
    assert res["target_byte"] == "0x82" and res["panel_before"] == "0x80" and res["panel_after"] == "0x82"
    assert res["adopted_after_ms"] is not None and res["adopted_after_ms"] >= 0
    assert res["heater_confirmed"] is True
    assert "adopted by roof unit" in res["message"]
    _real_sleep(0.1)
    assert _tx_bytes0(dev) == [0x82] * 3
    assert dev.tx_mode_at_send == [True] * 3
    assert not dev.tx_on  # listen-only afterwards
    bp = sn._request(server.gvret.cmd(server.gvret.CMD_GET_BUS_PARAMS), server.gvret.BusParams)
    assert bp.can0_listen_only
    out = sn.stop_capture()
    assert "3 Tx frames" in out


@pytest.mark.parametrize("panel_byte,expected", [(0x80, 0x82), (0x00, 0x02)])
def test_bit7_mirrored(panel_env, panel_byte, expected):
    sn, dev = panel_env(FakePanel(byte=panel_byte, adopt_after=2))
    _wait_panel(sn)
    sn.enable_tx(True)
    res = sn.set_heater_mode("heat", interval_ms=200)
    assert res["ok"] is True, res
    assert res["target_byte"] == f"0x{expected:02X}"
    assert set(_tx_bytes0(dev)) == {expected}


def test_vent_rejected_reports_not_adopted(panel_env):
    panel = FakePanel(byte=0x80, adopt_after=1, accept=lambda v: v != 0x84)
    sn, dev = panel_env(panel)
    _wait_panel(sn)
    sn.enable_tx(True)
    res = sn.set_heater_mode("vent", max_frames=4, interval_ms=150)
    assert res["ok"] is False, res
    assert res["frames_sent"] == 4 and res["adopted_after_ms"] is None
    assert res["target_byte"] == "0x84" and res["panel_after"] == "0x80"
    assert "not adopted by roof unit" in res["message"]
    _real_sleep(0.1)
    assert _tx_bytes0(dev) == [0x84] * 4
    assert not dev.tx_on


def test_sleeping_bus_uses_wake_frame(panel_env):
    # nothing in the ring; the panel wakes on our first frame and sends 0x00 (bit 7 clear)
    panel = FakePanel(byte=0x00, adopt_after=2, asleep=True)
    sn, dev = panel_env(panel)
    _real_sleep(0.3)
    assert sn._last_panel_mode() is None
    sn.enable_tx(True)
    res = sn.set_heater_mode("heat", interval_ms=200)
    assert res["ok"] is True, res
    tx = _tx_bytes0(dev)
    assert tx[0] == 0x82  # wake frame: default bit 7 = 0x80
    assert tx[1:] == [0x02, 0x02]  # then bit 7 from the panel's first frame
    assert res["target_byte"] == "0x02" and res["panel_before"] == "0x00"
    assert res["frames_sent"] == 3
    assert "Wake frame" in res["message"]
    assert not dev.tx_on


def test_send_frame_unaffected_by_panel(panel_env):
    sn, dev = panel_env(FakePanel(byte=0x80))
    sn.enable_tx(True)
    assert "sent 1x 5D3#8200" in sn.send_frame(0x5D3, b"\x82\x00", False, 1, 0)
