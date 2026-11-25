#pragma once
// Full-screen, large-font OLED pages shown while the heater runs (Heating, RunOn, Ventilating).
// One page per second. Not thread safe and does not own the display: the UI task, which is the only
// owner of the U8g2 instance, calls update() about every 100 ms.
#include <Arduino.h>
#include <U8g2lib.h>

#include "heater_types.h"
#include "oled_page_logic.h"

class OledPages {
 public:
  static constexpr uint32_t kPageMs = 1000;

  void begin(U8G2& display) { u8g2_ = &display; }

  // True while the pages own the screen (heater on). The normal view must not be drawn then.
  bool active(const wh::SystemSnapshot& s) const { return wh::oledHeaterOn(s.heater.phase); }

  // Returns true if it drew (or kept) the full-screen page, false if the heater is off (nothing drawn).
  bool update(uint32_t now, const wh::SystemSnapshot& s);

  unsigned long renderCount() const { return renderCount_; }
  wh::OledPage lastPage() const { return cursor_.page; }

 private:
  void draw(const wh::OledPageInput& in);

  U8G2* u8g2_ = nullptr;
  wh::OledPageCursor cursor_;
  uint32_t lastAdvanceMs_ = 0;
  bool running_ = false;
  bool dirty_ = true;
  unsigned long renderCount_ = 0;
};
