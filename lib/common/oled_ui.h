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

 private:
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
  String lines[6];
  bool isInverted = false;
};
