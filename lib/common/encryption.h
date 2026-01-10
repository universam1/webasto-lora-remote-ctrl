#pragma once

#include <Arduino.h>
#include <cstring>

namespace crypto {

// AES-128-CTR encryption/decryption (symmetric - same operation for both)
// Nonce is implicitly derived from packet seq + src + dst (no explicit IV transmitted)
class AES128CTR {
 public:
  // PSK must be exactly 16 bytes
  static constexpr size_t KEY_SIZE = 16;
  static constexpr size_t NONCE_SIZE = 16;
  static constexpr size_t PAYLOAD_SIZE = 32;  // size of proto::Packet union

  // Set the pre-shared key (call once at setup)
  // key must be 16 bytes
  static void setKey(const uint8_t key[KEY_SIZE]);

  // Encrypt/decrypt payload (CTR is symmetric, same operation for both)
  // nonce is derived from: seq (4 bytes) + src (1 byte) + dst (1 byte) + zeros (10 bytes)
  static void encryptPayload(const uint8_t* plaintext, uint8_t* ciphertext,
                              uint32_t seq, uint8_t src, uint8_t dst);

  static void decryptPayload(const uint8_t* ciphertext, uint8_t* plaintext,
                              uint32_t seq, uint8_t src, uint8_t dst);

 private:
  static uint8_t psk[KEY_SIZE];

  // Build 16-byte nonce from seq + src + dst
  static void buildNonce(uint8_t nonce[NONCE_SIZE], uint32_t seq, uint8_t src, uint8_t dst);
};

}  // namespace crypto
