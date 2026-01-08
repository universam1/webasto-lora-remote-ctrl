#include "wbus_simple.h"

static constexpr uint16_t WBUS_MAX_LEN = 256;

static uint16_t be16(uint8_t hi, uint8_t lo) {
  return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}

static uint8_t makeHeader(uint8_t src, uint8_t dst) {
  return static_cast<uint8_t>(((src & 0x0F) << 4) | (dst & 0x0F));
}

static uint8_t wbusTxHeader() {
  return makeHeader(WBUS_ADDR_CONTROLLER, WBUS_ADDR_HEATER);
}

static uint8_t wbusRxHeader() {
  return makeHeader(WBUS_ADDR_HEATER, WBUS_ADDR_CONTROLLER);
}

void WBusSimple::maybeEnableTx(bool enable) {
  if (WBUS_EN_PIN < 0) return;
  pinMode(WBUS_EN_PIN, OUTPUT);
  digitalWrite(WBUS_EN_PIN, enable ? HIGH : LOW);
}

void WBusSimple::sendBreakPulse() {
  // Many heaters expect an initial "break" (dominant low) before first command.
  // Exact timing depends on the physical interface; this follows the common pattern:
  // idle-high delay -> low break -> recovery -> re-enable UART.
  if (WBUS_TX_PIN < 0) return;

  port.flush();
  port.end();

  pinMode(WBUS_TX_PIN, OUTPUT);
  digitalWrite(WBUS_TX_PIN, HIGH);
  delay(1000);

  digitalWrite(WBUS_TX_PIN, LOW);
  delay(50);

  digitalWrite(WBUS_TX_PIN, HIGH);
  delay(50);

  // Release line. If your interface requires open-collector, you may prefer INPUT here.
  // We keep it HIGH to avoid floating when using a level-shifter/transceiver.

  port.begin(2400, SERIAL_8E1, WBUS_RX_PIN, WBUS_TX_PIN);
}

bool WBusSimple::begin() {
  if (WBUS_RX_PIN < 0 || WBUS_TX_PIN < 0) return false;

  maybeEnableTx(false);

  port.begin(2400, SERIAL_8E1, WBUS_RX_PIN, WBUS_TX_PIN);
  didBreak = false;
  rxState = RxState::FindHeader;
  rxIndex = 0;
  havePacket = false;
  return true;
}

bool WBusSimple::sendCommand(uint8_t cmd, const uint8_t* data, uint8_t dataLen) {
  if (WBUS_SEND_BREAK && !didBreak) {
    sendBreakPulse();
    didBreak = true;
  }

  // Wire format:
  // header (0xF4), length (= payload bytes + checksum byte), payload (cmd + data...), checksum (XOR)
  uint8_t header = makeHeader(WBUS_ADDR_CONTROLLER, WBUS_ADDR_HEATER);
  uint8_t length = static_cast<uint8_t>(dataLen + 2); // cmd + data + checksum

  uint8_t csum = 0;
  csum ^= header;
  csum ^= length;
  csum ^= cmd;
  for (uint8_t i = 0; i < dataLen; i++) csum ^= data[i];

  maybeEnableTx(true);
  port.write(header);
  port.write(length);
  port.write(cmd);
  if (dataLen > 0) port.write(data, dataLen);
  port.write(csum);
  port.flush();
  maybeEnableTx(false);

  return true;
}

void WBusSimple::poll() {
  while (port.available() > 0) {
    uint8_t b = static_cast<uint8_t>(port.read());

    switch (rxState) {
      case RxState::FindHeader: {
        const uint8_t tx = wbusTxHeader();
        const uint8_t rx = wbusRxHeader();
        if (b == tx || b == rx) {
          rxInProgress = WBusPacket{};
          rxInProgress.header = b;
          rxState = RxState::ReadLength;
        }
        break;
      }

      case RxState::ReadLength: {
        rxInProgress.length = b;
        rxInProgress.payloadLen = 0;
        rxIndex = 0;

        // length counts (payload + checksum) and must be at least 2 (cmd + checksum).
        if (rxInProgress.length < 2 || rxInProgress.length > WBUS_MAX_LEN) {
          rxState = RxState::FindHeader;
          break;
        }
        if (rxInProgress.length > sizeof(rxInProgress.payload)) {
          rxState = RxState::FindHeader;
          break;
        }

        rxInProgress.payloadLen = rxInProgress.length;
        rxState = RxState::ReadPayload;
        break;
      }

      case RxState::ReadPayload: {
        if (rxIndex < rxInProgress.payloadLen) {
          rxInProgress.payload[rxIndex++] = b;
        }

        if (rxIndex >= rxInProgress.payloadLen) {
          const uint8_t expected = rxInProgress.checksum();
          const uint8_t got = rxInProgress.payload[rxInProgress.payloadLen - 1];

          if (expected == got) {
            // Single-slot queue: keep the most recent valid packet.
            packetQueue = rxInProgress;
            havePacket = true;
          }

          rxState = RxState::FindHeader;
          rxIndex = 0;
        }
        break;
      }
    }
  }
}

