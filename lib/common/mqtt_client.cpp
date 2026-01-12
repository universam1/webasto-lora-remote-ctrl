#include "mqtt_client.h"
#include "wifi_manager.h"
#include "project_config.h"
#include <time.h>

#ifdef ENABLE_MQTT_CONTROL

#ifdef MQTT_ENABLE_OTA
#include "ota_updater.h"
#endif

MQTTClient* MQTTClient::instance_ = nullptr;

MQTTClient::MQTTClient(WiFiManager& wifiMgr)
  : wifiMgr_(wifiMgr),
    mqtt_(wifiClient_),
    broker_(nullptr),
    port_(0),
    clientId_(nullptr),
    username_(nullptr),
    password_(nullptr),
    lastReconnectMs_(0),
    reconnectIntervalMs_(5000),
    discoveryPublished_(false),
    cmdCallback_(nullptr)
#ifdef MQTT_ENABLE_OTA
    , otaUpdater_(nullptr)
#endif
{
  instance_ = this;
  mqtt_.setCallback(messageCallback);
}

void MQTTClient::begin(const char* broker, uint16_t port, const char* clientId,
                       const char* username, const char* password) {
  broker_ = broker;
  port_ = port;
  clientId_ = clientId;
  username_ = username;
  password_ = password;

  mqtt_.setServer(broker_, port_);
  mqtt_.setBufferSize(1024);  // Increase from default 256 bytes for discovery messages
  mqtt_.setKeepAlive(60);
  mqtt_.setSocketTimeout(5);  // 5 second socket timeout

  Serial.println("[MQTT] Client initialized");
  Serial.printf("[MQTT] Broker: %s:%d, Client ID: %s\n", broker_, port_, clientId_);
  Serial.println("[MQTT] Buffer size: 1024 bytes");
}

void MQTTClient::setCommandCallback(MQTTCommandCallback callback) {
  cmdCallback_ = callback;
}

#ifdef MQTT_ENABLE_OTA
void MQTTClient::setOTAUpdater(OTAUpdater* otaUpdater) {
  otaUpdater_ = otaUpdater;
  Serial.println("[MQTT] OTA updater registered");
}
#endif

void MQTTClient::update() {
  // Don't do anything if WiFi is not connected
  if (!wifiMgr_.isConnected()) {
    if (mqtt_.connected()) {
      mqtt_.disconnect();
      discoveryPublished_ = false;
    }
    return;
  }

  // Try to connect/reconnect to MQTT broker
  if (!mqtt_.connected()) {
    const uint32_t now = millis();
    if (now - lastReconnectMs_ > reconnectIntervalMs_) {
      tryConnect();
      lastReconnectMs_ = now;
    }
    return;
  }

  // Process incoming messages (non-blocking)
  mqtt_.loop();

  // Publish discovery message if not yet done
  if (!discoveryPublished_) {
    if (publishDiscovery()) {
      #ifdef MQTT_ENABLE_DIAGNOSTIC_SENSORS
      publishDiagnosticDiscovery();  // Phase 6: Also publish diagnostic sensors
      #endif
      discoveryPublished_ = true;
      publishAvailability(true);  // Announce we're online
    }
  }
}

void MQTTClient::tryConnect() {
  Serial.println("[MQTT] Connecting to broker...");

  // Set Last Will and Testament (LWT) - announce offline on disconnect
  String lwTopic = getAvailabilityTopic();
  
  bool connected = false;
  if (username_ && strlen(username_) > 0) {
    connected = mqtt_.connect(clientId_, username_, password_,
                              lwTopic.c_str(), 1, true, "offline");
  } else {
    connected = mqtt_.connect(clientId_, 
                              lwTopic.c_str(), 1, true, "offline");
  }

  if (connected) {
    Serial.println("[MQTT] Connected to broker!");
    subscribe();
    discoveryPublished_ = false;  // Trigger republish on reconnect
  } else {
    Serial.printf("[MQTT] Connection failed, rc=%d\n", mqtt_.state());
  }
}

