#include <Arduino.h>
#include <esp_sleep.h>
#include <time.h>

#include "project_config.h"
#include "lora_link.h"
#include "oled_ui.h"
#include "protocol.h"
#include "status_led.h"
#include "wbus_simple.h"
#include "encryption.h"
#include "menu_handler.h"

#ifdef ENABLE_MQTT_CONTROL
#include "wifi_manager.h"
#include "mqtt_client.h"
#ifdef MQTT_ENABLE_OTA
#include "ota_updater.h"
#endif
#endif

static OledUi ui;
static LoRaLink loraLink;
static StatusLed statusLed;
static WBusSimple wbus(Serial2);
static MenuHandler menu;

#ifdef ENABLE_MQTT_CONTROL
static WiFiManager wifiMgr;
static MQTTClient mqttClient(wifiMgr);
#ifdef MQTT_ENABLE_OTA
static OTAUpdater otaUpdater;
#endif
#endif

static uint16_t gSeq = 1;
static proto::StatusPayload gStatus{};
static uint32_t gLastCmdMs = 0;
static uint32_t gLastPollMs = 0;
static uint8_t gLastRunMinutes = DEFAULT_RUN_MINUTES;

#ifdef ENABLE_MQTT_CONTROL
// Phase 6: Track last command source and diagnostic publish time
static const char* gLastCommandSource = "none";
static uint32_t gLastDiagnosticPublishMs = 0;
static int gLastLoRaRssi = -157;
static float gLastLoRaSNR = -20.0f;
#endif

// Persist across deep sleep to deduplicate sender retries.
RTC_DATA_ATTR static uint16_t gLastProcessedCmdSeq = 0;

// Persist across deep sleep so we don't re-probe TLV support on every wake.
// 0 = unknown, 1 = not supported, 2 = supported
RTC_DATA_ATTR static uint8_t gTlvSupportCache = 0;

static bool gTlvSupportKnown = false;
static bool gTlvSupported = false;

static void sendStatus(int rssiDbm, float snrDb) {
  proto::Packet pkt{};
  pkt.h.magic_version = proto::kMagicVersion;
  pkt.h.type = proto::MsgType::Status;
  pkt.h.src = LORA_NODE_RECEIVER;
  pkt.h.dst = LORA_NODE_SENDER;
  pkt.h.seq = gSeq++;

  gStatus.lastRssiDbm = (int8_t)rssiDbm;
  gStatus.lastSnrDb = (int8_t)snrDb;
  pkt.p.status = gStatus;
  pkt.crc = proto::calcCrc(pkt);

  loraLink.send(pkt);
  statusLed.toggle();  // Flash LED on TX
}

static void enterDeepSleepMs(uint32_t sleepMs) {
#ifdef DISABLE_SLEEP
  // Sleep disabled for testing - just delay instead
  Serial.printf("[TEST] Sleep disabled, delaying %lu ms instead\n", sleepMs);
  delay(sleepMs);
#else
  // Turn radio off as best-effort.
  LoRa.sleep();

  // Turn OLED off while sleeping.
  ui.setPowerSave(true);

  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleepMs) * 1000ULL);
  esp_deep_sleep_start();
#endif
}

static bool tryReceiveCommandWindow(uint32_t windowMs, int& lastCmdRssi, float& lastCmdSnr, proto::Packet& outPkt) {
  LoRa.receive();
  const uint32_t start = millis();
  while (millis() - start < windowMs) {
    if (loraLink.recv(outPkt, lastCmdRssi, lastCmdSnr)) {
      statusLed.toggle();  // Flash LED on RX
      Serial.printf("[LORA-RX] Got packet: magic_version=0x%02X type=%d src=%d dst=%d seq=%d\n",
                    outPkt.h.magic_version, static_cast<int>(outPkt.h.type),
                    outPkt.h.src, outPkt.h.dst, outPkt.h.seq);
      if (outPkt.h.type == proto::MsgType::Command && outPkt.h.dst == LORA_NODE_RECEIVER) {
        return true;
      }
    }
    delay(5);
  }
  return false;
}

static proto::HeaterState mapOpState(uint8_t opState) {
  // webasto_wbus.txt defines a large state machine; we use a coarse mapping.
  // 0x04 is explicitly "Off state".
  if (opState == 0x04) return proto::HeaterState::Off;
  if (opState == 0x00) return proto::HeaterState::Off; // burn out / off-ish
  // Anything else: treat as running.
  return proto::HeaterState::Running;
}

