#include "lora_link.h"

#include <SPI.h>

// Static members for ISR
volatile bool LoRaLink::packetAvailable = false;
uint8_t LoRaLink::isrPacketBuffer[256];
volatile int LoRaLink::isrPacketSize = 0;
volatile int LoRaLink::isrPacketRssi = 0;
volatile float LoRaLink::isrPacketSnr = 0.0f;
volatile uint32_t LoRaLink::isrCallCount = 0;

void LoRaLink::onReceiveISR(int pktSize) {
  // CRITICAL: This runs in ISR context - must read packet data NOW!
  // The LoRa library makes packet data available only during this callback.
  // After this callback returns, the data may be lost.
  
  isrCallCount++;  // Debug counter - increment every time ISR is called
  
  if (pktSize <= 0 || pktSize > 256) return;  // Sanity check
  
  // Read all packet bytes immediately into buffer
  for (int i = 0; i < pktSize; i++) {
    if (LoRa.available()) {
      isrPacketBuffer[i] = LoRa.read();
    } else {
      // Packet data exhausted early - abort
      return;
    }
  }
  
  // Capture RSSI and SNR while packet context is valid
  isrPacketRssi = LoRa.packetRssi();
  isrPacketSnr = LoRa.packetSnr();
  
  // Store size and set flag last
  isrPacketSize = pktSize;
  packetAvailable = true;
  
  // Note: Serial.println() in ISR is generally unsafe on ESP32
}

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

void LoRaLink::enableInterrupt() {
  if (!configured) {
    Serial.println("[LORA] Cannot enable interrupt - not configured!");
    return;
  }
  
  Serial.printf("[LORA] Registering ISR on DIO0 (pin %d)...\n", LORA_DIO0);
  Serial.printf("[LORA] DIO0 is digitalPinToInterrupt(%d) = %d\n", LORA_DIO0, digitalPinToInterrupt(LORA_DIO0));
  
  // Read initial DIO0 pin state (should be LOW in idle)
  pinMode(LORA_DIO0, INPUT);
  int dio0State = digitalRead(LORA_DIO0);
  Serial.printf("[LORA] DIO0 pin initial state: %s\n", dio0State ? "HIGH" : "LOW");
  
  // Ensure radio is idle before setting up interrupt
  LoRa.idle();
  delay(10);  // Let radio settle in idle mode
  
  // Set up interrupt on DIO0 pin for RX done
  LoRa.onReceive(onReceiveISR);
  packetAvailable = false;
  interruptMode = true;  // Track that we're in interrupt mode
  
  delay(50);  // Give time for interrupt to register (timing jitter)
  
  Serial.printf("[LORA] ISR registered, calling LoRa.receive() to start RX mode...\n");
  
  // CRITICAL: Must call receive() to start RX mode after setting up interrupt
  LoRa.receive();
  
  delay(50);  // Let RX mode stabilize (timing jitter)
  
  Serial.println("[LORA] Interrupt-based reception enabled on DIO0");
  Serial.printf("[LORA] Verify: packetAvailable=%d, interruptMode=%d, isrCallCount=%lu\n",
                packetAvailable, interruptMode, (unsigned long)isrCallCount);
  
  // Final DIO0 state check
  dio0State = digitalRead(LORA_DIO0);
  Serial.printf("[LORA] DIO0 pin after setup: %s\n", dio0State ? "HIGH" : "LOW");
}

void LoRaLink::enablePolling() {
  if (!configured) {
    Serial.println("[LORA] Cannot enable polling - not configured!");
    return;
  }
  
  Serial.println("[LORA] Enabling polling-based reception (DIO0 interrupt bypassed)");
  pollingMode = true;
  
  // Put radio in continuous RX mode
  LoRa.receive();
  
  Serial.println("[LORA] Polling-based reception enabled");
}

void LoRaLink::poll() {
  if (!pollingMode || !configured) return;
  
  // Check if packet is available using parsePacket (non-blocking)
  int pktSize = LoRa.parsePacket();
  if (pktSize > 0 && pktSize <= 256) {
    // Read packet data into buffer (same as ISR does)
    for (int i = 0; i < pktSize; i++) {
      if (LoRa.available()) {
        isrPacketBuffer[i] = LoRa.read();
      } else {
        return;  // Data exhausted early
      }
    }
    
    // Capture RSSI and SNR
    isrPacketRssi = LoRa.packetRssi();
    isrPacketSnr = LoRa.packetSnr();
    
    // Store size and set flag
    isrPacketSize = pktSize;
    packetAvailable = true;
  }
}


