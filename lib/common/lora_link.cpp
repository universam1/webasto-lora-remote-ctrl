#include "lora_link.h"

#include <SPI.h>

bool LoRaLink::begin() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

  Serial.printf("[LORA] Init: freq=%lu sync=0x%02X bw=%lu sf=%d cr=%d\n",
                (unsigned long)LORA_FREQUENCY_HZ, LORA_SYNC_WORD,
                (unsigned long)LORA_BW, LORA_SF, LORA_CR);
  Serial.printf("[LORA] Pins: SCK=%d MISO=%d MOSI=%d CS=%d RST=%d DIO0=%d\n",
                LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY_HZ)) {
    Serial.println("[LORA] LoRa.begin() FAILED!");
    configured = false;
    return false;
  }

  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.enableCrc();

  // Put radio into continuous receive mode
  LoRa.receive();

  Serial.println("[LORA] Init OK");
  configured = true;
  return true;
}

bool LoRaLink::send(const proto::Packet& pkt) {
  if (!configured) {
    Serial.println("[LORA] send: not configured!");
    return false;
  }

  // Create a copy to encrypt
  proto::Packet encrypted_pkt = pkt;
  
  // Encrypt payload union in-place
  proto::encryptPacket(encrypted_pkt);
  
  // Recalculate CRC on encrypted payload
  encrypted_pkt.crc = proto::calcCrc(encrypted_pkt);

  if (!LoRa.beginPacket()) {
    Serial.println("[LORA] send: beginPacket failed!");
    return false;
  }
  
  size_t written = LoRa.write(reinterpret_cast<const uint8_t*>(&encrypted_pkt), sizeof(encrypted_pkt));
  if (written != sizeof(encrypted_pkt)) {
    Serial.printf("[LORA] send: write failed, wrote %d of %d bytes\n", (int)written, (int)sizeof(encrypted_pkt));
    return false;
  }
  
  int result = LoRa.endPacket();
  if (result != 1) {
    Serial.printf("[LORA] send: endPacket failed with %d\n", result);
    return false;
  }
  
  // IMPORTANT: Put radio back into receive mode after transmitting!
  // Without this, the radio stays in standby and won't receive responses.
  LoRa.receive();
  
  Serial.printf("[LORA] send: transmitted %d bytes OK (seq=%d, encrypted)\n", (int)sizeof(encrypted_pkt), pkt.h.seq);
  return true;
}

bool LoRaLink::recv(proto::Packet& pkt, int& rssi, float& snr) {
  if (!configured) return false;

  int packetSize = LoRa.parsePacket();
  
  static uint32_t lastRecvDebug = 0;
  static int recvCalls = 0;
  recvCalls++;
  if (millis() - lastRecvDebug > 5000) {
    Serial.printf("[LORA] recv: %d calls in last 5s, last parsePacket=%d\n", recvCalls, packetSize);
    recvCalls = 0;
    lastRecvDebug = millis();
  }
  
  if (packetSize <= 0) return false;
  
  Serial.printf("[LORA] Received packet size=%d expected=%d\n", packetSize, (int)sizeof(pkt));;
  
  if (packetSize != static_cast<int>(sizeof(pkt))) {
    // Drain
    while (LoRa.available()) LoRa.read();
    return false;
  }

  uint8_t* out = reinterpret_cast<uint8_t*>(&pkt);
  for (size_t i = 0; i < sizeof(pkt); i++) {
    int b = LoRa.read();
    if (b < 0) return false;
    out[i] = static_cast<uint8_t>(b);
  }

  rssi = LoRa.packetRssi();
  snr = LoRa.packetSnr();

  // First validate CRC on encrypted packet
  if (!proto::validate(pkt)) {
    Serial.println("[LORA] recv: CRC validation failed on encrypted packet");
    return false;
  }

  // Decrypt payload in-place
  proto::decryptPacket(pkt);

  Serial.printf("[LORA] recv: decrypted packet OK (seq=%d, type=%d)\n", pkt.h.seq, static_cast<uint8_t>(pkt.h.type));
  return true;
}
