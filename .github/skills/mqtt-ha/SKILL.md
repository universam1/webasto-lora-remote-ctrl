---
name: mqtt-ha
description: MQTT and HomeAssistant integration for Webasto receiver. WPA2 Enterprise WiFi, MQTT autodiscovery, OTA updates, diagnostic sensors. Use when implementing WiFi/MQTT control, HomeAssistant integration, or remote firmware updates.
compatibility: Requires MQTT broker (Mosquitto recommended), WiFi network access, HomeAssistant optional
metadata:
  author: webastolora
  phases: 1-7 complete
  features: WPA2-Enterprise, MQTT, OTA, diagnostics, low-power sleep
---

# MQTT/HomeAssistant Integration

Implementation details and lessons learned for tri-modal Webasto control: LoRa (primary), MQTT (WiFi), and local button.

## Overview

The receiver supports **tri-modal control**:
1. **LoRa control** (primary): Sender device → Receiver via 433MHz LoRa
2. **Local button control**: GPIO0 menu on receiver
3. **MQTT/HomeAssistant control**: WiFi-based remote control (when available)

**Critical constraint**: MQTT is supplementary. WiFi/MQTT operations must NEVER block LoRa reception or W-BUS control.

## Power Management & Sleep Behavior

### Normal Operation (DISABLE_SLEEP=0)

The receiver implements aggressive power management when running on battery:

**1. Startup Sequence**
- Initialize LoRa, W-BUS, WiFi, MQTT concurrently
- WiFi attempts connection for up to 10s (non-blocking)
- LoRa RX is active immediately regardless of WiFi state
- System ready within 2-3s, WiFi may still be connecting

**2. Heater OFF (Idle Mode)**
- **Sleep cycle**: Wake every 4s, listen for 400ms, then deep sleep
- OLED powered off to save energy
- WiFi/MQTT connection attempts continue in background (non-blocking)
- W-BUS polling minimized during sleep to reduce wake time
- Power consumption: ~10mA average (vs ~150mA when fully awake)

**3. Command Received**
- Any command (LoRa, MQTT, or button) wakes receiver fully
- If command starts heater: transition to Running mode
- If command received but heater stays OFF: stay awake for RX window

**4. Heater RUNNING**
- Stay fully awake (no deep sleep)
- OLED stays on per requirement
- LoRa RX continuous for low-latency status updates
- WiFi/MQTT maintain connection for real-time monitoring
- W-BUS polled every 2s for status updates
- Power consumption: ~150-200mA

**5. Heater Turns OFF → Extended Wake**
- **Stay awake for 60s after heater shuts down**
- Allows final status updates to reach sender/MQTT
- Ensures clean shutdown communication
- W-BUS continues polling during extended wake
- After 60s: return to sleep cycle if no new commands

**6. W-BUS Polling Behavior (Power Management)**
- **CRITICAL**: W-BUS polling wakes the Webasto heater, drawing unnecessary power
- **Solution**: Only poll W-BUS when absolutely necessary:
  - ✅ Heater is RUNNING (continuous monitoring required)
  - ✅ In extended wake period (final status updates)
  - ✅ Explicit `QueryStatus` command received (on-demand polling)
  - ❌ **NOT during idle wake windows** (would wake Webasto unnecessarily)
- Use `QueryStatus` command to poll status on-demand without starting heater
- Available via LoRa, MQTT, or button menu

**7. QueryStatus Command**
- **Purpose**: Poll W-BUS for current status without starting the heater
- **Use case**: Check heater temperature/voltage while sleeping to avoid waking it continuously
- **Availability**:
  - **LoRa**: Sender transmits `CommandKind::QueryStatus`
  - **MQTT**: Publish to `webasto/receiver/query` topic
  - **Button**: Select "STATUS?" from menu
- **Behavior**: Sets flag → polls W-BUS on next 2s interval → sends status back
- **Power impact**: Single W-BUS query (~250ms) instead of continuous 2s polling

**8. Testing Mode (DISABLE_SLEEP=1)**
- Fully awake at all times
- W-BUS polls continuously every 2s (as heater always considered "running")
- Useful for serial debugging and development
- Set in `platformio.ini`: `-D DISABLE_SLEEP=1`

### Configuration

