#pragma once
// Pure BLE command decoder / result encoder (no NimBLE, no Arduino). Host testable.
// Command (write): [op u8][cmdId u8][args...], Result (notify): [cmdId][stage][result][detail].
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../webasto_core/ble_profile.h"
#include "../rt/commands.h"

constexpr uint8_t kBleClimateMinC = 10;
constexpr uint8_t kBleClimateMaxC = 24;
constexpr uint16_t kBleClimateMinMin = 10;
constexpr uint16_t kBleClimateMaxMin = 480;

// Returns false for unknown op, cmdId 0, wrong length or out-of-range climate args.
// On false, `out` is left untouched.
inline bool decodeBleCommand(const uint8_t* data, size_t len, rt::Command& out) {
  if (data == nullptr || len < 2) return false;
  const uint8_t op = data[0];
  const uint8_t id = data[1];
  if (id == 0) return false;

  rt::Command c;
  memset(&c, 0, sizeof(c));
  c.src = rt::Source::Ble;
  c.tag = id;

  switch (op) {
    case ble::OP_HEAT:
      if (len != 2) return false;
      c.type = rt::CmdType::Heat;
      break;
    case ble::OP_STOP:
      if (len != 2) return false;
      c.type = rt::CmdType::Stop;
      break;
    case ble::OP_CLIMATE_SET: {
      if (len != 5) return false;
      const uint8_t t = data[2];
      const uint16_t m = (uint16_t)(data[3] | (data[4] << 8));
      if (t < kBleClimateMinC || t > kBleClimateMaxC) return false;
      if (m < kBleClimateMinMin || m > kBleClimateMaxMin) return false;
      c.type = rt::CmdType::ClimateSet;
      c.targetC = t;
      c.minutes = m;
      break;
    }
    case ble::OP_CLIMATE_OFF:
      if (len != 2) return false;
      c.type = rt::CmdType::ClimateOff;
      break;
    case ble::OP_QUERY:
      if (len != 2) return false;
      c.type = rt::CmdType::Query;
      break;
    default:
      return false;
  }
  out = c;
  return true;
}

// Always writes 4 bytes, returns 4. detail is reserved (0).
inline size_t encodeBleResult(const rt::CmdEvent& ev, uint8_t out[4]) {
  out[0] = ev.tag;
  out[1] = ev.stage;
  out[2] = ev.result;
  out[3] = 0;
  return 4;
}
