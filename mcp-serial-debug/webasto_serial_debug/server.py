"""MCP server for debugging the Webasto ESP32 boards over USB serial and BLE.

Serial: attach without resetting the board, background ring buffer, grep/since reads,
commands with wait-for-regex, reset with boot capture, health summary.
BLE: scan, pair (BlueZ agent with passkey), read/decode info + status, commands with result
stages, status watch.
"""

from __future__ import annotations

import asyncio
import json
import logging
import re
import sys
import time
from typing import Any

from mcp.server.fastmcp import FastMCP

from . import ble as B
from .serial_link import SerialManager, analyze, fmt_lines, select_lines

logging.basicConfig(level=logging.INFO, stream=sys.stderr)
logger = logging.getLogger("webasto-serial-debug")

serial_mgr = SerialManager()
ble_tool = B.BleTool()

mcp = FastMCP(
    "webasto-device-debug",
    instructions=(
        "Debug the Webasto ESP32 boards (receiver, sender, simulator). Serial: attach opens WITHOUT "
        "resetting the board; read_log/send_command/health work on a background buffer. Call detach_all "
        "before flashing with PlatformIO or opening a port elsewhere. BLE: ble_scan, ble_pair (needs the "
        "device's pairing window open: menu 'BLE koppeln'), ble_read, ble_command, ble_watch. The CAN "
        "sniffer is handled by the webasto-can-capture server."
    ),
)


def _j(obj: Any) -> str:
    return json.dumps(obj, indent=1, ensure_ascii=False, default=str)


async def _run(fn, *args) -> str:
    try:
        res = await asyncio.to_thread(fn, *args)
        return res if isinstance(res, str) else _j(res)
    except Exception as e:
        logger.warning("tool error: %s", e)
        return f"Error: {e}"


def _held_note(link) -> str:
    extra = []
    if link.read_errors:
        extra.append(f"read errors so far: {link.read_errors}, reopens {link.reopens}")
    if link.port_lost:
        extra.append(f"PORT LOST, retrying ({link.last_error})")
    return ("; ".join(extra) + "\n") if extra else ""


# ---------------------------------------------------------------------- serial tools

@mcp.tool()
async def list_devices() -> str:
    """List the known boards (receiver, sender, simulator, cansniffer) with resolved port, USB path,
    presence, VID:PID and whether this server holds the port open, plus any other ttyUSB/ttyACM.
    Does not open any port. The CAN sniffer is only listed; use the webasto-can-capture server for it."""
    return await _run(serial_mgr.list_devices)


@mcp.tool()
async def attach(device: str, reset: bool = False) -> str:
    """Open a board's serial port (115200) and start buffering its log (last 5000 lines, host timestamps).

    `device`: receiver | sender | simulator, or a /dev path. By default the board is NOT reset: DTR and
    RTS are released before open(), so EN/IO0 stay high. `reset=True` pulses EN after opening and the
    boot log lands in the buffer. Attaching an already attached device is a no-op (or just resets).
    The port is held exclusively until detach/detach_all, so flashing or `pio device monitor` fail
    meanwhile."""

    def work():
        link, new = serial_mgr.attach(device, reset)
        time.sleep(0.5 if not reset else 1.5)
        lines = link.snapshot()
        head = (f"{link.label}: {'attached' if new else 'already attached'} {link.path}"
                f"{' (reset)' if reset else ' (no reset)'}; {len(lines)} lines buffered\n")
        return head + _held_note(link) + fmt_lines(lines[-20:])

    return await _run(work)


@mcp.tool()
async def detach(device: str) -> str:
    """Release one board's serial port and stop its reader (the buffer is discarded).
    Detach before flashing it with PlatformIO (`pio run -t upload`) or opening a serial monitor."""
    return await _run(serial_mgr.detach, device)


@mcp.tool()
async def detach_all() -> str:
    """Release every serial port this server holds. Call this before flashing any board with PlatformIO
    or before another program needs the ports."""
    return await _run(lambda: "\n".join(serial_mgr.detach_all()))


