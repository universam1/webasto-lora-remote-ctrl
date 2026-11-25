#include "ds18b20_sensor.h"

Ds18b20::Ds18b20() : ow_(DS18B20_PIN), dt_(&ow_) {}

void Ds18b20::begin() {
  dt_.begin();
  dt_.setWaitForConversion(false);
  haveAddr_ = dt_.getAddress(addr_, 0);
  if (haveAddr_) dt_.setResolution(addr_, 12);
  began_ = true;
}

void Ds18b20::step(uint32_t now) {
  if (!began_) begin();

  if (state_ == State::Idle) {
    if (!everStarted_ || (uint32_t)(now - lastStart_) >= kPeriodMs) {
      if (!haveAddr_) {
        dt_.begin();  // retry device discovery (sensor hot-plugged or wiring fixed)
        haveAddr_ = dt_.getAddress(addr_, 0);
        if (haveAddr_) dt_.setResolution(addr_, 12);
      }
      lastStart_ = now;
      everStarted_ = true;
      if (haveAddr_) {
        dt_.requestTemperaturesByAddress(addr_);  // returns immediately (no wait)
        state_ = State::Converting;
      } else {
        filter_.addFailure(now);
      }
    }
  } else if ((uint32_t)(now - lastStart_) >= kConversionMs) {
    const float c = dt_.getTempC(addr_);  // DEVICE_DISCONNECTED_C (-127) on failure
    filter_.addSample(c, now);            // rejects -127, 85 and out of range as failures
    state_ = State::Idle;
  }

  valid_ = filter_.valid(now);
  temp_dC_ = valid_ ? filter_.temp_dC() : INT16_MIN;
}
