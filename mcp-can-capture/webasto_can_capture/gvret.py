"""
GVRET binary protocol (SavvyCAN-compatible) encode/parse helpers.

Pure functions and a streaming parser; no I/O happens here.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Union

ENTER_BINARY = b"\xE7\xE7"
START = 0xF1

CMD_FRAME = 0x00
CMD_TIME_SYNC = 0x01
CMD_SET_BUS_CONFIG = 0x05
CMD_GET_BUS_PARAMS = 0x06
CMD_DEVICE_INFO = 0x07
CMD_KEEPALIVE = 0x09
CMD_NUM_BUSES = 0x0C
CMD_STATUS = 0x40  # vendor extension
CMD_SET_TX = 0x41  # vendor extension (host -> device only)

EXTENDED_FLAG = 0x80000000
BUS_CFG_VALID = 0x80000000
BUS_CFG_ENABLED = 0x40000000
BUS_CFG_LISTEN_ONLY = 0x20000000
BITRATE_MASK = 0x0FFFFFFF

MAX_STD_ID = 0x7FF
MAX_EXT_ID = 0x1FFFFFFF

STATE_NAMES = {0: "stopped", 1: "running", 2: "bus-off", 3: "recovering"}

# Payload lengths (bytes after F1 <cmd>) of fixed-size device -> host replies.
_FIXED_REPLY_LEN = {
    CMD_TIME_SYNC: 4,
    CMD_GET_BUS_PARAMS: 10,
    CMD_DEVICE_INFO: 6,
    CMD_KEEPALIVE: 2,
    CMD_NUM_BUSES: 1,
    CMD_STATUS: 14,
}


# --------------------------------------------------------------------------- messages


@dataclass(frozen=True)
class CanFrame:
    timestamp_us: int  # raw 32-bit device timestamp
    can_id: int
    extended: bool
    bus: int
    data: bytes


@dataclass(frozen=True)
class TimeSync:
    timestamp_us: int


@dataclass(frozen=True)
class BusParams:
    can0_enabled: bool
    can0_listen_only: bool
    can0_bitrate: int
    can1_enabled: bool
    can1_listen_only: bool
    can1_bitrate: int


@dataclass(frozen=True)
class DeviceInfo:
    build: int
    eeprom_version: int
    file_type: int
    auto_log: int
    single_wire_mode: int


@dataclass(frozen=True)
class Keepalive:
    payload: bytes


@dataclass(frozen=True)
class NumBuses:
    count: int


@dataclass(frozen=True)
class DeviceStatus:
    tx_allowed_by_firmware: bool
    tx_enabled_now: bool
    rx_frames: int
    rx_missed: int
    bus_errors: int
    state: int

    @property
    def state_name(self) -> str:
        return STATE_NAMES.get(self.state, f"unknown({self.state})")


Message = Union[CanFrame, TimeSync, BusParams, DeviceInfo, Keepalive, NumBuses, DeviceStatus]


# --------------------------------------------------------------------------- encoding


def cmd(command: int) -> bytes:
    return bytes((START, command))


def bus_config_value(bitrate: int, enabled: bool = True, listen_only: bool = True) -> int:
    if not 0 < bitrate <= BITRATE_MASK:
        raise ValueError(f"bitrate out of range: {bitrate}")
    value = (bitrate & BITRATE_MASK) | BUS_CFG_VALID
    if enabled:
        value |= BUS_CFG_ENABLED
    if listen_only:
        value |= BUS_CFG_LISTEN_ONLY
    return value


def encode_set_bus_config(
    can0_bitrate: int,
    can0_enabled: bool = True,
    can0_listen_only: bool = True,
    can1_bitrate: int | None = None,
    can1_enabled: bool = False,
    can1_listen_only: bool = True,
) -> bytes:
    """F1 05 can0_u32le can1_u32le."""
    v0 = bus_config_value(can0_bitrate, can0_enabled, can0_listen_only)
    v1 = bus_config_value(can1_bitrate or can0_bitrate, can1_enabled, can1_listen_only)
    return cmd(CMD_SET_BUS_CONFIG) + struct.pack("<II", v0, v1)


def encode_tx_frame(can_id: int, data: bytes, extended: bool = False, bus: int = 0) -> bytes:
    """F1 00 id_u32le bus len data[len] (bit31 of id = extended)."""
    data = bytes(data)
    if len(data) > 8:
        raise ValueError("classic CAN frames carry at most 8 data bytes")
    limit = MAX_EXT_ID if extended else MAX_STD_ID
    if not 0 <= can_id <= limit:
        raise ValueError(f"CAN id 0x{can_id:X} out of range for {'extended' if extended else 'standard'} id")
    raw_id = can_id | (EXTENDED_FLAG if extended else 0)
    return cmd(CMD_FRAME) + struct.pack("<IBB", raw_id, bus, len(data)) + data


def encode_set_tx(enable: bool) -> bytes:
    return cmd(CMD_SET_TX) + (b"\x01" if enable else b"\x00")


# --------------------------------------------------------------------------- parsing


def _decode_fixed(command: int, p: bytes) -> Message:
    if command == CMD_TIME_SYNC:
        return TimeSync(struct.unpack_from("<I", p)[0])
    if command == CMD_GET_BUS_PARAMS:
        f0, b0, f1, b1 = struct.unpack("<BIBI", p)
        return BusParams(bool(f0 & 1), bool(f0 & 0x10), b0, bool(f1 & 1), bool(f1 & 0x10), b1)
    if command == CMD_DEVICE_INFO:
        build, eeprom, ftype, autolog, sw = struct.unpack("<HBBBB", p)
        return DeviceInfo(build, eeprom, ftype, autolog, sw)
    if command == CMD_KEEPALIVE:
        return Keepalive(bytes(p))
    if command == CMD_NUM_BUSES:
        return NumBuses(p[0])
    if command == CMD_STATUS:
        flags, rx, missed, errs, state = struct.unpack("<BIIIB", p)
        return DeviceStatus(bool(flags & 1), bool(flags & 2), rx, missed, errs, state)
    raise AssertionError(command)


@dataclass
class GvretParser:
    """Streaming parser for device -> host GVRET traffic.

    Bytes that cannot start a valid message (ASCII boot text, corrupted data,
    unknown commands) are skipped one at a time until the stream resyncs.
    """

    buf: bytearray = field(default_factory=bytearray)
    skipped_bytes: int = 0
    max_buffer: int = 1 << 16

    def feed(self, data: bytes) -> list[Message]:
        self.buf += data
        out: list[Message] = []
        buf = self.buf
        i = 0
        n = len(buf)
        while i < n:
            if buf[i] != START:
                i += 1
                self.skipped_bytes += 1
                continue
            if i + 1 >= n:
                break  # need command byte
            command = buf[i + 1]
            if command == CMD_FRAME:
                if i + 11 > n:
                    break
                ts, raw_id, buslen = struct.unpack_from("<IIB", buf, i + 2)
                length = buslen & 0x0F
                total = 11 + length + 1
                if length > 8 or (raw_id & 0x7FFFFFFF) > MAX_EXT_ID:
                    i += 1
                    self.skipped_bytes += 1
                    continue
                if i + total > n:
                    break
                if buf[i + total - 1] != 0x00:
                    i += 1
                    self.skipped_bytes += 1
                    continue
                ext = bool(raw_id & EXTENDED_FLAG)
                can_id = raw_id & MAX_EXT_ID
                if not ext and can_id > MAX_STD_ID:
                    i += 1
                    self.skipped_bytes += 1
                    continue
                data_bytes = bytes(buf[i + 11 : i + 11 + length])
                out.append(CanFrame(ts, can_id, ext, buslen >> 4, data_bytes))
                i += total
                continue
            plen = _FIXED_REPLY_LEN.get(command)
            if plen is None:
                i += 1
                self.skipped_bytes += 1
                continue
            if i + 2 + plen > n:
                break
            out.append(_decode_fixed(command, bytes(buf[i + 2 : i + 2 + plen])))
            i += 2 + plen
        del buf[:i]
        if len(buf) > self.max_buffer:  # should never happen; avoid unbounded growth
            self.skipped_bytes += len(buf)
            buf.clear()
        return out


class TimestampUnwrapper:
    """Extends the device's 32-bit microsecond counter to 64 bits."""

    def __init__(self) -> None:
        self._last: int | None = None
        self._epoch = 0

    def __call__(self, ts32: int) -> int:
        if self._last is not None and ts32 < self._last and self._last - ts32 > 0x80000000:
            self._epoch += 1
        self._last = ts32
        return (self._epoch << 32) | ts32