void MQTTClient::subscribe() {
  String modeTopic = getModeCmdTopic();
  String minutesTopic = getMinutesCmdTopic();
  String queryTopic = getQueryTopic();

  if (mqtt_.subscribe(modeTopic.c_str())) {
    Serial.printf("[MQTT] Subscribed to: %s\n", modeTopic.c_str());
  }

  if (mqtt_.subscribe(minutesTopic.c_str())) {
    Serial.printf("[MQTT] Subscribed to: %s\n", minutesTopic.c_str());
  }
  
  if (mqtt_.subscribe(queryTopic.c_str())) {
    Serial.printf("[MQTT] Subscribed to: %s\n", queryTopic.c_str());
  }
  
  #ifdef MQTT_ENABLE_OTA
  // Subscribe to OTA update topic (Phase 7)
  if (mqtt_.subscribe(OTA_UPDATE_TOPIC)) {
    Serial.printf("[MQTT] Subscribed to OTA topic: %s\n", OTA_UPDATE_TOPIC);
  }
  #endif
}

bool MQTTClient::isConnected() const {
  return const_cast<PubSubClient&>(mqtt_).connected();
}

bool MQTTClient::publishState(const char* mode) {
  if (!mqtt_.connected()) return false;
  
  String topic = getModeStateTopic();
  bool result = mqtt_.publish(topic.c_str(), mode, true);  // Retained
  
  if (result) {
    Serial.printf("[MQTT] Published state: %s\n", mode);
  }
  
  return result;
}

bool MQTTClient::publishTemperature(int16_t tempC) {
  if (!mqtt_.connected()) return false;
  
  String topic = getTempStateTopic();
  char payload[16];
  snprintf(payload, sizeof(payload), "%d", tempC);
  
  return mqtt_.publish(topic.c_str(), payload, true);  // Retained
}

bool MQTTClient::publishVoltage(uint16_t voltageMv) {
  if (!mqtt_.connected()) return false;
  
  String topic = getVoltageStateTopic();
  char payload[16];
  snprintf(payload, sizeof(payload), "%.2f", voltageMv / 1000.0f);
  
  return mqtt_.publish(topic.c_str(), payload, true);  // Retained
}

bool MQTTClient::publishPower(uint16_t power) {
  if (!mqtt_.connected()) return false;
  
  String topic = getPowerStateTopic();
  char payload[16];
  snprintf(payload, sizeof(payload), "%u", power);
  
  return mqtt_.publish(topic.c_str(), payload, true);  // Retained
}

bool MQTTClient::publishAvailability(bool available) {
  if (!mqtt_.connected()) return false;
  
  String topic = getAvailabilityTopic();
  const char* payload = available ? "online" : "offline";
  
  bool result = mqtt_.publish(topic.c_str(), payload, true);  // Retained
  
  if (result) {
    Serial.printf("[MQTT] Published availability: %s\n", payload);
  }
  
  return result;
}

bool MQTTClient::publishStatus(const proto::StatusPayload& status) {
  if (!mqtt_.connected()) return false;

  // Publish mode
  const char* mode = (status.state == proto::HeaterState::Running) ? "heat" : "off";
  publishState(mode);

  // Publish sensors
  if (status.temperatureC != INT16_MIN) {
    publishTemperature(status.temperatureC);
  }

  if (status.voltage_mV > 0) {
    publishVoltage(status.voltage_mV);
  }

  if (status.power > 0) {
    publishPower(status.power);
  }

  return true;
}

