"""End-to-end test of CanSniffer against a fake GVRET device on a pseudo-terminal."""

import os
import pty
import struct
import threading
import time
import tty

import pytest

from webasto_can_capture import analysis, server


class FakeDevice(threading.Thread):
    def __init__(self, tx_allowed=True):
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
        self.lock = threading.Lock()

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
                    self.received_tx.append(bytes(buf[:n]))
                    self.tx_mode_at_send.append(self.tx_on)
                    del buf[:n]
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
                data = bytes([0x04, 0x05, 0, 0, 0, 0xA0, 1, 0])
                self.send(b"\xF1\x00" + struct.pack("<IIB", self.ts, 0x3E5, 8) + data + b"\x00")

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