bool LoRaLink::send(const proto::Packet& pkt) {
  if (!configured) {
    Serial.println("[LORA] send: not configured!");
    return false;
  }

  Serial.printf("[LORA] send: BEFORE TX, isrCallCount=%lu\n", (unsigned long)isrCallCount);

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

  Serial.printf("[LORA] send: AFTER TX (before receive()), isrCallCount=%lu\n", (unsigned long)isrCallCount);

  // IMPORTANT: Put radio back into receive mode after transmitting!
  // In interrupt mode, must call LoRa.receive() WITHOUT arguments to keep interrupt active
  // In polling mode, can just call LoRa.receive()
  if (interruptMode) {
    // Re-register callback after every TX (workaround for potential callback loss)
    LoRa.onReceive(onReceiveISR);
    LoRa.receive();  // Restart RX with interrupt still active
  } else {
    LoRa.receive();  // Restart RX in polling mode
  }
  
  Serial.printf("[LORA] send: AFTER LoRa.receive(), isrCallCount=%lu\n", (unsigned long)isrCallCount);
  
  Serial.printf("[LORA] send: transmitted %d bytes OK (seq=%d, encrypted, payloadSize=%d)\n", 
                (int)wireSize, encrypted_pkt.h.seq, (int)payloadSize);
  return true;
}

bool LoRaLink::recv(proto::Packet& pkt, int& rssi, float& snr) {
  if (!configured) return false;

  int pktSize = 0;
  uint8_t rawBuffer[256];
  
  if (packetAvailable) {
    // ISR already read the packet into buffer - copy it out
    packetAvailable = false;  // Clear flag first
    pktSize = isrPacketSize;
    rssi = isrPacketRssi;
    snr = isrPacketSnr;
    
    if (pktSize <= 0 || pktSize > 256) {
      Serial.printf("[LORA] recv: invalid ISR packet size %d\n", pktSize);
      return false;
    }
    
    // Copy packet data from ISR buffer
    memcpy(rawBuffer, (const uint8_t*)isrPacketBuffer, pktSize);
    Serial.printf("[LORA] recv: ISR packet size=%d rssi=%d snr=%.1f\n", pktSize, rssi, snr);
  } else {
    // No interrupt flag - poll for packet (backwards compatibility)
    pktSize = LoRa.parsePacket();
    if (pktSize <= 0) return false;
    
    // Read packet data directly from LoRa
    for (int i = 0; i < pktSize && i < 256; i++) {
      if (LoRa.available()) {
        rawBuffer[i] = LoRa.read();
      } else {
        Serial.println("[LORA] recv: packet data exhausted");
        return false;
      }
    }
    
    rssi = LoRa.packetRssi();
    snr = LoRa.packetSnr();
  }
  
  // Now parse the raw buffer into our packet structure
  
  // Now parse the raw buffer into our packet structure
  // Minimum wire packet: header (10) + crc (2) = 12 bytes
  // Maximum wire packet: header (10) + status payload (14) + crc (2) = 26 bytes
  const int MIN_PACKET_SIZE = sizeof(proto::PacketHeader) + sizeof(uint16_t);
  const int MAX_PACKET_SIZE = sizeof(proto::PacketHeader) + sizeof(proto::StatusPayload) + sizeof(uint16_t);
  
  if (pktSize < MIN_PACKET_SIZE || pktSize > MAX_PACKET_SIZE) {
    Serial.printf("[LORA] Received packet size=%d (expected %d-%d), discarding\n", 
                  pktSize, MIN_PACKET_SIZE, MAX_PACKET_SIZE);
    return false;
  }

  // Parse header from buffer
  int bufferPos = 0;
  memcpy(&pkt.h, &rawBuffer[bufferPos], sizeof(pkt.h));
  bufferPos += sizeof(pkt.h);
  
  // Determine expected payload size based on received packet size
  int expectedPayloadBytes = pktSize - sizeof(pkt.h) - sizeof(pkt.crc);
  
  if (expectedPayloadBytes < 0 || expectedPayloadBytes > static_cast<int>(sizeof(pkt.p))) {
    Serial.printf("[LORA] recv: invalid payload size %d\n", expectedPayloadBytes);
    return false;
  }
  
  // Parse payload from buffer
  memcpy(&pkt.p, &rawBuffer[bufferPos], expectedPayloadBytes);
  bufferPos += expectedPayloadBytes;
  
  // Clear the rest of the union to avoid stale data
  if (expectedPayloadBytes < static_cast<int>(sizeof(pkt.p))) {
    memset(reinterpret_cast<uint8_t*>(&pkt.p) + expectedPayloadBytes, 0, sizeof(pkt.p) - expectedPayloadBytes);
  }
  
  // Parse CRC from buffer
  memcpy(&pkt.crc, &rawBuffer[bufferPos], sizeof(pkt.crc));
  bufferPos += sizeof(pkt.crc);

  // First validate CRC on encrypted packet
  if (!proto::validate(pkt)) {
    Serial.println("[LORA] recv: CRC validation failed on encrypted packet");
    return false;
  }

  // Decrypt payload in-place
  proto::decryptPacket(pkt);

  // Restart receive mode for next packet (important for interrupt mode)
  LoRa.receive();

  Serial.printf("[LORA] recv: decrypted packet OK (seq=%d, type=%d, wireSize=%d, payloadSize=%d)\n", 
                pkt.h.seq, static_cast<uint8_t>(pkt.h.type), pktSize, expectedPayloadBytes);
  return true;
}
