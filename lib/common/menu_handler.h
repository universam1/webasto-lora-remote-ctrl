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
  Start,     // Start heating
  Stop,      // Stop heating
  Run10min,  // Run for 10 minutes
  Run20min,  // Run for 20 minutes
  Run30min,  // Run for 30 minutes
  Run90min,  // Run for 90 minutes
  Count      // Number of menu items (sentinel)
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

 private:
  uint8_t buttonPin = GPIO_NUM_0;
  MenuState currentState = MenuState::Hidden;
  MenuItem selectedItem = MenuItem::Start;
  uint32_t lastButtonMs = 0;
  uint32_t buttonPressStartMs = 0;
  bool buttonWasPressed = false;
  uint32_t menuShowTimeMs = 0;
  bool itemActivated = false;

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
    return "START";
  case MenuItem::Stop:
    return "STOP";
  case MenuItem::Run10min:
    return "RUN 10min";
  case MenuItem::Run20min:
    return "RUN 20min";
  case MenuItem::Run30min:
    return "RUN 30min";
  case MenuItem::Run90min:
    return "RUN 90min";
  default:
    return "UNKNOWN";
  }
}
