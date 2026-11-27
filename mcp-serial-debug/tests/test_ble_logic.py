"""BLE logic without a radio: matching, passkey source, result collection, a fake client flow."""

import asyncio

import pytest

from webasto_serial_debug import ble as B
from webasto_serial_debug import profile as P

CANDS = [("30:ae:a4:37:89:ca", "Webasto-RX-89CA"), ("30:AE:A4:37:89:F2", "Webasto-TX-89F2"), ("AA:BB:CC:DD:EE:FF", None)]


def test_match_device():
    assert B.match_device(CANDS, "Webasto-RX-89CA") == "30:AE:A4:37:89:CA"
    assert B.match_device(CANDS, "receiver") == "30:AE:A4:37:89:CA"
    assert B.match_device(CANDS, "sender") == "30:AE:A4:37:89:F2"
    assert B.match_device(CANDS, "tx-89f2") == "30:AE:A4:37:89:F2"
    assert B.match_device(CANDS, "30:ae:a4:37:89:f2") == "30:AE:A4:37:89:F2"
    assert B.match_device(CANDS, "11:22:33:44:55:66") == "11:22:33:44:55:66"  # MAC passes through
    assert B.match_device(CANDS, "nothing") is None
    assert B.match_device([], "receiver") is None
    with pytest.raises(B.BleError, match="ambiguous"):
        B.match_device(CANDS, "Webasto-")


def test_is_webasto():
    assert B.is_webasto("Webasto-RX-89CA")
    assert B.is_webasto(None, [P.SERVICE_UUID.upper()])
    assert not B.is_webasto("Phone", ["0000180f-0000-1000-8000-00805f9b34fb"])


def test_passkey_sources(tmp_path, monkeypatch):
    creds = tmp_path / "credentials.h"
    creds.write_text('#define WIFI_PASSWORD "secret"\n#define BLE_PASSKEY 2143u\n')
    monkeypatch.delenv("WEBASTO_BLE_PASSKEY", raising=False)
    assert B.resolve_passkey(None, creds) == (2143, f"BLE_PASSKEY in {creds}")
    assert B.resolve_passkey(777, creds) == (777, "parameter")
    monkeypatch.setenv("WEBASTO_BLE_PASSKEY", "424242")
    assert B.resolve_passkey(None, creds) == (424242, "env WEBASTO_BLE_PASSKEY")
    monkeypatch.delenv("WEBASTO_BLE_PASSKEY")
    assert B.resolve_passkey(None, tmp_path / "missing.h")[0] == 123456


def _feed(q, items, delay=0.01):
    async def go():
        for it in items:
            await asyncio.sleep(delay)
            q.put_nowait(bytes(it))
    return asyncio.ensure_future(go())


def test_collect_results_until_done():
    async def run():
        q = asyncio.Queue()
        _feed(q, [[9, 0, 0, 0], [7, 0, 0, 0], [7, 1, 0, 0], [7, 2, 0, 0], [7, 3, 1, 0], [7, 3, 1, 0]])
        return await B.collect_results(q, 7, 2)

    r = asyncio.run(run())
    assert r["sequence"] == "queued(pending) -> sent(pending) -> acked(pending) -> done(ok)"
    assert r["final"]["result"] == "ok" and not r["timed_out"]
    assert [o["cmd_id"] for o in r["other_results"]] == [9]


def test_collect_results_failed_and_timeout():
    async def run(items, t):
        q = asyncio.Queue()
        _feed(q, items)
        return await B.collect_results(q, 3, t)

    r = asyncio.run(run([[3, 0, 0, 0], [3, 4, 6, 0]], 2))
    assert r["final"]["stage"] == "failed" and r["final"]["result"] == "no_bus"
    r = asyncio.run(run([[3, 0, 0, 0], [3, 1, 0, 0]], 0.3))
    assert r["timed_out"] and len(r["stages"]) == 2
    r = asyncio.run(run([[1, 2]], 0.2))
    assert r["other_results"][0]["error"].startswith("result too short")


def test_dedup():
    a, b = bytes([1]), bytes([2])
    assert B.dedup_consecutive([(0, a), (1, a), (2, b), (3, a)]) == [(0, a), (2, b), (3, a)]


