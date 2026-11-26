#pragma once
// Shared command/event structs. Plain old data, no Arduino includes.
#include <stdint.h>

namespace rt {

enum class Source : uint8_t { Menu, Lora, Mqtt, Ble, Climate, Serial };
enum class CmdType : uint8_t { Heat = 1, Stop = 2, Query = 3, ClimateSet = 4, ClimateOff = 5 };

struct Command {
  Source src;
  CmdType type;
  uint8_t targetC;
  uint8_t tag;
  uint16_t minutes;
  uint16_t loraSeq;
};

struct CmdEvent {
  Source src;
  uint8_t tag;
  uint16_t loraSeq;
  uint8_t result;  // wh::CmdResult
  uint8_t stage;   // 0 queued, 1 sent, 2 acked, 3 done, 4 failed
  bool isStop;     // result of a Stop command (counted separately when it is dropped)
};

}  // namespace rt
