---
name: wbus
description: Webasto W-BUS protocol implementation for ESP32. Single-wire automotive K-line style communication at 2400 baud 8E1. Use when interfacing with Webasto heaters, implementing W-BUS commands, or debugging heater communication.
compatibility: Requires W-BUS transceiver hardware (K-line or MC33290/MC33660-class) for ESP32 UART to 12V bus level shifting
metadata:
  author: webastolora
  protocol: W-BUS
  baud: 2400
---

# W‑BUS Protocol Implementation Notes

This document captures practical W-BUS protocol details for controlling Webasto heaters from ESP32 via the `WBusSimple` implementation.

## Big picture

- **W‑BUS is not TTL UART.** It is a single‑wire, automotive “K‑line style” physical layer. You need an interface/level shifter/transceiver between the ESP32 UART pins and the heater’s W‑BUS pin.
- On the wire, the payload framing is **UART at 2400 baud, 8E1** (8 data bits, even parity, 1 stop bit).
- Frames are short and checksum is simple (XOR), but **timing/physical‑layer matters** a lot (idle levels, dominant low, bus contention, etc.).

## Physical layer / wiring assumptions

- The heater W‑BUS line is typically around vehicle battery voltage (12V system). The ESP32 UART pins are 3.3V.
- Use a proper transceiver (common approaches in public projects include K‑line transceivers; some builds use NXP MC33290/MC33660‑class parts or equivalents).
- ESP32 UART TX should not drive the bus directly.
- If your interface requires it, you may need:
  - An **enable pin** to switch the bus driver on/off during TX.
  - Open‑collector/open‑drain behavior on TX.

In this repo those knobs exist in configuration:
- `WBUS_TX_PIN`, `WBUS_RX_PIN`
- `WBUS_EN_PIN` (optional)
- `WBUS_SEND_BREAK` (optional “break” pulse before first command)

## Frame format

W‑BUS messages are framed like:

- **Header**: 1 byte (source/destination encoded as nibbles)
- **Length**: 1 byte
- **Payload**: `length - 1` bytes
- **Checksum**: 1 byte (included in the payload byte count)

### Header

Header is a packed byte: high nibble is **source address**, low nibble is **destination address**.

In this repo we generate it as:

- `header = ((src & 0x0F) << 4) | (dst & 0x0F)`

Typical addressing used here:
- Controller address: `WBUS_ADDR_CONTROLLER` (commonly `0xF`)
- Heater address: `WBUS_ADDR_HEATER` (commonly `0x4`)

So the common bytes you’ll see are:
- Controller → Heater: `0xF4`
- Heater → Controller: `0x4F`

Important: some public implementations accept additional header bytes (other address pairs). In this repo we now treat “valid header” as whatever matches your configured addresses.

### Length

The **length** byte counts **(payload bytes + checksum byte)** after the length field.

If you send a command byte plus `N` data bytes:

- `length = 1 (cmd) + N (data) + 1 (checksum)`

### Checksum

Checksum is a simple XOR:

- `csum = header XOR length XOR payload_bytes...`

Where `payload_bytes...` means **everything except the checksum itself**.

So, when verifying a received frame, compute XOR across:
- header
- length
- payload bytes excluding the final checksum byte

and compare the result to the final byte.

## Serial “break” pulse

Some heaters/interfaces expect an initial “break” before the first command.

In this repo `WBUS_SEND_BREAK` triggers a one‑time sequence roughly like:
- UART off
- idle/high for a while
- drive low for a short period
- drive high for a short period
- UART on again

Exact timing is hardware dependent. If you see unreliable first‑packet behavior, this is the first knob to experiment with.

## Command/response conventions used here

Many interactions use a command byte and optionally a “sub‑index” (one data byte) to select a page of data.
### Commands

| Command | Name | Data | Notes |
|---------|------|------|-------|
| `0x10` | Stop | none | Stops heating/ventilation |
| `0x21` | Parking Heater | 1 byte (minutes) | Start heating for N minutes |
| `0x22` | Ventilation | 1 byte (minutes) | Start ventilation (fan only) for N minutes |
| `0x44` | Keep-alive | 2-3 bytes | Maintains active session |
| `0x50` | Status request | 1+ bytes (index or 0x30+IDs) | Query status pages |
### Operating state

