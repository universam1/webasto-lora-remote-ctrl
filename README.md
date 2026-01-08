# Webasto ThermoTop C LoRa Remote (TTGO LoRa32 SX1276 OLED)

Two TTGO LoRa32 SX1276 OLED boards:

- **Sender**: sends `start`, `stop`, `run <minutes>` over LoRa; shows receiver/heater status on OLED.
- **Receiver**: receives commands, controls the heater over **W‑BUS**, reports status back over LoRa; shows state on OLED.

## What’s implemented

- LoRa P2P link using `sandeepmistry/LoRa`
- OLED UI using `U8g2` (SSD1306 I2C)
- W‑BUS framing for:
  - `0x21` **Parking heater on** (1 byte: minutes)
  - `0x10` **Shutdown**
  - `0x50` index `0x07` **Operating state read**

The W‑BUS packet structure and commands above come from `webasto_wbus.txt` in H4jen/webasto.

## Hardware notes (W‑BUS is NOT TTL)

W‑BUS is a **single-wire 12V K‑line style bus** (open collector) using **2400 baud, 8E1 (even parity)**.

You must use a proper interface between the ESP32 and W‑BUS, for example:

- a dedicated transceiver (e.g. NXP `MC33660` / `MC33290`), or
- an open‑collector transistor level-shifter circuit.

This project talks to that interface via ESP32 `Serial2` using the pins in `include/project_config.h`.

## Pinout defaults

Defaults are set for **TTGO LoRa32-OLED V1.0**:

- LoRa: `SCK=5`, `MISO=19`, `MOSI=27`, `CS=18`, `RST=14`, `DIO0=26`
- OLED I2C: `SDA=4`, `SCL=15`, `RST=16`

If your board revision differs, adjust `include/project_config.h`.

## Build & flash

Open this folder in VS Code with PlatformIO installed.

- Build/Upload sender: select environment `sender`
- Build/Upload receiver: select environment `receiver`

Or CLI:

- `pio run -e sender -t upload`
- `pio run -e receiver -t upload`

## Bench W-BUS simulator (ESP32 Dev Module)

This repo includes a third firmware target:

- **simulator**: an ESP32 Dev Module that emulates a ThermoTop-like W-BUS device over **TTL UART**.

This is intended for bench testing the **receiver** without a real heater.

Wiring (TTL UART, cross TX/RX + common GND):

- TTGO receiver `WBUS_TX_PIN` (GPIO17) -> simulator `WBUS_RX_PIN` (GPIO16)
- simulator `WBUS_TX_PIN` (GPIO17) -> TTGO receiver `WBUS_RX_PIN` (GPIO34)

Build/upload:

- `pio run -e simulator -t upload`

The simulator responds to the receivers existing requests:

- `0x21` start (minutes)
- `0x10` stop
- `0x50` index pages (including `0x07` operating state)
- `0x50 0x30 ...` multi-status snapshot

## Configure LoRa frequency

Set your region’s band in `platformio.ini`:

- `-D LORA_FREQUENCY_HZ=433E6` or `868E6` or `915E6`

Both sender and receiver must match.

## Sender usage

Open Serial Monitor at `115200` and type:

- `run 30`
- `start`
- `stop`

`start` uses the last `run <minutes>` value (defaults to `DEFAULT_RUN_MINUTES`).

## Receiver status polling

Receiver polls W‑BUS operating state every ~2 seconds (command `0x50` index `0x07`) and sends status back over LoRa.

## Safety

You are controlling a fuel-burning heater. Validate wiring, fusing, and interlocks, and test safely.
