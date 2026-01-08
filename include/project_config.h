#pragma once

// -------------------------
// Board pinout (TTGO LoRa32-OLED V1.0)
// -------------------------
// Sourced from ESPBoards TTGO LoRa32-OLED pinout table:
// - OLED (V1.0 only): RST=GPIO16, SDA=GPIO4, SCL=GPIO15
// - LoRa RST (V1.0 only): GPIO14
// Other LoRa SPI pins are consistent across revisions:
// - LoRa SPI: SCK=5, MOSI=27, MISO=19, CS=18, DIO0=26
// https://www.espboards.dev/esp32/ttgo-lora32/

// LoRa
#ifndef LORA_SCK
#define LORA_SCK 5
#endif
#ifndef LORA_MISO
#define LORA_MISO 19
#endif
#ifndef LORA_MOSI
#define LORA_MOSI 27
#endif
#ifndef LORA_CS
#define LORA_CS 18
#endif
#ifndef LORA_RST
#define LORA_RST 14
#endif
#ifndef LORA_DIO0
#define LORA_DIO0 26
#endif

// OLED (SSD1306 I2C)
#ifndef OLED_SDA
#define OLED_SDA 4
#endif
#ifndef OLED_SCL
#define OLED_SCL 15
#endif
#ifndef OLED_RST
#define OLED_RST 16
#endif

// -------------------------
// LoRa radio configuration
// -------------------------
#ifndef LORA_FREQUENCY_HZ
// Must be provided via build flag. Example: -D LORA_FREQUENCY_HZ=868E6
#error "LORA_FREQUENCY_HZ is not defined. Set it in platformio.ini build_flags."
#endif

#ifndef LORA_SYNC_WORD
#define LORA_SYNC_WORD 0x12
#endif

#ifndef LORA_BW
#define LORA_BW 125E3
#endif

#ifndef LORA_SF
#define LORA_SF 7
#endif

#ifndef LORA_CR
// LoRa library uses coding rate denominator 5..8 (4/5 .. 4/8)
#define LORA_CR 5
#endif

// -------------------------
// Application configuration
// -------------------------
// Addressing based on webasto_wbus.txt (Thermo Top V example):
// - Diagnosis/controller address = 0xF, Heater address = 0x4
// Header byte = (src<<4) | dst
#define WBUS_ADDR_CONTROLLER 0xF
#define WBUS_ADDR_HEATER 0x4

// Default duration used by "start" if no prior "run <minutes>" command has been sent.
#ifndef DEFAULT_RUN_MINUTES
#define DEFAULT_RUN_MINUTES 30
#endif

// -------------------------
// Low-power / latency tuning
// -------------------------
// With a sleeping receiver, the sender may need to retry a command until the receiver
// wakes up and responds with a status update that includes lastCmdSeq.

// Receiver: how long to keep LoRa in RX after waking (ms)
#ifndef RX_IDLE_LISTEN_WINDOW_MS
#define RX_IDLE_LISTEN_WINDOW_MS 400
#endif

// Receiver: deep-sleep interval when heater is OFF/idle (ms)
#ifndef RX_IDLE_SLEEP_MS
#define RX_IDLE_SLEEP_MS 4000
#endif

// Sender: how long to retry a command waiting for correlated status ACK (ms)
#ifndef SENDER_CMD_ACK_TIMEOUT_MS
#define SENDER_CMD_ACK_TIMEOUT_MS 10000
#endif

// Sender: resend interval while waiting for ACK (ms)
#ifndef SENDER_CMD_RETRY_INTERVAL_MS
#define SENDER_CMD_RETRY_INTERVAL_MS 1000
#endif

// W-BUS UART (ESP32 Serial2 default pins are board-specific; we define explicit pins)
// NOTE: W-BUS is a single-wire 12V K-line style bus. You MUST use a proper interface
// (e.g., NXP MC33660/MC33290 or a transistor level-shifter/open-collector circuit).
// These pins are the ESP32 side of that interface.
#ifndef WBUS_TX_PIN
#define WBUS_TX_PIN 17
#endif
#ifndef WBUS_RX_PIN
#define WBUS_RX_PIN 34
#endif

// Optional: if your interface needs a GPIO to enable TX (transceiver enable).
#ifndef WBUS_EN_PIN
#define WBUS_EN_PIN -1
#endif

// Should we emit a BREAK low pulse before the first command?
// webasto_wbus.txt notes many heaters require a ~50ms break.
#ifndef WBUS_SEND_BREAK
#define WBUS_SEND_BREAK 1
#endif

// LoRa addressing (simple point-to-point)
#ifndef LORA_NODE_SENDER
#define LORA_NODE_SENDER 1
#endif
#ifndef LORA_NODE_RECEIVER
#define LORA_NODE_RECEIVER 2
#endif
