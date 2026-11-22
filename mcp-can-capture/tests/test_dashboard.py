"""Dashboard: 0x3E5 decoding / derived state, and the HTTP API against the pty FakeDevice."""

import json
import threading
import time
import urllib.error
import urllib.request

import pytest

from webasto_can_capture import dashboard, server
from test_server_pty import FakeDevice, _fast_sleep, _real_sleep

OFF = bytes.fromhex("00 00 00 40 00 4C 00 00")
HEATING = bytes.fromhex("00 0A 01 40 44 65 02 00")
RUN_ON = bytes.fromhex("00 08 0A 40 00 7C 02 00")
VENTILATING = bytes.fromhex("00 09 08 40 00 70 02 01")


# ---------------------------------------------------------------------- decoding


@pytest.mark.parametrize(
    "payload,state",
    [(OFF, "off"), (HEATING, "heating"), (RUN_ON, "run_on"), (VENTILATING, "ventilating")],
)
def test_derived_state(payload, state):
    assert dashboard.decode_status(payload)["state"] == state


def test_decode_values():
    off = dashboard.decode_status(OFF)
    assert off["coolant_c"] == 9.0 and off["power_raw"] == 0 and off["fuel_used_ml"] == 0
    assert not off["fuel_pump"] and not off["water_pump"]
    assert off["fields"]["parking_heater"] == 1 and off["fields"]["kl30_lost"] == 0
    assert off["error"] == {"code": 0, "name": "none", "text": "kein Fehler", "stored": False}

    heat = dashboard.decode_status(HEATING)
    assert heat["coolant_c"] == 27.75 and heat["power_raw"] == 0x44 and heat["fuel_used_ml"] == 1
    assert not heat["fuel_pump"] and heat["water_pump"] and heat["fields"]["control_led"] == 1

    run_on = dashboard.decode_status(RUN_ON)
    assert run_on["coolant_c"] == 45.0 and run_on["fuel_used_ml"] == 10 and run_on["water_pump"]

    vent = dashboard.decode_status(VENTILATING)
    assert vent["coolant_c"] == 36.0 and vent["fields"]["vehicle_blower"] == 1


def test_decode_errors_and_coolant_error():
    d = dashboard.decode_status(bytes.fromhex("00 E0 00 00 00 FF 00 00"))
    assert d["error"] == {"code": 6, "name": "overheat", "text": "Überhitzung", "stored": True}
    assert d["coolant_c"] is None and d["coolant_error"]
    assert dashboard.decode_status(bytes([0, 0x10]))["error"]["text"] == "Batteriespannung zu niedrig"


def test_decode_mode_and_active():
    assert dashboard.decode_mode(b"\x02\x00") == "heat"
    assert dashboard.decode_mode(b"\x04\x00") == "vent"
    assert dashboard.decode_mode(b"\x00\x00") == "off"
    assert dashboard.decode_mode(b"\x07\x00") == "unknown(0x07)"
    assert dashboard.decode_active(bytes([0, 0, 0, 0, 1, 0, 0, 0])) is True
    assert dashboard.decode_active(bytes(8)) is False
    assert dashboard.decode_active(b"\x00") is None


# ---------------------------------------------------------------------- HTTP against the fake device


