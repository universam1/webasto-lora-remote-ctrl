#include "mqtt_client.h"
#include "wifi_manager.h"
#include "project_config.h"
#include <time.h>

#ifdef ENABLE_MQTT_CONTROL

#include "mqtt_discovery.h"

#ifdef MQTT_ENABLE_OTA
#include "ota_updater.h"
#endif

namespace {
constexpr uint16_t kBufferSize = 1280;
constexpr uint32_t kBackoffMinMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;
constexpr uint32_t kDiscoveryRetryMs = 5000;
}  // namespace

MQTTClient* MQTTClient::instance_ = nullptr;

MQTTClient::MQTTClient(WiFiManager& wifiMgr)
  : wifiMgr_(wifiMgr),
    mqtt_(wifiClient_),
    broker_(nullptr),
    port_(0),
    clientId_(nullptr),
    username_(nullptr),
    password_(nullptr),
    nextAttemptMs_(0),
    backoffMs_(kBackoffMinMs),
    discoveryPublished_(false),
    discoveryRetryMs_(0),
    rememberedPending_(false),
    epoch_(0),
    tagCounter_(0),
    cmdCallback_(nullptr),
    rtCallback_(nullptr)
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
  mqtt_.setBufferSize(kBufferSize);  // discovery payloads and the JSON state
  mqtt_.setKeepAlive(60);
  mqtt_.setSocketTimeout(2);  // a connect attempt blocks the net task at most ~2 s

  Serial.println("[MQTT] Client initialized");
  Serial.printf("[MQTT] Broker: %s:%d, Client ID: %s, buffer %u bytes\n", broker_, port_, clientId_,
                (unsigned)kBufferSize);
}

void MQTTClient::setCommandCallback(MQTTCommandCallback callback) { cmdCallback_ = callback; }
void MQTTClient::setRtCommandCallback(MQTTRtCommandCallback callback) { rtCallback_ = callback; }

void MQTTClient::setSystemView(const wh::SystemSnapshot& snap) {
  defaults_.targetC = snap.climateTargetC;
  defaults_.durationMin = snap.climateDurationMin;
  defaults_.climateActive = snap.climateActive;
}

#ifdef MQTT_ENABLE_OTA
void MQTTClient::setOTAUpdater(OTAUpdater* otaUpdater) {
  otaUpdater_ = otaUpdater;
  Serial.println("[MQTT] OTA updater registered");
}
#endif

void MQTTClient::update() {
  if (!wifiMgr_.isConnected()) {
    if (mqtt_.connected()) mqtt_.disconnect();
    discoveryPublished_ = false;
    return;
  }

  const uint32_t now = millis();

  if (!mqtt_.connected()) {
    discoveryPublished_ = false;
    if ((int32_t)(now - nextAttemptMs_) >= 0) {
      tryConnect();  // may block up to the socket timeout
      if (mqtt_.connected()) {
        backoffMs_ = kBackoffMinMs;
      } else {
        nextAttemptMs_ = millis() + backoffMs_;  // 1 s, 2 s, 4 s ... 30 s
        backoffMs_ = backoffMs_ * 2 > kBackoffMaxMs ? kBackoffMaxMs : backoffMs_ * 2;
      }
    }
    return;
  }

  mqtt_.loop();

  if (!discoveryPublished_ && (int32_t)(now - discoveryRetryMs_) >= 0) {
    if (publishDiscovery()) {
      discoveryPublished_ = true;
      publishAvailability(true);
    } else {
      discoveryRetryMs_ = now + kDiscoveryRetryMs;
    }
  }
}

void MQTTClient::tryConnect() {
  Serial.printf("[MQTT] Connecting to %s:%d as %s (user %s)\n", broker_, port_, clientId_,
                username_ && strlen(username_) > 0 ? username_ : "(none)");

  bool connected = false;
  if (username_ && strlen(username_) > 0) {
    connected = mqtt_.connect(clientId_, username_, password_, MQTT_T_AVAILABILITY, 1, true, "offline");
  } else {
    connected = mqtt_.connect(clientId_, MQTT_T_AVAILABILITY, 1, true, "offline");
  }

  if (connected) {
    Serial.println("[MQTT] Connected to broker");
    subscribe();
    discoveryPublished_ = false;  // republish discovery on every (re)connect
    discoveryRetryMs_ = 0;
    epoch_++;
  } else {
    Serial.printf("[MQTT] Connection failed, rc=%d, retry in %lu ms\n", mqtt_.state(),
                  (unsigned long)backoffMs_);
  }
}

void MQTTClient::subscribe() {
  static const char* const kTopics[] = {
    MQTT_T_MODE_SET, MQTT_T_MINUTES_SET, MQTT_T_QUERY,
    MQTT_T_TARGET_SET, MQTT_T_DURATION_SET, MQTT_T_ENABLE_SET,
#ifdef MQTT_ENABLE_OTA
    OTA_UPDATE_TOPIC,
#endif
  };
  for (const char* t : kTopics) {
    if (mqtt_.subscribe(t)) Serial.printf("[MQTT] Subscribed to: %s\n", t);
  }
}