bool MQTTClient::publishDiscovery() {
  if (!mqtt_.connected()) return false;

  Serial.println("[MQTT] Publishing HomeAssistant discovery...");

  JsonDocument doc;
  
  // Device info
  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"].add("webasto_receiver_001");
  device["name"] = "Webasto ThermoTop C";
  device["manufacturer"] = "Custom";
  device["model"] = "TTGO LoRa32 + W-BUS";
  device["sw_version"] = "1.0.0";

  // Origin info
  JsonObject origin = doc["origin"].to<JsonObject>();
  origin["name"] = "Webasto LoRa Controller";
  origin["sw"] = "1.0.0";
  origin["url"] = "https://github.com/yourusername/webastolora";

  // Climate entity configuration
  doc["name"] = "Webasto Heater";
  doc["unique_id"] = "webasto_ttgo_receiver";
  doc["modes"].add("off");
  doc["modes"].add("heat");
  
  doc["mode_command_topic"] = getModeCmdTopic();
  doc["mode_state_topic"] = getModeStateTopic();
  
  doc["temperature_command_topic"] = getMinutesCmdTopic();
  doc["current_temperature_topic"] = getTempStateTopic();
  
  doc["min_temp"] = 10;
  doc["max_temp"] = 90;
  doc["temp_step"] = 10;
  doc["temperature_unit"] = "C";
  
  doc["availability_topic"] = getAvailabilityTopic();
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";

  // Serialize and publish
  String payload;
  serializeJson(doc, payload);
  
  Serial.printf("[MQTT] Discovery payload size: %d bytes\n", payload.length());
  
  String topic = getDiscoveryTopic();
  Serial.printf("[MQTT] Discovery topic: %s\n", topic.c_str());
  
  bool result = mqtt_.publish(topic.c_str(), payload.c_str(), true);  // Retained
  
  if (result) {
    Serial.println("[MQTT] Discovery published successfully");
  } else {
    Serial.printf("[MQTT] Discovery publish failed (payload: %d bytes, buffer: 1024 bytes)\n", payload.length());
  }

  return result;
}

void MQTTClient::messageCallback(char* topic, byte* payload, unsigned int length) {
  if (instance_) {
    instance_->handleMessage(topic, payload, length);
  }
}

void MQTTClient::handleMessage(const char* topic, const byte* payload, 
                                unsigned int length) {
  Serial.printf("[MQTT] Message received on topic: %s\n", topic);
  
  // Create null-terminated payload string
  char payloadStr[length + 1];
  memcpy(payloadStr, payload, length);
  payloadStr[length] = '\0';
  
  Serial.printf("[MQTT] Payload: %s\n", payloadStr);

  String topicStr(topic);
  String modeCmdTopic = getModeCmdTopic();
  String minutesCmdTopic = getMinutesCmdTopic();
  String queryTopic = getQueryTopic();
  
  #ifdef MQTT_ENABLE_OTA
  // Handle OTA update messages (Phase 7)
  if (topicStr == OTA_UPDATE_TOPIC) {
    Serial.println("[MQTT] OTA update command received");
    
    if (otaUpdater_) {
      // Parse JSON payload: {"url": "http://...", "username": "...", "password": "..."}
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload, length);
      
      if (!error) {
        const char* url = doc["url"] | "";
        if (strlen(url) > 0) {
          Serial.printf("[MQTT] OTA URL: %s\n", url);
          otaUpdater_->requestUpdate(url);
          publishOTAStatus("requested", "OTA update requested");
        } else {
          Serial.println("[MQTT] ERROR: No URL in OTA message");
          publishOTAStatus("error", "No URL provided");
        }
      } else {
        Serial.println("[MQTT] ERROR: Failed to parse OTA JSON");
        publishOTAStatus("error", "Invalid JSON payload");
      }
    } else {
      Serial.println("[MQTT] ERROR: OTA updater not registered");
      publishOTAStatus("error", "OTA updater not initialized");
    }
    return;
  }
  #endif

  MQTTCommand cmd;

  if (topicStr == modeCmdTopic) {
    // Mode command: "off" or "heat"
    if (strcmp(payloadStr, "off") == 0) {
      cmd.type = MQTTCommand::STOP;
      cmd.timestampSec = time(nullptr);  // Current time
    } else if (strcmp(payloadStr, "heat") == 0) {
      cmd.type = MQTTCommand::START;
      cmd.minutes = 30;  // Default runtime
      cmd.timestampSec = time(nullptr);
    } else {
      Serial.printf("[MQTT] Unknown mode: %s\n", payloadStr);
      return;
    }
  } else if (topicStr == minutesCmdTopic) {
    // Minutes command: integer value
    int minutes = atoi(payloadStr);
    if (minutes < 10 || minutes > 90) {
      Serial.printf("[MQTT] Invalid minutes: %d\n", minutes);
      return;
    }
    
    cmd.type = MQTTCommand::RUN_MINUTES;
    cmd.minutes = (uint8_t)minutes;
    cmd.timestampSec = time(nullptr);
  } else if (topicStr == queryTopic) {
    // Query status command: triggers W-BUS polling without starting heater
    Serial.println("[MQTT] Query status command received");
    cmd.type = MQTTCommand::QUERY_STATUS;
    cmd.timestampSec = time(nullptr);
  } else {
    Serial.println("[MQTT] Unknown topic");
    return;
  }

  // Validate command freshness
  if (!isCommandFresh(cmd.timestampSec)) {
    Serial.println("[MQTT] Command rejected: too old");
    return;
  }

  // Execute callback
  if (cmdCallback_) {
    Serial.println("[MQTT] Executing command callback");
    cmdCallback_(cmd);
  }
}