@mcp.tool()
async def read_log(device: str, lines: int = 50, since_s: float | None = None, grep: str | None = None) -> str:
    """Return buffered log lines of an attached board (attaches without reset if needed).

    `lines`: at most this many of the newest matching lines (0 = all). `since_s`: only lines received
    in the last N seconds. `grep`: Python regex filter, e.g. `\\[(WiFi|MQTT)\\]` or `seq=4`.
    Each line is prefixed with its age, e.g. `[-  3.20s]`. Lines starting `[mcp]` are notes from this
    server (resets, reconnects)."""

    def work():
        link = serial_mgr.get(device)
        if grep:
            re.compile(grep)
        all_lines = link.snapshot()
        sel = select_lines(all_lines, lines, since_s, grep)
        head = f"{link.label} {link.path}: {len(sel)} of {len(all_lines)} buffered lines\n"
        return head + _held_note(link) + (fmt_lines(sel) or "(no matching lines)")

    return await _run(work)


@mcp.tool()
async def send_command(device: str, text: str, wait_for: str | None = None, timeout_s: float = 10) -> str:
    """Write one line (newline appended) to the board. With `wait_for` (regex), wait until a NEW line
    matches and return it together with all lines received in between; otherwise return the lines that
    arrived within ~1.5 s.

    Sender commands: start | stop | run <min> | climate <C> <min> | climateoff | query. Typical waits:
    `\\[JOB\\] done` (job finished, shows `ok`/failure), `\\[LORA\\] recv`."""

    def work():
        link = serial_mgr.get(device)
        rx = re.compile(wait_for) if wait_for else None
        start = link.seq
        link.write_line(text)
        if rx is None:
            time.sleep(1.5)
            got = [ln for ln in link.lines_after(start)]
            return f"sent {text!r} to {link.label}; {len(got)} lines since:\n" + (fmt_lines(got) or "(none)")
        hit = link.wait_for(rx, start, timeout_s)
        got = [ln for ln in link.lines_after(start) if hit is None or ln.seq <= hit.seq]
        if hit is None:
            return (f"sent {text!r} to {link.label}; TIMEOUT after {timeout_s:.0f} s, no line matched "
                    f"/{wait_for}/. {len(got)} lines received:\n" + (fmt_lines(got[-80:]) or "(none)"))
        return (f"sent {text!r} to {link.label}; matched: {hit.text}\n{len(got)} lines up to the match:\n"
                + fmt_lines(got[-80:]))

    return await _run(work)


@mcp.tool()
async def reset_device(device: str, wait_for: str | None = None, timeout_s: float = 15) -> str:
    """Reset the board (EN pulse via RTS, DTR low so it boots normally) and capture the boot log.
    Attaches if needed. Without `wait_for`, returns the first ~40 lines after the reset. With
    `wait_for` (regex, e.g. `WEBASTO LORA RECEIVER` or `\\[WiFi\\] Connected`), waits for it and
    returns everything up to the match."""

    def work():
        link = serial_mgr.get(device)
        start = link.seq
        link.reset()
        if wait_for:
            hit = link.wait_for(re.compile(wait_for), start, timeout_s)
            got = [ln for ln in link.lines_after(start) if hit is None or ln.seq <= hit.seq]
            status = f"matched: {hit.text}" if hit else f"TIMEOUT after {timeout_s:.0f} s waiting for /{wait_for}/"
            return f"{link.label} reset; {status}\n" + fmt_lines(got[-120:])
        deadline = time.monotonic() + timeout_s
        got = []
        while time.monotonic() < deadline:
            got = [ln for ln in link.lines_after(start) if not ln.text.startswith("[mcp]")]
            if len(got) >= 40:
                break
            time.sleep(0.2)
        if not got:
            return f"{link.label} reset; no output within {timeout_s:.0f} s (powered? right port?)"
        return f"{link.label} reset; first {min(40, len(got))} boot lines:\n" + fmt_lines(got[:40])

    return await _run(work)