bool WBusSimple::popPacket(WBusPacket& out) {
  if (!havePacket) return false;
  out = packetQueue;
  havePacket = false;
  return true;
}

bool WBusSimple::tryParseStatusTlv(const WBusPacket& pkt, WBusStatus& out) {
  // Expected payload layout for many "multi status" responses:
  // [0]=0xD0 (0x50|0x80), [1]=0x30, then TLV-ish pairs/tuples: <id><value..>
  if (pkt.payloadLen < 4) return false;
  if ((pkt.payload[0] & 0x7F) != 0x50) return false;
  if (pkt.payload[1] != 0x30) return false;

  WBusStatus s;

  // Start after the 2-byte prefix (type + sub-type)
  uint16_t pos = 2;
  const uint16_t end = static_cast<uint16_t>(pkt.payloadLen - 1); // exclude checksum

  auto need = [&](uint16_t n) { return static_cast<uint16_t>(pos + n) <= end; };

  auto isKnownId = [&](uint8_t candidate) {
    switch (candidate) {
      case 0x01:
      case 0x03:
      case 0x05:
      case 0x06:
      case 0x07:
      case 0x08:
      case 0x0A:
      case 0x0C:
      case 0x0E:
      case 0x0F:
      case 0x10:
      case 0x11:
      case 0x13:
      case 0x1E:
      case 0x1F:
      case 0x24:
      case 0x27:
      case 0x29:
      case 0x2A:
      case 0x2C:
      case 0x2D:
      case 0x32:
      case 0x34:
      case 0x3D:
      case 0x52:
      case 0x57:
      case 0x5F:
      case 0x78:
      case 0x89:
        return true;
      default:
        return false;
    }
  };

  // Some IDs appear in public “multi-status” request lists but their payload size
  // is not well documented and may vary by heater/firmware. To avoid hard-failing
  // (or desyncing parsing), we use a small heuristic:
  // - Prefer 2-byte big-endian when the next byte after 2 bytes looks like another known ID.
  // - Otherwise fall back to 1-byte.
  auto parseMaybeU16 = [&](uint16_t& dst) {
    if (need(2)) {
      const uint8_t b0 = pkt.payload[pos];
      const uint8_t b1 = pkt.payload[pos + 1];
      const uint16_t after = static_cast<uint16_t>(pos + 2);
      if (after >= end || isKnownId(pkt.payload[after])) {
        dst = be16(b0, b1);
        pos += 2;
        return true;
      }
    }
    if (need(1)) {
      const uint8_t b0 = pkt.payload[pos];
      const uint16_t after = static_cast<uint16_t>(pos + 1);
      if (after >= end || isKnownId(pkt.payload[after])) {
        dst = b0;
        pos += 1;
        return true;
      }
    }
    return false;
  };

  while (pos < end) {
    const uint8_t id = pkt.payload[pos++];
    switch (id) {
      // 1-byte fields
      case 0x01: if (!need(1)) return false; s.status_01 = pkt.payload[pos++]; break;
      case 0x03: if (!need(1)) return false; s.status_03 = pkt.payload[pos++]; break;
      case 0x05: if (!need(1)) return false; s.status_05 = pkt.payload[pos++]; break;
      case 0x06: if (!need(1)) return false; s.status_06 = pkt.payload[pos++]; break;
      case 0x07: if (!need(1)) return false; s.status_07 = pkt.payload[pos++]; break;
      case 0x08: if (!need(1)) return false; s.status_08 = pkt.payload[pos++]; break;
      case 0x0A: if (!need(1)) return false; s.status_0A = pkt.payload[pos++]; break;
      case 0x10: if (!need(1)) return false; s.status_10 = pkt.payload[pos++]; break;
      case 0x1F: if (!need(1)) return false; s.status_1F = pkt.payload[pos++]; break;
      case 0x24: if (!need(1)) return false; s.status_24 = pkt.payload[pos++]; break;
      case 0x27: if (!need(1)) return false; s.status_27 = pkt.payload[pos++]; break;
      case 0x2A: if (!need(1)) return false; s.status_2A = pkt.payload[pos++]; break;
      case 0x2C: if (!need(1)) return false; s.status_2C = pkt.payload[pos++]; break;
      case 0x2D: if (!need(1)) return false; s.status_2D = pkt.payload[pos++]; break;
      case 0x32: if (!need(1)) return false; s.status_32 = pkt.payload[pos++]; break;

      // temperature (commonly raw-50)
      case 0x0C: {
        if (!need(1)) return false;
        s.temperatureC = static_cast<int16_t>(static_cast<int>(pkt.payload[pos++]) - 50);
        break;
      }

      // 2-byte fields (big-endian)
      case 0x0E: {
        if (!need(2)) return false;
        s.voltage_mV = be16(pkt.payload[pos], pkt.payload[pos + 1]);
        pos += 2;
        break;
      }
      case 0x0F: {
        if (!need(2)) return false;
        s.status_0F = be16(pkt.payload[pos], pkt.payload[pos + 1]);
        pos += 2;
        break;
      }
      case 0x11: {
        if (!need(2)) return false;
        s.power = be16(pkt.payload[pos], pkt.payload[pos + 1]);
        pos += 2;
        break;
      }
      case 0x13: {
        if (!need(2)) return false;
        s.glowResistance_mOhm = be16(pkt.payload[pos], pkt.payload[pos + 1]);
        pos += 2;
        break;
      }
      case 0x1E: {
        if (!need(2)) return false;
        s.combustionFan = be16(pkt.payload[pos], pkt.payload[pos + 1]);
        pos += 2;
        break;
      }
      case 0x29: {
        if (!need(2)) return false;
        s.status_29 = be16(pkt.payload[pos], pkt.payload[pos + 1]);
        pos += 2;
        break;
      }
      case 0x34: {
        if (!need(2)) return false;
        s.status_34 = be16(pkt.payload[pos], pkt.payload[pos + 1]);
        pos += 2;
        break;
      }

      // Additional IDs seen in common multi-status request lists
      case 0x3D: {
        // Often appears as two bytes (example traces show 0x3D 0x01 0x0E)
        if (!need(2)) return false;
        s.status_3D = be16(pkt.payload[pos], pkt.payload[pos + 1]);
        pos += 2;
        break;
      }
      case 0x52: {
        // Often appears as two bytes (example traces show 0x52 0x00 0x00)
        if (!need(2)) return false;
        s.status_52 = be16(pkt.payload[pos], pkt.payload[pos + 1]);
        pos += 2;
        break;
      }
      case 0x57: {
        if (!parseMaybeU16(s.status_57)) return false;
        break;
      }
      case 0x5F: {
        if (!parseMaybeU16(s.status_5F)) return false;
        break;
      }
      case 0x78: {
        if (!parseMaybeU16(s.status_78)) return false;
        break;
      }
      case 0x89: {
        if (!parseMaybeU16(s.status_89)) return false;
        break;
      }

      default:
        // Unknown field. We don't know if it's 1 or 2 bytes (or more), so we cannot safely skip.
        // Returning false is safer than desynchronizing parsing.
        return false;
    }
  }

  s.valid = true;
  out = s;
  return true;
}

