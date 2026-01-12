# Sender & Receiver OLED Menu Implementation

## Overview

Added an interactive OLED menu system to both sender and receiver firmware with button control on GPIO0 (the boot button on TTGO LoRa32).

## Hardware Setup

- **Button Pin**: GPIO0 (TTGO LoRa32 boot button)
- **Button Logic**: Pulled HIGH by default, active LOW (pressed)
- **No additional hardware required** - uses existing button on the board

## Menu Features

### Menu Items
1. **START** - Starts heating with preset minutes
2. **STOP** - Stops heating
3. **RUN 10min** - Starts heating for 10 minutes
4. **RUN 20min** - Starts heating for 20 minutes
5. **RUN 30min** - Starts heating for 30 minutes
6. **RUN 90min** - Starts heating for 90 minutes

### Button Behavior

| Action | Behavior |
|--------|----------|
| **Short Press (while menu hidden)** | Opens the menu |
| **Short Press (while menu visible)** | Moves to next menu item, resets timeout |
| **Long Press (≥800ms while menu visible)** | Activates selected item, closes menu, sends command |
| **Menu Timeout (10 seconds)** | Menu automatically closes if no activity |

### OLED Display

**Status View (menu hidden):**
- Line 0: "Webasto LoRa Sender"
- Line 1: Preset duration, battery voltage
- Line 2: Heater state and status age
- Line 3: Temperature, voltage, power readings
- Line 4: Signal quality (RSSI, SNR)
- Line 5: Last command sequence or waiting ACK status

**Menu View (menu visible):**
- Line 0: "=== MENU ===" banner
- Lines 2-5: Menu items with `>` indicator for selected item
- Line 5: "Long press to activate" hint

## Implementation Details

### New Files

1. **`lib/common/menu_handler.h`**
   - `MenuHandler` class for button/menu management
   - `MenuState` enum (Hidden, Visible)
   - `MenuItem` enum (Start, Stop, Run10min, Run20min, Run30min, Run90min)

2. **`lib/common/menu_handler.cpp`**
   - Implementation with debounced button reading
   - Timeout handling
   - State machine for menu transitions

### Modified Files

1. **`src/sender/main.cpp`**
   - Includes `menu_handler.h`
   - Creates `MenuHandler` instance
   - Calls `menu.begin(MENU_BUTTON_PIN)` in `setup()`
   - Calls `menu.update()` and checks `menu.isItemActivated()` in `loop()`
   - Added `handleMenuSelection()` function to send LoRa commands
   - Added `menuItemToStr()` helper function
   - Updated OLED rendering to show menu or status based on `menu.getState()`

2. **`src/receiver/main.cpp`**
   - Includes `menu_handler.h`
   - Creates `MenuHandler` instance
   - Calls `menu.begin(MENU_BUTTON_PIN)` in `setup()`
   - Calls `menu.update()` and checks `menu.isItemActivated()` in `loop()`
   - Added `handleMenuSelection()` function to directly control heater via W-BUS
   - Added `menuItemToStr()` helper function
   - Updated OLED rendering to show menu or status based on `menu.getState()`

3. **`include/project_config.h`**
   - Added `MENU_BUTTON_PIN` definition (GPIO_NUM_0)

## Technical Details

### Button Debouncing
- **Debounce delay**: 20ms
- Prevents jitter from mechanical button presses

### Press Duration Detection
- **Long press threshold**: 800ms
- Distinction between short press (menu navigation) and long press (activation)

### Menu Timeout
- **Timeout duration**: 10 seconds
- Menu automatically closes if user doesn't interact
- Resets on each short press in the menu

### State Machine

```
[Hidden] 
  ↓ (short press) 
[Visible]
  ↓ (short press) → next item, reset timeout
  ↓ (long press) → activate item, send command
  ↓ (timeout) → close
  ↓
[Hidden]
```

## Command Sending

**Sender:**
When a menu item is activated, `handleMenuSelection()` calls `sendCommandWithAck()` with appropriate parameters:

- **START**: Sends `CommandKind::Start` with `gLastMinutes`
- **STOP**: Sends `CommandKind::Stop` with 0 minutes
- **RUN Xmin**: Sends `CommandKind::RunMinutes` with X minutes, updates `gLastMinutes`

All commands follow the existing ACK-based acknowledgment protocol.

**Receiver:**
When a menu item is activated, `handleMenuSelection()` directly calls `wbus.startParkingHeater()` or `wbus.stop()` to control the heater:

- **START**: Calls `wbus.startParkingHeater(gLastRunMinutes)`
- **STOP**: Calls `wbus.stop()`
- **RUN Xmin**: Updates `gLastRunMinutes` and calls `wbus.startParkingHeater(X)`

The receiver bypasses LoRa and controls the heater directly via W-BUS.

## Serial Logging

Button and menu interactions are logged to Serial for debugging:

**Sender:**
```
[SETUP] Menu button initialized on GPIO0
[MENU] Activated: START
[LORA] Sending command kind=0 minutes=30 seq=123
```

**Receiver:**
```
[SETUP] Menu button initialized on GPIO0
[MENU] Activated: START
[WBUS] Menu: Sending START for 30 min
[WBUS] Menu START OK
```

## Default Configuration

In `include/project_config.h`:

```cpp
#ifndef MENU_BUTTON_PIN
#define MENU_BUTTON_PIN GPIO_NUM_0
#endif
```

This can be overridden via platformio.ini build flags if needed.

## Testing Checklist

**Both devices (Sender and Receiver):**
- [ ] Short press opens menu
- [ ] Short presses cycle through menu items (10, 20, 30, 90 minutes, then back)
- [ ] Menu indicator (>) shows selected item
- [ ] Long press (hold 1+ second) activates item and sends command
- [ ] Menu closes after 10 seconds of inactivity
- [ ] Status view displays normally when menu is closed
- [ ] Serial output shows menu activation log messages
- [ ] LED blinks indicate activity

**Sender specific:**
- [ ] Commands are sent via LoRa with correct parameters
- [ ] OLED displays "Waiting ACK" while awaiting response
- [ ] LED fast-blinks while waiting for acknowledgment

**Receiver specific:**
- [ ] Heater state changes immediately after menu selection
- [ ] W-BUS commands are sent correctly
- [ ] Status updates reflect heater state change
- [ ] Menu can control heater independently of sender commands
