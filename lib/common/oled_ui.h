#pragma once

#include <Arduino.h>
#include <U8g2lib.h>

class OledUi {
 public:
  OledUi();

  void begin();

  void setPowerSave(bool enable);

  void setLine(uint8_t idx, const String& text);
  void render();
  void setInverted(bool inverted);
  void drawProgressBar(uint8_t y, uint8_t width, float progress);  // progress 0.0-1.0

 private:
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
  String lines[6];
  bool isInverted = false;
  uint8_t progressBarY = 0;
  uint8_t progressBarWidth = 0;
  float progressBarValue = 0.0f;
};
