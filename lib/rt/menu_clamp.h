#pragma once
// Menu item count, selection clamping and the visible-item mask, host testable (menu_handler.h
// needs Arduino.h).
#include <stdint.h>

namespace rt {

// 0 or more than maxCount means "all items".
inline uint8_t menuClampCount(uint8_t n, uint8_t maxCount) { return (n == 0 || n > maxCount) ? maxCount : n; }

// Selection after the item count changed: never beyond the last visible item.
inline uint8_t menuClampSelected(uint8_t selected, uint8_t count) {
  return (count > 0 && selected >= count) ? (uint8_t)(count - 1) : selected;
}

// ---- Visible-item mask ---------------------------------------------------------------------
// Bit i of the mask = menu item i is shown. Lets the sender show "Ping-Pong" and the receiver hide
// it, and the BLE items appear only while the BLE stack runs, without a gap-free item order.

// A mask without any valid bit means "all items".
inline uint16_t menuMaskSanitize(uint16_t mask, uint8_t maxItems) {
  const uint16_t all = maxItems >= 16 ? (uint16_t)0xFFFFu : (uint16_t)((1u << maxItems) - 1u);
  const uint16_t m = (uint16_t)(mask & all);
  return m ? m : all;
}

inline uint8_t menuMaskCount(uint16_t mask, uint8_t maxItems) {
  mask = menuMaskSanitize(mask, maxItems);
  uint8_t n = 0;
  for (uint8_t i = 0; i < maxItems && i < 16; i++) n += (uint8_t)((mask >> i) & 1u);
  return n;
}

// Item index at position pos (0 based) among the visible ones, 0xFF if pos is out of range.
inline uint8_t menuMaskItemAt(uint16_t mask, uint8_t maxItems, uint8_t pos) {
  mask = menuMaskSanitize(mask, maxItems);
  uint8_t n = 0;
  for (uint8_t i = 0; i < maxItems && i < 16; i++) {
    if ((mask >> i) & 1u) {
      if (n == pos) return i;
      n++;
    }
  }
  return 0xFF;
}

// Position of an item among the visible ones, 0xFF if it is hidden.
inline uint8_t menuMaskPosOf(uint16_t mask, uint8_t maxItems, uint8_t item) {
  mask = menuMaskSanitize(mask, maxItems);
  if (item >= maxItems || item >= 16 || !((mask >> item) & 1u)) return 0xFF;
  uint8_t n = 0;
  for (uint8_t i = 0; i < item; i++) n += (uint8_t)((mask >> i) & 1u);
  return n;
}

inline uint8_t menuMaskFirst(uint16_t mask, uint8_t maxItems) { return menuMaskItemAt(mask, maxItems, 0); }

// The next visible item after `item`, wrapping around. A hidden item gives the first visible one.
inline uint8_t menuMaskNext(uint16_t mask, uint8_t maxItems, uint8_t item) {
  const uint8_t n = menuMaskCount(mask, maxItems);
  const uint8_t pos = menuMaskPosOf(mask, maxItems, item);
  if (pos == 0xFF) return menuMaskFirst(mask, maxItems);
  return menuMaskItemAt(mask, maxItems, (uint8_t)((pos + 1) % n));
}

// `item` if it is visible, otherwise the first visible item.
inline uint8_t menuMaskFix(uint16_t mask, uint8_t maxItems, uint8_t item) {
  return menuMaskPosOf(mask, maxItems, item) == 0xFF ? menuMaskFirst(mask, maxItems) : item;
}

}  // namespace rt
