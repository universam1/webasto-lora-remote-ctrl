#include <Arduino.h>
#include <esp_sleep.h>

#include "project_config.h"
#include "lora_link.h"
#include "oled_ui.h"
#include "protocol.h"
#include "wbus_simple.h"

static OledUi ui;
static LoRaLink loraLink;
static WBusSimple wbus;

static uint16_t gSeq = 1;
static proto::StatusPayload gStatus{};
static uint32_t gLastCmdMs = 0;
static uint32_t gLastPollMs = 0;
static uint8_t gLastRunMinutes = DEFAULT_RUN_MINUTES;

// Cache whether the connected W-BUS device supports the 0x50/0x30 multi-status TLV snapshot.
static bool gTlvSupportKnown = false;
static bool gTlvSupported = false;

// Persist across deep sleep to deduplicate sender retries.
RTC_DATA_ATTR static uint16_t gLastProcessedCmdSeq = 0;

static void sendStatus(int rssiDbm, float snrDb) {
  proto::Packet pkt{};
  pkt.h.magic = proto::kMagic;
  pkt.h.version = proto::kVersion;
  pkt.h.type = proto::MsgType::Status;
  pkt.h.src = LORA_NODE_RECEIVER;
  pkt.h.dst = LORA_NODE_SENDER;
  pkt.h.seq = gSeq++;

  gStatus.lastRssiDbm = (int8_t)rssiDbm;
  gStatus.lastSnrDb = (int8_t)snrDb;
  pkt.p.status = gStatus;
  pkt.crc = proto::calcCrc(pkt);

  loraLink.send(pkt);
}

static void enterDeepSleepMs(uint32_t sleepMs) {
  // Turn radio off as best-effort.
  LoRa.sleep();

  // Turn OLED off while sleeping.
  ui.setPowerSave(true);

  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleepMs) * 1000ULL);
  esp_deep_sleep_start();
}

static bool tryReceiveCommandWindow(uint32_t windowMs, int& lastCmdRssi, float& lastCmdSnr, proto::Packet& outPkt) {
  LoRa.receive();
  const uint32_t start = millis();
  while (millis() - start < windowMs) {
    if (loraLink.recv(outPkt, lastCmdRssi, lastCmdSnr)) {
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

void setup() {
  Serial.begin(115200);
  delay(200);

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

  Serial.println("Receiver ready.");

  // Initialize decoded measurement fields as unknown.
  gStatus.temperatureC = INT16_MIN;
  gStatus.voltage_mV = 0;
  gStatus.power = 0;

  gStatus.lastCmdSeq = gLastProcessedCmdSeq;

  // Probe once whether the connected W-BUS device supports the multi-status TLV snapshot.
  // If it doesn't, avoid re-trying it on every poll cycle and go straight to the fallback pages.
  {
    proto::StatusPayload scratch = gStatus;
    gTlvSupported = tryPollMultiStatusOnce(wbus, scratch);
    gTlvSupportKnown = true;
    Serial.print("WBUS TLV multi-status support: ");
    Serial.println(gTlvSupported ? "yes" : "no");
  }
}

void loop() {
  // If heater is not running, we can save power by waking periodically,
  // opening a short RX window, and then deep sleeping again.
  const bool heaterRunning = (gStatus.state == proto::HeaterState::Running);

  int lastCmdRssi = 0;
  float lastCmdSnr = 0;
  proto::Packet pkt{};

  if (!heaterRunning) {
    // OLED off while idle.
    ui.setPowerSave(true);

    // Short command listen window.
    bool gotCmd = tryReceiveCommandWindow(static_cast<uint32_t>(RX_IDLE_LISTEN_WINDOW_MS), lastCmdRssi, lastCmdSnr, pkt);
    if (gotCmd) {
      // fall through to command handling below
    } else {
      // Quick check: if the heater is actually running (started externally), stay awake.
      uint8_t op = 0;
      if (wbus.readOperatingState(op)) {
        gStatus.lastWbusOpState = op;
        gStatus.state = mapOpState(op);
      }

      if (gStatus.state != proto::HeaterState::Running) {
        // Sleep until next scan.
        enterDeepSleepMs(static_cast<uint32_t>(RX_IDLE_SLEEP_MS));
      }
      // else: heater is running; keep OLED on and continue in running mode.
    }
  } else {
    // Running mode: keep OLED on continuously.
    ui.setPowerSave(false);
  }

  // Receive commands (continuous in running mode, or immediately after a wake window)
  {
    if (heaterRunning) {
      if (!loraLink.recv(pkt, lastCmdRssi, lastCmdSnr)) {
        pkt = proto::Packet{};
      }
    }

    if (pkt.h.type == proto::MsgType::Command && pkt.h.dst == LORA_NODE_RECEIVER) {
      // Deduplicate sender retries.
      if (pkt.h.seq == gLastProcessedCmdSeq) {
        gStatus.lastCmdSeq = gLastProcessedCmdSeq;
        sendStatus(lastCmdRssi, lastCmdSnr);
      } else {
        bool ok = true;

        switch (pkt.p.cmd.kind) {
          case proto::CommandKind::Stop:
            ok = wbus.stop();
            if (ok) {
              gStatus.state = proto::HeaterState::Off;
            }
            break;

          case proto::CommandKind::Start:
            gLastRunMinutes = pkt.p.cmd.minutes ? pkt.p.cmd.minutes : gLastRunMinutes;
            ok = wbus.startParkingHeater(gLastRunMinutes);
            if (ok) {
              gStatus.state = proto::HeaterState::Running;
            }
            break;

          case proto::CommandKind::RunMinutes:
            gLastRunMinutes = pkt.p.cmd.minutes ? pkt.p.cmd.minutes : gLastRunMinutes;
            ok = wbus.startParkingHeater(gLastRunMinutes);
            if (ok) {
              gStatus.state = proto::HeaterState::Running;
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

        gLastProcessedCmdSeq = pkt.h.seq;
        gStatus.lastCmdSeq = gLastProcessedCmdSeq;

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
      if (!gTlvSupportKnown || gTlvSupported) {
        gotTlv = tryPollMultiStatusOnce(wbus, gStatus);
        if (!gTlvSupportKnown) {
          gTlvSupported = gotTlv;
          gTlvSupportKnown = true;
        }
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

  // OLED refresh
  static uint32_t lastUiMs = 0;
  if (millis() - lastUiMs > 250) {
    lastUiMs = millis();

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

    ui.setLine(5, "WBUS 2400 8E1");
    ui.render();
  }
}