bool MQTTClient::parseCommand(const char* payload, unsigned int length, 
                               MQTTCommand& cmd) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.printf("[MQTT] JSON parse error: %s\n", error.c_str());
    return false;
  }

  // Parse command type
  const char* cmdType = doc["command"];
  if (!cmdType) {
    Serial.println("[MQTT] Missing 'command' field");
    return false;
  }

  if (strcmp(cmdType, "start") == 0) {
    cmd.type = MQTTCommand::START;
  } else if (strcmp(cmdType, "stop") == 0) {
    cmd.type = MQTTCommand::STOP;
  } else if (strcmp(cmdType, "run") == 0) {
    cmd.type = MQTTCommand::RUN_MINUTES;
  } else {
    Serial.printf("[MQTT] Unknown command type: %s\n", cmdType);
    return false;
  }

  // Parse minutes (optional for stop)
  if (cmd.type != MQTTCommand::STOP) {
    cmd.minutes = doc["minutes"] | 30;  // Default 30 minutes
  }

  // Parse timestamp
  cmd.timestampSec = doc["timestamp"] | 0;

  return true;
}

bool MQTTClient::isCommandFresh(uint32_t timestampSec) {
  if (timestampSec == 0) {
    // No timestamp provided, assume fresh
    return true;
  }

  uint32_t currentTimeSec = time(nullptr);
  if (currentTimeSec < 1000000000) {
    // Time not synced yet, assume fresh
    Serial.println("[MQTT] Warning: System time not synced, accepting command");
    return true;
  }

  uint32_t ageSec = currentTimeSec - timestampSec;
  if (ageSec > MQTT_CMD_MAX_AGE_SEC) {
    Serial.printf("[MQTT] Command too old: %lu seconds\n", ageSec);
    return false;
  }

  return true;
}

// Topic helper methods
String MQTTClient::getModeCmdTopic() {
  return String(MQTT_TOPIC_BASE) + "/mode/set";
}

String MQTTClient::getModeStateTopic() {
  return String(MQTT_TOPIC_BASE) + "/mode/state";
}

String MQTTClient::getMinutesCmdTopic() {
  return String(MQTT_TOPIC_BASE) + "/minutes/set";
}

String MQTTClient::getQueryTopic() {
  return String(MQTT_TOPIC_BASE) + "/query";
}

String MQTTClient::getTempStateTopic() {
  return String(MQTT_TOPIC_BASE) + "/temperature/state";
}

String MQTTClient::getVoltageStateTopic() {
  return String(MQTT_TOPIC_BASE) + "/voltage/state";
}

