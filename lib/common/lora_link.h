#pragma once

#include <Arduino.h>

#include <LoRa.h>

#include "project_config.h"
#include "protocol.h"

class LoRaLink {
 public:
  bool begin();

  bool send(const proto::Packet& pkt);
  bool recv(proto::Packet& pkt, int& rssi, float& snr);

 private:
  bool configured = false;
};
