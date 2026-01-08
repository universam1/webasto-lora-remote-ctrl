# TTGO LoRa32-OLED V1.0 pin mapping (SX1276/SX1278 + SSD1306)

This repo targets the **TTGO LoRa32-OLED V1.0** (ESP32 + SX1276/SX1278 + 0.96" SSD1306 I2C OLED).

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

## Concept update (battery-power, 10s acceptable command latency)

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

