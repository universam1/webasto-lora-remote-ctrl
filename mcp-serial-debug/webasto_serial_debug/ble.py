"""BLE side: scan, pair (BlueZ D-Bus agent), read, command, watch for the Webasto GATT profile.

All functions run on the server's single asyncio loop; BleTool serializes them with a lock and
puts a hard timeout on every step, because BlueZ can hang (e.g. reading the encrypted info
characteristic without a bond waits ~30 s instead of failing).
"""

from __future__ import annotations

import asyncio
import itertools
import logging
import os
import re
import time
from pathlib import Path
from typing import Any, Awaitable, Callable, Iterable

from . import profile as P

logger = logging.getLogger("webasto-serial-debug")

NAME_PREFIX = "Webasto-"
DEFAULT_PASSKEY = 123456  # BLE_DEFAULT_PASSKEY in lib/ble_gatt/ble_server.h
AGENT_PATH = "/webasto/debug_agent"
MAC_RE = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")
CREDENTIALS_H = Path(__file__).resolve().parents[2] / "include" / "credentials.h"

# Board aliases from serial_link.ALIASES (kept here to avoid importing serial for BLE-only use).
BLE_ALIASES = {
    "receiver": ("Webasto-RX-89CA", "30:AE:A4:37:89:CA"),
    "rx": ("Webasto-RX-89CA", "30:AE:A4:37:89:CA"),
    "sender": ("Webasto-TX-89F2", "30:AE:A4:37:89:F2"),
    "tx": ("Webasto-TX-89F2", "30:AE:A4:37:89:F2"),
}


class BleError(RuntimeError):
    pass


# ---------------------------------------------------------------------- pure helpers

def resolve_passkey(passkey: int | str | None, credentials_path: Path = CREDENTIALS_H) -> tuple[int, str]:
    """Return (passkey, source). Order: parameter, env WEBASTO_BLE_PASSKEY, BLE_PASSKEY define in
    include/credentials.h (only that line is read), firmware default 123456."""
    if passkey not in (None, ""):
        return int(passkey), "parameter"
    env = os.environ.get("WEBASTO_BLE_PASSKEY", "").strip()
    if env:
        return int(env), "env WEBASTO_BLE_PASSKEY"
    try:
        with open(credentials_path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r"\s*#\s*define\s+BLE_PASSKEY\s+(\d+)[uUlL]*\b", line)
                if m:
                    return int(m.group(1)), f"BLE_PASSKEY in {credentials_path}"
    except OSError:
        pass
    return DEFAULT_PASSKEY, "firmware default (BLE_DEFAULT_PASSKEY)"


def is_webasto(name: str | None, service_uuids: Iterable[str] | None = None) -> bool:
    if name and name.startswith(NAME_PREFIX):
        return True
    return any(u.lower() == P.SERVICE_UUID for u in (service_uuids or ()))


def match_device(candidates: Iterable[tuple[str, str | None]], query: str) -> str | None:
    """Pick the address for `query` among (address, name) pairs.

    Accepts a MAC (case-insensitive), an exact name, a board alias (receiver/sender), or a
    unique case-insensitive substring of a name (e.g. "RX" or "89F2")."""
    q = query.strip()
    cands = [(a.upper(), n) for a, n in candidates]
    if MAC_RE.match(q):
        return q.upper()
    alias = BLE_ALIASES.get(q.lower())
    if alias:
        for a, n in cands:
            if n == alias[0] or a == alias[1]:
                return a
        q = alias[0]
    for a, n in cands:
        if n == q:
            return a
    sub = [a for a, n in cands if n and q.lower() in n.lower()]
    sub = list(dict.fromkeys(sub))
    if len(sub) == 1:
        return sub[0]
    if len(sub) > 1:
        raise BleError(f"{query!r} is ambiguous: {', '.join(sub)}")
    return None


