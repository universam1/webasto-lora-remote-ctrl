import struct

import pytest

from webasto_can_capture import gvret


def dev_frame(ts, can_id, data, ext=False, bus=0):
    raw = can_id | (0x80000000 if ext else 0)
    return b"\xF1\x00" + struct.pack("<IIB", ts, raw, (bus << 4) | len(data)) + bytes(data) + b"\x00"


def test_parse_single_frame():
    p = gvret.GvretParser()
    msgs = p.feed(dev_frame(39747828, 0x5EB, bytes.fromhex("E845000000000000")))
    assert msgs == [gvret.CanFrame(39747828, 0x5EB, False, 0, bytes.fromhex("E845000000000000"))]
    assert p.skipped_bytes == 0


def test_parse_extended_and_bus():
    p = gvret.GvretParser()
    (f,) = p.feed(dev_frame(1, 0x1ABCDEF0, b"\x01\x02", ext=True, bus=1))
    assert f.extended and f.can_id == 0x1ABCDEF0 and f.bus == 1 and f.data == b"\x01\x02"


def test_parse_split_across_feeds():
    p = gvret.GvretParser()
    raw = dev_frame(5, 0x3E5, bytes(range(8))) + dev_frame(6, 0x123, b"")
    out = []
    for i in range(len(raw)):
        out += p.feed(raw[i : i + 1])
    assert [m.can_id for m in out] == [0x3E5, 0x123]
    assert out[1].data == b""


def test_resync_after_ascii_boot_text_and_garbage():
    p = gvret.GvretParser()
    text = b"ets Jun  8 2016 00:22:57\r\nrst:0x1 (POWERON)\r\nCAN sniffer ready\r\n"
    garbage = b"\xF1\xFF\x13\x37\xF1"  # unknown command, stray start byte
    bad_len = b"\xF1\x00" + struct.pack("<IIB", 1, 0x100, 0x0F)  # len 15 is invalid
    raw = text + garbage + bad_len + dev_frame(7, 0x3E5, b"\xAA") + b"junk" + dev_frame(8, 0x3E5, b"\xBB")
    msgs = p.feed(raw)
    frames = [m for m in msgs if isinstance(m, gvret.CanFrame)]
    assert [f.data for f in frames] == [b"\xAA", b"\xBB"]
    assert p.skipped_bytes > len(text)
    assert p.buf == bytearray()


def test_frame_with_bad_trailer_is_skipped():
    p = gvret.GvretParser()
    broken = dev_frame(1, 0x10, b"\x01")[:-1] + b"\x55"
    msgs = p.feed(broken + dev_frame(2, 0x20, b"\x02"))
    assert [m.can_id for m in msgs if isinstance(m, gvret.CanFrame)] == [0x20]


def test_parse_replies():
    p = gvret.GvretParser()
    raw = (
        b"\xF1\x01" + struct.pack("<I", 123456)
        + b"\xF1\x06" + struct.pack("<BIBI", 0x11, 100000, 0x00, 500000)
        + b"\xF1\x07" + struct.pack("<HBBBB", 618, 2, 1, 0, 0)
        + b"\xF1\x09\xDE\xAD"
        + b"\xF1\x0C\x01"
        + b"\xF1\x40" + struct.pack("<BIIIB", 0x01, 1000, 3, 7, 1)
    )
    m = p.feed(raw)
    assert m[0] == gvret.TimeSync(123456)
    assert m[1] == gvret.BusParams(True, True, 100000, False, False, 500000)
    assert m[2] == gvret.DeviceInfo(618, 2, 1, 0, 0)
    assert m[3] == gvret.Keepalive(b"\xDE\xAD")
    assert m[4] == gvret.NumBuses(1)
    st = m[5]
    assert st.tx_allowed_by_firmware and not st.tx_enabled_now
    assert (st.rx_frames, st.rx_missed, st.bus_errors, st.state_name) == (1000, 3, 7, "running")


def test_encode_set_bus_config_listen_only():
    pkt = gvret.encode_set_bus_config(100000, True, True)
    assert pkt[:2] == b"\xF1\x05"
    v0, v1 = struct.unpack("<II", pkt[2:])
    assert v0 == 100000 | 0x80000000 | 0x40000000 | 0x20000000
    assert v1 & 0x40000000 == 0 and v1 & 0x80000000
    assert len(pkt) == 10


def test_encode_set_bus_config_normal_mode():
    v0 = struct.unpack("<I", gvret.encode_set_bus_config(500000, True, False)[2:6])[0]
    assert v0 == 500000 | 0xC0000000


def test_encode_tx_frame():
    assert gvret.encode_tx_frame(0x3E5, b"\x60\x80") == b"\xF1\x00\xE5\x03\x00\x00\x00\x02\x60\x80"
    ext = gvret.encode_tx_frame(0x1ABCDEF0, b"", extended=True)
    assert ext == b"\xF1\x00" + struct.pack("<I", 0x9ABCDEF0) + b"\x00\x00"


@pytest.mark.parametrize("cid,ext,data", [(0x800, False, b""), (0x20000000, True, b""), (0x1, False, bytes(9))])
def test_encode_tx_frame_rejects_invalid(cid, ext, data):
    with pytest.raises(ValueError):
        gvret.encode_tx_frame(cid, data, extended=ext)


def test_encode_set_tx():
    assert gvret.encode_set_tx(True) == b"\xF1\x41\x01"
    assert gvret.encode_set_tx(False) == b"\xF1\x41\x00"


def test_timestamp_unwrap():
    u = gvret.TimestampUnwrapper()
    assert u(0xFFFFFF00) == 0xFFFFFF00
    assert u(0x10) == (1 << 32) | 0x10
    assert u(0x20) == (1 << 32) | 0x20