bool WBusSimple::readPacket(WBusPacket& out, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    poll();
    if (popPacket(out)) return true;
    delay(1);
  }
  return false;
}

bool WBusSimple::startParkingHeater(uint8_t minutes) {
  return sendCommand(0x21, &minutes, 1);
}

bool WBusSimple::stop() {
  return sendCommand(0x10, nullptr, 0);
}

bool WBusSimple::readOperatingState(uint8_t& opState) {
  // Command 0x50, index 0x07 returns operating state (first byte in response payload after index)
  uint8_t index = 0x07;
  if (!sendCommand(0x50, &index, 1)) return false;

  // Read responses; ignore echo and look for heater->controller header 0x4F
  WBusPacket pkt;
  uint32_t deadlineMs = millis() + 250;
  while (millis() < deadlineMs) {
    if (!readPacket(pkt, 250)) return false;
    if (pkt.header != wbusRxHeader()) continue;

    // payload: [cmd|0x80, index, ...data..., checksum]
    if (pkt.payloadLen < 4) continue;
    uint8_t cmdAck = pkt.payload[0];
    uint8_t idxAck = pkt.payload[1];
    if ((cmdAck & 0x7F) != 0x50) continue;
    if (idxAck != 0x07) continue;

    opState = pkt.payload[2];
    return true;
  }

  return false;
}

bool WBusSimple::requestStatusMulti(const uint8_t* ids, uint8_t idsLen) {
  if (idsLen == 0 || ids == nullptr) return false;
  // request = cmd 0x50 with data[0]=0x30 and then IDs to read
  uint8_t buf[1 + 64] = {0};
  if (idsLen > 64) return false;
  buf[0] = 0x30;
  memcpy(&buf[1], ids, idsLen);
  return sendCommand(0x50, buf, static_cast<uint8_t>(idsLen + 1));
}

bool WBusSimple::sendKeepAlive() {
  // Common keep-alive pattern seen in the wild is command 0x44 with 2 bytes.
  // This is intentionally optional; not all heaters require it.
  const uint8_t data[2] = {0x2A, 0x00};
  return sendCommand(0x44, data, 2);
}
