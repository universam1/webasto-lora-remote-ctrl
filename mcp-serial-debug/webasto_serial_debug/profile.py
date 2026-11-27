"""Pure codec for the Webasto BLE GATT profile (lib/webasto_core/ble_profile.h).

No I/O here, so it can be unit tested without a radio. pwa/parser.js mirrors the same layouts.
"""

from __future__ import annotations

import struct
from typing import Any

SERVICE_UUID = "7e5a0001-4b1d-4c5e-9a3f-2f6d0c1b8a10"
INFO_UUID = "7e5a0002-4b1d-4c5e-9a3f-2f6d0c1b8a10"
STATUS_UUID = "7e5a0003-4b1d-4c5e-9a3f-2f6d0c1b8a10"
COMMAND_UUID = "7e5a0004-4b1d-4c5e-9a3f-2f6d0c1b8a10"
RESULT_UUID = "7e5a0005-4b1d-4c5e-9a3f-2f6d0c1b8a10"

STATUS_VERSION = 1
INFO_VERSION = 1
STATUS_LEN = 34

INT16_MIN = -32768
ODO_UNKNOWN = 0xFFFFFFFF

ROLE = {0: "receiver", 1: "sender"}
TRANSPORT = {0: "can", 1: "wbus", 2: "none"}
# wh::HeaterPhase (lib/webasto_core/heater_types.h)
PHASE = {0: "unknown", 1: "off", 2: "heating", 3: "run_on", 4: "ventilating", 5: "error"}
# wh::ClimateStatus
CLIMATE = {
    0: "off", 1: "idle", 2: "starting", 3: "heater_running",
    4: "pause", 5: "sensor_fault", 6: "low_voltage", 7: "failed",
}
# wh::CmdResult
CMD_RESULT = {
    0: "pending", 1: "ok", 2: "ok_unconfirmed", 3: "not_adopted", 4: "unsupported",
    5: "busy", 6: "no_bus", 7: "bus_off", 8: "timeout",
}
# Heater error code (status byte 2, bits 0-3); 8.. are synthetic codes, see pwa/parser.js
ERROR = {
    0: "none", 1: "battery_low", 2: "low_fuel", 3: "heater_defective", 4: "crash_shutdown",
    5: "other", 6: "overheat", 8: "command_not_adopted", 9: "unsupported", 10: "no_bus",
}
STAGE = {0: "queued", 1: "sent", 2: "acked", 3: "done", 4: "failed"}
STAGE_DONE, STAGE_FAILED = 3, 4
FINAL_STAGES = (STAGE_DONE, STAGE_FAILED)

OP = {"heat": 1, "stop": 2, "climate_set": 3, "climate_off": 4, "query": 5}
OP_NAME = {v: k for k, v in OP.items()}


def name(table: dict[int, str], value: int) -> str:
    return table.get(value, f"unknown({value})")


def phase_name(v: int) -> str:
    return name(PHASE, v)


def error_name(v: int) -> str:
    return name(ERROR, v)


def result_name(v: int) -> str:
    return name(CMD_RESULT, v)


def stage_name(v: int) -> str:
    return name(STAGE, v)


def _temp(dc: int) -> str | float:
    return "--" if dc == INT16_MIN else round(dc / 10, 1)


def decode_status(data: bytes) -> dict[str, Any]:
    """Decode the 34 byte status characteristic (layout ble::StatusOff)."""
    b = bytes(data)
    if len(b) < STATUS_LEN:
        raise ValueError(f"status too short: {len(b)} bytes (need {STATUS_LEN}): {b.hex(' ')}")
    if b[0] != STATUS_VERSION:
        raise ValueError(f"unsupported status version {b[0]} (expected {STATUS_VERSION})")
    f, e = b[1], b[2]
    power, fuel, rate = struct.unpack_from("<HHH", b, 4)
    interior, outside, coolant, engine = struct.unpack_from("<hhhh", b, 10)
    (volt,) = struct.unpack_from("<H", b, 18)
    (odo,) = struct.unpack_from("<I", b, 20)
    duration, remaining = struct.unpack_from("<HH", b, 25)
    rssi, snr = struct.unpack_from("<bb", b, 29)
    return {
        "version": b[0],
        "phase": phase_name(f & 0x07),
        "phase_code": f & 0x07,
        "flags": {
            "bus_awake": bool(f & 0x08),
            "climate_active": bool(f & 0x10),
            "interior_valid": bool(f & 0x20),
            "transport_healthy": bool(f & 0x40),
            "ignition_on": bool(f & 0x80),
        },
        "error": {
            "code": e & 0x0F,
            "name": error_name(e & 0x0F),
            "stored": bool(e & 0x10),
            "kl30_lost": bool(e & 0x20),
            "assumed": bool(e & 0x40),
            "stale": bool(e & 0x80),
        },
        "climate": {
            "status": name(CLIMATE, b[3]),
            "target_c": b[24],
            "duration_min": duration,
            "remaining_min": remaining,
        },
        "power_w": power,
        "fuel_used_ml": fuel,
        "fuel_rate_mlph": rate,
        "temp_c": {
            "interior": _temp(interior),
            "outside": _temp(outside),
            "heater_coolant": _temp(coolant),
            "engine_coolant": _temp(engine),
        },
        "voltage_v": "--" if volt == 0 else round(volt / 1000, 3),
        "odometer_km": "--" if odo == ODO_UNKNOWN else odo,
        "link": {"rssi_dbm": rssi, "snr_db": snr, "age_s": b[31], "age_saturated": b[31] == 255},
        "last_cmd": {"id": b[32], "result": result_name(b[33])},
        "raw": b[:STATUS_LEN].hex(" "),
    }


