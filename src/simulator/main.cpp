#include <Arduino.h>

#include "project_config.h"
#include "wbus_simple.h"

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

  Serial2.write(header);
  Serial2.write(length);
  Serial2.write(cmdWithAckBit);
  if (dataLen > 0) Serial2.write(data, dataLen);
  Serial2.write(csum);
  Serial2.flush();
}

enum class SimState : uint8_t {
  Off,
  Starting,
  Running,
  Cooling,
  Error,
};

struct SimModel {
  SimState state = SimState::Off;
  uint32_t stateSinceMs = 0;
  uint8_t requestedMinutes = 0;

  float ambientC = 20.0f;
  float tempC = 20.0f;

  uint16_t voltage_mV = 12400;
  uint16_t heaterPower_x10 = 0; // in 0.1% / 0.1 units depending on page
  uint16_t combustionFanRpm = 0;
  uint16_t glowResistance_mOhm = 1800;

  bool flame = false;

  void setState(SimState s) {
    state = s;
    stateSinceMs = millis();
  }

  uint8_t opStateCode() const {
    // Coarse mapping. Receiver currently treats anything except 0x04/0x00 as “Running”.
    switch (state) {
      case SimState::Off:
        return 0x04;
      case SimState::Starting:
        return 0x01;
      case SimState::Running:
        return 0x06;
      case SimState::Cooling:
        return 0x02;
      case SimState::Error:
        return 0xFF;
    }
    return 0x04;
  }

  void tick() {
    const uint32_t now = millis();
    const float targetRunC = 75.0f;

    // Simple timing/state progression.
    switch (state) {
      case SimState::Starting:
        if (now - stateSinceMs > 15000) setState(SimState::Running);
        break;
      case SimState::Cooling:
        if (now - stateSinceMs > 20000) setState(SimState::Off);
        break;
      default:
        break;
    }

    // Update synthetic measurements.
    const float dt = 0.1f; // approximate smoothing, not wall-clock precise

    if (state == SimState::Off) {
      flame = false;
      heaterPower_x10 = 0;
      combustionFanRpm = 0;
      // Cool toward ambient
      tempC += (ambientC - tempC) * 0.08f;
    } else if (state == SimState::Starting) {
      flame = false;
      heaterPower_x10 = 250; // some preheat-ish value
      combustionFanRpm = 1800;
      tempC += (targetRunC - tempC) * 0.03f;
      (void)dt;
    } else if (state == SimState::Running) {
      flame = true;
      heaterPower_x10 = 700;
      combustionFanRpm = 4200;
      tempC += (targetRunC - tempC) * 0.02f;
    } else if (state == SimState::Cooling) {
      flame = false;
      heaterPower_x10 = 100;
      combustionFanRpm = 1500;
      tempC += (ambientC - tempC) * 0.03f;
    } else {
      flame = false;
      heaterPower_x10 = 0;
      combustionFanRpm = 0;
    }

    // Very slight voltage sag when active.
    if (state == SimState::Off) {
      voltage_mV = 12400;
    } else {
      voltage_mV = 12150;
    }
  }
};

static SimModel gSim;
static WBusSimple gRxParser;

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
    case 0x21: { // start with minutes
      if (pkt.payloadLen < 3) break;
      gSim.requestedMinutes = pkt.payload[1];
      gSim.setState(SimState::Starting);
      // Optional ACK
      sendFrame(static_cast<uint8_t>(0x21 | 0x80), nullptr, 0);
      Serial.printf("WBUS SIM: start %u min\n", gSim.requestedMinutes);
      break;
    }

    case 0x10: { // stop
      if (gSim.state != SimState::Off) gSim.setState(SimState::Cooling);
      sendFrame(static_cast<uint8_t>(0x10 | 0x80), nullptr, 0);
      Serial.println("WBUS SIM: stop");
      break;
    }

    case 0x44: { // keep-alive
      sendFrame(static_cast<uint8_t>(0x44 | 0x80), nullptr, 0);
      break;
    }

    case 0x50: { // status requests
      // Data begins at payload[1].
      if (pkt.payloadLen < 4) break;
      const uint8_t idxOrSub = pkt.payload[1];
      if (idxOrSub == 0x30) {
        respondMultiStatus(pkt);
      } else if (idxOrSub == 0x07) {
        respondOperatingState();
      } else if (idxOrSub == 0x05) {
        respondSimple05();
      } else if (idxOrSub == 0x0F) {
        respondSimple0F();
      } else if (idxOrSub == 0x02 || idxOrSub == 0x03 || idxOrSub == 0x06) {
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
  Serial.begin(115200);
  delay(200);

  Serial.println("WBUS simulator (ThermoTop-like) starting...");
  Serial.printf("UART pins: RX=%d TX=%d\n", WBUS_RX_PIN, WBUS_TX_PIN);
  Serial.printf("WBUS addrs: controller=0x%X heater=0x%X\n", WBUS_ADDR_CONTROLLER, WBUS_ADDR_HEATER);
  Serial.printf("Headers: ctrl->heat=0x%02X heat->ctrl=0x%02X\n", controllerToHeaterHeader(), heaterToControllerHeader());

  // Use the existing parser for validated frame capture.
  gRxParser.begin();

  gSim.setState(SimState::Off);
  gSim.tempC = gSim.ambientC;
}

void loop() {
  gSim.tick();

  gRxParser.poll();

  WBusPacket pkt;
  while (gRxParser.popPacket(pkt)) {
    handlePacket(pkt);
  }

  delay(10);
}