static uint16_t be16(uint8_t hi, uint8_t lo) {
  return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}

static bool waitForStatusIndex(WBusSimple& wbus, uint8_t index, WBusPacket& out, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (!wbus.readPacket(out, timeoutMs)) return false;
    Serial.printf("[WBUS_RX] hdr=0x%02X len=%u payload[0]=0x%02X payload[1]=0x%02X\n", 
                  out.header, out.payloadLen, out.payloadLen > 0 ? out.payload[0] : 0xFF, 
                  out.payloadLen > 1 ? out.payload[1] : 0xFF);
    if (out.payloadLen < 3) continue;
    // Response payload begins with cmd|0x80 (typically 0xD0 for cmd 0x50 responses).
    if ((out.payload[0] & 0x7F) != 0x50) continue;
    if ((out.payload[0] & 0x80) == 0) continue;
    if (out.payload[1] != index) continue;
    return true;
  }
  return false;
}

static bool tryPollMultiStatusOnce(WBusSimple& wbus, proto::StatusPayload& outStatus) {
  static const uint8_t kIds[] = {
      0x01, 0x03, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0C, 0x0E, 0x0F, 0x10, 0x11, 0x13,
      0x1E, 0x1F, 0x24, 0x27, 0x29, 0x2A, 0x2C, 0x2D, 0x32, 0x34, 0x3D, 0x52, 0x57,
      0x5F, 0x78, 0x89,
  };

  if (!wbus.requestStatusMulti(kIds, sizeof(kIds))) return false;

  // Wait briefly for a matching response.
  WBusPacket pkt;
  uint32_t deadline = millis() + 250;
  while (millis() < deadline) {
    if (!wbus.readPacket(pkt, 250)) break;
    if (pkt.payloadLen < 4) continue;
    if ((pkt.payload[0] & 0x7F) != 0x50) continue;
    if ((pkt.payload[0] & 0x80) == 0) continue;
    if (pkt.payload[1] != 0x30) continue;

    WBusStatus st;
    if (WBusSimple::tryParseStatusTlv(pkt, st) && st.valid) {
      outStatus.temperatureC = st.temperatureC;
      outStatus.voltage_mV = st.voltage_mV;
      outStatus.power = st.power;
      return true;
    }
    break;
  }

  return false;
}

static void logSimpleStatus05(const WBusPacket& pkt) {
  // Expected: payload[0]=0xD0, payload[1]=0x05, then data bytes.
  if (pkt.payloadLen < 10) {
    Serial.println("WBUS simple 0x05: short frame");
    return;
  }

  const int16_t tempC = static_cast<int16_t>(static_cast<int>(pkt.payload[2]) - 50);
  const uint16_t v_mV = be16(pkt.payload[3], pkt.payload[4]);
  const uint8_t flameDetect = pkt.payload[5];
  const uint16_t heaterPower_x10 = be16(pkt.payload[6], pkt.payload[7]);

  Serial.print("WBUS simple idx=0x05 tempC=");
  Serial.print(tempC);
  Serial.print(" v=");
  Serial.print(v_mV);
  Serial.print("mV flame=");
  Serial.print(flameDetect);
  Serial.print(" hp_x10=");
  Serial.print(heaterPower_x10);
  Serial.print(" raw=");
  for (uint8_t i = 0; i < pkt.payloadLen; i++) {
    if (i) Serial.print(' ');
    if (pkt.payload[i] < 16) Serial.print('0');
    Serial.print(pkt.payload[i], HEX);
  }
  Serial.println();
}

