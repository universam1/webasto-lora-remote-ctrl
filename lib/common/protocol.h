#pragma once

#include <Arduino.h>
#include "encryption.h"

namespace proto {

static constexpr uint32_t kMagic = 0x574C5231; // 'WLR1'
static constexpr uint8_t kVersion = 3;

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

  // Correlates receiver status updates to the most recently processed command.
  // The sender can use this as an ACK for retry loops.
  uint16_t lastCmdSeq;

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

// Encrypt payload union in-place using AES-128-CTR with implicit nonce
// Nonce is derived from packet seq + src + dst
inline void encryptPacket(Packet& pkt) {
  uint8_t plaintext[32];
  uint8_t ciphertext[32];
  memcpy(plaintext, pkt.p.raw, sizeof(plaintext));
  
  crypto::AES128CTR::encryptPayload(plaintext, ciphertext, pkt.h.seq, pkt.h.src, pkt.h.dst);
  
  memcpy(pkt.p.raw, ciphertext, sizeof(ciphertext));
}

// Decrypt payload union in-place using AES-128-CTR with implicit nonce
inline void decryptPacket(Packet& pkt) {
  uint8_t ciphertext[32];
  uint8_t plaintext[32];
  memcpy(ciphertext, pkt.p.raw, sizeof(ciphertext));
  
  crypto::AES128CTR::decryptPayload(ciphertext, plaintext, pkt.h.seq, pkt.h.src, pkt.h.dst);
  
  memcpy(pkt.p.raw, plaintext, sizeof(plaintext));
}

} // namespace proto
