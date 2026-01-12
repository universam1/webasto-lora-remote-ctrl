# OLED Menu System - Summary

## What Was Added

The sender and receiver now both have an interactive OLED menu system controlled via the GPIO0 button.

## Quick Facts

- **Button**: GPIO0 (boot button on TTGO LoRa32)
- **Menu Items**: START, STOP, RUN 10min, RUN 20min, RUN 30min, RUN 90min
- **Opening**: Short press when menu is hidden
- **Navigation**: Short press cycles through items
- **Activation**: Long press (≥800ms) activates selected item
- **Timeout**: Menu auto-closes after 10 seconds of inactivity

## Files Changed

### New Files
- `lib/common/menu_handler.h` - Menu system header
- `lib/common/menu_handler.cpp` - Menu system implementation

### Modified Files
- `src/sender/main.cpp` - Added menu to sender firmware
- `src/receiver/main.cpp` - Added menu to receiver firmware
- `include/project_config.h` - Added MENU_BUTTON_PIN definition

### Documentation
- `SENDER_MENU.md` - Complete technical documentation
- `SENDER_MENU_USAGE.md` - User-friendly guide (updated for both devices)

## Sender Behavior

When you press the GPIO0 button on the sender:
1. Menu opens with status view replaced
2. Use short presses to cycle through menu items
3. Use long press to send LoRa command to receiver
4. Menu closes automatically after selection or 10-second timeout
5. Waits for acknowledgment from receiver before closing

## Receiver Behavior

When you press the GPIO0 button on the receiver:
1. Menu opens with status view replaced
2. Use short presses to cycle through menu items
3. Use long press to send W-BUS command directly to heater
4. Menu closes automatically after selection or 10-second timeout
5. W-BUS response updates heater state immediately

## Key Differences

| Aspect | Sender | Receiver |
|--------|--------|----------|
| **Target** | Receiver device | Heater via W-BUS |
| **Protocol** | LoRa radio | W-BUS TTL UART |
| **Feedback** | Waits for LoRa ACK | Direct W-BUS response |
| **Use Case** | Remote command | Local direct control |

## Implementation Highlights

✅ **Button Debouncing**: 20ms debounce prevents jitter
✅ **Long Press Detection**: 800ms threshold for activation
✅ **Menu Timeout**: Auto-closes after 10 seconds
✅ **State Machine**: Clean Hidden/Visible state transitions
✅ **OLED Rendering**: Conditional display of menu vs status
✅ **Serial Logging**: Full debug output for all actions
✅ **Zero External Dependencies**: Uses only Arduino/ESP32 built-ins

## No Breaking Changes

- All existing functionality preserved
- Serial commands still work on sender (start, stop, run)
- Receiver LoRa command reception unchanged
- W-BUS communication unchanged
- Low-power sleep modes unaffected

## Testing

To verify the implementation:
1. Compile and upload sender firmware
2. Compile and upload receiver firmware
3. Press GPIO0 on sender/receiver to open menu
4. Use short presses to navigate items
5. Use long press to activate
6. Check serial output for confirmation logs
7. Verify heater responds appropriately
