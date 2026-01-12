#include "menu_handler.h"

MenuHandler::MenuHandler() = default;

void MenuHandler::begin(uint8_t pin)
{
  buttonPin = pin;
  pinMode(buttonPin, INPUT);
  lastButtonMs = millis();
}

bool MenuHandler::readButton()
{
  // GPIO0 is pulled high by default on ESP32, button pulls it to ground
  // So button press = LOW
  return !digitalRead(buttonPin);
}

void MenuHandler::update()
{
  uint32_t now = millis();
  bool buttonPressed = readButton();

  // Debounce
  if (buttonPressed != buttonWasPressed)
  {
    if (now - lastButtonMs >= DEBOUNCE_MS)
    {
      buttonWasPressed = buttonPressed;
      lastButtonMs = now;

      if (buttonWasPressed)
      {
        // Button just pressed
        buttonPressStartMs = now;

        // If menu is hidden, show it on button press
        if (currentState == MenuState::Hidden)
        {
          show();
        }
      }
      else
      {
        // Button just released
        uint32_t pressDuration = now - buttonPressStartMs;

        // Only process releases in visible state
        if (currentState == MenuState::Visible)
        {
          if (pressDuration >= LONG_PRESS_MS)
          {
            // Long press: mark selected item as activated
            itemActivated = true;
            hide();
          }
          else
          {
            // Short press: move to next menu item
            selectedItem = static_cast<MenuItem>(
                (static_cast<uint8_t>(selectedItem) + 1) %
                static_cast<uint8_t>(MenuItem::Count));
            menuShowTimeMs = now;  // Reset timeout
          }
        }
      }
    }
  }

  // Check for menu timeout
  if (currentState == MenuState::Visible)
  {
    if (isTimedOut())
    {
      hide();
    }
  }
}

void MenuHandler::show()
{
  currentState = MenuState::Visible;
  menuShowTimeMs = millis();
  selectedItem = MenuItem::Start;  // Reset to first item
}

void MenuHandler::hide()
{
  currentState = MenuState::Hidden;
  menuShowTimeMs = 0;
}

bool MenuHandler::isTimedOut() const
{
  if (currentState != MenuState::Visible || menuShowTimeMs == 0)
    return false;

  return (millis() - menuShowTimeMs) > MENU_TIMEOUT_MS;
}
