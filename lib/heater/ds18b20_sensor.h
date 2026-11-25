#pragma once
// Non blocking DS18B20 driver. Call step(millis()) from one task every ~100 ms (ctrlTask).
//
// OneWire bit banging disables interrupts for short moments (bit slots of about 70 us, a
// reset/presence sequence up to ~1 ms in total with interrupts masked in places). That can
// delay the 2 ms canTask by a few hundred microseconds. Measure on the target (plan, section
// Verifikation). Fallback plan: an RMT based OneWire implementation, selected with
// DS18B20_USE_RMT=1. NOT IMPLEMENTED: defining it is a compile error.
#include <stdint.h>

#include "project_config.h"

#ifndef DS18B20_USE_RMT
#define DS18B20_USE_RMT 0
#endif
#if DS18B20_USE_RMT
#error "DS18B20_USE_RMT is a placeholder for the RMT OneWire fallback and not implemented"
#endif

#include <DallasTemperature.h>
#include <OneWire.h>

#include "ds18b20_filter.h"

class Ds18b20 {
 public:
  Ds18b20();
  void begin();             // once, from the owning task
  void step(uint32_t now);  // every ~100 ms
  bool valid() const { return valid_; }
  int16_t temp_dC() const { return temp_dC_; }  // 0.1 degC, INT16_MIN while invalid

  static constexpr uint32_t kPeriodMs = 5000;
  static constexpr uint32_t kConversionMs = 800;

 private:
  enum class State : uint8_t { Idle, Converting };
  OneWire ow_;
  DallasTemperature dt_;
  wh::Ds18b20Filter filter_;
  State state_ = State::Idle;
  bool began_ = false;
  bool haveAddr_ = false;
  DeviceAddress addr_;
  uint32_t lastStart_ = 0;
  bool everStarted_ = false;
  bool valid_ = false;
  int16_t temp_dC_ = INT16_MIN;
};