class _Dash:
    def __init__(self, allow_tx, tx_allowed_by_firmware=True):
        self.dev = FakeDevice(tx_allowed=tx_allowed_by_firmware)
        self.dev.frames = [
            (0x3E5, HEATING),
            (0x5D3, b"\x02\x00"),
            (0x3E3, bytes([0, 0, 0, 0, 1, 0, 0, 0])),
        ]
        self.dev.start()
        self.sn = server.CanSniffer()
        self.sn.connect(self.dev.port, 100000)
        self.mon = dashboard.HeaterMonitor(self.sn, allow_tx)
        self.mon.start()
        self.httpd = dashboard.DashboardServer(("127.0.0.1", 0), self.mon)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.httpd.server_port}"

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=5) as r:
            return r.status, r.headers.get("Content-Type"), r.read()

    def state(self):
        return json.loads(self.get("/api/state")[2])

    def post(self, body, headers=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        req = urllib.request.Request(
            self.base + "/api/command",
            data=data,
            headers={"Content-Type": "application/json", **(headers or {})},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.mon.wait_idle(10)
        self.mon.stop()
        self.sn.disconnect()
        self.dev.stop()


@pytest.fixture
def make_dash(monkeypatch):
    monkeypatch.setattr(server.time, "sleep", _fast_sleep)
    made = []

    def make(**kw):
        d = _Dash(**kw)
        made.append(d)
        return d

    yield make
    for d in made:
        d.close()


def _wait(pred, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        _real_sleep(0.05)
    raise AssertionError("condition not met in time")


def test_page_and_state(make_dash):
    d = make_dash(allow_tx=False)
    status, ctype, body = d.get("/")
    assert status == 200 and ctype.startswith("text/html")
    assert b"Heizen" in body and b"L\xc3\xbcften" in body and b"prefers-color-scheme" in body
    assert b"http://" not in body.replace(b"http://www.w3.org/2000/svg", b"")  # no CDN

    s = _wait(lambda: (st := d.state())["heater"] and st["device"] and st["history"]["t"] and st)
    hz = s["heater"]
    assert hz["state"] == "heating" and hz["raw"] == "00 0A 01 40 44 65 02 00"
    assert hz["coolant_c"] == 27.75 and hz["power_raw"] == 0x44 and hz["fuel_used_ml"] == 1
    assert hz["water_pump"] is True and hz["fuel_pump"] is False
    assert hz["error"]["code"] == 0 and hz["error"]["text"] == "kein Fehler"
    assert s["mode_5d3"]["mode"] == "heat"
    assert s["active_3e3"]["active"] is True
    assert s["bus"]["awake"] is True and s["bus"]["distinct_ids"] == 3
    assert s["bus"]["frames_per_s"] > 0 and s["bus"]["since_last_frame_s"] < 2
    dv = s["device"]
    assert dv["state"] == "running" and dv["rx_missed"] == 1 and dv["bus_errors"] == 0
    assert dv["tx_allowed_by_firmware"] is True
    assert s["tx"]["allow_tx_flag"] is False and s["tx"]["available"] is False
    assert "--allow-tx" in s["tx"]["reason"]
    h = s["history"]
    assert len(h["t"]) == len(h["coolant_c"]) == len(h["power_raw"]) == len(h["fuel_used_ml"])
    assert h["coolant_c"][-1] == 27.75 and h["power_raw"][-1] == 0x44 and h["fuel_used_ml"][-1] == 1
    assert len(set(h["t"])) == len(h["t"])  # at most one sample per second
    assert s["commands"] == []


def test_bus_asleep_when_silent(make_dash):
    d = make_dash(allow_tx=False)
    _wait(lambda: d.state()["heater"])
    d.dev.frames = []
    s = _wait(lambda: (st := d.state())["bus"]["awake"] is False and st, timeout=6)
    assert s["bus"]["since_last_frame_s"] > dashboard.AWAKE_TIMEOUT_S
    assert s["heater"]["stale"] is True and s["heater"]["state"] == "heating"  # last known


def test_command_refused_without_allow_tx(make_dash):
    d = make_dash(allow_tx=False)
    _wait(lambda: d.state()["device"])
    status, body = d.post({"mode": "heat"})
    assert status == 403 and "--allow-tx" in body["error"]
    _real_sleep(0.3)
    assert d.dev.received_tx == [] and not d.dev.tx_on
    assert d.state()["commands"] == []


def test_command_refused_when_firmware_disallows(make_dash):
    d = make_dash(allow_tx=True, tx_allowed_by_firmware=False)
    _wait(lambda: d.state()["device"])
    status, body = d.post({"mode": "heat"})
    assert status == 403 and "Firmware" in body["error"]
    assert d.dev.received_tx == []


def test_bad_requests(make_dash):
    d = make_dash(allow_tx=True)
    _wait(lambda: d.state()["device"])
    assert d.post({"mode": "turbo"})[0] == 400
    assert d.post(b"not json")[0] == 400
    assert d.post({"mode": "heat"}, headers={"Origin": "http://evil.example"})[0] == 403
    req = urllib.request.Request(d.base + "/api/state", headers={"Host": "evil.example:8765"})
    with pytest.raises(urllib.error.HTTPError) as e:
        urllib.request.urlopen(req, timeout=5)
    assert e.value.code == 403
    assert d.dev.received_tx == []


def test_command_sends_three_frames_and_returns_to_listen_only(make_dash):
    d = make_dash(allow_tx=True)
    _wait(lambda: d.state()["tx"]["available"])
    status, body = d.post({"mode": "heat"})
    assert status == 202 and body["status"] == "pending" and body["mode"] == "heat"
    # only one command at a time
    assert d.post({"mode": "off"})[0] == 409

    entry = _wait(lambda: (c := d.state()["commands"]) and c[0]["status"] != "pending" and c[0])
    assert entry["status"] == "ok", entry
    assert "sent 3x 5D3#0200" in entry["message"]
    _real_sleep(0.1)
    frame = bytes.fromhex("F1 00 D3 05 00 00 00 02 02 00")
    assert d.dev.received_tx == [frame] * 3
    assert d.dev.tx_mode_at_send == [True] * 3
    assert not d.dev.tx_on  # back to listen-only
    s = d.state()
    assert s["tx"]["session_armed"] is True and s["tx"]["available"] is True
    assert s["device"]["tx_enabled_now"] is False

    # second command reuses the armed session; log is newest first
    assert d.post({"mode": "off"})[0] == 202
    _wait(lambda: (c := d.state()["commands"]) and len(c) == 2 and c[0]["status"] == "ok")
    log = d.state()["commands"]
    assert [c["mode"] for c in log] == ["off", "heat"] and log[0]["frame"] == "5D3#0000 x3"
    assert d.dev.received_tx[3:] == [bytes.fromhex("F1 00 D3 05 00 00 00 02 00 00")] * 3
    assert not d.dev.tx_on


def test_command_log_keeps_last_20(make_dash, monkeypatch):
    d = make_dash(allow_tx=True)
    _wait(lambda: d.state()["tx"]["available"])
    monkeypatch.setattr(d.sn, "send_frame", lambda *a: "sent (stub)")
    for i in range(23):
        assert d.post({"mode": "off"})[0] == 202
        assert d.mon.wait_idle(5)
    log = d.state()["commands"]
    assert len(log) == 20 and log[0]["id"] == 23 and log[-1]["id"] == 4


def test_second_open_reports_port_busy(make_dash):
    d = make_dash(allow_tx=False)
    other = server.CanSniffer()
    with pytest.raises(server.PortBusyError, match="busy"):
        other.connect(d.dev.port, 100000)
    assert d.sn.connected  # the first owner is unaffected


def test_main_exits_cleanly_when_port_busy(make_dash, capsys):
    d = make_dash(allow_tx=False)
    assert dashboard.main(["--port", d.dev.port, "--http", "127.0.0.1:0"]) == 2
    assert "busy" in capsys.readouterr().err
    assert d.sn.connected
