#include "encryption.h"
#include "mbedtls/aes.h"

namespace crypto {

uint8_t AES128CTR::psk[AES128CTR::KEY_SIZE] = {0};

void AES128CTR::setKey(const uint8_t key[KEY_SIZE]) {
  memcpy(psk, key, KEY_SIZE);
}

void AES128CTR::buildNonce(uint8_t nonce[NONCE_SIZE], uint32_t seq, uint8_t src, uint8_t dst) {
  memset(nonce, 0, NONCE_SIZE);
  
  // Layout: seq (little-endian, 4 bytes) + src (1) + dst (1) + zeros (10)
  nonce[0] = (seq >> 0) & 0xFF;
  nonce[1] = (seq >> 8) & 0xFF;
  nonce[2] = (seq >> 16) & 0xFF;
  nonce[3] = (seq >> 24) & 0xFF;
  nonce[4] = src;
  nonce[5] = dst;
  // nonce[6..15] remain zero
}

void AES128CTR::encryptPayload(const uint8_t* plaintext, uint8_t* ciphertext,
                                uint32_t seq, uint8_t src, uint8_t dst) {
  uint8_t nonce[NONCE_SIZE];
  buildNonce(nonce, seq, src, dst);

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, psk, 128);

  // CTR mode: nonce_counter starts at nonce, stream_block is working buffer, nc_off tracks offset
  uint8_t stream_block[16];
  size_t nc_off = 0;

  mbedtls_aes_crypt_ctr(&ctx, PAYLOAD_SIZE, &nc_off, nonce, stream_block, plaintext, ciphertext);

  mbedtls_aes_free(&ctx);
}

void AES128CTR::decryptPayload(const uint8_t* ciphertext, uint8_t* plaintext,
                                uint32_t seq, uint8_t src, uint8_t dst) {
  // CTR decryption is identical to encryption
  encryptPayload(ciphertext, plaintext, seq, src, dst);
}

}  // namespace crypto
