#pragma once

#include <Arduino.h>

#include <LoRa.h>

#include "project_config.h"
#include "protocol.h"

class LoRaLink {
 public:
  bool begin();
  void enableInterrupt();  // Enable interrupt-based reception
  void enablePolling();    // Enable polling-based reception (fallback for broken DIO0)
  void poll();             // Manual poll for packets (call in loop if using polling mode)

  bool send(const proto::Packet& pkt);
  bool recv(proto::Packet& pkt, int& rssi, float& snr);
  
  bool hasPacket() const { return packetAvailable; }  // Check if packet ready without polling
  uint32_t getIsrCallCount() const { return isrCallCount; }  // Debug: get ISR call count

 private:
  bool configured = false;
  bool interruptMode = false;  // Track if interrupt mode is enabled
  bool pollingMode = false;    // Track if polling mode is enabled
  
  // ISR packet buffer - stores complete packet read in ISR
  static volatile bool packetAvailable;       // Set by ISR when packet arrives
  static uint8_t isrPacketBuffer[256];        // Buffer to store raw packet bytes
  static volatile int isrPacketSize;          // Size of packet in buffer
  static volatile int isrPacketRssi;          // RSSI captured in ISR
  static volatile float isrPacketSnr;         // SNR captured in ISR
  static volatile uint32_t isrCallCount;      // Debug counter for ISR calls
  
  static void onReceiveISR(int packetSize);   // ISR callback - reads packet immediately
};