@mcp.tool()
async def health(device: str, window_s: float = 60) -> str:
    """Summarize an attached board's buffered log: boots/resets (`rst:` lines and boot banners) and crash
    lines (Guru Meditation, Backtrace, abort, brownout) within `window_s`, last WiFi and MQTT state,
    last `[rt] stack` watermark per task, failed tasks, recent error lines, plus serial read errors and
    reconnects of this server's reader."""

    def work():
        link = serial_mgr.get(device)
        rep = analyze(link.snapshot(), window_s)
        rep = {
            "device": link.label, "path": link.path,
            "attached_for_s": round(time.monotonic() - link.attached_at, 1),
            "serial": {"bytes_rx": link.bytes_rx, "read_errors": link.read_errors, "reopens": link.reopens,
                       "port_lost": link.port_lost, "last_error": link.last_error or None,
                       "resets_by_tool": link.resets_by_tool},
            **rep,
        }
        return _j(rep)

    return await _run(work)


# ---------------------------------------------------------------------- BLE tools

@mcp.tool()
async def ble_scan(timeout_s: float = 8) -> str:
    """Scan for Webasto BLE devices (name prefix `Webasto-` or the profile's service UUID). Returns address,
    name, RSSI and whether this PC holds a bond, plus bonded Webasto devices that are not advertising
    (a device stops advertising while a phone/PWA is connected)."""
    t = max(1.0, min(float(timeout_s), 30.0))
    return _j(await B.guarded(ble_tool.scan(t), t + 20))


@mcp.tool()
async def ble_pair(name_or_address: str, passkey: int | None = None) -> str:
    """Pair (bond) this PC with a board. The device only accepts a NEW bond while its pairing window is
    open: on the device, open the menu 'BLE koppeln' (120 s) first, otherwise it drops the link right
    after pairing. `name_or_address`: receiver | sender, a BLE name (Webasto-RX-89CA), a unique part of
    it, or a MAC. Passkey: parameter, else env WEBASTO_BLE_PASSKEY, else BLE_PASSKEY from
    include/credentials.h, else the firmware default 123456. A BlueZ agent (KeyboardOnly) answers the
    passkey request; it is registered once per server process. Returns ok or the failure reason; a
    half-made bond is removed again."""
    return _j(await B.guarded(ble_tool.pair(name_or_address, passkey), 90))


@mcp.tool()
async def ble_unpair(name_or_address: str) -> str:
    """Remove the bond (and BlueZ's device entry) for a board on this PC. The device keeps its own bond
    entry until it is re-paired or cleared on the device."""
    return _j(await B.guarded(ble_tool.unpair(name_or_address), 20))


@mcp.tool()
async def ble_read(name_or_address: str) -> str:
    """Connect to a bonded board, read and decode info (role, transport, fw) and status (phase, flags,
    error code + name, temperatures with "--" for unknown, voltage, odometer, climate session, LoRa
    rssi/snr/link age, last command id + result name), then disconnect."""
    return _j(await B.guarded(ble_tool.read(name_or_address), 60))


@mcp.tool()
async def ble_command(name_or_address: str, op: str, target_c: int | None = None,
                      duration_min: int | None = None, timeout_s: float = 25) -> str:
    """Send a GATT command to a bonded board and follow it. `op`: heat | stop | climate_set | climate_off |
    query, or 1..5. climate_set needs `target_c` and `duration_min`. Subscribes to result + status,
    writes [op][cmdId][args], collects result notifications until stage done/failed or `timeout_s`,
    and returns the stage sequence (queued -> sent -> acked -> done/failed with CmdResult names), the
    status notifications seen meanwhile and the final decoded status. heat/stop/climate_* really switch
    the heater."""
    t = max(2.0, min(float(timeout_s), 120.0))
    return _j(await B.guarded(ble_tool.command(name_or_address, op, target_c, duration_min, t), t + 60))


@mcp.tool()
async def ble_watch(name_or_address: str, seconds: float = 10) -> str:
    """Subscribe to a bonded board's status notifications for `seconds` (max 120) and return the decoded
    sequence (the initial read plus every change; consecutive duplicates removed)."""
    s = max(1.0, min(float(seconds), 120.0))
    return _j(await B.guarded(ble_tool.watch(name_or_address, s), s + 60))


async def serve() -> None:
    logger.info("Webasto device debug MCP server starting")
    try:
        await mcp.run_stdio_async()
    finally:
        serial_mgr.detach_all()
