# Webasto CAN Capture MCP Server

Model Context Protocol (MCP) server that lets an AI agent record a car's CAN bus through an
ESP32 CAN sniffer while you operate the Webasto heater control panel (VW T5.2), and then
analyse the recordings.

The sniffer firmware speaks the **GVRET binary protocol** (the same as SavvyCAN uses) over USB
serial at 921600 baud (the CP2102 cannot do 1 Mbaud cleanly; in SavvyCAN pick 921600 or 1000000, Linux rounds it), plus two vendor commands (`F1 40` status, `F1 41` TX enable).

## Installation

```bash
cd mcp-can-capture
uv venv venv
uv pip install --python venv/bin/python -e '.[dev]'
```

The repo root contains `.mcp.json`, which registers the server as `webasto-can-capture`
(`venv/bin/python -m webasto_can_capture`), so Claude Code picks it up when started in the repo.

The default port is `/dev/ttyCANSNIFF`. Add a udev symlink for the sniffer board (see
`../99-webasto-devices.rules` for the pattern) or pass the real port, e.g. `/dev/ttyUSB0`,
to `connect`.

Captures are written to `mcp-can-capture/captures/` (git-ignored). Override with the
`WEBASTO_CAN_CAPTURE_DIR` environment variable.

## Tools

| Tool | Purpose |
|------|---------|
| `connect(port="/dev/ttyCANSNIFF", bitrate=100000)` | Open the port, release DTR/RTS, wait for boot, enter binary mode, query device info and bus count, configure can0 listen-only, return bus params and device status. |
| `disconnect()` | Stop any capture and close the port. |
| `stats(window_s=5)` | Frames/s, totals, missed frames, bus errors, controller state, TX flags, distinct IDs. Device status is polled every second. |
| `tail(n=20, id_filter=None)` | Last frames from the 200k-frame ring buffer; `id_filter` like `"3E5,5EB"`. |
| `set_bitrate(bitrate, listen_only=True)` | Reconfigure can0 (100000 for VW comfort CAN). |
| `start_capture(name)` / `stop_capture()` | Record to `captures/<name>.csv` (SavvyCAN GVRET CSV), `<name>.log` (candump `-L`) and `<name>.markers.jsonl`. |
| `mark_event(label)` | Write a marker with device timestamp and host time. Fails when no capture is active. |
| `list_captures()` | Captures with frame and marker counts. |
| `summarize(capture, start_s=None, end_s=None)` | Per ID: count, mean period, DLCs, changing bytes and their values, toggled bits. Times are seconds from the first frame. |
| `diff(capture, marker=None, window_s=5, capture_b=None)` | Window before vs after a marker, or one capture vs another. Reports IDs only on one side, rate changes > 20 %, and differing bytes/bits ranked by relevance. `marker` is a label, `label#n` for the n-th occurrence, or an index. |
| `decode_3e5(capture, max_rows=50)` | Decode 0x3E5 with the (unverified, T5.1) parking-heater layout; prints only rows where a field changes, interleaved with markers. |
| `enable_tx(confirm)` | `confirm=true` sends `F1 41 01` and arms TX for this session; `false` disarms it. |
| `send_frame(id, data_hex, extended=False, count=1, interval_ms=100)` | Transmit on can0. Refused unless the firmware reports `tx_allowed_by_firmware` **and** `enable_tx(confirm=true)` was called this session. Sent frames go into the active capture with `Dir=Tx`. |

## Workflow: find the heater on/off signals

1. `connect()` and check `stats()`: the frame rate should be well above 0 with ignition on.
   If it stays at 0 or `bus_errors` climbs, try another `set_bitrate`.
2. `start_capture("heater_onoff_1")`.
3. Wait about 10 s of idle traffic, then press the heater button on the panel and immediately
   call `mark_event("heater on")`. Wait 20–30 s.
4. Press it again and call `mark_event("heater off")`. Wait 20 s, then `stop_capture()`.
5. Analyse:
   - `diff("heater_onoff_1", marker="heater on", window_s=5)`
   - `diff("heater_onoff_1", marker="heater off", window_s=5)`
   - `decode_3e5("heater_onoff_1")` to check the T5.1 hypothesis for 0x3E5
   - `summarize("heater_onoff_1", start_s=10, end_s=40)` for a closer look
6. Repeat the recording at least once; signals that show up in both runs are the candidates.

## SavvyCAN

SavvyCAN cannot open the serial port while this server is connected (only one program can
own the port). Call `disconnect()` first if you want to use SavvyCAN live. SavvyCAN can open
the `captures/*.csv` files directly (File > Load Log File, format "GVRET").

## Tests

```bash
venv/bin/python -m pytest
```

`tests/test_server_pty.py` runs the full server logic against a fake GVRET device on a
pseudo-terminal, so no hardware is needed.
