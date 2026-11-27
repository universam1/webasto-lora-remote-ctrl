from webasto_serial_debug import profile as P
import pytest

RECEIVER = bytes.fromhex(
    "01 40 00 00 00 00 00 00 00 00 00 80 00 80 00 80 00 80 00 00 ff ff ff ff 00 78 00 00 00 00 00 00 00 01"
)


def test_receiver_capture():
    s = P.decode_status(RECEIVER)
    assert s["phase"] == "unknown"
    assert s["flags"] == {"bus_awake": False, "climate_active": False, "interior_valid": False,
                          "transport_healthy": True, "ignition_on": False}
    assert s["error"]["code"] == 0 and s["error"]["name"] == "none" and not s["error"]["stale"]
    assert s["temp_c"] == {"interior": "--", "outside": "--", "heater_coolant": "--", "engine_coolant": "--"}
    assert s["voltage_v"] == "--"
    assert s["odometer_km"] == "--"
    assert s["climate"] == {"status": "off", "target_c": 0, "duration_min": 120, "remaining_min": 0}
    assert s["link"]["rssi_dbm"] == 0 and s["link"]["age_s"] == 0
    assert s["last_cmd"] == {"id": 0, "result": "ok"}


def test_encode_matches_receiver_capture():
    assert P.encode_status(transport_healthy=True, duration_min=120, last_cmd_result=1) == RECEIVER


def test_sender_with_rssi_and_link_age():
    raw = P.encode_status(
        phase=2, bus_awake=True, climate_active=True, interior_valid=True, transport_healthy=True,
        ignition_on=False, error_code=0, climate_status=3, power_w=2500, fuel_ml=12, fuel_rate=310,
        interior_dc=185, outside_dc=-35, coolant_dc=620, voltage_mv=12480, odometer_km=123456,
        target_c=21, duration_min=60, remaining_min=42, rssi=-25, snr=9, link_age_s=7, last_cmd_id=4,
        last_cmd_result=2,
    )
    assert len(raw) == 34
    assert raw[29] == 0xE7  # -25 as i8
    s = P.decode_status(raw)
    assert s["phase"] == "heating"
    assert s["flags"]["climate_active"] and s["flags"]["bus_awake"] and not s["flags"]["ignition_on"]
    assert s["climate"]["status"] == "heater_running"
    assert s["temp_c"]["interior"] == 18.5 and s["temp_c"]["outside"] == -3.5
    assert s["temp_c"]["heater_coolant"] == 62.0 and s["temp_c"]["engine_coolant"] == "--"
    assert s["voltage_v"] == 12.48 and s["odometer_km"] == 123456
    assert s["link"] == {"rssi_dbm": -25, "snr_db": 9, "age_s": 7, "age_saturated": False}
    assert s["last_cmd"] == {"id": 4, "result": "ok_unconfirmed"}


def test_error_bits_and_saturated_link():
    s = P.decode_status(P.encode_status(phase=5, error_code=6, err_bits=0x10 | 0x80, link_age_s=255))
    assert s["phase"] == "error"
    assert s["error"] == {"code": 6, "name": "overheat", "stored": True, "kl30_lost": False,
                          "assumed": False, "stale": True}
    assert s["link"]["age_saturated"]


def test_status_errors():
    with pytest.raises(ValueError, match="too short"):
        P.decode_status(RECEIVER[:20])
    with pytest.raises(ValueError, match="version"):
        P.decode_status(b"\x02" + RECEIVER[1:])


def test_info():
    i = P.decode_info(bytes([1, 1, 2]) + b"0.4.2-ble\0\0\0")
    assert i["role"] == "sender" and i["transport"] == "none" and i["fw"] == "0.4.2-ble"
    i = P.decode_info(bytes([1, 0, 0]) + b"abc")
    assert i["role"] == "receiver" and i["transport"] == "can" and i["fw"] == "abc"
    assert P.decode_info(bytes([1, 9, 9]))["role"] == "unknown(9)"


def test_command_encode():
    assert P.encode_command("heat", 7) == bytes([1, 7])
    assert P.encode_command("STOP", 8) == bytes([2, 8])
    assert P.encode_command("climate-set", 9, 21, 300) == bytes([3, 9, 21, 0x2C, 0x01])
    assert P.encode_command(4, 10) == bytes([4, 10])
    assert P.encode_command("5", 11) == bytes([5, 11])
    with pytest.raises(ValueError):
        P.encode_command("climate_set", 1)
    with pytest.raises(ValueError):
        P.encode_command("boost", 1)
    with pytest.raises(ValueError):
        P.encode_command(9, 1)


def test_result_decode():
    r = P.decode_result(bytes([42, 3, 1, 0]))
    assert r["cmd_id"] == 42 and r["stage"] == "done" and r["result"] == "ok" and r["final"]
    r = P.decode_result(bytes([42, 4, 8, 3]))
    assert r["stage"] == "failed" and r["result"] == "timeout" and r["detail"] == 3 and r["final"]
    assert not P.decode_result(bytes([1, 1, 0, 0]))["final"]


def test_names():
    assert [P.phase_name(i) for i in range(6)] == ["unknown", "off", "heating", "run_on", "ventilating", "error"]
    assert P.result_name(6) == "no_bus" and P.result_name(7) == "bus_off" and P.result_name(99) == "unknown(99)"
    assert P.error_name(1) == "battery_low" and P.error_name(10) == "no_bus"
