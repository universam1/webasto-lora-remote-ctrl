#pragma once

#include <Arduino.h>

namespace proto {

static constexpr uint32_t kMagic = 0x574C5231; // 'WLR1'
static constexpr uint8_t kVersion = 2;

enum class MsgType : uint8_t {
  Command = 1,
  Status = 2,
  Ack = 3,
};

enum class CommandKind : uint8_t {
  Stop = 1,
  Start = 2,
  RunMinutes = 3,
};

enum class HeaterState : uint8_t {
  Unknown = 0,
  Off = 1,
  Running = 2,
  Error = 3,
};

#pragma pack(push, 1)
struct PacketHeader {
  uint32_t magic;
  uint8_t version;
  MsgType type;
  uint8_t src;
  uint8_t dst;
  uint16_t seq;
};

struct CommandPayload {
  CommandKind kind;
  uint8_t minutes; // used for RunMinutes and Start
};

struct StatusPayload {
  HeaterState state;
  uint8_t minutesRemaining; // best-effort (0 if unknown)
  int8_t lastRssiDbm;
  int8_t lastSnrDb;
  uint8_t lastWbusOpState; // raw 0x50 idx 0x07 byte0 (if known)
  uint8_t lastErrorCode;   // best-effort (0 if unknown)

  // Decoded measurements from multi-status responses (best-effort).
  // If unknown, temperatureC is INT16_MIN and others are 0.
  int16_t temperatureC;
  uint16_t voltage_mV;
  uint16_t power;
};

struct Packet {
  PacketHeader h;
  union {
    CommandPayload cmd;
    StatusPayload status;
    uint8_t raw[32];
  } p;
  uint16_t crc;
};
#pragma pack(pop)

static_assert(sizeof(Packet) <= 64, "Packet too large");

uint16_t crc16_ccitt(const uint8_t* data, size_t len);

inline uint16_t calcCrc(const Packet& pkt) {
  return crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), sizeof(Packet) - sizeof(pkt.crc));
}

inline bool validate(const Packet& pkt) {
  if (pkt.h.magic != kMagic) return false;
  if (pkt.h.version != kVersion) return false;
  return pkt.crc == calcCrc(pkt);
}

} // namespace proto