```cpp
// project_config.h
#define RX_IDLE_LISTEN_WINDOW_MS 400   // LoRa RX window when waking
#define RX_IDLE_SLEEP_MS 4000          // Sleep interval when heater OFF
#define RX_OFF_EXTENDED_WAKE_MS 60000  // Stay awake 60s after heater stops
```

**Tuning notes**:
- Shorter `RX_IDLE_SLEEP_MS` = faster command response, higher power draw
- Longer `RX_IDLE_LISTEN_WINDOW_MS` = higher command catch rate, more wake power
- Extended wake ensures WiFi has time to publish final status before sleeping

## Architecture

### Component Stack
```
HomeAssistant Dashboard
         ↓ (MQTT commands)
    MQTT Broker (Mosquitto)
         ↓
WiFiManager (WPA2 Enterprise) ───→ MQTTClient (PubSubClient wrapper)
         │                              ↓
         │                         Command Handler
         │                              ↓
         └─────────────→ W-BUS Controller → Webasto Heater
                             ↑
                        (Status polling)
```

### Non-Blocking Design
- **WiFi connection**: 10s timeout, non-blocking state machine
- **MQTT reconnect**: 5s retry interval, async
- **Main loop overhead**: <10ms per iteration
- **Graceful degradation**: LoRa/W-BUS continue working when WiFi unavailable

## WiFi Manager (Phase 1)

**Files**: `lib/common/wifi_manager.{h,cpp}`

### WPA2 Enterprise PEAP
Uses ESP32's `esp_wpa2` API for enterprise authentication:
```cpp
#include <esp_wpa2.h>
esp_wifi_sta_wpa2_ent_set_username((uint8_t*)username, strlen(username));
esp_wifi_sta_wpa2_ent_set_password((uint8_t*)password, strlen(password));
esp_wifi_sta_wpa2_ent_enable();
WiFi.begin(ssid);
```

### Fast Reconnect
- Caches BSSID and channel on first successful connection
- Uses `WiFi.begin(ssid, pass, channel, bssid)` for 1-2s reconnect (vs 5s full scan)

### State Machine
- `DISCONNECTED` → `CONNECTING` (10s max) → `CONNECTED`
- `FAILED` → retry after 60s
- `update()` is non-blocking, checks status without delays

## MQTT Client (Phase 2)

**Files**: `lib/common/mqtt_client.{h,cpp}`

### Library Choice
Uses `PubSubClient` v2.8 (~15KB flash):
- Proven, lightweight, Arduino-compatible
- Non-blocking `loop()` for message processing
- Supports QoS 0/1, retained messages, LWT

### Command Structure
```cpp
struct MQTTCommand {
  enum Type { STOP, START, RUN_MINUTES };
  Type type;
  uint8_t minutes;
  uint32_t timestampSec;  // Unix epoch for freshness check
};
```

### Stale Command Protection
- Commands older than 1 hour rejected: `current_time - cmd_timestamp > 3600s`
- Uses NTP time sync: `configTime(0, 0, "pool.ntp.org")`
- Prevents executing outdated commands after WiFi reconnection

### Topic Structure
```
webasto/receiver/mode/set          (command: "off" or "heat")
webasto/receiver/minutes/set       (command: integer 10-90)
webasto/receiver/mode/state        (status: current mode)
webasto/receiver/temperature/state (status: heater temp °C)
webasto/receiver/voltage/state     (status: battery voltage V)
webasto/receiver/power/state       (status: heater power W)
webasto/receiver/availability      (LWT: "online" or "offline")
```

## HomeAssistant Autodiscovery (Phase 3)

### Discovery Payload
Published to: `homeassistant/climate/webasto_receiver/config`

Key fields:
- **Device info**: identifiers, manufacturer, model, sw_version
- **Climate entity**: modes ["off", "heat"], temperature as runtime minutes
- **Command topics**: mode/set, minutes/set
- **State topics**: mode/state, temperature/state, voltage/state, power/state
- **Availability**: LWT topic for online/offline status

Discovery message is **retained** and republished on MQTT reconnect.

### Climate Entity Mapping
- **Mode "off"** → W-BUS `stop()` command
- **Mode "heat"** → W-BUS `startParkingHeater(minutes)` command
- **"Temperature" control** → Repurposed as runtime minutes (10-90 in steps of 10)
- **Current temperature sensor** → Actual heater temperature from W-BUS page 0x05

## Integration (Phase 4)