async def collect_results(queue: "asyncio.Queue[bytes]", cmd_id: int, timeout_s: float,
                          clock: Callable[[], float] = time.monotonic) -> dict[str, Any]:
    """Read result notifications from `queue` until a final stage for `cmd_id` or timeout.
    Notifications for other command ids are kept under `other`."""
    start = clock()
    deadline = start + timeout_s
    stages, other = [], []
    final = None
    while True:
        left = deadline - clock()
        if left <= 0:
            break
        try:
            raw = await asyncio.wait_for(queue.get(), left)
        except asyncio.TimeoutError:
            break
        try:
            r = P.decode_result(raw)
        except ValueError as e:
            other.append({"error": str(e)})
            continue
        r["t_s"] = round(clock() - start, 2)
        if r["cmd_id"] != cmd_id:
            other.append(r)
            continue
        stages.append(r)
        if r["final"]:
            final = r
            break
    return {
        "cmd_id": cmd_id,
        "stages": stages,
        "sequence": " -> ".join(f"{s['stage']}({s['result']})" for s in stages) or "(none)",
        "final": final,
        "timed_out": final is None,
        "other_results": other,
    }


def dedup_consecutive(items: list[tuple[float, bytes]]) -> list[tuple[float, bytes]]:
    out, last = [], None
    for t, b in items:
        if b != last:
            out.append((t, b))
            last = b
    return out


def decode_status_safe(b: bytes) -> dict[str, Any]:
    try:
        return P.decode_status(b)
    except ValueError as e:
        return {"error": str(e)}


# ---------------------------------------------------------------------- BlueZ D-Bus

class _BlueZ:
    """Small D-Bus helper: device list with Paired/Bonded, RemoveDevice, passkey agent."""

    def __init__(self):
        self.bus = None
        self.agent = None
        self.agent_registered = False
        self.agent_log: list[str] = []

    async def _ensure_bus(self):
        from dbus_fast import BusType
        from dbus_fast.aio import MessageBus

        if self.bus is None or not self.bus.connected:
            self.bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
            self.agent_registered = False
        return self.bus

    async def managed_objects(self) -> dict[str, Any]:
        from dbus_fast import Message

        bus = await self._ensure_bus()
        reply = await bus.call(Message(destination="org.bluez", path="/", interface="org.freedesktop.DBus.ObjectManager",
                                       member="GetManagedObjects"))
        if reply.message_type.name == "ERROR":
            raise BleError(f"BlueZ GetManagedObjects failed: {reply.body}")
        return reply.body[0]

    async def devices(self) -> dict[str, dict[str, Any]]:
        out = {}
        for path, ifaces in (await self.managed_objects()).items():
            d = ifaces.get("org.bluez.Device1")
            if not d:
                continue
            g = lambda k, default=None: d[k].value if k in d else default  # noqa: E731
            addr = g("Address", "").upper()
            out[addr] = {
                "path": path, "address": addr, "name": g("Name") or g("Alias"),
                "paired": bool(g("Paired", False)), "bonded": bool(g("Bonded", g("Paired", False))),
                "trusted": bool(g("Trusted", False)), "connected": bool(g("Connected", False)),
                "rssi": g("RSSI"), "uuids": [u.lower() for u in g("UUIDs", [])],
            }
        return out

    async def remove_device(self, dev_path: str) -> None:
        from dbus_fast import Message

        bus = await self._ensure_bus()
        adapter = dev_path.rsplit("/", 1)[0]
        reply = await bus.call(Message(destination="org.bluez", path=adapter, interface="org.bluez.Adapter1",
                                       member="RemoveDevice", signature="o", body=[dev_path]))
        if reply.message_type.name == "ERROR":
            raise BleError(f"RemoveDevice failed: {reply.error_name} {reply.body}")

    async def ensure_agent(self, passkey: int) -> str:
        """Register a KeyboardOnly agent once per process; later calls only update the passkey."""
        from dbus_fast.service import ServiceInterface, method

        bus = await self._ensure_bus()
        if self.agent is not None and self.agent_registered:
            self.agent.passkey = passkey
            return "agent already registered"
        log = self.agent_log

        class Agent(ServiceInterface):
            def __init__(self, pk):
                super().__init__("org.bluez.Agent1")
                self.passkey = pk

            @method()
            def Release(self):  # noqa: N802
                log.append("Release")

            @method()
            def RequestPasskey(self, device: "o") -> "u":  # noqa: F821,N802
                log.append(f"RequestPasskey {device}")
                return self.passkey

            @method()
            def DisplayPasskey(self, device: "o", passkey: "u", entered: "q"):  # noqa: F821,N802
                log.append(f"DisplayPasskey {passkey:06d}")

            @method()
            def RequestConfirmation(self, device: "o", passkey: "u"):  # noqa: F821,N802
                log.append(f"RequestConfirmation {passkey:06d}")

            @method()
            def RequestAuthorization(self, device: "o"):  # noqa: F821,N802
                log.append("RequestAuthorization")

            @method()
            def AuthorizeService(self, device: "o", uuid: "s"):  # noqa: F821,N802
                pass

            @method()
            def Cancel(self):  # noqa: N802
                log.append("Cancel")

        if self.agent is None:
            self.agent = Agent(passkey)
            bus.export(AGENT_PATH, self.agent)
        self.agent.passkey = passkey
        intro = await bus.introspect("org.bluez", "/org/bluez")
        mgr = bus.get_proxy_object("org.bluez", "/org/bluez", intro).get_interface("org.bluez.AgentManager1")
        try:
            await mgr.call_register_agent(AGENT_PATH, "KeyboardOnly")
        except Exception as e:
            if "AlreadyExists" not in str(e):
                raise
        note = "agent registered"
        try:
            await mgr.call_request_default_agent(AGENT_PATH)
            note += " (default)"
        except Exception as e:  # another default agent, e.g. a desktop applet
            note += f" (not default: {e})"
        self.agent_registered = True
        return note


