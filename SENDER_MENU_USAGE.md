# Sender & Receiver Menu - Quick Reference

## How to Use

### Opening the Menu (Both Devices)
1. **Short press** the GPIO0 button (boot button) on the TTGO LoRa32
   - The OLED will switch from status view to menu view
   - The menu starts with "START" selected (marked with `>`)

### Navigating the Menu
1. **Short press** the button again to move to the next menu item
2. The selection cycles through:
   - START → STOP → RUN 10min → RUN 20min → RUN 30min → RUN 90min → START (loops)
3. Each short press resets the 10-second timeout

### Selecting an Item
1. **Long press** (hold for ~1 second) the button on your desired menu item
2. **Sender**: The command is sent to the receiver over LoRa
3. **Receiver**: The command is sent to the heater over W-BUS
4. The menu automatically closes
5. The OLED shows status reflecting the command result

### Menu Timeout
- If you don't interact with the menu for **10 seconds**, it automatically closes
- The OLED returns to the normal status view
- No command is sent when the menu times out

## Command Reference - Sender

| Menu Item | Command Sent | Effect |
|-----------|--------------|--------|
| START | Start with preset minutes | Heater starts for the last-set duration (default 30 min) |
| STOP | Stop | Heater stops immediately |
| RUN 10min | Run for 10 minutes | Heater runs for 10 min, preset is updated to 10 min |
| RUN 20min | Run for 20 minutes | Heater runs for 20 min, preset is updated to 20 min |
| RUN 30min | Run for 30 minutes | Heater runs for 30 min, preset is updated to 30 min |
| RUN 90min | Run for 90 minutes | Heater runs for 90 min, preset is updated to 90 min |

## Command Reference - Receiver

| Menu Item | Command Sent | Effect |
|-----------|--------------|--------|
| START | Start Parking Heater | Heater starts for the last-set duration (default 30 min) via W-BUS |
| STOP | Stop | Heater stops immediately via W-BUS |
| RUN 10min | Start Parking Heater | Heater runs for 10 min, preset updated, sent via W-BUS |
| RUN 20min | Start Parking Heater | Heater runs for 20 min, preset updated, sent via W-BUS |
| RUN 30min | Start Parking Heater | Heater runs for 30 min, preset updated, sent via W-BUS |
| RUN 90min | Start Parking Heater | Heater runs for 90 min, preset updated, sent via W-BUS |

## Status Display - Sender

**Preset Duration:** Shows the minutes used by the START command on the status view

**Battery Voltage:** Displayed in real-time

**Heater State:** Shows OFF, RUN, or ERR (received from receiver)

**Signal Quality:** RSSI and SNR from the last received status message

## Status Display - Receiver

**Heater State:** Shows OFF, RUN, or ERR (from W-BUS)

**Last Run Minutes:** The preset duration set by the last menu selection or command

**Operating State:** Raw W-BUS operating state code (0x04 = off, others = running)

**Last Command Time:** Time since the last command was received

**Status Cycling:** Displays temperature, voltage, power, and operating state, cycling every 3 seconds

## Troubleshooting

### Menu Won't Open
- Check that GPIO0 button is making contact (it's mechanical)
- Verify the button pulls GPIO0 to LOW when pressed
- Check serial output for "[SETUP] Menu button initialized on GPIO0"

### Sender: Menu Item Won't Activate
- Ensure you **hold** the button for at least 800ms (about 1 second)
- Watch the OLED - it should show "Waiting ACK" after activation
- Check serial output for [LORA] messages indicating transmission
- Verify receiver is powered on and in range

### Receiver: Menu Item Won't Activate
- Ensure you **hold** the button for at least 800ms (about 1 second)
- Check serial output for [WBUS] messages indicating W-BUS command
- Verify W-BUS device is powered on and responding
- Check heater status display - it should change after activation

### Menu Closes Immediately
- The timeout is 10 seconds - if menu was open earlier, wait a moment
- Try opening again with a short press

## Button Specs

- **Pin**: GPIO0 (boot button on TTGO LoRa32)
- **Active Level**: LOW (button pulls to ground)
- **Debounce**: 20ms (handled in firmware)
- **Normal State**: HIGH (pulled up internally)

## Differences Between Sender & Receiver

| Aspect | Sender | Receiver |
|--------|--------|----------|
| **Command Delivery** | Via LoRa radio | Direct W-BUS |
| **Requires** | Receiver to be on | Heater/W-BUS to be on |
| **Latency** | LoRa transmit time (500ms-1s) | Immediate (~100ms) |
| **Feedback** | ACK from receiver | Direct W-BUS response |
| **Use Case** | Remote control | Local direct control |
| **Power Draw** | Radio + wait for ACK | W-BUS I/O only |
