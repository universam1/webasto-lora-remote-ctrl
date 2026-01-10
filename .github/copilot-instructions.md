# Copilot instructions for this repo (webastolora)

This repository contains a PlatformIO project for **two TTGO LoRa32-OLED V1.0** ESP32 boards:

- **sender**: sends commands over LoRa (start / stop / run N minutes) and shows status on OLED.
- **receiver**: receives commands, controls a Webasto ThermoTop C via **W‑BUS**, shows status on OLED, and reports status back over LoRa.

It also includes a third firmware target:

- **simulator**: an **ESP32 Dev Module** that emulates a ThermoTop-like W‑BUS device over **TTL UART** for bench testing with the receiver.

Use this file as the single source of truth for key implementation constraints.

---

## Hardware assumptions (critical)

### Board
- Target board: **TTGO LoRa32-OLED V1.0** (`ttgo-lora32-v1` in PlatformIO)
- Radio: SX1276/SX1278 class LoRa transceiver
- OLED: SSD1306 I2C 128x64

### Exact pin mapping (TTGO example verified)
Pin mapping matches the TTGO example sketch from sfambach.

LoRa (SX1276/SX1278):
- `SCK` = GPIO5
- `MISO` = GPIO19
- `MOSI` = GPIO27
- `CS/NSS` = GPIO18
- `RST` = GPIO14
- `DIO0` = GPIO26

OLED (SSD1306 I2C):
- `SDA` = GPIO4
- `SCL` = GPIO15
- `RST` = GPIO16

These are configured in `include/project_config.h` and used by `lib/common/lora_link.cpp` and `lib/common/oled_ui.cpp`.

---

## LoRa link details

- Library: `sandeepmistry/LoRa`
- **CRITICAL**: The current TTGO LoRa32 boards are **433 MHz variants**. The SX1276 chip is frequency-agile but the antenna is tuned for 433 MHz only. Using 868E6 or 915E6 will result in no signal reception (RSSI -157).
- Frequency is configured via `LORA_FREQUENCY_HZ` build flag in `platformio.ini` (currently set to `433E6`).
- PHY parameters are set in `lib/common/lora_link.cpp`:
  - `LORA_SYNC_WORD`
  - `LORA_BW` (bandwidth)
  - `LORA_SF` (spreading factor)
  - `LORA_CR` (coding rate denominator 5..8)
  - CRC enabled

### Range tuning (high-level)
- Higher `LORA_SF` and lower `LORA_BW` improve sensitivity/range but increase time-on-air.
- Higher `LORA_CR` improves robustness but reduces throughput.
- Be mindful of regional regulations (e.g., EU433 ISM band) when increasing time-on-air.

---

## W‑BUS (Webasto) constraints

### Physical layer
- **W‑BUS is not TTL UART.** It is a single-wire, automotive K-line style physical layer.
- The ESP32 must connect through a proper interface/transceiver/level shifting circuitry.

### UART framing
- `2400` baud
- `8E1` (even parity)

### Frame format and checksum
- Header is `((src & 0x0F) << 4) | (dst & 0x0F)`.
- Length counts **(payload bytes + checksum byte)** after the length field.
- Checksum is XOR of:
  - header
  - length
  - payload bytes **excluding** the checksum byte

Headers vary by address pair; do not assume fixed constants.

### Supported commands
| Command | Name | Notes |
|---------|------|-------|
| `0x10` | Stop | Stops heating/ventilation |
| `0x21` | Parking Heater | Start heating for N minutes |
| `0x22` | Ventilation | Start ventilation (fan only) for N minutes |
| `0x44` | Keep-alive | Maintains active session |
| `0x50` | Status request | Query status pages |

### Status page readers
- `readStateFlags()` → page 0x03 (heat_request, vent_request, combustion_fan, glowplug, fuel_pump, nozzle_heating)
- `readActuators()` → page 0x04 (glowplug %, fuel pump Hz, combustion fan %)
- `readCounters()` → page 0x06 (working hours, operating hours, start counter)
- `readOperatingState()` → page 0x07 (operating state code)

### Keep-alive and auto-renewal
- Commands retry up to 3 times with ACK verification
- `needsKeepAlive(nowMs)`: Returns true every 10s while command is active
- `needsRenewal(nowMs)`: Returns true when <30s remain (re-issue start command)

### Status polling styles supported
- Multi-status TLV snapshot (H4jen-style): `0x50` with subcommand `0x30` and a list of IDs.
- “Simple status pages” (Moki38-style): `0x50` with index pages `0x05`, `0x0F`, `0x02`, `0x03`, `0x06`.
- The receiver includes a fallback: if the TLV snapshot is not received/parsed, it polls simple pages and logs raw frames to Serial.

More details are documented in:
- `.github/skills/wbus/SKILL.md`

---

## Power management behavior (battery powered)

The repo implements a low-power approach that assumes up to ~10 seconds of command latency is acceptable.

### Receiver
- When heater is OFF/idle:
  - Uses **deep sleep** periodic wake
  - Opens a short LoRa RX window
  - If no command and heater is still OFF, returns to deep sleep
  - OLED is turned off (power save) while idle/sleeping
- When heater is RUNNING:
  - Stays awake (or mostly awake)
  - **OLED stays on** (per requirement)

Tuning macros (defaults in `include/project_config.h`):
- `RX_IDLE_LISTEN_WINDOW_MS`
- `RX_IDLE_SLEEP_MS`

### Sender
- Sender retries a command until it sees a correlated status response.
- Timeout/retry macros:
  - `SENDER_CMD_ACK_TIMEOUT_MS`
  - `SENDER_CMD_RETRY_INTERVAL_MS`

---

## LoRa protocol versioning (important)

- Packet structs are in `lib/common/protocol.h`.
- Protocol version is currently `kVersion = 3`.
- `StatusPayload` includes `lastCmdSeq` which acts as an ACK correlation field.
- Sender retries commands until it receives a `Status` message with `lastCmdSeq == command.seq`.

If you change packet layout:
- Bump `kVersion`.
- Keep packet size <= 64 bytes (see static_assert).
- Ensure both sender and receiver are updated together.

---

## Implementation conventions / guardrails

- Keep changes minimal and avoid introducing additional UX/pages unless explicitly requested.
- Don’t hard-code new pin mappings; use `include/project_config.h`.
- For W‑BUS parsing, favor defensive parsing: heater firmware differs; avoid desync.
- Prefer non-blocking patterns in loops; avoid long delays while the heater is running.

---

## Relevant code locations

- LoRa wrapper: `lib/common/lora_link.*`
- OLED wrapper: `lib/common/oled_ui.*`
- W‑BUS transport + parsing: `lib/common/wbus_simple.*`
- Sender firmware: `src/sender/main.cpp`
- Receiver firmware: `src/receiver/main.cpp`
- W‑BUS simulator firmware: `src/simulator/main.cpp`
- TTGO pin mapping skill note: `.github/skills/ttgo-lora32-oled/SKILL.md`
- W‑BUS skill note: `.github/skills/wbus/SKILL.md`
