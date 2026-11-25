#include "oled_pages.h"

// Fonts (bytes in flash, from u8g2_fonts.c): logisoso32_tn 525, logisoso24_tn 421,
// logisoso16_tr 1564 (error texts), 7x13B_tr 1083 (unit), 6x12_tr 969 (title, sub), 5x8_tr (long sub).
// The _tn fonts hold ' ' and '*'..':' (digits, comma, minus, period).
namespace {
constexpr int kW = 128;
constexpr int kValueBase = 47;  // baseline of the big value
constexpr int kSubY = 49;       // top of the sub line
constexpr int kDotY = 62;       // centre of the page dots

int centerX(int w) { return (kW - w) / 2 > 0 ? (kW - w) / 2 : 0; }
}  // namespace

bool OledPages::update(uint32_t now, const wh::SystemSnapshot& s) {
  if (!u8g2_ || !active(s)) {
    running_ = false;
    return false;
  }
  const wh::OledPageInput in = wh::makeOledInput(s);
  if (!running_) {  // entering heater-on mode: start on the first page
    running_ = true;
    cursor_ = wh::OledPageCursor();
    lastAdvanceMs_ = now;
    dirty_ = true;
  } else if ((uint32_t)(now - lastAdvanceMs_) >= kPageMs) {
    lastAdvanceMs_ = now;
    cursor_.advance(in);
    dirty_ = true;
  }
  // The page is redrawn once per second; the I2C transfer is not repeated every 100 ms.
  if (dirty_) {
    draw(in);
    dirty_ = false;
  }
  return true;
}

void OledPages::draw(const wh::OledPageInput& in) {
  U8G2& d = *u8g2_;
  const wh::PageText t = wh::formatPage(cursor_.page, in);

  d.clearBuffer();
  d.setFontMode(1);  // transparent
  if (t.alert) {
    d.setDrawColor(1);
    d.drawBox(0, 0, kW, 64);
    d.setDrawColor(0);
  } else {
    d.setDrawColor(1);
  }

  d.setFontPosTop();
  d.setFont(u8g2_font_6x12_tr);
  d.drawStr(0, 0, t.title);

  d.setFontPosBaseline();
  if (!t.alert) {
    d.setFont(u8g2_font_7x13B_tr);
    const int unitW = t.unit[0] ? d.getStrWidth(t.unit) + 3 : 0;
    d.setFont(u8g2_font_logisoso32_tn);
    int w = d.getStrWidth(t.value);
    if (w + unitW > kW) {
      d.setFont(u8g2_font_logisoso24_tn);
      w = d.getStrWidth(t.value);
    }
    const int x = centerX(w + unitW);
    d.drawStr(x, kValueBase, t.value);
    if (t.unit[0]) {
      d.setFont(u8g2_font_7x13B_tr);
      d.drawStr(x + w + 3, kValueBase, t.unit);
    }
  } else {
    d.setFont(u8g2_font_logisoso16_tr);
    d.drawStr(centerX(d.getStrWidth(t.value)), kValueBase - 10, t.value);
  }

  d.setFontPosTop();
  if (t.sub[0]) {
    d.setFont(u8g2_font_6x12_tr);
    if (d.getStrWidth(t.sub) > kW) d.setFont(u8g2_font_5x8_tr);
    d.drawStr(centerX(d.getStrWidth(t.sub)), kSubY, t.sub);
  }

  // Page dots, one per available page, the current one filled.
  const uint8_t n = wh::pageCount(in);
  const uint8_t cur = wh::pageIndex(cursor_.page, in);
  if (n > 1) {
    const int pitch = 8;
    const int x0 = (kW - (n - 1) * pitch) / 2;
    for (uint8_t i = 0; i < n; i++) {
      const int x = x0 + i * pitch;
      if (i == cur) d.drawDisc(x, kDotY, 2);
      else d.drawCircle(x, kDotY, 1);
    }
  }

  d.setDrawColor(1);
  d.sendBuffer();
  renderCount_++;
}
