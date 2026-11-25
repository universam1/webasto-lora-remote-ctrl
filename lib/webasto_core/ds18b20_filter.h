#pragma once
// Pure DS18B20 sample filtering: plausibility, median of 3, EMA, validity timing.
// No Arduino includes; the driver in lib/heater feeds it samples and failures.
#include <stdint.h>

namespace wh {

class Ds18b20Filter {
 public:
  static constexpr float kMinC = -40.0f, kMaxC = 60.0f;
  static constexpr float kAlpha = 0.3f;
  static constexpr uint8_t kGoodNeeded = 2;
  static constexpr uint8_t kMaxFailures = 3;
  static constexpr uint32_t kMaxAgeMs = 60000;

  // Plausibility only: rejects -127 (disconnected), 85 (power-on default), NaN, out of range.
  static bool plausible(float c) {
    if (c != c) return false;
    if (c == -127.0f || c == 85.0f) return false;
    return c >= kMinC && c <= kMaxC;
  }

  // Feed one raw reading in degC. Returns true if it was accepted.
  bool addSample(float c, uint32_t now) {
    if (!plausible(c)) {
      addFailure(now);
      return false;
    }
    failures_ = 0;
    hist_[head_] = c;
    head_ = (uint8_t)((head_ + 1) % 3);
    if (count_ < 3) count_++;
    const float m = median();
    ema_ = haveEma_ ? ema_ + kAlpha * (m - ema_) : m;
    haveEma_ = true;
    if (good_ < 255) good_++;
    lastGoodAt_ = now;
    haveGood_ = true;
    return true;
  }

  // A failed read (CRC error, no device, conversion not ready).
  void addFailure(uint32_t /*now*/) {
    if (failures_ < 255) failures_++;
    if (failures_ >= kMaxFailures) reset();
  }

  bool valid(uint32_t now) const {
    if (good_ < kGoodNeeded || !haveGood_) return false;
    return (uint32_t)(now - lastGoodAt_) <= kMaxAgeMs;
  }

  // 0.1 degC, rounded. Only meaningful while valid().
  int16_t temp_dC() const {
    if (!haveEma_) return INT16_MIN;
    const float t = ema_ * 10.0f;
    return (int16_t)(t >= 0 ? t + 0.5f : t - 0.5f);
  }

  void reset() {
    count_ = 0;
    head_ = 0;
    good_ = 0;
    haveEma_ = false;
    haveGood_ = false;
    failures_ = 0;
  }

 private:
  float median() const {
    if (count_ == 1) return hist_[(head_ + 2) % 3];
    if (count_ == 2) {
      const float a = hist_[(head_ + 1) % 3], b = hist_[(head_ + 2) % 3];
      return (a + b) * 0.5f;
    }
    const float a = hist_[0], b = hist_[1], c = hist_[2];
    if ((a <= b && b <= c) || (c <= b && b <= a)) return b;
    if ((b <= a && a <= c) || (c <= a && a <= b)) return a;
    return c;
  }

  float hist_[3] = {0, 0, 0};
  uint8_t head_ = 0, count_ = 0, good_ = 0, failures_ = 0;
  float ema_ = 0;
  bool haveEma_ = false, haveGood_ = false;
  uint32_t lastGoodAt_ = 0;
};

}  // namespace wh