# ---------------------------------------------------------------------- tool implementation

class BleTool:
    def __init__(self, bluez: _BlueZ | None = None):
        self.lock = asyncio.Lock()
        self.bluez = bluez or _BlueZ()
        self._cmd_ids = itertools.cycle(range(1, 256))
        self._seen: dict[str, tuple[str | None, Any]] = {}  # address -> (name, BLEDevice)

    async def _bonded_map(self) -> dict[str, dict[str, Any]]:
        try:
            return await asyncio.wait_for(self.bluez.devices(), 5)
        except Exception as e:
            logger.info("BlueZ device query failed: %s", e)
            return {}

    async def _scan(self, timeout_s: float) -> dict[str, tuple[Any, Any]]:
        from bleak import BleakScanner

        found = await asyncio.wait_for(BleakScanner.discover(timeout=timeout_s, return_adv=True), timeout_s + 10)
        for addr, (dev, adv) in found.items():
            self._seen[addr.upper()] = (adv.local_name or dev.name, dev)
        return found

    async def _resolve(self, query: str, scan_s: float = 8.0) -> tuple[Any, str, str | None]:
        """(BLEDevice or address str, address, name). Scans if the device was not seen recently."""
        from bleak import BleakScanner

        bonded = await self._bonded_map()
        cands = [(a, n) for a, (n, _) in self._seen.items()] + [(a, d["name"]) for a, d in bonded.items()]
        addr = match_device(cands, query)
        if addr is None:
            await self._scan(scan_s)
            cands = [(a, n) for a, (n, _) in self._seen.items()]
            addr = match_device(cands, query)
        if addr is None:
            raise BleError(f"no BLE device matching {query!r} found (scanned {scan_s:.0f} s); is it powered and "
                           "advertising? Another central (phone/PWA) connected to it stops its advertising.")
        name = self._seen.get(addr, (None, None))[0] or bonded.get(addr, {}).get("name")
        dev = await asyncio.wait_for(BleakScanner.find_device_by_address(addr, timeout=scan_s), scan_s + 5)
        if dev is None:
            raise BleError(f"{addr} ({name}) not advertising right now (scanned {scan_s:.0f} s)")
        return dev, addr, name

    async def _connect(self, dev, timeout_s: float = 20.0):
        from bleak import BleakClient

        client = BleakClient(dev, timeout=timeout_s)
        await asyncio.wait_for(client.connect(), timeout_s + 5)
        return client

    @staticmethod
    async def _disconnect(client) -> None:
        if client is None:
            return
        try:
            await asyncio.wait_for(client.disconnect(), 8)
        except Exception as e:
            logger.info("disconnect failed: %s", e)

    @staticmethod
    async def _read(client, uuid: str, timeout_s: float = 8.0) -> bytes:
        try:
            return bytes(await asyncio.wait_for(client.read_gatt_char(uuid), timeout_s))
        except asyncio.TimeoutError:
            raise BleError(f"read {uuid[4:8]} timed out after {timeout_s:.0f} s (no bond? an encrypted read "
                           "without a bond hangs; run ble_pair with the pairing window open)") from None

    async def _require_bond(self, addr: str) -> dict[str, Any] | None:
        bonded = await self._bonded_map()
        d = bonded.get(addr)
        if d is not None and not d["paired"]:
            raise BleError(f"{addr} is not bonded with this PC; run ble_pair first (open 'BLE koppeln' on the device)")
        return d

    # -- tools
    async def scan(self, timeout_s: float) -> dict[str, Any]:
        async with self.lock:
            found = await self._scan(timeout_s)
            bonded = await self._bonded_map()
            out = []
            for addr, (dev, adv) in found.items():
                name = adv.local_name or dev.name
                if not is_webasto(name, adv.service_uuids):
                    continue
                b = bonded.get(addr.upper(), {})
                out.append({"address": addr.upper(), "name": name, "rssi": adv.rssi,
                            "bonded": b.get("paired", False), "connected_to_this_pc": b.get("connected", False)})
            seen = {d["address"] for d in out}
            not_seen = [{"address": a, "name": d["name"], "bonded": d["paired"], "connected_to_this_pc": d["connected"]}
                        for a, d in bonded.items() if a not in seen and is_webasto(d["name"], d["uuids"])]
            out.sort(key=lambda d: -(d["rssi"] or -999))
            return {"advertising": out, "known_to_bluez_not_advertising": not_seen}

    async def pair(self, query: str, passkey: int | str | None) -> dict[str, Any]:
        async with self.lock:
            pk, source = resolve_passkey(passkey)
            dev, addr, name = await self._resolve(query)
            pre = (await self._bonded_map()).get(addr)
            if pre and pre["paired"]:
                return {"ok": True, "address": addr, "name": name, "already_bonded": True,
                        "message": "already bonded; ble_unpair first to pair again"}
            agent_note = await asyncio.wait_for(self.bluez.ensure_agent(pk), 10)
            self.bluez.agent_log.clear()
            client = None
            step = "connect"
            try:
                client = await self._connect(dev)
                step = "pair"
                await asyncio.wait_for(client.pair(), 40)
                step = "verify (read info)"
                await asyncio.sleep(0.5)
                if not client.is_connected:
                    raise BleError("device dropped the connection right after pairing: its pairing window is "
                                   "probably closed (menu 'BLE koppeln', 120 s) or the passkey is wrong")
                info = P.decode_info(await self._read(client, P.INFO_UUID, 10))
                return {"ok": True, "address": addr, "name": name, "passkey_source": source, "agent": agent_note,
                        "agent_log": list(self.bluez.agent_log), "info": info}
            except Exception as e:
                reason = f"{step}: {type(e).__name__}: {e}"
                if step == "verify (read info)" and client is not None and not client.is_connected:
                    reason += " (device disconnected: pairing window closed?)"
                removed = False
                post = (await self._bonded_map()).get(addr)
                if post and post["paired"]:  # half bond on our side only: drop it, it would not work
                    try:
                        await self._disconnect(client)
                        client = None
                        await self.bluez.remove_device(post["path"])
                        removed = True
                    except Exception as e2:
                        reason += f"; removing stale bond failed: {e2}"
                return {"ok": False, "address": addr, "name": name, "reason": reason, "passkey_source": source,
                        "agent_log": list(self.bluez.agent_log), "stale_bond_removed": removed,
                        "hint": "open the pairing window on the device (menu 'BLE koppeln', 120 s), then retry"}
            finally:
                await self._disconnect(client)

    async def unpair(self, query: str) -> dict[str, Any]:
        async with self.lock:
            bonded = await self._bonded_map()
            cands = [(a, d["name"]) for a, d in bonded.items()]
            addr = match_device(cands, query)
            if addr is None or addr not in bonded:
                return {"ok": False, "reason": f"{query!r} not known to BlueZ", "known": [c for c in cands]}
            await asyncio.wait_for(self.bluez.remove_device(bonded[addr]["path"]), 10)
            self._seen.pop(addr, None)
            return {"ok": True, "address": addr, "name": bonded[addr]["name"], "was_paired": bonded[addr]["paired"]}

    async def read(self, query: str) -> dict[str, Any]:
        async with self.lock:
            dev, addr, name = await self._resolve(query)
            await self._require_bond(addr)
            client = None
            try:
                client = await self._connect(dev)
                info = P.decode_info(await self._read(client, P.INFO_UUID))
                status = decode_status_safe(await self._read(client, P.STATUS_UUID))
                return {"address": addr, "name": name, "info": info, "status": status}
            finally:
                await self._disconnect(client)

    async def command(self, query: str, op: str | int, target_c: int | None, duration_min: int | None,
                      timeout_s: float) -> dict[str, Any]:
        async with self.lock:
            cmd_id = next(self._cmd_ids)
            payload = P.encode_command(op, cmd_id, target_c, duration_min)
            dev, addr, name = await self._resolve(query)
            await self._require_bond(addr)
            results: asyncio.Queue[bytes] = asyncio.Queue()
            statuses: list[tuple[float, bytes]] = []
            t0 = time.monotonic()
            client = None
            try:
                client = await self._connect(dev)
                await asyncio.wait_for(client.start_notify(P.RESULT_UUID, lambda _c, d: results.put_nowait(bytes(d))), 10)
                await asyncio.wait_for(
                    client.start_notify(P.STATUS_UUID, lambda _c, d: statuses.append((time.monotonic() - t0, bytes(d)))), 10)
                await asyncio.wait_for(client.write_gatt_char(P.COMMAND_UUID, payload, response=True), 10)
                seq = await collect_results(results, cmd_id, timeout_s)
                final_status = None
                try:
                    final_status = decode_status_safe(await self._read(client, P.STATUS_UUID))
                except Exception as e:
                    final_status = {"error": str(e)}
                return {
                    "address": addr, "name": name, "op": P.OP_NAME[payload[0]], "payload": payload.hex(" "),
                    **seq,
                    "status_notifications": [
                        {"t_s": round(t, 2), "phase": s.get("phase"), "error": s.get("error", {}).get("name")
                         if isinstance(s.get("error"), dict) else s.get("error"),
                         "climate": s.get("climate", {}).get("status") if isinstance(s.get("climate"), dict) else None,
                         "last_cmd": s.get("last_cmd")}
                        for t, s in ((t, decode_status_safe(b)) for t, b in dedup_consecutive(statuses))
                    ],
                    "final_status": final_status,
                }
            finally:
                await self._disconnect(client)

    async def watch(self, query: str, seconds: float) -> dict[str, Any]:
        async with self.lock:
            dev, addr, name = await self._resolve(query)
            await self._require_bond(addr)
            statuses: list[tuple[float, bytes]] = []
            client = None
            try:
                client = await self._connect(dev)
                t0 = time.monotonic()
                first = await self._read(client, P.STATUS_UUID)
                statuses.append((0.0, first))
                await asyncio.wait_for(
                    client.start_notify(P.STATUS_UUID, lambda _c, d: statuses.append((time.monotonic() - t0, bytes(d)))), 10)
                await asyncio.sleep(seconds)
                uniq = dedup_consecutive(statuses)
                return {"address": addr, "name": name, "seconds": seconds,
                        "notifications": len(statuses) - 1, "distinct": len(uniq),
                        "sequence": [{"t_s": round(t, 2), **decode_status_safe(b)} for t, b in uniq]}
            finally:
                await self._disconnect(client)


async def guarded(coro: Awaitable[dict[str, Any]], timeout_s: float) -> dict[str, Any]:
    """Outer hard timeout for a whole BLE tool call."""
    try:
        return await asyncio.wait_for(coro, timeout_s)
    except asyncio.TimeoutError:
        return {"ok": False, "error": f"BLE operation exceeded {timeout_s:.0f} s and was cancelled"}
    except BleError as e:
        return {"ok": False, "error": str(e)}
    except ImportError as e:
        return {"ok": False, "error": f"bleak/dbus-fast not installed: {e}"}
    except Exception as e:
        logger.warning("BLE error: %s", e, exc_info=True)
        return {"ok": False, "error": f"{type(e).__name__}: {e}"}