String MQTTClient::getPowerStateTopic() {
  return String(MQTT_TOPIC_BASE) + "/power/state";
}

String MQTTClient::getAvailabilityTopic() {
  return String(MQTT_TOPIC_BASE) + "/availability";
}

String MQTTClient::getDiscoveryTopic() {
  return String(MQTT_DISCOVERY_PREFIX) + "/climate/" + 
         String(MQTT_CLIENT_ID) + "/config";
}

// ============================================================================
// Phase 6: Diagnostic Sensor Publishing
// ============================================================================

bool MQTTClient::publishLoRaRSSI(int rssi) {
  if (!mqtt_.connected()) return false;
  String topic = getLoRaRSSITopic();
  return mqtt_.publish(topic.c_str(), String(rssi).c_str(), true);
}

bool MQTTClient::publishLoRaSNR(float snr) {
  if (!mqtt_.connected()) return false;
  String topic = getLoRaSNRTopic();
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%.2f", snr);
  return mqtt_.publish(topic.c_str(), buffer, true);
}

bool MQTTClient::publishLastCommandSource(const char* source) {
  if (!mqtt_.connected()) return false;
  String topic = getLastCmdSourceTopic();
  return mqtt_.publish(topic.c_str(), source, true);
}

bool MQTTClient::publishWBusHealth(bool healthy, const char* details) {
  if (!mqtt_.connected()) return false;
  
  // Publish as JSON with status and optional details
  JsonDocument doc;
  doc["status"] = healthy ? "healthy" : "unhealthy";
  if (details && strlen(details) > 0) {
    doc["details"] = details;
  }
  
  String payload;
  serializeJson(doc, payload);
  
  String topic = getWBusHealthTopic();
  return mqtt_.publish(topic.c_str(), payload.c_str(), true);
}

bool MQTTClient::publishDiagnostics(int loraRssi, float loraSNR, 
                                     const char* cmdSource, bool wbusHealthy) {
  // Publish all diagnostic sensors at once
  bool success = true;
  success &= publishLoRaRSSI(loraRssi);
  success &= publishLoRaSNR(loraSNR);
  success &= publishLastCommandSource(cmdSource);
  success &= publishWBusHealth(wbusHealthy);
  return success;
}

String MQTTClient::getLoRaRSSITopic() {
  return String(MQTT_TOPIC_BASE) + "/lora_rssi/state";
}

String MQTTClient::getLoRaSNRTopic() {
  return String(MQTT_TOPIC_BASE) + "/lora_snr/state";
}

String MQTTClient::getLastCmdSourceTopic() {
  return String(MQTT_TOPIC_BASE) + "/last_cmd_source/state";
}

String MQTTClient::getWBusHealthTopic() {
  return String(MQTT_TOPIC_BASE) + "/wbus_health/state";
}

// ============================================================================
// Phase 6: Diagnostic Sensor Discovery (HomeAssistant)
// ============================================================================