### Main Loop Structure
```cpp
void loop() {
  // 1. Primary functions (highest priority, always run)
  menu.update();
  loraLink.recv(...);
  wbus.readOperatingState(...);
  
  // 2. MQTT (lowest priority, non-blocking)
  #ifdef ENABLE_MQTT_CONTROL
    wifiMgr.update();  // ~1-2ms
    if (wifiMgr.isConnected()) {
      mqttClient.update();  // ~2-5ms
      
      // Publish status every 30s
      if (millis() - lastPublish > 30000) {
        mqttClient.publishStatus(gStatus);
        lastPublish = millis();
      }
    }
  #endif
  
  // 3. OLED refresh
  ui.render();
}
```

### Command Callback
MQTT commands execute W-BUS commands via callback:
```cpp
mqttClient.setCommandCallback([](const MQTTCommand& cmd) {
  switch (cmd.type) {
    case MQTTCommand::STOP:
      wbus.stop();
      gStatus.state = HeaterState::Off;
      break;
    case MQTTCommand::START:
      wbus.startParkingHeater(cmd.minutes);
      gStatus.state = HeaterState::Running;
      break;
    // ...
  }
  mqttClient.publishStatus(gStatus);  // Immediate ACK
});
```

## Phase 6: Diagnostic Sensors ✅

**Files**: Extended `mqtt_client.{h,cpp}`, modified `main.cpp`

### Additional Sensors
1. **LoRa RSSI** (dBm): Signal strength from last received LoRa command
2. **LoRa SNR** (dB): Signal-to-noise ratio for link quality monitoring
3. **Last Command Source**: `"lora"`, `"mqtt"`, or `"button"` (tracks control method)
4. **W-BUS Health**: `{"status": "healthy|unhealthy", "details": "..."}` (JSON payload)

### Autodiscovery
Each sensor gets discovery message published to:
- `homeassistant/sensor/webasto_receiver_lora_rssi/config`
- `homeassistant/sensor/webasto_receiver_lora_snr/config`
- `homeassistant/sensor/webasto_receiver_last_cmd_source/config`
- `homeassistant/sensor/webasto_receiver_wbus_health/config`

### Publishing Interval
- **Status sensors** (temp/voltage/power): Every 30s
- **Diagnostic sensors**: Every 60s (configurable via `MQTT_DIAGNOSTIC_INTERVAL_MS`)

### Command Source Tracking
```cpp
// In LoRa command handler
gLastCommandSource = "lora";

// In MQTT command callback
gLastCommandSource = "mqtt";

// In menu button handler
gLastCommandSource = "button";
```

## Phase 7: OTA Updates ✅

**Files**: `lib/common/ota_updater.{h,cpp}`, extended `mqtt_client.cpp` and `main.cpp`

### OTA Trigger
MQTT topic: `webasto/receiver/ota/update`
Payload:
```json
{
  "url": "http://192.168.1.100:8080/firmware.bin",
  "username": "optional_http_basic_auth",
  "password": "optional_password"
}
```

### Safety Checks
1. **Heater running check**: OTA BLOCKED if heater is running
2. **WiFi requirement**: OTA only when WiFi connected
3. **Error recovery**: ESP32 rollback protection prevents bricking

### OTA Status Publishing
Topic: `webasto/receiver/ota/status`
Statuses:
- `"requested"` - OTA command received
- `"starting"` - Beginning firmware download
- `"success"` - Update successful (device will reboot)
- `"failed"` - Update failed (error message included)
- `"deferred"` - Cannot update now (heater running)
- `"error"` - Error occurred

### OTA Progress Reporting
```cpp
otaUpdater.setProgressCallback([](size_t current, size_t total) {
  uint8_t percent = (current * 100) / total;
  ui.setLine(0, "OTA Update");
  ui.setLine(1, String(percent) + "% complete");
  ui.render();
});
```

### Implementation Details
Uses ESP32's `Update.h` library:
```cpp
HTTPClient http;
http.begin(url);
int httpCode = http.GET();
int contentLength = http.getSize();
Update.begin(contentLength);
WiFiClient* stream = http.getStreamPtr();
size_t written = Update.writeStream(*stream);
Update.end();
ESP.restart();  // Reboot with new firmware
```

## Credential Management (Production)

**Pattern**: Template-based credentials (never commit real credentials to git)

### Files
- `include/credentials.h.template` - Template with placeholders (commit to git)
- `include/credentials.h` - Real credentials (in .gitignore, never commit)
- `.gitignore` - Contains `include/credentials.h`

