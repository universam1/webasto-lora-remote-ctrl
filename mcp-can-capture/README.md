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
| `set_heater_mode(mode, max_frames=10, radio_emulation=False, radio_wake_bit=True, radio_frames=5)` | Switch the heater to `off`, `heat` or `vent` through the roof control unit (0x5D3) and verify it. Same TX gating as `send_frame`. Mirrors bit 7 (0x80) of the panel's current 0x5D3 byte (from a panel frame younger than 2 s, or after one wake frame on a sleeping bus, waiting up to 600 ms). Sends one frame per 500 ms with the device in normal mode for the whole sequence, and stops as soon as a panel 0x5D3 carries the target byte. After `max_frames` it reports `ok: false`, "not adopted by roof unit". Optionally confirms with 0x3E5 (heat: control LED, vent: byte 7 bit 0, off: both clear). Returns JSON with `ok`, `mode`, `target_byte`, `panel_before`, `panel_after`, `frames_sent`, `adopted_after_ms`, `heater_confirmed`, `message`, `radio_emulation` and `timeline` (`[t_ms, id, dir, hex]` for 0x5D3/0x3E5 from the first Tx, at most about 60 entries). See "Radio-start emulation" below for `radio_emulation`. |
| `enable_status_spoof(confirm)` | Separate session arm for spoofing the heater's **own** status frame 0x3E5 (`radio_emulation`, `radio_impulse`), needed in addition to `enable_tx`. Only call with `confirm=true` after the user explicitly approved it. `enable_tx(false)` and `disconnect` disarm it too. Sends nothing by itself. |
| `radio_impulse(kind, wake_bit=True, frames=5, observe_s=3, vent_bits=False)` | Emulate a VW radio remote ON/OFF impulse (`kind` = `on`/`off`) by spoofing 0x3E5 only, no 0x5D3. Needs `enable_tx` and `enable_status_spoof`. See below. |

## Radio-start emulation (experimental, spoofs 0x3E5)

Vent-on over 0x5D3 is never adopted by the roof unit: the heater does ventilate (0x3E5 byte 1 =
`09`, byte 7 = `01`), but the panel keeps sending `80`. Hypothesis: when a Telestart radio remote
starts the heater, the heater reports it in 0x3E5 byte 0 (b0 on via radio, b1 off via radio, b2
heat, b3 vent, b7 wake Komfort-CAN), and the roof unit adopts the external start from those bits.
Both tools below test this by **transmitting frames under the heater's own status ID 0x3E5**.
They are refused unless `enable_tx(confirm=true)` **and** `enable_status_spoof(confirm=true)` were
called this session; only arm the latter after the user explicitly approved it.

`set_heater_mode(..., radio_emulation=true)` runs the normal 0x5D3 sequence and additionally:

- waits after the first 0x5D3 up to 1.5 s for a **real** 0x3E5 confirming the target (vent:
  byte 7 b0, heat: byte 1 b1, off: both clear). Without one it sends no spoof and says so;
- then sends up to `radio_frames` (1..20) frames `3E5 <radio bits> <bytes 1..7 of that real
  frame>`, each about 100 ms after a confirming real 0x3E5 (halfway between the heater's 200 ms
  frames), at most one per real-frame interval, and only after a real frame that still confirms
  the target, so blower, LED, coolant and fuel stay consistent;
- radio bits in byte 0: vent `09`, heat `05`, off `02`, each plus `80` if `radio_wake_bit`;
- keeps the 0x5D3 schedule in the same loop and stops 0x5D3 and spoofing at the panel echo.

The result adds `spoof_frames_sent`, `spoof_payload` and `heater_confirmed_before_spoof`.

`radio_impulse(kind, vent_bits=True)` (only for `on`, experimental) makes the spoofed frames also
claim active ventilation:
- byte 0 b3 = explicit ventilation via radio, so byte 0 = `89`
- byte 1 b0 = vehicle blower on, the heater's request to the Climatic
- byte 7 b0 = ventilation active

The real heater keeps sending these bits as 0 every 200 ms in between.