def encode_status(
    *, phase=0, bus_awake=False, climate_active=False, interior_valid=False, transport_healthy=False,
    ignition_on=False, error_code=0, err_bits=0, climate_status=0, power_w=0, fuel_ml=0,
    fuel_rate=0, interior_dc=INT16_MIN, outside_dc=INT16_MIN, coolant_dc=INT16_MIN,
    engine_dc=INT16_MIN, voltage_mv=0, odometer_km=ODO_UNKNOWN, target_c=0, duration_min=0,
    remaining_min=0, rssi=0, snr=0, link_age_s=0, last_cmd_id=0, last_cmd_result=0,
) -> bytes:
    """Build a status payload (inverse of decode_status). Used by tests and fakes."""
    flags = (phase & 7) | (bus_awake << 3) | (climate_active << 4) | (interior_valid << 5) \
        | (transport_healthy << 6) | (ignition_on << 7)
    out = struct.pack(
        "<BBBBHHHhhhhHIBHHbbBBB",
        STATUS_VERSION, flags, (error_code & 0x0F) | err_bits, climate_status, power_w, fuel_ml,
        fuel_rate, interior_dc, outside_dc, coolant_dc, engine_dc, voltage_mv, odometer_km,
        target_c, duration_min, remaining_min, rssi, snr, link_age_s, last_cmd_id, last_cmd_result,
    )
    assert len(out) == STATUS_LEN
    return out


def decode_info(data: bytes) -> dict[str, Any]:
    """[ver][role][transport][fw ascii, NUL terminated or to the end]"""
    b = bytes(data)
    if len(b) < 3:
        raise ValueError(f"info too short: {b.hex(' ')}")
    fw = b[3:].split(b"\0")[0].decode("ascii", errors="replace")
    return {
        "version": b[0],
        "role": name(ROLE, b[1]),
        "transport": name(TRANSPORT, b[2]),
        "fw": fw,
        "raw": b.hex(" "),
    }


def parse_op(op: str | int) -> int:
    if isinstance(op, int):
        code = op
    else:
        s = str(op).strip().lower().replace("-", "_").replace(" ", "_")
        if s.isdigit():
            code = int(s)
        elif s in OP:
            code = OP[s]
        else:
            raise ValueError(f"unknown op {op!r}; use one of {', '.join(OP)} or 1..5")
    if code not in OP_NAME:
        raise ValueError(f"unknown op code {code}; valid: {OP_NAME}")
    return code


def encode_command(op: str | int, cmd_id: int, target_c: int | None = None,
                   duration_min: int | None = None) -> bytes:
    """[op][cmdId][args]; climate_set needs target_c (u8) and duration_min (u16 LE)."""
    code = parse_op(op)
    if not 0 <= cmd_id <= 255:
        raise ValueError("cmd_id must be 0..255")
    out = bytes([code, cmd_id])
    if code == OP["climate_set"]:
        if target_c is None or duration_min is None:
            raise ValueError("climate_set needs target_c and duration_min")
        if not 0 <= int(target_c) <= 255 or not 0 <= int(duration_min) <= 0xFFFF:
            raise ValueError("target_c must be 0..255, duration_min 0..65535")
        out += struct.pack("<BH", int(target_c), int(duration_min))
    return out


def decode_result(data: bytes) -> dict[str, Any]:
    """[cmdId][stage][result][detail]"""
    b = bytes(data)
    if len(b) < 4:
        raise ValueError(f"result too short: {b.hex(' ')}")
    return {
        "cmd_id": b[0],
        "stage": stage_name(b[1]),
        "stage_code": b[1],
        "result": result_name(b[2]),
        "result_code": b[2],
        "detail": b[3],
        "final": b[1] in FINAL_STAGES,
    }
