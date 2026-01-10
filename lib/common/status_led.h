#pragma once

#include <Arduino.h>
#include "project_config.h"

/**
 * Simple status LED controller for the onboard LED (GPIO25 on TTGO LoRa32-OLED).
 * Provides solid on/off and blinking modes.
 */
class StatusLed {
 public:
  enum class Mode : uint8_t {
    Off,      // LED off
    On,       // LED on solid
    Blink,    // Blinking (configured period)
  };

  StatusLed(uint8_t pin = STATUS_LED_PIN) : ledPin(pin) {}

  void begin() {
    pinMode(ledPin, OUTPUT);
    digitalWrite(ledPin, LOW);  // Start off
    currentMode = Mode::Off;
    lastToggleMs = 0;
  }

  // Set LED to solid on
  void setOn() {
    if (currentMode != Mode::On) {
      currentMode = Mode::On;
      digitalWrite(ledPin, HIGH);
    }
  }

  // Set LED to solid off
  void setOff() {
    if (currentMode != Mode::Off) {
      currentMode = Mode::Off;
      digitalWrite(ledPin, LOW);
    }
  }

  // Toggle LED immediately (pulse/blink indicator)
  // This creates a brief pulse regardless of current mode
  void toggle() {
    uint32_t now = millis();
    // Only allow toggling if we're not in a pulse window
    if (now - lastPulseMs > 100) {  // 100ms minimum between pulses
      digitalWrite(ledPin, digitalRead(ledPin) == HIGH ? LOW : HIGH);
      lastPulseMs = now;
      pulseActive = true;
    }
  }

  // Set LED to blink with given period (ms on + ms off)
  void setBlink(uint16_t periodMs = 500) {
    currentMode = Mode::Blink;
    blinkPeriodMs = periodMs;
    lastToggleMs = millis();
    isBlinkOn = true;
    digitalWrite(ledPin, HIGH);
  }

  // Call frequently from loop() to update blinking
  void update() {
    uint32_t now = millis();
    
    // If we're in a pulse, wait until it's done before resuming normal mode
    if (pulseActive && (now - lastPulseMs) > 50) {  // 50ms pulse duration
      pulseActive = false;
      // Re-apply the current mode
      if (currentMode == Mode::On) {
        digitalWrite(ledPin, HIGH);
      } else if (currentMode == Mode::Off) {
        digitalWrite(ledPin, LOW);
      } else if (currentMode == Mode::Blink) {
        // Resume blinking from where we left off
        isBlinkOn = !isBlinkOn;
        digitalWrite(ledPin, isBlinkOn ? HIGH : LOW);
        lastToggleMs = now;
      }
      return;
    }
    
    if (currentMode != Mode::Blink) return;

    if (now - lastToggleMs >= blinkPeriodMs) {
      isBlinkOn = !isBlinkOn;
      digitalWrite(ledPin, isBlinkOn ? HIGH : LOW);
      lastToggleMs = now;
    }
  }

  Mode getMode() const { return currentMode; }

 private:
  uint8_t ledPin;
  Mode currentMode = Mode::Off;
  uint16_t blinkPeriodMs = 500;
  uint32_t lastToggleMs = 0;
  bool isBlinkOn = false;
  uint32_t lastPulseMs = 0;
  bool pulseActive = false;
};
