# Webasto device debug MCP server

MCP server (`webasto-device-debug`, package `webasto_serial_debug`) that lets Claude Code debug the
project's ESP32 boards over USB serial and BLE.

| Alias | Port | BLE name |
|---|---|---|
| `receiver` | `/dev/ttyWEBASTORECV` (USB path 1.4) | `Webasto-RX-89CA` |
| `sender` | `/dev/ttyWEBASTOSEND` (USB path 1.2), fallback `/dev/ttyUSB0` if that is a CP2102 no other alias uses | `Webasto-TX-89F2` |
| `simulator` | `/dev/ttyWEBASTOSIM` (USB path 1.1) | |
| `cansniffer` | `/dev/ttyCANSNIFF` | listed only, use the `webasto-can-capture` server |

The symlinks come from `../99-webasto-devices.rules`.

## Setup

```bash
cd mcp-serial-debug
uv venv venv
uv pip install --python venv/bin/python -e '.[dev]'
venv/bin/python -m pytest -q
```

`.mcp.json` in the repository root registers the server:

```json
"webasto-device-debug": {
  "command": "/home/sam/src/webasto-lora-remote-ctrl/mcp-serial-debug/venv/bin/python",
  "args": ["-m", "webasto_serial_debug"]
}
```

## Serial tools

| Tool | What it does |
|---|---|
| `list_devices()` | Aliases with resolved port, USB path, presence, VID:PID, and whether this server holds the port; other ttyUSB/ttyACM. Opens nothing. |
| `attach(device, reset=False)` | Opens the port without resetting the board and buffers the log (5000 lines, host timestamps). `reset=True` resets after opening. |
| `detach(device)` / `detach_all()` | Release the port(s). |
| `read_log(device, lines=50, since_s=None, grep=None)` | Newest buffered lines, optionally only the last N seconds and/or a regex filter. Each line shows its age. |
| `send_command(device, text, wait_for=None, timeout_s=10)` | Writes a line. With `wait_for` it waits for a new matching line and returns it with everything in between. |
| `reset_device(device, wait_for=None, timeout_s=15)` | EN pulse, returns the first ~40 boot lines or everything up to `wait_for`. |
| `health(device, window_s=60)` | Boots (`rst:`/banner), crash lines (Guru Meditation, Backtrace, abort, brownout), WiFi/MQTT state, last `[rt] stack` watermark per task, recent errors, serial read errors and reopens. |

`device` is an alias or a `/dev/...` path. `read_log`, `send_command`, `reset_device` and `health`
attach on demand (without reset).

Sender serial commands: `start | stop | run <min> | climate <C> <min> | climateoff | query`. Useful
waits: `\[JOB\] done`, `\[LORA\] recv`, `WEBASTO LORA RECEIVER`, `\[WiFi\] Connected`.

### No reset on attach

The CP2102 auto-reset circuit wires RTS to EN and DTR to IO0, and a plain open pulses both, which
resets the ESP32. This server sets `dtr=False` and `rts=False` on the pyserial object before
`open()`, so attaching does not touch the board. `reset_device` (and `attach(reset=True)`) pulses RTS
with DTR held low, so the board boots normally instead of entering download mode.

Ports are opened exclusively (flock + `TIOCEXCL`). If another program holds a port, `attach` fails
with "busy".

The reader thread survives transient USB errors such as "device reports readiness to read but
returned no data": it closes the port, waits 0.5 s (backing off to 5 s while the device is gone), and
reopens without a reset. `health` reports the error and reopen counts, and the log shows an
`[mcp] ... reopened` note.

### Detach before flashing

While attached, this server holds the port, so `pio run -t upload` and `pio device monitor` fail.
Call `detach_all()` (or `detach(device)`) first, then attach again after flashing.

## BLE tools

| Tool | What it does |
|---|---|
| `ble_scan(timeout_s=8)` | Webasto devices (name `Webasto-*` or service UUID) with address, name, RSSI and bond state; also bonded devices that are not advertising. |
| `ble_pair(name_or_address, passkey=None)` | Bonds this PC with the device through a BlueZ passkey agent. |
| `ble_unpair(name_or_address)` | Removes the bond on this PC. |
| `ble_read(name_or_address)` | Reads and decodes info (role, transport, fw) and status (phase, flags, error, temperatures, voltage, odometer, climate session, LoRa link, last command). |
| `ble_command(name_or_address, op, target_c=None, duration_min=None, timeout_s=25)` | `heat`, `stop`, `climate_set`, `climate_off`, `query` (or 1..5). Returns the result stages (queued, sent, acked, done/failed with CmdResult name), the status notifications seen and the final status. |
| `ble_watch(name_or_address, seconds=10)` | Decoded status sequence from notifications, consecutive duplicates removed. |

`name_or_address` is `receiver`, `sender`, the full BLE name, a unique part of it (`RX`, `89F2`) or a
MAC. The decoders live in `webasto_serial_debug/profile.py` and mirror `lib/webasto_core/ble_profile.h`
and `pwa/parser.js`.

BLE calls run one at a time on the server's asyncio loop. Every step has its own timeout, every call
has an overall limit, and the connection is always closed at the end. An encrypted read without a
bond would otherwise hang for about 30 s. A device that a phone or the PWA is connected to stops
advertising, so it will not show up in a scan.

### Pairing

- **Pairing window:** the boards only accept a new bond while their pairing window is open (menu
  "BLE koppeln", 120 s). Outside it they drop the connection right after pairing, and `ble_pair`
  reports that and removes the half-made bond on the PC.
- **Passkey source, in this order:** the `passkey` parameter, env `WEBASTO_BLE_PASSKEY`, the
  `BLE_PASSKEY` define in `../include/credentials.h` (the server reads only that line), and finally
  the firmware default 123456.
- A BlueZ agent (KeyboardOnly) on the system D-Bus answers the passkey request. It is registered once
  per server process and becomes the default agent if no other one is.

`ble_read`, `ble_command` and `ble_watch` refuse to run if BlueZ knows the device but it is not
bonded.

## Tests

`venv/bin/python -m pytest -q` runs without hardware:

- `test_profile.py`: status/info/result decoding, command encoding, names, including a real
  receiver status capture.
- `test_serial_pty.py`: a fake ESP32 on a pseudo-terminal: attach without reset, grep/since, send
  with wait_for, reset pulse, health counts, recovery after read errors. A guard fails any test that
  tries to open a non-pty port.
- `test_ble_logic.py`: name/address matching, passkey source, result-stage collection, and a full
  command/read/watch flow against a fake client.
- `test_stdio.py`: starts the server over stdio and lists its tools.