bool MQTTClient::publishDiagnosticDiscovery() {
  if (!mqtt_.connected()) return false;
  
  // Device info (shared across all entities)
  JsonDocument deviceDoc;
  JsonArray identifiers = deviceDoc["identifiers"].to<JsonArray>();
  identifiers.add(MQTT_CLIENT_ID);
  deviceDoc["name"] = "Webasto ThermoTop C";
  deviceDoc["manufacturer"] = "Custom";
  deviceDoc["model"] = "TTGO LoRa32 + W-BUS";
  deviceDoc["sw_version"] = "1.0.0";
  
  // 1. LoRa RSSI sensor
  {
    JsonDocument doc;
    doc["name"] = "LoRa RSSI";
    doc["unique_id"] = String(MQTT_CLIENT_ID) + "_lora_rssi";
    doc["state_topic"] = getLoRaRSSITopic();
    doc["device_class"] = "signal_strength";
    doc["unit_of_measurement"] = "dBm";
    doc["availability_topic"] = getAvailabilityTopic();
    doc["device"] = deviceDoc;
    
    String payload;
    serializeJson(doc, payload);
    String topic = String(MQTT_DISCOVERY_PREFIX) + "/sensor/" + 
                   String(MQTT_CLIENT_ID) + "_lora_rssi/config";
    if (!mqtt_.publish(topic.c_str(), payload.c_str(), true)) {
      Serial.println("[MQTT] Failed to publish LoRa RSSI discovery");
      return false;
    }
  }
  
  // 2. LoRa SNR sensor
  {
    JsonDocument doc;
    doc["name"] = "LoRa SNR";
    doc["unique_id"] = String(MQTT_CLIENT_ID) + "_lora_snr";
    doc["state_topic"] = getLoRaSNRTopic();
    doc["unit_of_measurement"] = "dB";
    doc["availability_topic"] = getAvailabilityTopic();
    doc["device"] = deviceDoc;
    
    String payload;
    serializeJson(doc, payload);
    String topic = String(MQTT_DISCOVERY_PREFIX) + "/sensor/" + 
                   String(MQTT_CLIENT_ID) + "_lora_snr/config";
    if (!mqtt_.publish(topic.c_str(), payload.c_str(), true)) {
      Serial.println("[MQTT] Failed to publish LoRa SNR discovery");
      return false;
    }
  }
  
  // 3. Last command source sensor
  {
    JsonDocument doc;
    doc["name"] = "Last Command Source";
    doc["unique_id"] = String(MQTT_CLIENT_ID) + "_last_cmd_source";
    doc["state_topic"] = getLastCmdSourceTopic();
    doc["icon"] = "mdi:source-branch";
    doc["availability_topic"] = getAvailabilityTopic();
    doc["device"] = deviceDoc;
    
    String payload;
    serializeJson(doc, payload);
    String topic = String(MQTT_DISCOVERY_PREFIX) + "/sensor/" + 
                   String(MQTT_CLIENT_ID) + "_last_cmd_source/config";
    if (!mqtt_.publish(topic.c_str(), payload.c_str(), true)) {
      Serial.println("[MQTT] Failed to publish Last Command Source discovery");
      return false;
    }
  }
  
  // 4. W-BUS health sensor
  {
    JsonDocument doc;
    doc["name"] = "W-BUS Health";
    doc["unique_id"] = String(MQTT_CLIENT_ID) + "_wbus_health";
    doc["state_topic"] = getWBusHealthTopic();
    doc["value_template"] = "{{ value_json.status }}";
    doc["json_attributes_topic"] = getWBusHealthTopic();
    doc["icon"] = "mdi:heart-pulse";
    doc["availability_topic"] = getAvailabilityTopic();
    doc["device"] = deviceDoc;
    
    String payload;
    serializeJson(doc, payload);
    String topic = String(MQTT_DISCOVERY_PREFIX) + "/sensor/" + 
                   String(MQTT_CLIENT_ID) + "_wbus_health/config";
    if (!mqtt_.publish(topic.c_str(), payload.c_str(), true)) {
      Serial.println("[MQTT] Failed to publish W-BUS Health discovery");
      return false;
    }
  }
  
  Serial.println("[MQTT] Published diagnostic sensor discovery");
  return true;
}

// ============================================================================
// Phase 7: OTA Update Status Publishing
// ============================================================================

#ifdef MQTT_ENABLE_OTA
bool MQTTClient::publishOTAStatus(const char* status, const char* message) {
  if (!mqtt_.connected()) return false;
  
  JsonDocument doc;
  doc["status"] = status;
  doc["timestamp"] = time(nullptr);
  if (message && strlen(message) > 0) {
    doc["message"] = message;
  }
  
  String payload;
  serializeJson(doc, payload);
  
  return mqtt_.publish(OTA_STATUS_TOPIC, payload.c_str(), true);
}
#endif

#endif // ENABLE_MQTT_CONTROL


