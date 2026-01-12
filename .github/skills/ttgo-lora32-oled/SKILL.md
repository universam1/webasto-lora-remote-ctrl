---
name: ttgo-lora32-oled
description: Hardware pin mapping for TTGO LoRa32-OLED V1.0 board (ESP32 + SX1276/SX1278 + SSD1306 OLED). Use when configuring LoRa SPI pins, OLED I2C pins, or verifying hardware connections for this specific board.
compatibility: TTGO LoRa32-OLED V1.0 hardware only (433MHz variant)
metadata:
  author: webastolora
  board: ttgo-lora32-v1
  chip: SX1276/SX1278
  display: SSD1306 128x64 I2C
---

# TTGO LoRa32-OLED V1.0 Pin Mapping

Hardware configuration for TTGO LoRa32-OLED V1.0 board (ESP32 + SX1276/SX1278 LoRa + 0.96" SSD1306 I2C OLED).

## Source

Pin mapping cross-checked against the TTGO example sketch here:
- https://raw.githubusercontent.com/sfambach/esp32/refs/heads/master/boards/TTGO_LoRa32-Oled/src/TTGO_Examples/OLED_LoRa_Receiver/OLED_LoRa_Receiver.ino

## LoRa (SX1276/SX1278)

These pins are used for SPI + control:

- `SCK`  = GPIO5
- `MISO` = GPIO19
- `MOSI` = GPIO27
- `CS/NSS/SS` = GPIO18
- `RST` = GPIO14
- `DIO0` (IRQ / RxDone) = GPIO26

In this repo these correspond to the compile-time config:
- `LORA_SCK=5`
- `LORA_MISO=19`
- `LORA_MOSI=27`
- `LORA_CS=18`
- `LORA_RST=14`
- `LORA_DIO0=26`

### Notes

- `DIO0` is on **GPIO26**, which is an **RTC-capable** GPIO on ESP32. That means it can be used as a wake source in some sleep modes.

## OLED (SSD1306 I2C)

- `SDA` = GPIO4
- `SCL` = GPIO15
- `RST` = GPIO16

In this repo these correspond to:
- `OLED_SDA=4`
- `OLED_SCL=15`
- `OLED_RST=16`

## Interactive OLED Menu System

Both sender and receiver now feature an interactive menu controlled via **GPIO0** (the boot button on TTGO LoRa32).

### Button Behavior
| Action | Behavior |
|--------|----------|
| **Short Press (menu hidden)** | Opens menu |
| **Short Press (menu visible)** | Moves to next menu item, resets timeout |
| **Long Press ≥800ms (menu visible)** | Activates selected item, closes menu, sends command |
| **Menu Timeout (10s)** | Menu automatically closes if no activity |
| **Debounce** | 20ms mechanical debounce prevents jitter |

### Menu Items
1. **START** - Starts heating with last-set duration (default 30 min)
2. **STOP** - Stops heating
3. **RUN 10min** - Starts heating for 10 minutes
4. **RUN 20min** - Starts heating for 20 minutes
5. **RUN 30min** - Starts heating for 30 minutes
6. **RUN 90min** - Starts heating for 90 minutes

### Sender Menu Behavior
- Sends LoRa command to receiver
- Waits for acknowledgment (shows "Waiting ACK" on OLED)
- Preset duration is remembered and used by START command
- OLED shows status view normally, menu replaces status when opened

### Receiver Menu Behavior
- Sends W-BUS command directly to heater
- Immediate W-BUS response updates heater state
- Preset duration is remembered for next menu activation
- OLED shows status view normally, menu replaces status when opened

### OLED Display Modes

**Status View (normal, menu hidden)**
```
Webasto LoRa Sender        | Receiver heating status
Preset: 30min | 12.5V      | showing W-BUS state
OFF          5s ago        | Last command: 120s ago
50°C  13.2V  2200W         | Operating state: 0x05
RSSI: -95dBm SNR: 8dB      | Power: 2200W
Waiting for status...      | Temperature: 65°C
```

**Menu View (menu visible)**
```
=== MENU ===

> START
STOP
RUN 10min
RUN 20min

Long press to activate
```

### Implementation Files
- **New files**: `lib/common/menu_handler.h`, `lib/common/menu_handler.cpp`
- **Modified**: `src/sender/main.cpp`, `src/receiver/main.cpp`, `include/project_config.h`
- **Config**: `MENU_BUTTON_PIN` defined as GPIO_NUM_0 in `project_config.h`

### Button-Free Alternative
If you don't want interactive menus, the devices still work with serial commands:
- **Sender**: Serial input parses commands (start, stop, run N)
- **Receiver**: Responds to LoRa commands without requiring button interaction

---

## Concept: Battery Power & 10-Second Command Latency

Given that up to ~10 seconds of command latency is acceptable, the most battery-efficient approach is:

### Sender (handheld)

- Default state: **ESP32 deep sleep** (almost always).
- Wake on button press.
- “Fire-and-forget” command transmit:
  - Send the command **multiple times over ~10 seconds** (or fewer times if receiver listen windows are more frequent).
  - Include a command `seq`/nonce and have the receiver deduplicate so repeated packets don’t re-trigger actions.
- OLED on only during user interaction, otherwise power-saved.

### Receiver (heater side)

Two power modes:

1) **Heater OFF / idle (dominant case)**
- Use **duty-cycled receive windows**:
  - Wake periodically (timer) and listen for a short window (e.g. 200–300 ms every 5–10 s).
  - Keep the SX1276 in RX only during the window; put it to sleep otherwise.
- This yields very low average current and still catches a repeated sender command within the 10s latency budget.

2) **Heater RUNNING**
- Stay awake (or mostly awake) because you’re polling W‑BUS and reporting status.
- Per your requirement: keep the **receiver OLED on** while running.
- You can still reduce consumption by lowering OLED refresh rate and sleeping the radio between sends.

### About IRQ wake (SX1276 DIO0 → ESP32)

- It is possible to wire/design a system where SX1276 `DIO0` wakes the ESP32 (GPIO26).
- However, **if you want the radio to wake the ESP32**, the radio generally must be kept in RX (non-trivial current draw).
- With 10s acceptable latency, it is usually better to save power by sleeping both ESP32 and radio and relying on periodic RX windows + sender repeats.

