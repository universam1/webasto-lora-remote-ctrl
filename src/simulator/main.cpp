#include <Arduino.h>

#include "project_config.h"
#include "wbus_simple.h"

// Platform-agnostic W-BUS serial port definition
// Classic ESP32 has Serial2 by default, other platforms can define custom UART
#ifndef WBUS_SERIAL
#define WBUS_SERIAL Serial2
#endif

namespace {

static uint8_t makeHeader(uint8_t src, uint8_t dst) {
  return static_cast<uint8_t>(((src & 0x0F) << 4) | (dst & 0x0F));
}

static uint8_t controllerToHeaterHeader() {
  return makeHeader(WBUS_ADDR_CONTROLLER, WBUS_ADDR_HEATER);
}

static uint8_t heaterToControllerHeader() {
  return makeHeader(WBUS_ADDR_HEATER, WBUS_ADDR_CONTROLLER);
}

static uint16_t be16(uint8_t hi, uint8_t lo) {
  return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}

static void putBe16(uint8_t* dst, uint16_t v) {
  dst[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
  dst[1] = static_cast<uint8_t>(v & 0xFF);
}

static void sendFrame(uint8_t cmdWithAckBit, const uint8_t* data, uint8_t dataLen) {
  // Wire format:
  // header, length (= cmd + data + checksum), cmd, data..., checksum
  const uint8_t header = heaterToControllerHeader();
  const uint8_t length = static_cast<uint8_t>(dataLen + 2);

  uint8_t csum = 0;
  csum ^= header;
  csum ^= length;
  csum ^= cmdWithAckBit;
  for (uint8_t i = 0; i < dataLen; i++) csum ^= data[i];

  WBUS_SERIAL.write(header);
  WBUS_SERIAL.write(length);
  WBUS_SERIAL.write(cmdWithAckBit);
  if (dataLen > 0) WBUS_SERIAL.write(data, dataLen);
  WBUS_SERIAL.write(csum);
  WBUS_SERIAL.flush();
  
  Serial.printf("WBUS TX: hdr=0x%02X len=%u cmd=0x%02X dataLen=%u csum=0x%02X\n", header, length, cmdWithAckBit, dataLen, csum);
}

enum class SimState : uint8_t {
  Off,
  Starting,
  Running,
  Cooling,
  Error,
  TempOvershoot,
  FlameOutRestart,
};

enum class SimScenario : uint8_t {
  Normal,          // Smooth startup and running
  FlameFlutter,    // Flame drops and restarts
  HighTemp,        // Temperature overshoots, triggers cooling
  VoltageDropped,  // Temporary voltage sag
  ErrorShutdown,   // Automatic shutdown on error
};

struct SimModel {
  SimState state = SimState::Off;
  uint32_t stateSinceMs = 0;
  uint8_t requestedMinutes = 0;

  float ambientC = 20.0f;
  float tempC = 20.0f;
  float targetTempC = 75.0f;

  uint16_t voltage_mV = 12400;
  uint16_t heaterPower_x10 = 0;
  uint16_t combustionFanRpm = 0;
  uint16_t glowResistance_mOhm = 1800;

  bool flame = false;

  // Scenario tracking
  SimScenario scenario = SimScenario::Normal;
  uint32_t scenarioStartMs = 0;
  bool scenarioTriggered = false;

  // Noise/jitter for realism
  float tempNoise = 0.0f;
  float powerNoise = 0.0f;
  uint16_t voltageNoise = 0;

  void setState(SimState s) {
    state = s;
    stateSinceMs = millis();
    Serial.print("  [STATE] s=");
    Serial.println(static_cast<int>(s));
  }

  uint8_t opStateCode() const {
    switch (state) {
      case SimState::Off:
        return 0x04;
      case SimState::Starting:
        return 0x01;
      case SimState::Running:
        return 0x06;
      case SimState::Cooling:
        return 0x02;
      case SimState::TempOvershoot:
        return 0x06; // Still running, but overheated
      case SimState::FlameOutRestart:
        return 0x01; // Restarting after flame out
      case SimState::Error:
        return 0xFF;
    }
    return 0x04;
  }

  // Select a random scenario for variety
  void pickRandomScenario() {
    uint8_t rand = random(100);
    if (rand < 60) {
      scenario = SimScenario::Normal;
    } else if (rand < 75) {
      scenario = SimScenario::FlameFlutter;
      Serial.println("[SCENARIO] FlameFlutter");
    } else if (rand < 85) {
      scenario = SimScenario::HighTemp;
      Serial.println("[SCENARIO] HighTemp");
    } else if (rand < 95) {
      scenario = SimScenario::VoltageDropped;
      Serial.println("[SCENARIO] Voltage");
    } else {
      scenario = SimScenario::ErrorShutdown;
      Serial.println("[SCENARIO] Error");
    }
    scenarioStartMs = millis();
    scenarioTriggered = false;
  }

  void tick() {
    const uint32_t now = millis();
    const uint32_t elapsedMs = now - stateSinceMs;

    // Update noise values for realism
    tempNoise = (random(200) - 100) * 0.01f; // ±1°C noise
    powerNoise = (random(30) - 15); // ±15 power units
    voltageNoise = random(100) - 50; // ±50mV noise

    // State machine with scenario support
    switch (state) {
      case SimState::Starting: {
        // Check for flame flicker scenario
        if (scenario == SimScenario::FlameFlutter && !scenarioTriggered && elapsedMs > 8000) {
          scenarioTriggered = true;
          Serial.println("  [SCENARIO] Flame out detected, restarting...");
          setState(SimState::FlameOutRestart);
        }
        // Check for error scenario
        else if (scenario == SimScenario::ErrorShutdown && !scenarioTriggered && elapsedMs > 10000) {
          scenarioTriggered = true;
          Serial.println("  [SCENARIO] Error detected during startup!");
          setState(SimState::Error);
        }
        // Normal progression
        else if (elapsedMs > 15000) {
          setState(SimState::Running);
          pickRandomScenario(); // Pick next scenario for running phase
        }
        break;
      }

      case SimState::Running: {
        // Temperature overshoot check
        if (scenario == SimScenario::HighTemp && !scenarioTriggered && tempC > 80.0f) {
          scenarioTriggered = true;
          targetTempC = 85.0f;
          Serial.println("  [SCENARIO] Temperature overshoot, cooling initiated");
          setState(SimState::TempOvershoot);
        }
        break;
      }

      case SimState::TempOvershoot: {
        // Cool back down
        if (tempC < 70.0f) {
          targetTempC = 75.0f;
          setState(SimState::Running);
        }
        break;
      }

      case SimState::FlameOutRestart: {
        // Try to restart
        if (elapsedMs > 3000) {
          setState(SimState::Starting);
        }
        break;
      }

      case SimState::Cooling: {
        if (elapsedMs > 20000) {
          setState(SimState::Off);
        }
        break;
      }

      case SimState::Error: {
        // Auto-recover after 5 seconds
        if (elapsedMs > 5000) {
          setState(SimState::Off);
        }
        break;
      }

      default:
        break;
    }

    // Temperature dynamics with noise
    if (state == SimState::Off) {
      flame = false;
      heaterPower_x10 = 0;
      combustionFanRpm = 0;
      tempC += (ambientC - tempC) * 0.08f + tempNoise;

    } else if (state == SimState::Starting) {
      flame = false;
      heaterPower_x10 = static_cast<uint16_t>(constrain(250 + powerNoise, 0, 300));
      combustionFanRpm = 1800 + random(200) - 100;
      tempC += (targetTempC - tempC) * 0.03f + tempNoise;

    } else if (state == SimState::Running) {
      // Flame flickering scenario
      if (scenario == SimScenario::FlameFlutter) {
        uint32_t phase = (now / 500) % 4;
        flame = (phase < 3);
      } else {
        flame = true;
      }

      heaterPower_x10 = static_cast<uint16_t>(constrain(700 + powerNoise, 600, 800));
      combustionFanRpm = 4200 + random(300) - 150;
      tempC += (targetTempC - tempC) * 0.02f + tempNoise;

    } else if (state == SimState::TempOvershoot) {
      flame = true;
      // Reduce power to cool down faster
      heaterPower_x10 = static_cast<uint16_t>(constrain(400 + powerNoise, 300, 500));
      combustionFanRpm = 4500;
      tempC += (targetTempC - tempC) * 0.025f + tempNoise;

    } else if (state == SimState::FlameOutRestart) {
      flame = false;
      heaterPower_x10 = static_cast<uint16_t>(constrain(300 + powerNoise, 200, 400));
      combustionFanRpm = 2000 + random(300);
      tempC += (targetTempC - tempC) * 0.02f + tempNoise;

    } else if (state == SimState::Cooling) {
      flame = false;
      heaterPower_x10 = static_cast<uint16_t>(constrain(100 + powerNoise, 50, 150));
      combustionFanRpm = 1500 + random(200) - 100;
      tempC += (ambientC - tempC) * 0.03f + tempNoise;

    } else if (state == SimState::Error) {
      flame = false;
      heaterPower_x10 = 0;
      combustionFanRpm = random(500);
      tempC += (ambientC - tempC) * 0.05f;
    }

    // Clamp temperature to reasonable range
    tempC = constrain(tempC, ambientC - 5, 120.0f);

    // Voltage dynamics
    if (state == SimState::Off) {
      voltage_mV = 12400 + voltageNoise;
    } else {
      // Voltage sag proportional to load
      uint16_t sag = static_cast<uint16_t>((heaterPower_x10 / 10) + (combustionFanRpm / 50));
      voltage_mV = static_cast<uint16_t>(12400 - sag + voltageNoise);
    }

    // Voltage bounds
    voltage_mV = constrain(voltage_mV, 11000, 13200);
  }
};

static SimModel gSim;

static void respondOperatingState() {
  const uint8_t data[2] = {0x07, gSim.opStateCode()};
  sendFrame(static_cast<uint8_t>(0x50 | 0x80), data, sizeof(data));
}

static void respondSimple05() {
  // Receiver expects payloadLen >= 10.
  // Layout (based on receiver logging):
  // [cmdAck=0xD0][idx=0x05][tempRaw][voltage_be16][flame][heaterPower_x10_be16][dummy]
  uint8_t data[1 + 1 + 1 + 2 + 1 + 2 + 1] = {0};
  uint8_t pos = 0;
  data[pos++] = 0x05;

  const int tempRaw = static_cast<int>(lroundf(gSim.tempC)) + 50;
  data[pos++] = static_cast<uint8_t>(constrain(tempRaw, 0, 255));

  putBe16(&data[pos], gSim.voltage_mV);
  pos += 2;

  data[pos++] = gSim.flame ? 0x01 : 0x00;

  putBe16(&data[pos], gSim.heaterPower_x10);
  pos += 2;

  data[pos++] = gSim.opStateCode();

  sendFrame(static_cast<uint8_t>(0x50 | 0x80), data, pos);
}

static void respondSimple0F() {
  // Receiver expects: idx 0x0F and 3 bytes (gpp, fpf, afp).
  // We'll encode plausible values 0..255.
  uint8_t data[1 + 3] = {0};
  data[0] = 0x0F;

  const uint8_t glow = (gSim.state == SimState::Starting) ? 80 : 10;
  const uint8_t pump = (gSim.state == SimState::Running) ? 60 : 0;
  const uint8_t fan = static_cast<uint8_t>(constrain(gSim.combustionFanRpm / 100, 0, 255));

  data[1] = glow;
  data[2] = pump;
  data[3] = fan;

  sendFrame(static_cast<uint8_t>(0x50 | 0x80), data, sizeof(data));
}

static void respondSimpleFlags(uint8_t idx) {
  uint8_t flags = 0x00;
  if (gSim.state == SimState::Running) flags |= 0x01;
  if (gSim.state == SimState::Starting) flags |= 0x02;
  if (gSim.state == SimState::Cooling) flags |= 0x04;
  if (gSim.state == SimState::Error) flags |= 0x80;

  const uint8_t data[2] = {idx, flags};
  sendFrame(static_cast<uint8_t>(0x50 | 0x80), data, sizeof(data));
}

// Status page 0x03: State flags bitfield (per esphome-webasto)
static void respondPage03() {
  // Layout: [idx=0x03][flags]
  // Flags: 0x01=heat_request, 0x02=vent_request, 0x10=combustion_fan,
  //        0x20=glowplug, 0x40=fuel_pump, 0x80=nozzle_heating
  uint8_t flags = 0x00;
  if (gSim.state == SimState::Running) {
    flags |= 0x01; // heat_request
    flags |= 0x10; // combustion_fan
    flags |= 0x40; // fuel_pump
  }
  if (gSim.state == SimState::Starting) {
    flags |= 0x20; // glowplug
    flags |= 0x10; // combustion_fan
  }

  const uint8_t data[2] = {0x03, flags};
  sendFrame(static_cast<uint8_t>(0x50 | 0x80), data, sizeof(data));
}

// Status page 0x04: Actuator percentages (8 bytes per esphome-webasto)
static void respondPage04() {
  // Layout: [idx=0x04][8 bytes: unknown x4, glowplug%, fuel_pump, combustion_fan%, unknown]
  uint8_t data[1 + 8] = {0};
  data[0] = 0x04;

  // Bytes 1-4: unknown, keep 0
  // Byte 5 (idx 4 in esphome): glowplug% (0-100)
  const uint8_t glowPct = (gSim.state == SimState::Starting) ? 80 : 0;
  data[5] = glowPct;

  // Byte 6 (idx 5): fuel pump Hz * 100 / 2 (encoding: raw * 2 / 100 = Hz)
  const uint8_t fuelRaw = (gSim.state == SimState::Running) ? 150 : 0; // ~3 Hz
  data[6] = fuelRaw;

  // Byte 7 (idx 6): combustion fan % (0-200)
  const uint8_t fanPct = (gSim.state == SimState::Running) ? 100 :
                         (gSim.state == SimState::Starting) ? 50 :
                         (gSim.state == SimState::Cooling) ? 40 : 0;
  data[7] = fanPct;

  // Byte 8: unknown
  data[8] = 0;

  sendFrame(static_cast<uint8_t>(0x50 | 0x80), data, sizeof(data));
}

// Status page 0x06: Counters (8 bytes per esphome-webasto)
static void respondPage06() {
  // Layout: [idx=0x06][8 bytes]
  // byte0,1=working_hours, byte2=working_minutes
  // byte3,4=operating_hours, byte5=operating_minutes
  // byte6,7=start_counter
  uint8_t data[1 + 8] = {0};
  data[0] = 0x06;

  // Fake counter values
  const uint16_t workingHrs = 123;
  const uint8_t workingMins = 45;
  const uint16_t operatingHrs = 456;
  const uint8_t operatingMins = 30;
  const uint16_t startCount = 789;

  data[1] = static_cast<uint8_t>((workingHrs >> 8) & 0xFF);
  data[2] = static_cast<uint8_t>(workingHrs & 0xFF);
  data[3] = workingMins;
  data[4] = static_cast<uint8_t>((operatingHrs >> 8) & 0xFF);
  data[5] = static_cast<uint8_t>(operatingHrs & 0xFF);
  data[6] = operatingMins;
  data[7] = static_cast<uint8_t>((startCount >> 8) & 0xFF);
  data[8] = static_cast<uint8_t>(startCount & 0xFF);

  sendFrame(static_cast<uint8_t>(0x50 | 0x80), data, sizeof(data));
}

static void respondMultiStatus(const WBusPacket& req) {
  // Request payload: [0]=0x50, [1]=0x30, then IDs, then checksum.
  if (req.payloadLen < 4) return;
  if (req.payload[1] != 0x30) return;

  uint8_t out[220] = {0};
  uint8_t outLen = 0;
  out[outLen++] = 0x30; // subcommand

  const uint8_t idsStart = 2;
  const uint8_t idsEnd = static_cast<uint8_t>(req.payloadLen - 1); // exclude checksum

  for (uint8_t i = idsStart; i < idsEnd; i++) {
    const uint8_t id = req.payload[i];
    out[outLen++] = id;

    switch (id) {
      // 1-byte fields
      case 0x01: // status_01
      case 0x03:
      case 0x05:
      case 0x06:
      case 0x07:
      case 0x08:
      case 0x0A:
      case 0x10:
      case 0x1F:
      case 0x24:
      case 0x27:
      case 0x2A:
      case 0x2C:
      case 0x2D:
      case 0x32: {
        // Provide some stable but stateful values.
        uint8_t v = 0;
        if (id == 0x07) v = gSim.opStateCode();
        if (id == 0x05) v = gSim.flame ? 1 : 0;
        out[outLen++] = v;
        break;
      }

      case 0x0C: { // temperature raw (temp+50)
        const int tempRaw = static_cast<int>(lroundf(gSim.tempC)) + 50;
        out[outLen++] = static_cast<uint8_t>(constrain(tempRaw, 0, 255));
        break;
      }

      // 2-byte fields (big-endian)
      case 0x0E: { // voltage mV
        putBe16(&out[outLen], gSim.voltage_mV);
        outLen += 2;
        break;
      }

      case 0x0F: { // status_0F
        putBe16(&out[outLen], static_cast<uint16_t>(gSim.flame ? 0x0001 : 0x0000));
        outLen += 2;
        break;
      }

      case 0x11: { // power
        putBe16(&out[outLen], gSim.heaterPower_x10);
        outLen += 2;
        break;
      }

      case 0x13: { // glow resistance
        putBe16(&out[outLen], gSim.glowResistance_mOhm);
        outLen += 2;
        break;
      }

      case 0x1E: { // combustion fan
        putBe16(&out[outLen], static_cast<uint16_t>(constrain(gSim.combustionFanRpm, 0, 65535)));
        outLen += 2;
        break;
      }

      case 0x29:
      case 0x34:
      case 0x3D:
      case 0x52:
      case 0x57:
      case 0x5F:
      case 0x78:
      case 0x89: {
        // Provide two bytes for these (parser accepts 2 bytes for most; for the heuristic IDs
        // returning 2 bytes is fine as long as the following byte is another known ID or end).
        putBe16(&out[outLen], 0);
        outLen += 2;
        break;
      }

      default:
        // Unknown ID requested: remove the ID we appended and skip.
        outLen--;
        break;
    }

    if (outLen > (sizeof(out) - 4)) break;
  }

  // Send response: cmdAck=0xD0 and data starting with subcommand+TLVs.
  sendFrame(static_cast<uint8_t>(0x50 | 0x80), out, outLen);
}

static void handlePacket(const WBusPacket& pkt) {
  // Only respond to controller->heater frames.
  if (pkt.header != controllerToHeaterHeader()) return;
  if (pkt.payloadLen < 2) return;

  const uint8_t cmd = pkt.payload[0];

  switch (cmd) {
    case 0x21: { // parking heater start with minutes
      if (pkt.payloadLen < 3) break;
      gSim.requestedMinutes = pkt.payload[1];
      gSim.setState(SimState::Starting);
      gSim.pickRandomScenario(); // Pick a scenario for this startup
      const uint8_t ack[1] = {gSim.requestedMinutes};
      sendFrame(static_cast<uint8_t>(0x21 | 0x80), ack, sizeof(ack));
      Serial.printf("[WBUS SIM] START HEATING for %u minutes\n", gSim.requestedMinutes);
      break;
    }

    case 0x22: { // ventilation start with minutes
      if (pkt.payloadLen < 3) break;
      gSim.requestedMinutes = pkt.payload[1];
      gSim.setState(SimState::Starting);
      // ACK echoes the minutes
      const uint8_t ack[1] = {gSim.requestedMinutes};
      sendFrame(static_cast<uint8_t>(0x22 | 0x80), ack, sizeof(ack));
      Serial.printf("WBUS SIM: vent %u min\n", gSim.requestedMinutes);
      break;
    }

    case 0x10: { // stop
      if (gSim.state != SimState::Off) gSim.setState(SimState::Cooling);
      sendFrame(static_cast<uint8_t>(0x10 | 0x80), nullptr, 0);
      Serial.println("[WBUS SIM] STOP HEATING - cooling initiated");
      break;
    }

    case 0x44: { // keep-alive
      sendFrame(static_cast<uint8_t>(0x44 | 0x80), nullptr, 0);
      break;
    }

    case 0x50: { // status requests
      // Data begins at payload[1].
      // Note: payloadLen includes the checksum byte, so minimum length is 3 (cmd + idx + checksum)
      if (pkt.payloadLen < 3) break;
      const uint8_t idxOrSub = pkt.payload[1];
      Serial.printf("WBUS SIM: status request idx=0x%02X\n", idxOrSub);
      if (idxOrSub == 0x30) {
        respondMultiStatus(pkt);
      } else if (idxOrSub == 0x07) {
        respondOperatingState();
      } else if (idxOrSub == 0x05) {
        respondSimple05();
      } else if (idxOrSub == 0x0F) {
        respondSimple0F();
      } else if (idxOrSub == 0x03) {
        respondPage03();
      } else if (idxOrSub == 0x04) {
        respondPage04();
      } else if (idxOrSub == 0x06) {
        respondPage06();
      } else if (idxOrSub == 0x02) {
        respondSimpleFlags(idxOrSub);
      } else {
        // Unknown page; reply with just cmdAck+idx.
        const uint8_t data[1] = {idxOrSub};
        sendFrame(static_cast<uint8_t>(0x50 | 0x80), data, sizeof(data));
      }
      break;
    }

    default:
      // Unhandled command: reply with cmd|0x80 and no data.
      sendFrame(static_cast<uint8_t>(cmd | 0x80), nullptr, 0);
      break;
  }
}

} // namespace

void setup() {
  delay(500);  // Extra delay for serial port to stabilize
  Serial.begin(115200);
  delay(300);

  // Note: W-BUS UART will be initialized AFTER serial setup to avoid conflicts
  
  Serial.println("\n\n======= SIMULATOR BOOT =======");
  Serial.flush();
  delay(100);
  
  // Initialize W-BUS serial port (Serial2 on different pins)
  WBUS_SERIAL.begin(2400, SERIAL_8E1, WBUS_RX_PIN, WBUS_TX_PIN);
  delay(100);
  
  Serial.println("W-BUS initialized");
  Serial.flush();

  gSim.setState(SimState::Off);
  gSim.tempC = gSim.ambientC;
  
  Serial.println("READY");
  Serial.flush();
}

void loop() {
  static uint32_t lastDebug = 0;
  static uint32_t lastByteDebug = 0;
  static uint32_t byteCount = 0;
  static WBusSimple wbus(WBUS_SERIAL);
  static bool wbusInitialized = false;
  
  uint32_t now = millis();
  
  // Initialize WBusSimple on first call
  if (!wbusInitialized) {
    wbus.begin();
    wbusInitialized = true;
  }
  
  // Print periodic heartbeat
  if (now - lastDebug > 5000) {
    Serial.printf("Alive: %lu ms, state=%d, bytes_rx=%lu\n", now, static_cast<int>(gSim.state), byteCount);
    lastDebug = now;
    byteCount = 0;
  }
  
  gSim.tick();

  // Count incoming bytes for debugging
  if (WBUS_SERIAL.available() > 0) {
    const uint32_t avail = WBUS_SERIAL.available();
    byteCount += avail;
    if (now - lastByteDebug > 1000) {
      Serial.printf("WBUS: %lu bytes available\n", avail);
      lastByteDebug = now;
    }
  }

  // Process incoming W-BUS frames
  wbus.poll();
  WBusPacket pkt;
  while (wbus.popPacket(pkt)) {
    Serial.printf("WBUS RX: hdr=0x%02X cmd=0x%02X len=%u\n", pkt.header, pkt.payload[0], pkt.payloadLen);
    handlePacket(pkt);
  }

  delay(10);
}