class FakeClient:
    """Stands in for BleakClient: notifications fire on write like the firmware's result stages."""

    def __init__(self, status):
        self.status = status
        self.notify = {}
        self.written = []
        self.connected = False
        self.disconnects = 0

    is_connected = property(lambda self: self.connected)

    async def connect(self):
        self.connected = True

    async def disconnect(self):
        self.connected = False
        self.disconnects += 1

    async def read_gatt_char(self, uuid):
        if uuid == P.INFO_UUID:
            return bytearray([1, 1, 2]) + b"fw1\0"
        return bytearray(self.status)

    async def start_notify(self, uuid, cb):
        self.notify[uuid] = cb

    async def write_gatt_char(self, uuid, data, response=None):
        assert uuid == P.COMMAND_UUID and response is True
        self.written.append(bytes(data))
        cid = data[1]
        loop = asyncio.get_running_loop()
        for i, (stage, res) in enumerate([(0, 0), (1, 0), (2, 0), (3, 1)]):
            loop.call_later(0.01 * (i + 1), self.notify[P.RESULT_UUID], None, bytearray([cid, stage, res, 0]))
        new = P.encode_status(phase=2, last_cmd_id=cid, last_cmd_result=1)
        loop.call_later(0.02, self.notify[P.STATUS_UUID], None, bytearray(new))
        loop.call_later(0.03, self.notify[P.STATUS_UUID], None, bytearray(new))
        self.status = new


class FakeBlueZ:
    def __init__(self):
        self.removed = []

    async def devices(self):
        return {"30:AE:A4:37:89:F2": {"path": "/org/bluez/hci0/dev_30_AE_A4_37_89_F2", "name": "Webasto-TX-89F2",
                                      "paired": True, "connected": False, "uuids": []}}

    async def remove_device(self, path):
        self.removed.append(path)


def _tool(client):
    t = B.BleTool(bluez=FakeBlueZ())

    async def resolve(q, scan_s=8.0):
        addr = B.match_device([("30:AE:A4:37:89:F2", "Webasto-TX-89F2")], q)
        if addr is None:
            raise B.BleError("not found")
        return object(), addr, "Webasto-TX-89F2"

    async def connect(dev, timeout_s=20.0):
        await client.connect()
        return client

    t._resolve = resolve
    t._connect = connect
    return t


def test_command_flow_with_fake_client():
    client = FakeClient(P.encode_status(phase=1))
    tool = _tool(client)
    r = asyncio.run(tool.command("sender", "climate_set", 21, 90, 2))
    assert client.written[0][0] == 3 and client.written[0][2:] == bytes([21, 90, 0])
    assert r["sequence"] == "queued(pending) -> sent(pending) -> acked(pending) -> done(ok)"
    assert r["final_status"]["phase"] == "heating"
    assert len(r["status_notifications"]) == 1  # duplicates removed
    assert client.disconnects == 1 and not client.connected


def test_read_and_watch_disconnect_even_on_error():
    client = FakeClient(P.encode_status(phase=1))
    tool = _tool(client)
    r = asyncio.run(tool.read("TX"))
    assert r["info"]["role"] == "sender" and r["status"]["phase"] == "off"
    w = asyncio.run(tool.watch("sender", 0.1))
    assert w["distinct"] == 1 and w["sequence"][0]["phase"] == "off"

    async def boom(uuid):
        raise RuntimeError("gatt error")

    client.read_gatt_char = boom
    out = asyncio.run(B.guarded(tool.read("sender"), 5))
    assert out["ok"] is False and "gatt error" in out["error"]
    assert client.disconnects == 3


def test_guarded_timeout_and_unknown_op():
    async def slow():
        await asyncio.sleep(5)

    assert "exceeded" in asyncio.run(B.guarded(slow(), 0.05))["error"]
    tool = _tool(FakeClient(P.encode_status()))
    assert "unknown op" in asyncio.run(B.guarded(tool.command("sender", "boost", None, None, 1), 5))["error"]


def test_unpair_uses_bluez():
    tool = _tool(FakeClient(P.encode_status()))
    r = asyncio.run(tool.unpair("sender"))
    assert r["ok"] and tool.bluez.removed == ["/org/bluez/hci0/dev_30_AE_A4_37_89_F2"]
    assert asyncio.run(tool.unpair("receiver"))["ok"] is False