bool MQTTClient::isConnected() const {
  return const_cast<PubSubClient&>(mqtt_).connected();
}

// ---- publishing -------------------------------------------------------------------------

bool MQTTClient::publishState(const char* mode) {
  return mqtt_.connected() && mqtt_.publish(MQTT_T_MODE_STATE, mode, true);
}

bool MQTTClient::publishTemperatureDC(int16_t dC) {
  if (!mqtt_.connected()) return false;
  char p[16];
  mqttc::fmtDC(p, sizeof p, dC);
  return mqtt_.publish(MQTT_T_TEMP_STATE, p, true);
}

bool MQTTClient::publishVoltage(uint16_t voltageMv) {
  if (!mqtt_.connected()) return false;
  char p[16];
  mqttc::fmtMilli2(p, sizeof p, voltageMv);
  return mqtt_.publish(MQTT_T_VOLTAGE_STATE, p, true);
}

bool MQTTClient::publishPower(uint16_t power) {
  if (!mqtt_.connected()) return false;
  char p[16];
  snprintf(p, sizeof p, "%u", power);
  return mqtt_.publish(MQTT_T_POWER_STATE, p, true);
}

bool MQTTClient::publishAvailability(bool available) {
  if (!mqtt_.connected()) return false;
  return mqtt_.publish(MQTT_T_AVAILABILITY, available ? "online" : "offline", true);
}

bool MQTTClient::publishSnapshot(const wh::SystemSnapshot& snap, const char* stateJson) {
  if (!mqtt_.connected()) return false;
  const bool ok = mqtt_.publish(MQTT_T_STATE, stateJson, false);

  // Legacy topics, fed from the same snapshot.
  publishState(snap.heater.phase == wh::HeaterPhase::Heating ? "heat" : "off");
  const int16_t t = mqttc::currentTempDC(snap);
  if (t != wh::kUnknownDC) publishTemperatureDC(t);
  if (snap.heater.voltage_mV.seen && snap.heater.voltage_mV.v > 0) publishVoltage(snap.heater.voltage_mV.v);
  if (snap.heater.statusAtMs != 0) publishPower(wh::heatingPowerW(snap.heater.powerRaw));
  return ok;
}

bool MQTTClient::publishCommandResult(rt::Source src, uint8_t result, uint8_t tag) {
  if (!mqtt_.connected()) return false;
  char buf[96];
  if (mqttc::buildLastCmdJson(src, result, tag, buf, sizeof buf) == 0) return false;
  return mqtt_.publish(MQTT_T_LAST_CMD, buf, false);
}

bool MQTTClient::publishLoRaRSSI(int rssi) {
  if (!mqtt_.connected()) return false;
  char p[16];
  snprintf(p, sizeof p, "%d", rssi);
  return mqtt_.publish(MQTT_T_LORA_RSSI, p, true);
}

bool MQTTClient::publishLoRaSNR(float snr) {
  if (!mqtt_.connected()) return false;
  char p[16];
  snprintf(p, sizeof p, "%.2f", snr);
  return mqtt_.publish(MQTT_T_LORA_SNR, p, true);
}

bool MQTTClient::publishLastCommandSource(const char* source) {
  return mqtt_.connected() && mqtt_.publish(MQTT_T_LAST_CMD_SOURCE, source, true);
}

bool MQTTClient::publishWBusHealth(bool healthy, const char* details) {
  if (!mqtt_.connected()) return false;
  JsonDocument doc;
  doc["status"] = healthy ? "healthy" : "unhealthy";
  if (details && strlen(details) > 0) doc["details"] = details;
  char buf[128];
  if (serializeJson(doc, buf, sizeof buf) >= sizeof buf - 1) return false;
  const bool a = mqtt_.publish(MQTT_T_WBUS_HEALTH, buf, true);
  const bool b = mqtt_.publish(MQTT_T_BUS_HEALTH_ALIAS, buf, true);
  return a && b;
}

bool MQTTClient::publishDiagnostics(int loraRssi, float loraSNR, const char* cmdSource,
                                    bool transportHealthy, const char* healthDetails) {
  bool success = true;
  success &= publishLoRaRSSI(loraRssi);
  success &= publishLoRaSNR(loraSNR);
  success &= publishLastCommandSource(cmdSource);
  success &= publishWBusHealth(transportHealthy, healthDetails);
  return success;
}

// ---- discovery --------------------------------------------------------------------------

bool MQTTClient::emitDiscovery(void* ctx, const char* topic, const char* payload) {
  MQTTClient* self = static_cast<MQTTClient*>(ctx);
  if (!self->mqtt_.publish(topic, payload, true)) {
    Serial.printf("[MQTT] Discovery publish failed (%u bytes): %s\n", (unsigned)strlen(payload), topic);
    return false;
  }
  return true;
}