static void logSimpleStatus0F(const WBusPacket& pkt) {
  // Expected: payload[0]=0xD0, payload[1]=0x0F, then 3 bytes.
  if (pkt.payloadLen < 6) {
    Serial.println("WBUS simple 0x0F: short frame");
    return;
  }
  const uint16_t glowPlugPower = static_cast<uint16_t>(pkt.payload[2]) * 2;
  const uint16_t fuelPumpFreq = static_cast<uint16_t>(pkt.payload[3]) * 2;
  const uint16_t airFanPower = static_cast<uint16_t>(pkt.payload[4]) * 2;

  Serial.print("WBUS simple idx=0x0F gpp=");
  Serial.print(glowPlugPower);
  Serial.print(" fpf=");
  Serial.print(fuelPumpFreq);
  Serial.print(" afp=");
  Serial.print(airFanPower);
  Serial.print(" raw=");
  for (uint8_t i = 0; i < pkt.payloadLen; i++) {
    if (i) Serial.print(' ');
    if (pkt.payload[i] < 16) Serial.print('0');
    Serial.print(pkt.payload[i], HEX);
  }
  Serial.println();
}

static void logSimpleStatusFlags(const WBusPacket& pkt, const char* label) {
  if (pkt.payloadLen < 4) {
    Serial.print(label);
    Serial.println(": short frame");
    return;
  }
  Serial.print(label);
  Serial.print(" flags=0x");
  if (pkt.payload[2] < 16) Serial.print('0');
  Serial.print(pkt.payload[2], HEX);
  Serial.print(" raw=");
  for (uint8_t i = 0; i < pkt.payloadLen; i++) {
    if (i) Serial.print(' ');
    if (pkt.payload[i] < 16) Serial.print('0');
    Serial.print(pkt.payload[i], HEX);
  }
  Serial.println();
}

