#pragma once

#include <Arduino.h>

#include "project_config.h"

// W-BUS framing notes (per common public references and H4jen/webasto):
// - TX header is typically 0xF4 (controller/diagnostics -> heater)
// - RX header is typically 0x4F (heater -> controller/diagnostics)
// - Length byte counts (payload bytes + checksum byte)
// - Checksum is XOR of all bytes from header through last payload byte (excluding checksum)

struct WBusStatus {
  bool valid = false;

  // Common decoded measurements
  int16_t temperatureC = INT16_MIN; // usually encoded as (raw - 50)
  uint16_t voltage_mV = 0;
  uint16_t power = 0;
  uint16_t glowResistance_mOhm = 0;
  uint16_t combustionFan = 0;

  // Raw status fields (IDs vary per heater/firmware)
  uint8_t status_01 = 0;
  uint8_t status_03 = 0;
  uint8_t status_05 = 0;
  uint8_t status_06 = 0;
  uint8_t status_07 = 0;
  uint8_t status_08 = 0;
  uint8_t status_0A = 0;
  uint16_t status_0F = 0;
  uint8_t status_10 = 0;
  uint8_t status_1F = 0;
  uint8_t status_24 = 0;
  uint8_t status_27 = 0;
  uint16_t status_29 = 0;
  uint8_t status_2A = 0;
  uint8_t status_2C = 0;
  uint8_t status_2D = 0;
  uint8_t status_32 = 0;
  uint16_t status_34 = 0;

  // Remaining IDs seen in common multi-status request lists (lengths may vary by heater/firmware)
  uint16_t status_3D = 0;
  uint16_t status_52 = 0;
  uint16_t status_57 = 0;
  uint16_t status_5F = 0;
  uint16_t status_78 = 0;
  uint16_t status_89 = 0;
};

struct WBusPacket {
  uint8_t header = 0;
  uint8_t length = 0; // total bytes - 2 (includes checksum)
  uint8_t payload[256] = {0};
  uint8_t payloadLen = 0; // equals length

  uint8_t checksum() const {
    // Checksum byte is XOR of all bytes from header through last payload byte
    // (excluding the checksum itself).
    uint8_t x = 0;
    x ^= header;
    x ^= length;
    if (payloadLen == 0) return x;
    const uint8_t n = static_cast<uint8_t>(payloadLen - 1); // exclude checksum byte
    for (uint8_t i = 0; i < n; i++) x ^= payload[i];
    return x;
  }
};

class WBusSimple {
 public:
  bool begin();

  // Call frequently from loop() to parse incoming serial bytes.
  // Use popPacket() to retrieve completed frames.
  void poll();
  bool popPacket(WBusPacket& out);

  // Convenience helpers for typical "multi status" responses (0xD0 0x30 ...)
  static bool tryParseStatusTlv(const WBusPacket& pkt, WBusStatus& out);

  // High-level
  bool startParkingHeater(uint8_t minutes);
  bool stop();
  bool readOperatingState(uint8_t& opState);

  // Optional: request a multi-status snapshot (IDs depend on heater).
  // This mirrors the "0x50 0x30 <id...>" request style used by many units.
  bool requestStatusMulti(const uint8_t* ids, uint8_t idsLen);
  bool sendKeepAlive();

  // Low-level
  bool sendCommand(uint8_t cmd, const uint8_t* data, uint8_t dataLen);
  bool readPacket(WBusPacket& out, uint32_t timeoutMs);

 private:
  HardwareSerial& port = Serial2;
  bool didBreak = false;

  enum class RxState : uint8_t {
    FindHeader,
    ReadLength,
    ReadPayload,
  };

  RxState rxState = RxState::FindHeader;
  WBusPacket rxInProgress;
  uint16_t rxIndex = 0;

  bool havePacket = false;
  WBusPacket packetQueue;

  void maybeEnableTx(bool enable);
  void sendBreakPulse();
};
