#include "oled_ui.h"

#include <Wire.h>

#include "project_config.h"

OledUi::OledUi() : u8g2(U8G2_R0, /* reset */ OLED_RST) {}

void OledUi::begin() {
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.setFontPosTop();
}

void OledUi::setPowerSave(bool enable) {
  u8g2.setPowerSave(enable ? 1 : 0);
}

void OledUi::setLine(uint8_t idx, const String& text) {
  if (idx >= 6) return;
  lines[idx] = text;
}

void OledUi::render() {
  u8g2.clearBuffer();
  for (uint8_t i = 0; i < 6; i++) {
    if (lines[i].length() == 0) continue;
    u8g2.drawStr(0, i * 11, lines[i].c_str());
  }
  u8g2.sendBuffer();
}