static void handleMenuSelection(MenuItem item)
{
  Serial.printf("[MENU] Activated: %s\n", menuItemToStr(item));
  
  #ifdef ENABLE_MQTT_CONTROL
  gLastCommandSource = "button";  // Phase 6: Track command source
  #endif

  switch (item)
  {
  case MenuItem::Start:
    Serial.printf("[WBUS] Menu: Sending START for %d min\n", gLastRunMinutes);
    if (wbus.startParkingHeater(gLastRunMinutes))
    {
      gStatus.state = proto::HeaterState::Running;
      gLastCmdMs = millis();
      Serial.println("[WBUS] Menu START OK");
    }
    else
    {
      Serial.println("[WBUS] Menu START FAILED");
    }
    break;

  case MenuItem::Stop:
    Serial.println("[WBUS] Menu: Sending STOP");
    if (wbus.stop())
    {
      gStatus.state = proto::HeaterState::Off;
      gLastCmdMs = millis();
      Serial.println("[WBUS] Menu STOP OK");
    }
    else
    {
      Serial.println("[WBUS] Menu STOP FAILED");
    }
    break;

  case MenuItem::Run10min:
    gLastRunMinutes = 10;
    Serial.printf("[WBUS] Menu: Sending RUN for 10 min\n");
    if (wbus.startParkingHeater(gLastRunMinutes))
    {
      gStatus.state = proto::HeaterState::Running;
      gLastCmdMs = millis();
      Serial.println("[WBUS] Menu RUN 10 OK");
    }
    else
    {
      Serial.println("[WBUS] Menu RUN 10 FAILED");
    }
    break;

  case MenuItem::Run20min:
    gLastRunMinutes = 20;
    Serial.printf("[WBUS] Menu: Sending RUN for 20 min\n");
    if (wbus.startParkingHeater(gLastRunMinutes))
    {
      gStatus.state = proto::HeaterState::Running;
      gLastCmdMs = millis();
      Serial.println("[WBUS] Menu RUN 20 OK");
    }
    else
    {
      Serial.println("[WBUS] Menu RUN 20 FAILED");
    }
    break;

  case MenuItem::Run30min:
    gLastRunMinutes = 30;
    Serial.printf("[WBUS] Menu: Sending RUN for 30 min\n");
    if (wbus.startParkingHeater(gLastRunMinutes))
    {
      gStatus.state = proto::HeaterState::Running;
      gLastCmdMs = millis();
      Serial.println("[WBUS] Menu RUN 30 OK");
    }
    else
    {
      Serial.println("[WBUS] Menu RUN 30 FAILED");
    }
    break;

  case MenuItem::Run90min:
    gLastRunMinutes = 90;
    Serial.printf("[WBUS] Menu: Sending RUN for 90 min\n");
    if (wbus.startParkingHeater(gLastRunMinutes))
    {
      gStatus.state = proto::HeaterState::Running;
      gLastCmdMs = millis();
      Serial.println("[WBUS] Menu RUN 90 OK");
    }
    else
    {
      Serial.println("[WBUS] Menu RUN 90 FAILED");
    }
    break;

  default:
    break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);  // Longer delay for serial to stabilize

  Serial.println("\n\n==================================");
  Serial.println("  WEBASTO LORA RECEIVER");
  Serial.println("  Device ID: RECEIVER");
  Serial.println("==================================");

  statusLed.begin();

  ui.begin();
  ui.setLine(0, "Webasto LoRa Receiver");
  ui.setLine(1, "Init LoRa...");
  ui.render();

  bool loraOk = loraLink.begin();
  ui.setLine(1, loraOk ? "LoRa OK" : "LoRa FAIL");

  bool wbusOk = wbus.begin();
  ui.setLine(2, wbusOk ? "W-BUS OK" : "W-BUS FAIL");

  ui.setLine(3, String("Freq ") + String((uint32_t)LORA_FREQUENCY_HZ));
  ui.render();

  // Initialize encryption with default PSK
  crypto::AES128CTR::setKey(proto::kDefaultPSK);
  Serial.println("[SETUP] AES-128-CTR encryption initialized");

  // Initialize button/menu on GPIO0
  menu.begin(MENU_BUTTON_PIN);
  Serial.println("[SETUP] Menu button initialized on GPIO0");

#ifdef ENABLE_MQTT_CONTROL
  // Initialize WiFi and MQTT
  ui.setLine(4, "Init WiFi...");
  ui.render();
  
  wifiMgr.begin(MQTT_WIFI_SSID, MQTT_WIFI_USERNAME, MQTT_WIFI_PASSWORD, MQTT_WIFI_ANONYMOUS_ID);
  mqttClient.begin(MQTT_BROKER, MQTT_PORT, MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
  
  // Set MQTT command callback
  mqttClient.setCommandCallback([](const MQTTCommand& cmd) {
    Serial.printf("[MQTT] Command callback: type=%d minutes=%d\n", cmd.type, cmd.minutes);
    
    gLastCommandSource = "mqtt";  // Phase 6: Track command source
    
    bool ok = true;
    switch (cmd.type) {
      case MQTTCommand::STOP:
        Serial.println("[MQTT-CMD] Executing STOP");
        ok = wbus.stop();
        if (ok) {
          gStatus.state = proto::HeaterState::Off;
        }
        break;
        
      case MQTTCommand::START:
        Serial.printf("[MQTT-CMD] Executing START (%d min)\n", cmd.minutes);
        gLastRunMinutes = cmd.minutes ? cmd.minutes : gLastRunMinutes;
        ok = wbus.startParkingHeater(gLastRunMinutes);
        if (ok) {
          gStatus.state = proto::HeaterState::Running;
        }
        break;
        
      case MQTTCommand::RUN_MINUTES:
        Serial.printf("[MQTT-CMD] Executing RUN (%d min)\n", cmd.minutes);
        gLastRunMinutes = cmd.minutes;
        ok = wbus.startParkingHeater(gLastRunMinutes);
        if (ok) {
          gStatus.state = proto::HeaterState::Running;
        }
        break;
        
      default:
        ok = false;
        break;
    }
    
    if (!ok) {
      gStatus.state = proto::HeaterState::Error;
    }
    
    gLastCmdMs = millis();
    
    // Publish updated state immediately (Phase 6: Include diagnostic data)
    if (mqttClient.isConnected()) {
      mqttClient.publishStatus(gStatus);
      #ifdef MQTT_ENABLE_DIAGNOSTIC_SENSORS
      mqttClient.publishLastCommandSource(gLastCommandSource);
      #endif
    }
  });
  
  #ifdef MQTT_ENABLE_OTA
  // Register OTA updater with MQTT client (Phase 7)
  mqttClient.setOTAUpdater(&otaUpdater);
  
  // Set OTA callbacks
  otaUpdater.setProgressCallback([](size_t current, size_t total) {
    uint8_t percent = (current * 100) / total;
    Serial.printf("[OTA] Progress: %u%%\n", percent);
    ui.setLine(0, "OTA Update");
    ui.setLine(1, String(percent) + "% complete");
    ui.render();
  });
  
  otaUpdater.setCompleteCallback([](OTAResult result, const char* message) {
    Serial.printf("[OTA] Complete: result=%d message=%s\n", (int)result, message);
    mqttClient.publishOTAStatus(
      result == OTAResult::SUCCESS ? "success" : "failed",
      message
    );
  });
  #endif
  
  // Configure NTP for timestamp validation
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("[SETUP] NTP time sync configured");
  
  ui.setLine(4, "WiFi configured");
  ui.render();
#endif

  Serial.println("Receiver ready.");

  // Initialize decoded measurement fields as unknown.
  gStatus.temperatureC = INT16_MIN;
  gStatus.voltage_mV = 0;
  gStatus.power = 0;

  gStatus.lastCmdSeq = gLastProcessedCmdSeq;

  // Probe once whether the connected W-BUS device supports the multi-status TLV snapshot.
  // If it doesn't, avoid re-trying it on every poll cycle and go straight to the fallback pages.
  {
    if (gTlvSupportCache == 2) {
      gTlvSupported = true;
      gTlvSupportKnown = true;
    } else if (gTlvSupportCache == 1) {
      gTlvSupported = false;
      gTlvSupportKnown = true;
    } else {
      proto::StatusPayload scratch = gStatus;
      gTlvSupported = tryPollMultiStatusOnce(wbus, scratch);
      gTlvSupportKnown = true;
      gTlvSupportCache = gTlvSupported ? 2 : 1;
    }
    Serial.print("WBUS TLV multi-status support: ");
    Serial.println(gTlvSupported ? "yes" : "no");
  }
}

void loop() {
  // If heater is not running, we can save power by waking periodically,
  // opening a short RX window, and then deep sleeping again.
  const bool heaterRunning = (gStatus.state == proto::HeaterState::Running);

  // Update LED status only when heater state changes
  static bool lastHeaterRunning = false;
  if (heaterRunning != lastHeaterRunning) {
    lastHeaterRunning = heaterRunning;
    if (heaterRunning) {
      statusLed.setOn();  // Solid on while heater is running
    } else {
      statusLed.setBlink(1000);  // Slow blink while idle
    }
  }

  int lastCmdRssi = 0;
  float lastCmdSnr = 0;
  proto::Packet pkt{};

#ifdef DISABLE_SLEEP
  // When sleep is disabled for testing, stay fully awake and continuously receive
  ui.setPowerSave(false);
  
  static bool receiveModeSet = false;
  if (!receiveModeSet) {
    Serial.println("[TEST] Setting LoRa to receive mode...");
    LoRa.receive();
    receiveModeSet = true;
    Serial.println("[TEST] LoRa receive mode set!");
  }
  
  static uint32_t lastDebugPrint = 0;
  if (millis() - lastDebugPrint > 5000) {
    Serial.println("[TEST] DISABLE_SLEEP mode - continuously receiving LoRa");
    lastDebugPrint = millis();
  }
  
  // Receive LoRa packet in test/debug mode
  if (loraLink.recv(pkt, lastCmdRssi, lastCmdSnr)) {
    statusLed.toggle();  // Flash LED on RX
  }
  
  // Don't return early - process commands below!
#else
  if (!heaterRunning) {
    // OLED off while idle.
    ui.setPowerSave(true);

    // Short command listen window.
    bool gotCmd = tryReceiveCommandWindow(static_cast<uint32_t>(RX_IDLE_LISTEN_WINDOW_MS), lastCmdRssi, lastCmdSnr, pkt);
    if (gotCmd) {
      Serial.printf("[LORA] Got command in idle window! type=%d seq=%d\n", 
                    static_cast<int>(pkt.h.type), pkt.h.seq);
      // fall through to command handling below
    } else {
      // No command received: go back to sleep.
      // We intentionally avoid polling W-BUS here to minimize wake-time power draw.
      enterDeepSleepMs(static_cast<uint32_t>(RX_IDLE_SLEEP_MS));
      return;  // Return early when sleeping to avoid falling through
    }
  } else {
    // Running mode: keep OLED on continuously.
    ui.setPowerSave(false);
  }
#endif

  // Receive commands (continuous in running mode, or immediately after a wake window)
  {
#ifndef DISABLE_SLEEP
    // Only call recv here if we're in running mode (heater on)
    // In idle mode with sleep enabled, recv was already done in tryReceiveCommandWindow
    if (heaterRunning) {
      if (loraLink.recv(pkt, lastCmdRssi, lastCmdSnr)) {
        statusLed.toggle();  // Flash LED on RX
      } else {
        pkt = proto::Packet{};
      }
    }
#endif
    // With DISABLE_SLEEP, recv was already done above

    if (pkt.h.type == proto::MsgType::Command && pkt.h.dst == LORA_NODE_RECEIVER) {
      Serial.printf("[LORA] Received command: kind=%d minutes=%d seq=%d rssi=%d snr=%.1f\n",
                    static_cast<int>(pkt.p.cmd.kind), pkt.p.cmd.minutes, pkt.h.seq,
                    lastCmdRssi, lastCmdSnr);
      // Deduplicate sender retries.
      if (pkt.h.seq == gLastProcessedCmdSeq) {
        Serial.println("[LORA] Duplicate command, just ACKing");
        gStatus.lastCmdSeq = gLastProcessedCmdSeq;
        sendStatus(lastCmdRssi, lastCmdSnr);
      } else {
        bool ok = true;

        switch (pkt.p.cmd.kind) {
          case proto::CommandKind::Stop:
            Serial.println("[WBUS] Sending STOP command");
            ok = wbus.stop();
            if (ok) {
              gStatus.state = proto::HeaterState::Off;
              Serial.println("[WBUS] STOP OK");
            } else {
              Serial.println("[WBUS] STOP FAILED");
            }
            break;

          case proto::CommandKind::Start:
            gLastRunMinutes = pkt.p.cmd.minutes ? pkt.p.cmd.minutes : gLastRunMinutes;
            Serial.printf("[WBUS] Sending START command for %d minutes\n", gLastRunMinutes);
            ok = wbus.startParkingHeater(gLastRunMinutes);
            if (ok) {
              gStatus.state = proto::HeaterState::Running;
              Serial.println("[WBUS] START OK");
            } else {
              Serial.println("[WBUS] START FAILED");
            }
            break;

          case proto::CommandKind::RunMinutes:
            gLastRunMinutes = pkt.p.cmd.minutes ? pkt.p.cmd.minutes : gLastRunMinutes;
            Serial.printf("[WBUS] Sending RUN command for %d minutes\n", gLastRunMinutes);
            ok = wbus.startParkingHeater(gLastRunMinutes);
            if (ok) {
              gStatus.state = proto::HeaterState::Running;
              Serial.println("[WBUS] RUN OK");
            } else {
              Serial.println("[WBUS] RUN FAILED");
            }
            break;

          default:
            ok = false;
            break;
        }

        gLastCmdMs = millis();
        if (!ok) {
          gStatus.state = proto::HeaterState::Error;
        }
        
        gLastCommandSource = "lora";  // Phase 6: Track command source

        gLastProcessedCmdSeq = pkt.h.seq;
        gStatus.lastCmdSeq = gLastProcessedCmdSeq;
        
        // Store LoRa metrics for diagnostic publishing (Phase 6)
        #ifdef ENABLE_MQTT_CONTROL
        gLastLoRaRssi = lastCmdRssi;
        gLastLoRaSNR = lastCmdSnr;
        #endif

        // Send immediate status update as ACK.
        sendStatus(lastCmdRssi, lastCmdSnr);
      }
    }
  }

  // Poll W-BUS operating state periodically.
  if (millis() - gLastPollMs > 2000) {
    gLastPollMs = millis();

    uint8_t op = 0;
    if (wbus.readOperatingState(op)) {
      gStatus.lastWbusOpState = op;
      gStatus.state = mapOpState(op);
    }

    // Also request a multi-status snapshot and decode fields from the response.
    {
      bool gotTlv = false;
      if (gTlvSupportCache == 2) {
        gotTlv = tryPollMultiStatusOnce(wbus, gStatus);
      }

      // Fallback: poll a few “simple status pages” (as seen in Moki38) and log them.
      // This helps on heaters/firmware that don’t support 0x50/0x30 multi-status.
      if (!gotTlv) {
        WBusPacket pkt;

        // 0x05 carries temperature + voltage and other fields.
        {
          const uint8_t idx = 0x05;
          if (wbus.sendCommand(0x50, &idx, 1) && waitForStatusIndex(wbus, idx, pkt, 250)) {
            logSimpleStatus05(pkt);
            // Update our outbound status with the key fields we know.
            if (pkt.payloadLen >= 5) {
              gStatus.temperatureC = static_cast<int16_t>(static_cast<int>(pkt.payload[2]) - 50);
              gStatus.voltage_mV = be16(pkt.payload[3], pkt.payload[4]);
            }
          } else {
            Serial.println("WBUS simple idx=0x05: no response");
          }
        }

        // 0x0F carries fan/pump/glow values (scaled by *2 in Moki38).
        {
          const uint8_t idx = 0x0F;
          if (wbus.sendCommand(0x50, &idx, 1) && waitForStatusIndex(wbus, idx, pkt, 250)) {
            logSimpleStatus0F(pkt);
          } else {
            Serial.println("WBUS simple idx=0x0F: no response");
          }
        }

        // 0x02 / 0x03 are bitflag pages.
        {
          const uint8_t idx = 0x02;
          if (wbus.sendCommand(0x50, &idx, 1) && waitForStatusIndex(wbus, idx, pkt, 250)) {
            logSimpleStatusFlags(pkt, "WBUS simple idx=0x02");
          }
        }
        {
          const uint8_t idx = 0x03;
          if (wbus.sendCommand(0x50, &idx, 1) && waitForStatusIndex(wbus, idx, pkt, 250)) {
            logSimpleStatusFlags(pkt, "WBUS simple idx=0x03");
          }
        }

        // 0x06 holds counters/timers; log raw so we can interpret later.
        {
          const uint8_t idx = 0x06;
          if (wbus.sendCommand(0x50, &idx, 1) && waitForStatusIndex(wbus, idx, pkt, 250)) {
            logSimpleStatusFlags(pkt, "WBUS simple idx=0x06");
          }
        }
      }
    }

    // send status every poll
    sendStatus(0, 0);
  }

  // Update LED blinking
  statusLed.update();

  // Handle menu button
  menu.update();

  // Check if menu item was activated (long press)
  MenuItem activatedItem;
  if (menu.isItemActivated(activatedItem))
  {
    handleMenuSelection(activatedItem);
  }

#ifdef ENABLE_MQTT_CONTROL
  // Update WiFi and MQTT (non-blocking)
  wifiMgr.update();
  
  if (wifiMgr.isConnected()) {
    mqttClient.update();
    
    // Phase 7: Handle OTA update requests
    #ifdef MQTT_ENABLE_OTA
    if (otaUpdater.isUpdateRequested()) {
      if (otaUpdater.canUpdate(heaterRunning)) {
        Serial.println("[OTA] Performing OTA update...");
        mqttClient.publishOTAStatus("starting", "Beginning OTA update");
        
        // Read credentials from template or use defaults
        #ifdef OTA_UPDATE_URL
        const char* url = OTA_UPDATE_URL;
        #else
        const char* url = "";
        #endif
        
        #ifdef OTA_UPDATE_USERNAME
        const char* username = OTA_UPDATE_USERNAME;
        #else
        const char* username = "";
        #endif
        
        #ifdef OTA_UPDATE_PASSWORD
        const char* password = OTA_UPDATE_PASSWORD;
        #else
        const char* password = "";
        #endif
        
        if (strlen(url) > 0) {
          OTAResult result = otaUpdater.performUpdate(url, username, password);
          // If we reach here, update failed (success would have rebooted)
          if (result != OTAResult::SUCCESS) {
            Serial.printf("[OTA] Update failed: %s\n", otaUpdater.getLastError());
          }
        } else {
          Serial.println("[OTA] No URL configured");
          mqttClient.publishOTAStatus("error", "No OTA URL configured");
        }
        
        otaUpdater.clearUpdateRequest();
      } else {
        if (heaterRunning) {
          Serial.println("[OTA] Cannot update while heater is running");
          mqttClient.publishOTAStatus("deferred", "Heater must be OFF for OTA");
        } else {
          Serial.println("[OTA] Cannot update - WiFi not connected");
          mqttClient.publishOTAStatus("error", "WiFi required for OTA");
        }
        otaUpdater.clearUpdateRequest();
      }
    }
    #endif
    
    // Publish status updates periodically
    static uint32_t lastMqttPublishMs = 0;
    if (millis() - lastMqttPublishMs > MQTT_STATUS_INTERVAL_MS) {
      lastMqttPublishMs = millis();
      
      if (mqttClient.isConnected()) {
        mqttClient.publishStatus(gStatus);
        Serial.println("[MQTT] Status published");
      }
    }
    
    // Phase 6: Publish diagnostic sensors periodically
    #ifdef MQTT_ENABLE_DIAGNOSTIC_SENSORS
    if (millis() - gLastDiagnosticPublishMs > MQTT_DIAGNOSTIC_INTERVAL_MS) {
      gLastDiagnosticPublishMs = millis();
      
      if (mqttClient.isConnected()) {
        bool wbusHealthy = (gStatus.state != proto::HeaterState::Error);
        mqttClient.publishDiagnostics(gLastLoRaRssi, gLastLoRaSNR, gLastCommandSource, wbusHealthy);
        Serial.println("[MQTT] Diagnostics published");
      }
    }
    #endif
  }
#endif

  // OLED refresh
  static uint32_t lastUiMs = 0;
  if (millis() - lastUiMs > 250) {
    lastUiMs = millis();

    // Check if menu is visible
    if (menu.getState() == MenuState::Visible)
    {
      // Render menu
      ui.setLine(0, "=== MENU ===");
      ui.setLine(1, "");
      MenuItem selected = menu.getSelectedItem();
      for (int i = 0; i < static_cast<int>(MenuItem::Count); i++)
      {
        MenuItem item = static_cast<MenuItem>(i);
        String line;
        if (item == selected)
        {
          line = "> " + String(menuItemToStr(item));
        }
        else
        {
          line = "  " + String(menuItemToStr(item));
        }
        ui.setLine(2 + i, line);
      }
      ui.setLine(5, "Long press to activate");
    }
    else
    {
      // Render normal status view
      ui.setLine(0, "Webasto LoRa Receiver");
      ui.setLine(1,
                 String("State: ") +
                     (gStatus.state == proto::HeaterState::Running
                          ? "RUN"
                          : (gStatus.state == proto::HeaterState::Off
                                 ? "OFF"
                                 : (gStatus.state == proto::HeaterState::Error ? "ERR" : "UNK"))));
      ui.setLine(2, String("Last min: ") + String(gLastRunMinutes));
      ui.setLine(3, String("OpState: 0x") + String(gStatus.lastWbusOpState, HEX));

      if (gLastCmdMs == 0) {
        ui.setLine(4, "Last cmd: (none)");
      } else {
        ui.setLine(4, String("Last cmd: ") + String((millis() - gLastCmdMs) / 1000) + "s");
      }

      // Cycle through W-BUS status fields on line 5 every 3 seconds
      static uint32_t lastStatusCycleMs = 0;
      static uint8_t statusCycleIndex = 0;
      
      if (millis() - lastStatusCycleMs > 3000) {
        lastStatusCycleMs = millis();
#ifdef ENABLE_MQTT_CONTROL
        statusCycleIndex = (statusCycleIndex + 1) % 5;  // 5 items with WiFi/MQTT
#else
        statusCycleIndex = (statusCycleIndex + 1) % 4;  // 4 items without
#endif
      }
      
      String statusLine;
      switch (statusCycleIndex) {
        case 0:  // Temperature
          statusLine = String("Temp: ") + String(gStatus.temperatureC) + "C";
          break;
        case 1:  // Voltage
          statusLine = String("Volt: ") + String(gStatus.voltage_mV) + "mV";
          break;
        case 2:  // Heater power
          statusLine = String("Power: ") + String(gStatus.power);
          break;
        case 3:  // Operating state
          statusLine = String("OpState: 0x") + String(gStatus.lastWbusOpState, HEX);
          break;
#ifdef ENABLE_MQTT_CONTROL
        case 4:  // WiFi/MQTT status
          if (wifiMgr.isConnected()) {
            if (mqttClient.isConnected()) {
              statusLine = "WiFi+MQTT OK";
            } else {
              statusLine = "WiFi OK, MQTT...";
            }
          } else {
            statusLine = "WiFi: connecting";
          }
          break;
#endif
        default:
          statusLine = "WBUS 2400 8E1";
      }
      
      ui.setLine(5, statusLine);
    }

    ui.render();
  }
}