We use:
- request: command `0x50` with index `0x07`
- response: `0xD0 0x07 <opState> ...`

`opState` is a large heater state machine. In the receiver firmware we map it coarsely into `Off` vs `Running`.

## Status polling: two styles

There are (at least) two commonly seen status mechanisms.

### 1) Multi‑status TLV snapshot (0x50 / 0x30)

This is the richer mechanism and the primary one used in this repo.

- request: `0x50` with data `0x30` followed by a list of status IDs
- response: begins `0xD0 0x30 ...` and then a sequence like `<id><value…>` repeated

The catch: the response does not always include explicit per‑field lengths, and **field sizes can vary by heater/firmware**.

Implementation in this repo:
- Parser in `WBusSimple::tryParseStatusTlv()`
- Decodes common fields we care about (temperature, voltage, power) and also keeps raw “status_XX” fields.
- Uses a defensive heuristic for some ambiguous‑length IDs to avoid desynchronizing the parse.

### 2) “Simple status pages” (0x50 / index)

Some setups (and at least one public Arduino implementation) poll a small set of fixed pages:

| Page | Contents | Response size |
|------|----------|---------------|
| `0x03` | State flags bitfield (heat_request, vent_request, combustion_fan, glowplug, fuel_pump, nozzle_heating) | 1 byte |
| `0x04` | Actuator percentages (glowplug %, fuel pump Hz, combustion fan %) | 8 bytes |
| `0x05` | Temperature, voltage, flame, heater power | ~8 bytes |
| `0x06` | Counters (working hours, operating hours, start counter) | 8 bytes |
| `0x07` | Operating state | 4 bytes |
| `0x0F` | Component values (glowplug, pump, fan - scaled) | 3 bytes |

In this repo we have readers for:
- `readStateFlags()` → page 0x03
- `readActuators()` → page 0x04
- `readCounters()` → page 0x06
- `readOperatingState()` → page 0x07

## Keep-alive and auto-renewal

Per the esphome-webasto pattern, heaters may require periodic keep-alive messages to maintain an active heating/ventilation session.

The `WBusSimple` class now tracks:
- `activeCmd`: The currently running command (0x21=heat, 0x22=vent, 0=none)
- `activeUntilMs`: When the session expires
- `lastKeepAliveMs`: When the last keep-alive was sent

Helper methods:
- `needsKeepAlive(nowMs)`: Returns true every 10 seconds while a command is active
- `needsRenewal(nowMs)`: Returns true when <30 seconds remain (should re-issue the start command)
- `setActiveCommand(cmd, minutes)` / `clearActiveCommand()`: Manage state

## Retry logic

Commands now retry up to 3 times (configurable via `kCommandRetries`) with ACK verification, matching the esphome-webasto pattern.

In this repo we added a **fallback**: if the multi‑status TLV snapshot doesn’t arrive/parse during the poll window, the receiver tries these pages and **logs the raw bytes** to Serial.

## Logging / debugging workflow

When you’re bringing up real hardware:

- Start with verifying UART config: `2400 8E1`.
- Confirm your interface wiring and idle level.
- Enable Serial logging and look for:
  - valid frames with passing XOR checksum
  - responses to `0x50 0x07` (operating state)
  - responses to `0x50 0x05` and `0x50 0x0F` (fallback pages)

If you can capture raw byte streams from your heater, you can extend `tryParseStatusTlv()` safely by:
- adding IDs to the known‑ID set
- confirming fixed sizes for your heater firmware

## Practical gotchas

- Many “it doesn’t work” problems are electrical (wrong transceiver, no common ground, incorrect pull‑ups, bus being driven too strongly, etc.).
- Some heaters require periodic keep‑alive (`0x44 …` patterns appear in public projects); in this repo there is an optional `sendKeepAlive()`.
- Don’t assume every heater supports `0x50 0x30` multi‑status; keep the simple‑page fallback available.

## Where this is implemented in this repo

- W‑BUS framing + checksum + RX state machine: `lib/common/wbus_simple.*`
- Receiver poll loop with TLV + simple‑page fallback and Serial logs: `src/receiver/main.cpp`

