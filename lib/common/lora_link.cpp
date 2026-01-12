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

  // TX power boost for improved range
  #if LORA_TX_POWER_BOOST
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
  #endif

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
  
  // Calculate wire packet size (header + actual payload + crc)
  size_t wireSize = proto::getWirePacketSize(encrypted_pkt);
  
  // Write header
  size_t written = LoRa.write(reinterpret_cast<const uint8_t*>(&encrypted_pkt.h), sizeof(encrypted_pkt.h));
  if (written != sizeof(encrypted_pkt.h)) {
    Serial.printf("[LORA] send: write header failed, wrote %d of %d bytes\n", (int)written, (int)sizeof(encrypted_pkt.h));
    return false;
  }
  
  // Write payload (only actual payload size, not entire union)
  size_t payloadSize = proto::getPayloadSize(encrypted_pkt);
  written = LoRa.write(reinterpret_cast<const uint8_t*>(&encrypted_pkt.p), payloadSize);
  if (written != payloadSize) {
    Serial.printf("[LORA] send: write payload failed, wrote %d of %d bytes\n", (int)written, (int)payloadSize);
    return false;
  }
  
  // Write CRC
  written = LoRa.write(reinterpret_cast<const uint8_t*>(&encrypted_pkt.crc), sizeof(encrypted_pkt.crc));
  if (written != sizeof(encrypted_pkt.crc)) {
    Serial.printf("[LORA] send: write crc failed, wrote %d of %d bytes\n", (int)written, (int)sizeof(encrypted_pkt.crc));
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
  
  Serial.printf("[LORA] send: transmitted %d bytes OK (seq=%d, encrypted, payloadSize=%d)\n", 
                (int)wireSize, encrypted_pkt.h.seq, (int)payloadSize);
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
  
  // Minimum wire packet: header (10) + crc (2) = 12 bytes
  // Maximum wire packet: header (10) + status payload (14) + crc (2) = 26 bytes
  const int MIN_PACKET_SIZE = sizeof(proto::PacketHeader) + sizeof(uint16_t);
  const int MAX_PACKET_SIZE = sizeof(proto::PacketHeader) + sizeof(proto::StatusPayload) + sizeof(uint16_t);
  
  if (packetSize < MIN_PACKET_SIZE || packetSize > MAX_PACKET_SIZE) {
    Serial.printf("[LORA] Received packet size=%d (expected %d-%d), discarding\n", 
                  packetSize, MIN_PACKET_SIZE, MAX_PACKET_SIZE);
    // Drain
    while (LoRa.available()) LoRa.read();
    return false;
  }

  // Read header first
  uint8_t* headerOut = reinterpret_cast<uint8_t*>(&pkt.h);
  for (size_t i = 0; i < sizeof(pkt.h); i++) {
    int b = LoRa.read();
    if (b < 0) {
      Serial.println("[LORA] recv: failed to read header");
      return false;
    }
    headerOut[i] = static_cast<uint8_t>(b);
  }
  
  // Determine expected payload size based on received packet size
  // Calculate how many payload bytes we expect: packetSize - header - crc
  int expectedPayloadBytes = packetSize - sizeof(pkt.h) - sizeof(pkt.crc);
  
  if (expectedPayloadBytes < 0 || expectedPayloadBytes > static_cast<int>(sizeof(pkt.p))) {
    Serial.printf("[LORA] recv: invalid payload size %d\n", expectedPayloadBytes);
    return false;
  }
  
  // Read payload
  uint8_t* payloadOut = reinterpret_cast<uint8_t*>(&pkt.p);
  for (int i = 0; i < expectedPayloadBytes; i++) {
    int b = LoRa.read();
    if (b < 0) {
      Serial.println("[LORA] recv: failed to read payload");
      return false;
    }
    payloadOut[i] = static_cast<uint8_t>(b);
  }
  
  // Clear the rest of the union to avoid stale data
  memset(payloadOut + expectedPayloadBytes, 0, sizeof(pkt.p) - expectedPayloadBytes);
  
  // Read CRC
  uint8_t* crcOut = reinterpret_cast<uint8_t*>(&pkt.crc);
  for (size_t i = 0; i < sizeof(pkt.crc); i++) {
    int b = LoRa.read();
    if (b < 0) {
      Serial.println("[LORA] recv: failed to read crc");
      return false;
    }
    crcOut[i] = static_cast<uint8_t>(b);
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

  Serial.printf("[LORA] recv: decrypted packet OK (seq=%d, type=%d, wireSize=%d, payloadSize=%d)\n", 
                pkt.h.seq, static_cast<uint8_t>(pkt.h.type), packetSize, expectedPayloadBytes);
  return true;
}
