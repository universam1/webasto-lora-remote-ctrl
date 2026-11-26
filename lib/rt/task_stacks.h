#pragma once
// Stack sizes in bytes of all tasks, in one place (xTaskCreatePinnedToCore unit on ESP32).
#include <stdint.h>

namespace rt {

constexpr uint32_t kStackLora = 6144;  // two proto::Packet locals + Serial.printf with floats
constexpr uint32_t kStackCtrl = 6144;
constexpr uint32_t kStackUi = 6144;
constexpr uint32_t kStackBle = 4096;
constexpr uint32_t kStackCan = 6144;
constexpr uint32_t kStackWbus = 6144;

}  // namespace rt
