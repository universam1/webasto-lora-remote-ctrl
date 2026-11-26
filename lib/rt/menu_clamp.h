#pragma once
// Menu item count and selection clamping, host testable (menu_handler.h needs Arduino.h).
#include <stdint.h>

namespace rt {

// 0 or more than maxCount means "all items".
inline uint8_t menuClampCount(uint8_t n, uint8_t maxCount) { return (n == 0 || n > maxCount) ? maxCount : n; }

// Selection after the item count changed: never beyond the last visible item.
inline uint8_t menuClampSelected(uint8_t selected, uint8_t count) {
  return (count > 0 && selected >= count) ? (uint8_t)(count - 1) : selected;
}

}  // namespace rt
