#include "lora_link.h"

#include <SPI.h>

bool LoRaLink::begin() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY_HZ)) {
    configured = false;
    return false;
  }

  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.enableCrc();

  configured = true;
  return true;
}

bool LoRaLink::send(const proto::Packet& pkt) {
  if (!configured) return false;

  LoRa.beginPacket();
  LoRa.write(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  return LoRa.endPacket() == 1;
}

bool LoRaLink::recv(proto::Packet& pkt, int& rssi, float& snr) {
  if (!configured) return false;

  int packetSize = LoRa.parsePacket();
  if (packetSize <= 0) return false;
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

  return proto::validate(pkt);
}