`radio_impulse(kind)` models the VW hand transmitter, which only sends an ON (or OFF) impulse;
the roof unit picks mode and run time itself (menu FERNBED). It sends no 0x5D3 command, only
`frames` (5) spoofed 0x3E5 with byte 0 = `01` (on) or `02` (off), plus `80` if `wake_bit`, bytes
1..7 copied from the latest real 0x3E5, with the same spacing as above. On a sleeping bus (no
frame for 2 s) it first sends one neutral 0x5D3 equal to the panel's last value (default `80`)
and waits up to 1 s for a real 0x3E5; without one it sends no spoof. The device then returns to
listen-only and the bus is observed for `observe_s` (3 s). Result: `ok` (the panel changed its
0x5D3 on its own to a non-off mode for `on`, to off for `off`), `panel_before`, `panel_after`,
`panel_changed`, `panel_changed_after_ms`, `heater_before`/`heater_after` (derived state, plus
`*_raw`), `spoof_frames_sent`, `spoof_payload`, `wake_frame`, `timeline` and `message`.

All Tx frames (0x5D3 and 0x3E5) go into the active capture with `Dir=Tx`. Start a capture first
so the car test can be analysed afterwards.

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

## Dashboard

A small local web UI for the heater: state (Aus / Heizen / Lüften / Nachlauf), coolant
temperature, heating power (estimated kW and l/h, plus the raw value), fuel used, fuel and water
pump, error code, 30-min charts, bus and sniffer health, the panel's current 0x5D3 byte including
bit 7, the 0x3E3 echo, and Heizen / Lüften / Aus buttons.

Heating power is estimated from 0x3E5 byte 4, which is linear in the fuel rate (2.41 ml/h per
step; raw 241 is about 0.58 l/h). The kW value assumes 5 kW nominal at raw 241 (20.7 W per step)
and is an estimate.
Standard library only, no CDN, so it works offline in the car.

```bash
venv/bin/python -m webasto_can_capture.dashboard --port /dev/ttyCANSNIFF [--http 127.0.0.1:8765] [--allow-tx] [--allow-status-spoof]
```

Then open http://127.0.0.1:8765/. API: `GET /api/state` (JSON, polled once per second by the
page) and `POST /api/command` with `{"mode": "heat"|"vent"|"off"}` (optionally
`"radio_emulation": true`) or `{"impulse": "on"|"off"}`.

**One program per port.** The dashboard uses the same `CanSniffer` as the MCP server and opens the
port exclusively (flock plus `TIOCEXCL`). It cannot run at the same time as the MCP server
(`connect`) or SavvyCAN. If the port is taken it exits with `port ... is busy` instead of corrupting
the other program's GVRET stream. Call `disconnect()` in the MCP server first.

**TX safety.** Without `--allow-tx` the buttons are disabled and `POST /api/command` returns 403;
the device stays listen-only. With `--allow-tx`, a command is accepted only if the firmware also
reports `tx_allowed_by_firmware` (build env `cansniffer_tx`). Each command runs `set_heater_mode`:
`5D3 <mode | panel bit 7> 00` every 500 ms until the roof unit adopts the value (usually 1–4 frames,
at most 10). The device is in normal mode only for that sequence and goes back to listen-only
afterwards, also on error. Only one command runs at a time (a second one gets 409). The last 20
commands are logged with timestamp, frame (e.g. `5D3#8200`), frames sent, adoption time and
status: `ok` (adopted), `failed` (not adopted by the roof unit) or `error`. A ventilation command
that the roof unit does not adopt (known for vent-on while the panel sends `80`) shows the hint
"Lüften wurde vom Bedienteil nicht übernommen – am Bedienteil starten". Each button asks for confirmation. A sleeping bus (no frames
about 20 s after the last activity) is shown as "schläft", not as an error; the first command frame
wakes it.

**0x3E5 status spoof.** `--allow-status-spoof` (default off, effective only together with
`--allow-tx`) shows the checkbox "Funkstart emulieren (0x3E5)" and the buttons "Funk-Impuls EIN"
and "Funk-Impuls AUS" below the command buttons. Without the flag they stay hidden and the server
answers 403 to `radio_emulation` and `impulse` requests. With the checkbox ticked, Heizen / Lüften /
Aus run `set_heater_mode(..., radio_emulation=true)`; the impulse buttons run `radio_impulse`.
Starting the dashboard with the flag counts as the user's approval, so it arms
`enable_status_spoof` itself on first use. The log shows how many spoofed 0x3E5 frames were sent
and their payload, and for impulses the heater state before and after.

The server binds to 127.0.0.1 by default and rejects requests with a foreign `Host` or
`Origin` header.

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
