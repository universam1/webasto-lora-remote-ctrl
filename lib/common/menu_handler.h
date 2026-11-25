#pragma once

#include <Arduino.h>
#include <cstdint>

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
  BlePair,      // BLE koppeln (receiver only)
  BleClear,     // Bindungen loeschen (receiver only)
  Count         // Number of menu items (sentinel)
};

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

  // Number of items the menu cycles through (default: all, MenuItem::Count).
  // The sender has no BLE items yet and limits the menu to the first five.
  void setItemCount(uint8_t n) { itemCount = (n == 0 || n > static_cast<uint8_t>(MenuItem::Count)) ? static_cast<uint8_t>(MenuItem::Count) : n; }
  uint8_t getItemCount() const { return itemCount; }

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
  uint8_t itemCount = static_cast<uint8_t>(MenuItem::Count);
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
  default:
    return "-";
  }
}