### Setup Process
```bash
cp include/credentials.h.template include/credentials.h
# Edit credentials.h with real values
# Build firmware
```

### Template Structure
```cpp
// WiFi WPA2 Enterprise
#define MQTT_WIFI_SSID "your-ssid-here"
#define MQTT_WIFI_USERNAME "your-username@domain.edu"
#define MQTT_WIFI_PASSWORD "your-password-here"
#define MQTT_WIFI_ANONYMOUS_ID ""

// MQTT Broker
#define MQTT_BROKER "192.168.1.100"
#define MQTT_PORT 1883
#define MQTT_CLIENT_ID "webasto_receiver"
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""

// OTA Updates
#define OTA_UPDATE_URL ""
#define OTA_UPDATE_USERNAME ""
#define OTA_UPDATE_PASSWORD ""
```

## Configuration Options

All in `include/project_config.h`:

```cpp
// Feature flags
#define ENABLE_MQTT_CONTROL           // Master switch (comment out to disable all MQTT)
#define MQTT_ENABLE_DIAGNOSTIC_SENSORS // Phase 6
#define MQTT_ENABLE_OTA                // Phase 7

// WiFi parameters
#define MQTT_WIFI_TIMEOUT_MS 10000         // Connection timeout
#define MQTT_WIFI_RETRY_INTERVAL_MS 60000  // Retry interval on failure

// MQTT parameters
#define MQTT_TOPIC_BASE "webasto/receiver"
#define MQTT_DISCOVERY_PREFIX "homeassistant"
#define MQTT_CMD_MAX_AGE_SEC 3600          // Stale command threshold
#define MQTT_STATUS_INTERVAL_MS 30000      // Status publish interval
#define MQTT_DIAGNOSTIC_INTERVAL_MS 60000  // Diagnostic publish interval

// OTA parameters
#define OTA_CHECK_INTERVAL_MS 21600000     // 6 hours
#define OTA_UPDATE_TOPIC "webasto/receiver/ota/update"
#define OTA_STATUS_TOPIC "webasto/receiver/ota/status"
```

## Resource Usage

