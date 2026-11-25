#pragma once

#if __has_include(<Arduino.h>)
#include <Arduino.h>
#include "encryption.h"
#define PROTO_HAS_CRYPTO 1
#else
// Host build (native unit tests): packet layout and CRC only, no AES.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define PROTO_HAS_CRYPTO 0
#endif

namespace proto {

// Protocol v5 magic byte (fused magic + version)
// 0x35 = ASCII '5' = Protocol v5
static constexpr uint8_t kMagicVersion = 0x35;  // 'WLR5'

// Default pre-shared key for AES-128-CTR encryption
// CHANGE THIS FOR PRODUCTION: Use a unique key per device pair
// Key must be exactly 16 bytes
static constexpr uint8_t kDefaultPSK[16] = {
  0x57, 0x65, 0x62, 0x61, 0x73, 0x74, 0x6F, 0x4C,  // "WbastoL"
  0x6F, 0x52, 0x61, 0x32, 0x30, 0x32, 0x36, 0x00   // "oRa2026"
};

enum class MsgType : uint8_t {
  Command = 1,
  Status = 2,
  Ack = 3,
};

enum class CommandKind : uint8_t {
  Stop = 1,
  Start = 2,
  RunMinutes = 3,
  QueryStatus = 4,  // Request a status poll without starting the heater
  ClimateSet = 5,   // Enable climate control: targetC (10..24), minutes = session duration (10..480)
  ClimateOff = 6,   // Disable climate control (does not stop the heater)
};

enum class HeaterState : uint8_t {
  Unknown = 0,
  Off = 1,
  Running = 2,
  Error = 3,
};

#pragma pack(push, 1)
struct PacketHeader {
  uint8_t magic_version;  // 0x34 for Protocol v4 (fused magic + version)
  MsgType type;
  uint8_t src;
  uint8_t dst;
  uint16_t seq;
  // Total: 6 bytes (was 10)
};

struct CommandPayload {
  CommandKind kind;
  uint8_t targetC;   // ClimateSet: 10..24, otherwise 0
  uint16_t minutes;  // ClimateSet: session duration 10..480; RunMinutes/Start: 1..480 (CAN ignores); else 0
};

// Status v5, 19 bytes, little endian. Field encoding and the golden vectors live in
// lib/webasto_core/status_codec.h and pwa/test/vectors.json.
struct StatusPayload {
  uint8_t flags;         // b0-2 phase, b3 bus awake, b4 climate active, b5 interior valid, b6 transport healthy, b7 ignition on
  uint8_t errClimate;    // b0-3 error code (0..6 heater, 8 not adopted, 9 unsupported, 10 no bus), b4 error stored, b5 state assumed
  uint8_t climateStatus; // wh::ClimateStatus
  int8_t lastRssiDbm;    // overwritten by the sender with its own receive value
  int8_t lastSnrDb;
  uint16_t lastCmdSeq;   // ACK correlation: a status whose lastCmdSeq equals the command seq is the ACK
  uint8_t powerRaw;      // W = raw * 20.7, l/h = raw * 2.41 / 1000
  uint16_t fuelUsedMl;
  uint8_t coolantRaw;    // 0.75 * x - 48 degC, 0xFF unknown
  int16_t interior_dC;   // 0.1 degC, INT16_MIN unknown
  uint8_t outsideRaw;    // x / 2 - 50 degC, 0xFF unknown
  uint8_t voltageRaw;    // x * 0.05 + 5 V, 0 unknown
  uint8_t odometer_km[3];  // u24 little endian, 0xFFFFFF unknown
  uint8_t climateTargetC;  // 10..24, 0 unknown
};

static_assert(sizeof(StatusPayload) == 19, "StatusPayload v5 must be 19 bytes");
static_assert(sizeof(CommandPayload) == 4, "CommandPayload v5 must be 4 bytes");

// Packet structure with variable-length payload support (Protocol v4)
// Actual wire format: header (10 bytes) + payload (N bytes) + crc (2 bytes)
// The union below is used for in-memory representation only.
// For serialization, use getPayloadSize() and serialize manually.
struct Packet {
  PacketHeader h;
  union {
    CommandPayload cmd;
    StatusPayload status;
    uint8_t raw[32];  // Max payload size
  } p;
  uint16_t crc;
};
#pragma pack(pop)

// Get actual payload size in bytes based on message type
inline size_t getPayloadSize(const Packet& pkt) {
  switch (pkt.h.type) {
    case MsgType::Command:
      return sizeof(CommandPayload);  // 4 bytes
    case MsgType::Status:
      return sizeof(StatusPayload);   // 19 bytes
    case MsgType::Ack:
      return 0;  // No payload
    default:
      return 0;
  }
}

// Get total wire packet size (header + payload + crc)
inline size_t getWirePacketSize(const Packet& pkt) {
  return sizeof(PacketHeader) + getPayloadSize(pkt) + sizeof(pkt.crc);
}

uint16_t crc16_ccitt(const uint8_t* data, size_t len);

// Calculate CRC for variable-length packet
// Covers header + actual payload (not the entire union)
inline uint16_t calcCrc(const Packet& pkt) {
  size_t payloadSize = getPayloadSize(pkt);
  size_t totalSize = sizeof(PacketHeader) + payloadSize;
  return crc16_ccitt(reinterpret_cast<const uint8_t*>(&pkt), totalSize);
}

inline bool validate(const Packet& pkt) {
  if (pkt.h.magic_version != kMagicVersion) return false;
  return pkt.crc == calcCrc(pkt);
}

#if PROTO_HAS_CRYPTO
// Encrypt payload union in-place using AES-128-CTR with implicit nonce
// Nonce is derived from packet seq + src + dst
// Only encrypts the actual payload, not the entire union
inline void encryptPacket(Packet& pkt) {
  size_t payloadSize = getPayloadSize(pkt);
  uint8_t plaintext[32];
  uint8_t ciphertext[32];
  memcpy(plaintext, pkt.p.raw, sizeof(plaintext));

  crypto::AES128CTR::encryptPayload(plaintext, ciphertext, pkt.h.seq, pkt.h.src, pkt.h.dst);

  memcpy(pkt.p.raw, ciphertext, sizeof(ciphertext));
}

// Decrypt payload union in-place using AES-128-CTR with implicit nonce
inline void decryptPacket(Packet& pkt) {
  size_t payloadSize = getPayloadSize(pkt);
  uint8_t ciphertext[32];
  uint8_t plaintext[32];
  memcpy(ciphertext, pkt.p.raw, sizeof(ciphertext));

  crypto::AES128CTR::decryptPayload(ciphertext, plaintext, pkt.h.seq, pkt.h.src, pkt.h.dst);

  memcpy(pkt.p.raw, plaintext, sizeof(plaintext));
}
#endif  // PROTO_HAS_CRYPTO

} // namespace proto
