#pragma once

#include <Arduino.h>
#include "encryption.h"

namespace proto {

// Protocol v4 magic byte (fused magic + version)
// 0x34 = ASCII '4' = Protocol v4
static constexpr uint8_t kMagicVersion = 0x34;  // 'WLR4'

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
  uint8_t minutes; // used for RunMinutes and Start
};

struct StatusPayload {
  HeaterState state;
  uint8_t minutesRemaining; // best-effort (0 if unknown)
  int8_t lastRssiDbm;
  int8_t lastSnrDb;
  uint8_t lastWbusOpState; // raw 0x50 idx 0x07 byte0 (if known)
  uint8_t lastErrorCode;   // best-effort (0 if unknown)

  // Correlates receiver status updates to the most recently processed command.
  // The sender can use this as an ACK for retry loops.
  uint16_t lastCmdSeq;

  // Decoded measurements from multi-status responses (best-effort).
  // If unknown, temperatureC is INT16_MIN and others are 0.
  int16_t temperatureC;
  uint16_t voltage_mV;
  uint16_t power;
};

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
      return sizeof(CommandPayload);  // 2 bytes
    case MsgType::Status:
      return sizeof(StatusPayload);   // 14 bytes
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

} // namespace proto