### Flash (Program Memory)
- WiFi Manager: ~8 KB
- PubSubClient library: ~15 KB
- MQTT wrapper + autodiscovery: ~5 KB
- Phase 6 (diagnostics): ~5 KB
- Phase 7 (OTA): ~8 KB
- **Total: ~41 KB** (acceptable for ESP32's 4MB flash)

### RAM (Runtime Memory)
- WiFi state: ~1 KB
- MQTT client buffers: ~2 KB
- JSON parsing buffer: ~512 bytes
- Phase 6 state: ~1 KB
- Phase 7 state: ~2 KB
- **Total: ~6.5 KB** (acceptable for ESP32's 520KB SRAM)

### Performance
- WiFi manager update: ~1-2ms per loop
- MQTT client update: ~2-5ms per loop
- Status publishing: ~10-20ms every 30s
- **Loop overhead: <10ms** (negligible impact on LoRa reception)

## Troubleshooting

### WiFi Not Connecting
1. Check SSID/username/password in `credentials.h`
2. Monitor serial output: `[WiFi] Connecting...` → `[WiFi] Connected!`
3. For WPA2 Enterprise: Verify anonymous_id requirement with network admin
4. Check router DHCP availability
5. Verify ESP32 can reach the network (try simple WiFi.begin() test)

### MQTT Not Connecting
1. Verify broker IP/port in `credentials.h`
2. Check broker is running: `mosquitto -v` or check HA add-on status
3. Test broker with mosquitto_pub/sub: `mosquitto_sub -h BROKER_IP -t '#' -v`
4. Check firewall allows port 1883
5. Serial output shows: `[MQTT] Connection failed, rc=X` (see PubSubClient error codes)

### HomeAssistant Not Discovering
1. Check MQTT integration is enabled in HA
2. Verify discovery prefix matches HA config (default: `homeassistant`)
3. Check retained messages in broker: `mosquitto_sub -h BROKER_IP -t 'homeassistant/#' -v`
4. Force rediscovery: Disconnect/reconnect WiFi on receiver
5. Check HA logs for MQTT discovery errors

### Stale Command Rejection
1. Verify NTP sync working: `configTime(0, 0, "pool.ntp.org")`
2. Check system time: `time(nullptr)` should return current epoch
3. Serial shows: `[MQTT-CMD] Command too old: X seconds`
4. Ensure HomeAssistant and receiver clocks are synchronized

### OTA Update Issues
1. **"Heater must be OFF for OTA"**: Stop heater first, then retry
2. **"WiFi required for OTA"**: Ensure WiFi connected before triggering OTA
3. **Download failed**: Check firmware server is running and accessible
4. **Write failed**: Verify firmware file is valid .bin for ESP32
5. **Update loop**: Check firmware boots successfully (serial output on boot)

## Testing Checklist

### Basic MQTT Control
- [ ] WiFi connects successfully
- [ ] MQTT broker connection established
- [ ] HomeAssistant discovers device automatically
- [ ] Send "heat" mode from HA → heater starts
- [ ] Send "off" mode from HA → heater stops
- [ ] Status updates appear in HA sensors

### Command Source Tracking (Phase 6)
- [ ] Send LoRa command → sensor shows `"lora"`
- [ ] Send MQTT command → sensor shows `"mqtt"`
- [ ] Send button command → sensor shows `"button"`

### Diagnostic Monitoring (Phase 6)
- [ ] LoRa RSSI/SNR update when LoRa command received
- [ ] W-BUS health shows `"healthy"` during normal operation
- [ ] W-BUS health shows `"unhealthy"` when communication errors occur

### OTA Updates (Phase 7)
- [ ] OTA deferred when heater running (safety check)
- [ ] OTA proceeds when heater OFF + WiFi connected
- [ ] Progress displayed on OLED (0% → 100%)
- [ ] Device reboots with new firmware
- [ ] Status published to MQTT during OTA process

### Edge Cases
- [ ] WiFi drops mid-operation → MQTT disconnects gracefully, LoRa continues
- [ ] MQTT command while heater running → executes correctly
- [ ] Stale command (>1h old) → rejected with log message
- [ ] Multiple control sources (LoRa + MQTT) → state syncs correctly
- [ ] Deep sleep wake → WiFi reconnects, MQTT resumes

## Lessons Learned

### WiFi Manager
- **BSSID caching**: Reduces reconnect time from 5s to 1-2s (significant for car use)
- **Timeout enforcement**: Critical to prevent blocking main loop
- **Event callbacks**: Use WiFi events instead of polling `WiFi.status()` for efficiency

### MQTT Client
- **Callback-based commands**: Cleaner than polling topics in main loop
- **Retained messages**: Essential for state persistence across reboots
- **LWT (Last Will)**: Provides availability status without polling

### HomeAssistant Integration
- **Climate entity mapping**: "Temperature" control repurposed as runtime minutes (creative but works)
- **Autodiscovery timing**: Publish on boot + reconnect ensures HA always has current config
- **Sensor attributes**: JSON payloads allow rich diagnostic data (e.g., W-BUS health details)

### OTA Updates
- **Safety first**: Never update while heater running (could leave heater on)
- **Progress reporting**: User feedback critical for 1-2 minute update process
- **HTTP server**: Simple Python HTTP server sufficient for local testing: `python3 -m http.server 8080`
- **Rollback protection**: ESP32 Update library handles corrupted firmware gracefully

### Credential Management
- **Template pattern**: Industry standard for open-source projects
- **Git safety**: `.gitignore` prevents accidental credential commits
- **Build-time inclusion**: No runtime parsing overhead

## Future Enhancements

### Potential Improvements
- **NVS credential storage**: Web-based credential config (avoid recompilation)
- **MQTT TLS/SSL**: Encrypted MQTT connection (port 8883)
- **Certificate-based WPA2**: Use client certificates instead of username/password
- **Scheduled operations**: Cron-style heater scheduling via MQTT
- **Geofencing**: Auto-disable WiFi when car moves (GPS integration)
- **Battery protection**: Disable WiFi when battery voltage < threshold
- **A/B partition OTA**: Use ESP32's dual-partition feature for safer updates
- **Firmware version tracking**: Publish current version to MQTT
- **Remote diagnostics**: Detailed error reporting to HomeAssistant
- **Multi-language OLED**: Internationalization for status messages

## References

- **PubSubClient docs**: https://pubsubclient.knolleary.net/
- **ESP32 WPA2 Enterprise**: https://github.com/martinius96/ESP32-eduroam
- **HomeAssistant MQTT discovery**: https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery
- **ESP32 OTA updates**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ota.html
