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

// Status LED (onboard blue LED on TTGO LoRa32-OLED V1.0)
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN LED_BUILTIN
#endif

// Menu Button (GPIO0 on TTGO LoRa32-OLED V1.0)
#ifndef MENU_BUTTON_PIN
#define MENU_BUTTON_PIN GPIO_NUM_0
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

// LORA sets of bandwidth, spreading factor, and coding rate affect range and speed.
// Adjust these settings based on your environment and requirements.
//   ; LoRa params optimized for range:
//   ; SF11 = ~5-10 km range, ~1-2 kbps speed (recommended tradeoff)
//   ; BW125E3 = 125 kHz (standard, good sensitivity)
//   ; CR=7 = 4/7 coding rate (improved error correction for range)
//   ; For short-range high-speed: use SF7, CR5
//   ; For maximum range (extreme): use SF12, CR8
#ifdef LORA_RANGE_SHORT
#define LORA_BW 125E3
#define LORA_SF 7
#define LORA_CR 5
#endif
#ifdef LORA_RANGE_LONG
#define LORA_BW 125E3
#define LORA_SF 11
#define LORA_CR 7
#define LORA_TX_POWER_BOOST 1
#endif
#ifdef LORA_RANGE_EXTREME
#define LORA_BW 125E3
#define LORA_SF 12
#define LORA_CR 8
#define LORA_TX_POWER_BOOST 1
#endif

#ifndef LORA_BW
#define LORA_BW 125E3
#endif

#ifndef LORA_SF
// Spreading Factor (SF):
// SF7  = ~500m range, 5.5 kbps speed (short-range high-speed)
// SF11 = ~5-10km range, 1.5 kbps speed (recommended balance) - DEFAULT
// SF12 = ~15-20km range, 0.3 kbps speed (extreme range, very slow)
#define LORA_SF 11
#endif

#ifndef LORA_CR
// Coding Rate (CR): 4/N where N=5..8 (higher = better error correction)
// CR=5 (4/5) = fast, less robust
// CR=7 (4/7) = balanced - DEFAULT
// CR=8 (4/8) = slow, most robust
#define LORA_CR 7
#endif

#ifndef LORA_TX_POWER_BOOST
// Enable TX power boost (20 dBm PA_OUTPUT_PA_BOOST_PIN) for improved range.
// Set to 1 to enable, 0 to disable. Default: 0
#define LORA_TX_POWER_BOOST 0
#endif

// -------------------------
// W-BUS (Webasto) configuration
// -------------------------
// Addressing based on webasto_wbus.txt (Thermo Top V example):
// - Diagnosis/controller address = 0xF, Heater address = 0x4
// Header byte = (src<<4) | dst
#define WBUS_ADDR_CONTROLLER 0xF
#define WBUS_ADDR_HEATER 0x4

// UART pins (configurable for simulator vs receiver)
#ifndef WBUS_TX_PIN
#define WBUS_TX_PIN 17
#endif
#ifndef WBUS_RX_PIN
// GPIO25 is available on TTGO LoRa32 (GPIO16 is used by OLED_RST)
#define WBUS_RX_PIN 25
#endif
#ifndef WBUS_EN_PIN
#define WBUS_EN_PIN -1
#endif
#ifndef WBUS_SEND_BREAK
#define WBUS_SEND_BREAK 1
#endif

// -------------------------
// Application configuration
// -------------------------

// Default duration used by "start" if no prior "run <minutes>" command has been sent.
#ifndef DEFAULT_RUN_MINUTES
#define DEFAULT_RUN_MINUTES 30
#endif

// -------------------------
// Battery monitoring (TTGO LoRa32-OLED V1.0)
// -------------------------
// TTGO LoRa32 V1.0 exposes the battery via a resistor divider to ADC GPIO35.
// The divider is typically ~2:1, so the voltage at the pin is ~VBAT/2.
// You can tweak VBAT_DIVIDER_RATIO and VBAT_CALIBRATION to calibrate readings.
#ifndef VBAT_ADC_PIN
#define VBAT_ADC_PIN 35
#endif
#ifndef VBAT_DIVIDER_RATIO
#define VBAT_DIVIDER_RATIO 2.0f
#endif
#ifndef VBAT_CALIBRATION
#define VBAT_CALIBRATION 1.0f
#endif
#ifndef VBAT_UPDATE_INTERVAL_MS
#define VBAT_UPDATE_INTERVAL_MS 1000
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

// LoRa addressing (simple point-to-point)
#ifndef LORA_NODE_SENDER
#define LORA_NODE_SENDER 1
#endif
#ifndef LORA_NODE_RECEIVER
#define LORA_NODE_RECEIVER 2
#endif

// ============================================================================
// MQTT/HomeAssistant Configuration
// ============================================================================

// Feature flag - enable MQTT support
// This can be defined via build flags in platformio.ini, or uncomment here
// #define ENABLE_MQTT_CONTROL

#ifdef ENABLE_MQTT_CONTROL

// WiFi & MQTT credentials - Include from separate file (not in git)
// Copy include/credentials.h.template to include/credentials.h and fill in your credentials
#include "credentials.h"

// WiFi connection parameters
#ifndef MQTT_WIFI_TIMEOUT_MS
#define MQTT_WIFI_TIMEOUT_MS 10000        // Max 10s to connect
#endif
#ifndef MQTT_WIFI_RETRY_INTERVAL_MS
#define MQTT_WIFI_RETRY_INTERVAL_MS 60000 // Retry every 60s if failed
#endif

// NOTE: MQTT broker credentials are now in credentials.h (not in git)

// MQTT topics
#ifndef MQTT_TOPIC_BASE
#define MQTT_TOPIC_BASE "webasto/receiver"
#endif
#ifndef MQTT_DISCOVERY_PREFIX
#define MQTT_DISCOVERY_PREFIX "homeassistant"
#endif

// Command validation
#ifndef MQTT_CMD_MAX_AGE_SEC
#define MQTT_CMD_MAX_AGE_SEC 3600  // Reject commands older than 1 hour
#endif

// Status publishing interval
#ifndef MQTT_STATUS_INTERVAL_MS
#define MQTT_STATUS_INTERVAL_MS 30000  // Publish status every 30s
#endif

// ============================================================================
// Phase 6: Additional Diagnostic Sensors
// ============================================================================

// Enable/disable diagnostic sensors (can be defined via build flags)
// #define MQTT_ENABLE_DIAGNOSTIC_SENSORS

#ifdef MQTT_ENABLE_DIAGNOSTIC_SENSORS
// Diagnostic sensor publishing interval (less frequent than status)
#ifndef MQTT_DIAGNOSTIC_INTERVAL_MS
#define MQTT_DIAGNOSTIC_INTERVAL_MS 60000  // Publish diagnostics every 60s
#endif
#endif

// ============================================================================
// Phase 7: OTA Updates
// ============================================================================

// Enable/disable OTA updates (can be defined via build flags)
// #define MQTT_ENABLE_OTA

#ifdef MQTT_ENABLE_OTA
// OTA check interval (check for updates every 6 hours)
#ifndef OTA_CHECK_INTERVAL_MS
#define OTA_CHECK_INTERVAL_MS 21600000  // 6 hours
#endif

// OTA topic for triggering updates
#ifndef OTA_UPDATE_TOPIC
#define OTA_UPDATE_TOPIC "webasto/receiver/ota/update"
#endif

// OTA status topic
#ifndef OTA_STATUS_TOPIC
#define OTA_STATUS_TOPIC "webasto/receiver/ota/status"
#endif
#endif // MQTT_ENABLE_OTA

#endif // ENABLE_MQTT_CONTROL
