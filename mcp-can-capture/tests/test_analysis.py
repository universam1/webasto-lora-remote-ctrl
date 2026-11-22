import json

import pytest

from webasto_can_capture import analysis
from webasto_can_capture.analysis import Frame

OFF = bytes.fromhex("0000000000A00000")  # heater off, coolant 72 C
ON = bytes.fromhex("0405000000A00100")  # heating_on, vehicle_blower, combustion_on, fuel pump


@pytest.fixture
def capdir(tmp_path):
    """10 s synthetic capture: heater switched on at t=5 s."""
    rows = []
    t0 = 1_000_000
    for i in range(1000):  # 0x5EB every 10 ms, counter in byte 1
        rows.append(Frame(t0 + i * 10_000, 0x5EB, False, "Rx", 0, bytes([0xE8, i & 0xFF, 0, 0, 0, 0, 0, 0])))
    for i in range(100):  # 0x3E5 every 100 ms
        rows.append(Frame(t0 + i * 100_000, 0x3E5, False, "Rx", 0, OFF if i < 50 else ON))
    for i in range(50):  # 0x2C1 only after the switch, 10/s
        rows.append(Frame(t0 + 5_000_000 + i * 100_000, 0x2C1, False, "Rx", 0, b"\x01\x02"))
    for i in range(10):  # 0x470 rate goes 1/s -> 4/s
        rows.append(Frame(t0 + i * 1_000_000 if i < 5 else t0 + 5_000_000 + (i - 5) * 250_000, 0x470, False, "Rx", 0, b"\x00"))
    rows.append(Frame(t0 + 6_000_000, 0x3E5, False, "Tx", 0, b"\xFF"))
    rows.sort(key=lambda f: f.ts_us)
    csv = tmp_path / "heater_on.csv"
    csv.write_text(analysis.CSV_HEADER + "\n" + "\n".join(analysis.format_csv_row(f) for f in rows) + "\n")
    (tmp_path / "heater_on.markers.jsonl").write_text(
        json.dumps({"label": "heater on", "device_ts_us": t0 + 5_000_000, "host_time": 1.0}) + "\n"
    )
    return tmp_path


def test_csv_row_format():
    f = Frame(39747828, 0x5EB, False, "Rx", 0, bytes.fromhex("E845"))
    assert analysis.format_csv_row(f) == "39747828,000005EB,false,Rx,0,2,E8,45,00,00,00,00,00,00,"
    assert analysis.parse_csv_row(analysis.format_csv_row(f).split(",")) == f


def test_candump_format():
    f = Frame(0, 0x3E5, False, "Rx", 0, bytes.fromhex("6080EE8627000000"))
    assert analysis.format_candump(f, 1700000000.123456) == "(1700000000.123456) can0 3E5#6080EE8627000000"
    fx = Frame(0, 0x1ABCDEF0, True, "Rx", 1, b"")
    assert analysis.format_candump(fx, 1.0) == "(1.000000) can1 1ABCDEF0#"


def test_load_capture(capdir):
    cap = analysis.load_capture("heater_on", capdir)
    assert len(cap.frames) == 1161
    assert cap.markers[0].label == "heater on"
    assert cap.rel(cap.markers[0].ts_us) == pytest.approx(5.0)


def test_summarize(capdir):
    out = analysis.summarize(analysis.load_capture("heater_on", capdir))
    line_5eb = next(l for l in out.splitlines() if l.startswith("5EB"))
    assert " 1000 " in line_5eb and "10.0" in line_5eb and "B1" in line_5eb
    line_3e5 = next(l for l in out.splitlines() if l.startswith("3E5"))
    assert "B0{00,04}" in line_3e5 and "B0.2" in line_3e5 and "B1.0" in line_3e5 and "B1.2" in line_3e5
    assert "B6.0" in line_3e5
    assert "1 Tx frames" in out


def test_summarize_window(capdir):
    out = analysis.summarize(analysis.load_capture("heater_on", capdir), 0, 4.9)
    assert "2C1" not in out
    assert next(l for l in out.splitlines() if l.startswith("3E5")).endswith("static")


def test_diff_marker(capdir):
    out = analysis.diff_marker(analysis.load_capture("heater_on", capdir), "heater on", 4.0)
    assert "only in B: 2C1" in out
    assert "470" in out.split("rate change")[1].splitlines()[0]
    payload = out.split("payload differences")[1].splitlines()[1:]
    assert payload[0].lstrip().startswith("3E5")  # most relevant first
    assert "b2:0->1" in payload[0] and "B6" in payload[0]
    assert "5EB" not in out.split("rate change")[1].splitlines()[0]


def test_diff_two_captures(capdir, tmp_path):
    off = [Frame(i * 100_000, 0x3E5, False, "Rx", 0, OFF) for i in range(20)]
    (capdir / "off.csv").write_text(analysis.CSV_HEADER + "\n" + "\n".join(analysis.format_csv_row(f) for f in off) + "\n")
    out = analysis.diff_captures(analysis.load_capture("off", capdir), analysis.load_capture("heater_on", capdir))
    assert "only in B:" in out and "5EB" in out and "2C1" in out
    assert "3E5" in out.split("payload differences")[1]


def test_find_marker_errors(capdir):
    cap = analysis.load_capture("heater_on", capdir)
    with pytest.raises(ValueError):
        analysis.find_marker(cap, "nope")
    assert analysis.find_marker(cap, "0").label == "heater on"
    assert analysis.find_marker(cap, "HEATER").label == "heater on"


def test_decode_3e5_payload():
    d = analysis.decode_3e5_payload(bytes.fromhex("6080EE8627000000"))
    assert d["heater_mode_activating"] == 1 and d["heater_programming"] == 1
    assert d["error_stored"] == 1 and d["error_code"] == "0:none"
    assert d["fuel_used_ml"] == 0x86EE & 0x3FFF and d["kl30_lost"] == 1 and d["parking_heater"] == 0
    assert d["heating_power_raw"] == 0x27
    assert d["coolant_c"] == -48.0
    assert analysis.decode_3e5_payload(bytes.fromhex("00000000" "00FF0000"))["coolant_c"] == "error"
    assert analysis.decode_3e5_payload(bytes([0, 0x30]))["error_code"] == "3:heater_defective"


def test_decode_3e5_changes_only(capdir):
    out = analysis.decode_3e5(analysis.load_capture("heater_on", capdir))
    lines = out.splitlines()
    assert "unverified" in out
    body = [l for l in lines[2:]]
    assert len(body) == 3  # initial row, marker, change row
    assert "coolant_c=72.0" in body[0]
    assert "marker: heater on" in body[1]
    assert "heating_on=1" in body[2] and "combustion_on=1" in body[2] and "coolant_c" not in body[2]


def test_decode_3e5_max_rows(capdir):
    out = analysis.decode_3e5(analysis.load_capture("heater_on", capdir), max_rows=1)
    assert "1 more change rows" in out