bool MQTTClient::publishDiscovery() {
  if (!mqtt_.connected()) return false;
#ifdef MQTT_ENABLE_DIAGNOSTIC_SENSORS
  const bool diag = true;
#else
  const bool diag = false;
#endif
  const bool ok = mqttd::publishAllDiscovery(&MQTTClient::emitDiscovery, this, diag);
  Serial.println(ok ? "[MQTT] Discovery published" : "[MQTT] Discovery incomplete");
  return ok;
}

// ---- incoming commands ------------------------------------------------------------------

void MQTTClient::messageCallback(char* topic, byte* payload, unsigned int length) {
  if (instance_) instance_->handleMessage(topic, payload, length);
}

void MQTTClient::handleMessage(const char* topic, const byte* payload, unsigned int length) {
#ifdef MQTT_ENABLE_OTA
  if (strcmp(topic, OTA_UPDATE_TOPIC) == 0) {
    Serial.println("[MQTT] OTA update command received");
    if (otaUpdater_) {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload, length);
      if (!error) {
        const char* url = doc["url"] | "";
        if (strlen(url) > 0) {
          otaUpdater_->requestUpdate(url);
          publishOTAStatus("requested", "OTA update requested");
        } else {
          publishOTAStatus("error", "No URL provided");
        }
      } else {
        publishOTAStatus("error", "Invalid JSON payload");
      }
    } else {
      publishOTAStatus("error", "OTA updater not initialized");
    }
    return;
  }
#endif

  char payloadStr[64];
  const unsigned int n = length < sizeof(payloadStr) - 1 ? length : sizeof(payloadStr) - 1;
  memcpy(payloadStr, payload, n);
  payloadStr[n] = '\0';
  Serial.printf("[MQTT] %s = %s\n", topic, payloadStr);

  struct Route { const char* topic; mqttc::CmdTopic id; };
  static const Route kRoutes[] = {
    {MQTT_T_MODE_SET, mqttc::CmdTopic::Mode},
    {MQTT_T_MINUTES_SET, mqttc::CmdTopic::Minutes},
    {MQTT_T_QUERY, mqttc::CmdTopic::Query},
    {MQTT_T_TARGET_SET, mqttc::CmdTopic::ClimateTarget},
    {MQTT_T_DURATION_SET, mqttc::CmdTopic::ClimateDuration},
    {MQTT_T_ENABLE_SET, mqttc::CmdTopic::ClimateEnable},
  };
  const Route* route = nullptr;
  for (const Route& r : kRoutes) {
    if (strcmp(topic, r.topic) == 0) { route = &r; break; }
  }
  if (!route) {
    Serial.println("[MQTT] Unknown topic");
    return;
  }

  rt::Command cmd;
  const auto res = mapper_.map(route->id, payloadStr, defaults_, ++tagCounter_, cmd);
  if (res == mqttc::CommandMapper::Out::Ignored) {
    Serial.println("[MQTT] Invalid payload ignored");
    return;
  }
  if (res == mqttc::CommandMapper::Out::Remembered) {
    Serial.printf("[MQTT] Climate duration stored: %u min\n",
                  (unsigned)mapper_.effectiveDuration(defaults_));
    rememberedPending_ = true;
    return;
  }

  if (rtCallback_) rtCallback_(cmd);

  if (cmdCallback_) {
    MQTTCommand legacy;
    switch (cmd.type) {
      case rt::CmdType::Heat:
        legacy.type = route->id == mqttc::CmdTopic::Minutes ? MQTTCommand::RUN_MINUTES : MQTTCommand::START;
        break;
      case rt::CmdType::Stop: legacy.type = MQTTCommand::STOP; break;
      case rt::CmdType::Query: legacy.type = MQTTCommand::QUERY_STATUS; break;
      case rt::CmdType::ClimateSet: legacy.type = MQTTCommand::CLIMATE_SET; break;
      case rt::CmdType::ClimateOff: legacy.type = MQTTCommand::CLIMATE_OFF; break;
    }
    legacy.minutes = cmd.minutes;
    legacy.targetC = cmd.targetC;
    legacy.timestampSec = (uint32_t)time(nullptr);
    cmdCallback_(legacy);
  }
}

#ifdef MQTT_ENABLE_OTA
bool MQTTClient::publishOTAStatus(const char* status, const char* message) {
  if (!mqtt_.connected()) return false;
  JsonDocument doc;
  doc["status"] = status;
  doc["timestamp"] = time(nullptr);
  if (message && strlen(message) > 0) doc["message"] = message;
  String payload;
  serializeJson(doc, payload);
  return mqtt_.publish(OTA_STATUS_TOPIC, payload.c_str(), true);
}
#endif

#endif // ENABLE_MQTT_CONTROL
