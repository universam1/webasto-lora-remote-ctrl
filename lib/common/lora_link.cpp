#include "lora_link.h"

#include <SPI.h>
#include <stddef.h>

// Static members for ISR
volatile bool LoRaLink::packetAvailable = false;
uint8_t LoRaLink::isrPacketBuffer[256];
volatile int LoRaLink::isrPacketSize = 0;
volatile int LoRaLink::isrPacketRssi = 0;
volatile float LoRaLink::isrPacketSnr = 0.0f;
volatile uint32_t LoRaLink::isrCallCount = 0;
volatile bool LoRaLink::txDoneFlag = false;
bool LoRaLink::txInFlight = false;
uint32_t LoRaLink::txStartMs = 0;

// Async TX notes (LoRa 0.8.0, checked in LoRa.cpp): endPacket(true) maps DIO0 to TxDone (REG_DIO_MAPPING_1=0x40)
// and returns. onReceive() and onTxDone() attach the same DIO0 interrupt, and handleDio0Rise() dispatches on
// the IRQ flags (RxDone -> _onReceive, TxDone -> _onTxDone). LoRa.receive() maps DIO0 back to RxDone, so the
// radio must be re-armed with receive() after every TX. That is done in service(), not in the ISR (SPI access).
static const uint32_t kTxTimeoutMs = 4000;  // lost TxDone IRQ: SF11/BW125 airtime of the largest packet is ~1.1 s

#if defined(ARDUINO_ARCH_ESP32)
#define LORA_ISR_ATTR IRAM_ATTR
#else
#define LORA_ISR_ATTR
#endif

void LORA_ISR_ATTR LoRaLink::onTxDoneISR() {
  txDoneFlag = true;
}

// Payload size by message type for the receive length check (Command 4, Status 19, Ack 0, Ping 0, Pong 4).
static int payloadSizeForType(uint8_t type) {
  switch (static_cast<proto::MsgType>(type)) {
    case proto::MsgType::Command: return sizeof(proto::CommandPayload);
    case proto::MsgType::Status:  return sizeof(proto::StatusPayload);
    case proto::MsgType::Ack:     return 0;
    case proto::MsgType::Ping:    return 0;
    case proto::MsgType::Pong:    return sizeof(proto::PongPayload);
    default:                      return -1;
  }
}

void LoRaLink::service() {
  if (!txInFlight) return;
  if (txDoneFlag || (uint32_t)(millis() - txStartMs) > kTxTimeoutMs) {
    if (!txDoneFlag) Serial.println("[LORA] TX timeout, re-arming RX");
    txDoneFlag = false;
    txInFlight = false;
    LoRa.receive();  // DIO0 back to RxDone; onReceive callback stays registered
  }
}

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
  LoRa.onTxDone(onTxDoneISR);  // same DIO0 pin and attachInterrupt; dispatch by IRQ flag inside the library
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
  service();
  if (txInFlight) return false;  // previous transmission still on air

  // Create a copy to encrypt
  proto::Packet encrypted_pkt = pkt;
  proto::encryptPacket(encrypted_pkt);
  encrypted_pkt.crc = proto::calcCrc(encrypted_pkt);

  if (!LoRa.beginPacket()) {  // switches the radio to standby; fails if it is transmitting
    Serial.println("[LORA] send: beginPacket failed!");
    return false;
  }

  size_t wireSize = proto::getWirePacketSize(encrypted_pkt);
  size_t payloadSize = proto::getPayloadSize(encrypted_pkt);

  if (LoRa.write(reinterpret_cast<const uint8_t*>(&encrypted_pkt.h), sizeof(encrypted_pkt.h)) != sizeof(encrypted_pkt.h) ||
      LoRa.write(reinterpret_cast<const uint8_t*>(&encrypted_pkt.p), payloadSize) != payloadSize ||
      LoRa.write(reinterpret_cast<const uint8_t*>(&encrypted_pkt.crc), sizeof(encrypted_pkt.crc)) != sizeof(encrypted_pkt.crc)) {
    Serial.println("[LORA] send: FIFO write failed");
    LoRa.receive();
    return false;
  }

  if (interruptMode) {
    // Asynchronous: returns right after switching to TX. TxDone arrives on DIO0 -> onTxDoneISR.
    txDoneFlag = false;
    txInFlight = true;
    txStartMs = millis();
    LoRa.endPacket(true);
  } else {
    // Polling fallback has no DIO0 interrupt: keep the blocking variant.
    LoRa.endPacket(false);
    LoRa.receive();
  }

  Serial.printf("[LORA] send: %s %d bytes (seq=%d, type=%d)\n", interruptMode ? "async" : "sync",
                (int)wireSize, encrypted_pkt.h.seq, static_cast<int>(encrypted_pkt.h.type));
  return true;
}

bool LoRaLink::recv(proto::Packet& pkt, int& rssi, float& snr) {
  if (!configured) return false;
  service();

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
    // No interrupt flag - poll for packet (backwards compatibility). Never touch the radio during TX.
    if (txInFlight) return false;
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
  
  // Wire format (v5): header (6) + payload (by type) + crc (2). Accept only the exact length for the type:
  // command 12 B, status 27 B, ack 8 B, ping 8 B, pong 12 B. Everything else is dropped.
  const int kHeaderSize = sizeof(proto::PacketHeader);
  const int kCrcSize = sizeof(uint16_t);
  if (pktSize < kHeaderSize + kCrcSize) {
    Serial.printf("[LORA] drop: size=%d too short\n", pktSize);
    return false;
  }
  const int expectedPayloadBytes = payloadSizeForType(rawBuffer[offsetof(proto::PacketHeader, type)]);
  if (expectedPayloadBytes < 0) {
    Serial.printf("[LORA] drop: unknown type=%d size=%d\n", rawBuffer[offsetof(proto::PacketHeader, type)], pktSize);
    return false;
  }
  if (pktSize != kHeaderSize + expectedPayloadBytes + kCrcSize) {
    Serial.printf("[LORA] drop: size=%d, expected %d for type=%d\n", pktSize,
                  kHeaderSize + expectedPayloadBytes + kCrcSize, rawBuffer[offsetof(proto::PacketHeader, type)]);
    return false;
  }

  // Parse header and payload from buffer
  int bufferPos = 0;
  memcpy(&pkt.h, &rawBuffer[bufferPos], sizeof(pkt.h));
  bufferPos += sizeof(pkt.h);
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

  // Continuous RX stays active after a packet. Re-arm only when no TX is on air (it would abort the TX).
  if (!txInFlight) LoRa.receive();

  Serial.printf("[LORA] recv: decrypted packet OK (seq=%d, type=%d, wireSize=%d, payloadSize=%d)\n", 
                pkt.h.seq, static_cast<uint8_t>(pkt.h.type), pktSize, expectedPayloadBytes);
  return true;
}
