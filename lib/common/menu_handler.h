#pragma once

#include <Arduino.h>
#include <cstdint>

#include "menu_clamp.h"

enum class MenuState
{
  Hidden,    // Menu not shown
  Visible,   // Menu is open
};

enum class MenuItem
{
  Start,        // Heizen
  Stop,         // Aus
  ClimateOn,    // Klima an (stored target and duration)
  ClimateOff,   // Klima aus
  QueryStatus,  // Abfrage
  BlePair,      // BLE koppeln (only while the BLE stack runs)
  BleClear,     // Bindungen loeschen (only while the BLE stack runs)
  RangeTest,    // Ping-Pong LoRa range test (sender only)
  Count         // Number of menu items (sentinel)
};

// Item masks (bit i = MenuItem i). Order and numeric values of the existing items are unchanged.
constexpr uint16_t kMenuCommonMask = (1u << static_cast<uint8_t>(MenuItem::Start)) |
                                     (1u << static_cast<uint8_t>(MenuItem::Stop)) |
                                     (1u << static_cast<uint8_t>(MenuItem::ClimateOn)) |
                                     (1u << static_cast<uint8_t>(MenuItem::ClimateOff)) |
                                     (1u << static_cast<uint8_t>(MenuItem::QueryStatus));
constexpr uint16_t kMenuBleMask = (1u << static_cast<uint8_t>(MenuItem::BlePair)) |
                                  (1u << static_cast<uint8_t>(MenuItem::BleClear));
constexpr uint16_t kMenuRangeMask = 1u << static_cast<uint8_t>(MenuItem::RangeTest);

class MenuHandler
{
 public:
  MenuHandler();

  // Initialize button pin
  void begin(uint8_t buttonPin);

  // Call in loop() - handles button press detection
  void update();

  // Get current menu state
  MenuState getState() const { return currentState; }

  // Items the menu cycles through, as a mask of MenuItem bits (default: all). An item that gets
  // hidden while selected moves the selection to the first visible item. The page is derived from
  // the position of the selected item among the visible ones (there is no separate scroll offset).
  void setVisibleMask(uint16_t mask)
  {
    visibleMask = rt::menuMaskSanitize(mask, kCount);
    selectedItem = static_cast<MenuItem>(rt::menuMaskFix(visibleMask, kCount, static_cast<uint8_t>(selectedItem)));
  }
  // The first n items (compatibility).
  void setItemCount(uint8_t n)
  {
    const uint8_t c = rt::menuClampCount(n, kCount);
    setVisibleMask(static_cast<uint16_t>((1u << c) - 1u));
  }

  // Number of visible items, the item at a position (0 based) and the position of the selection.
  uint8_t getItemCount() const { return rt::menuMaskCount(visibleMask, kCount); }
  MenuItem getItemAt(uint8_t pos) const
  {
    const uint8_t i = rt::menuMaskItemAt(visibleMask, kCount, pos);
    return static_cast<MenuItem>(i == 0xFF ? 0 : i);
  }
  uint8_t getSelectedPos() const
  {
    const uint8_t p = rt::menuMaskPosOf(visibleMask, kCount, static_cast<uint8_t>(selectedItem));
    return p == 0xFF ? 0 : p;
  }

  // Get currently selected menu item
  MenuItem getSelectedItem() const { return selectedItem; }

  // Check if an item was just activated (user pressed long)
  // Returns true once per activation, then clears the flag
  bool isItemActivated(MenuItem &outItem)
  {
    if (itemActivated)
    {
      outItem = selectedItem;
      itemActivated = false;
      return true;
    }
    return false;
  }

  // Explicitly show/hide menu
  void show();
  void hide();

  // Check if menu has timed out (and auto-hide)
  bool isTimedOut() const;
  
  // Get long press progress (0.0 to 1.0) for visual feedback
  float getLongPressProgress() const;

 private:
  uint8_t buttonPin = GPIO_NUM_0;
  MenuState currentState = MenuState::Hidden;
  MenuItem selectedItem = MenuItem::Start;
  uint32_t lastButtonMs = 0;
  uint32_t buttonPressStartMs = 0;
  bool buttonWasPressed = false;
  uint32_t menuShowTimeMs = 0;
  bool itemActivated = false;
  static constexpr uint8_t kCount = static_cast<uint8_t>(MenuItem::Count);
  uint16_t visibleMask = static_cast<uint16_t>((1u << kCount) - 1u);
  bool longPressTriggered = false;  // Prevents re-triggering on same press

  // Constants for button handling
  static constexpr uint32_t DEBOUNCE_MS = 20;
  static constexpr uint32_t LONG_PRESS_MS = 800;
  static constexpr uint32_t MENU_TIMEOUT_MS = 10000;  // 10 seconds

  // Debounced button state
  bool readButton();
};

// Utility function to convert MenuItem to display string
inline const char *menuItemToStr(MenuItem item)
{
  switch (item)
  {
  case MenuItem::Start:
    return "Heizen";
  case MenuItem::Stop:
    return "Aus";
  case MenuItem::ClimateOn:
    return "Klima an";
  case MenuItem::ClimateOff:
    return "Klima aus";
  case MenuItem::QueryStatus:
    return "Abfrage";
  case MenuItem::BlePair:
    return "BLE koppeln";
  case MenuItem::BleClear:
    return "Bindungen loeschen";
  case MenuItem::RangeTest:
    return "Ping-Pong";
  default:
    return "-";
  }
}
