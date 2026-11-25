#pragma once

#include <PubSubClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

#include "commands.h"
#include "heater_types.h"
#include "mqtt_codec.h"

// Forward declarations
class WiFiManager;

#ifdef MQTT_ENABLE_OTA
class OTAUpdater;
#endif

// Parsed MQTT command (legacy view, kept for the callback API). The system receives rt::Command.
struct MQTTCommand {
  enum Type {
    NONE = 0,
    START,
    STOP,
    RUN_MINUTES,
    QUERY_STATUS,
    CLIMATE_SET,
    CLIMATE_OFF
  };

  Type type;
  uint16_t minutes;
  uint8_t targetC;
  uint32_t timestampSec;

  MQTTCommand() : type(NONE), minutes(0), targetC(0), timestampSec(0) {}
};

// Callbacks for MQTT commands. Both run inside update(), i.e. in the net task.
typedef void (*MQTTCommandCallback)(const MQTTCommand& cmd);
typedef void (*MQTTRtCommandCallback)(const rt::Command& cmd);

// Not thread safe: every method must be called from the net task only (see net_task.h).
class MQTTClient {
public:
  MQTTClient(WiFiManager& wifiMgr);

  void begin(const char* broker, uint16_t port, const char* clientId,
             const char* username = "", const char* password = "");

  void setCommandCallback(MQTTCommandCallback callback);
  void setRtCommandCallback(MQTTRtCommandCallback callback);

  // Current target/duration/active state of the climate controller. Used as defaults when a
  // command arrives that does not carry them.
  void setSystemView(const wh::SystemSnapshot& snap);

  // True once after a duration was stored while the climate is inactive: the owner publishes the state.
  bool takeRememberedFlag() { const bool r = rememberedPending_; rememberedPending_ = false; return r; }

  // Target and duration as last set over MQTT (or the snapshot values if none yet).
  const mqttc::CommandMapper& commandMapper() const { return mapper_; }

#ifdef MQTT_ENABLE_OTA
  void setOTAUpdater(OTAUpdater* otaUpdater);
#endif

  // One step of the connection state machine, then mqtt.loop(). May block up to the socket
  // timeout (2 s) during a connect attempt. No delay() and no waiting loops.
  void update();

  bool isConnected() const;

  // Increases with every successful (re)connect, so the owner can republish everything.
  uint32_t connectionEpoch() const { return epoch_; }

  // Publish methods (false when not connected or the broker refused).
  bool publishState(const char* mode);           // legacy mode/state: "off" or "heat"
  bool publishTemperatureDC(int16_t dC);         // legacy temperature/state, 0.1 degC
  bool publishVoltage(uint16_t voltageMv);
  bool publishPower(uint16_t power);
  bool publishAvailability(bool available);

  // JSON state (not retained) plus the legacy topics, from the same snapshot.
  bool publishSnapshot(const wh::SystemSnapshot& snap, const char* stateJson);
  // {"src":"mqtt","result":"ok","tag":N} on last_cmd/state (not retained).
  bool publishCommandResult(rt::Source src, uint8_t result, uint8_t tag);

  // Diagnostic sensors
  bool publishLoRaRSSI(int rssi);
  bool publishLoRaSNR(float snr);
  bool publishLastCommandSource(const char* source);  // "lora", "mqtt", "button"
  bool publishWBusHealth(bool healthy, const char* details = "");  // also on bus_health/state
  // transportHealthy: health of the heater bus transport (CAN or W-BUS).
  bool publishDiagnostics(int loraRssi, float loraSNR, const char* cmdSource, bool transportHealthy,
                          const char* healthDetails = "");

  // HomeAssistant discovery (all entities, table driven, see mqtt_discovery.h)
  bool publishDiscovery();

#ifdef MQTT_ENABLE_OTA
  bool publishOTAStatus(const char* status, const char* message = "");
#endif

private:
  WiFiManager& wifiMgr_;
  WiFiClient wifiClient_;
  PubSubClient mqtt_;

  const char* broker_;
  uint16_t port_;
  const char* clientId_;
  const char* username_;
  const char* password_;

  // Reconnect back-off: 1 s, 2 s, 4 s ... 30 s, reset after a successful connect.
  uint32_t nextAttemptMs_;
  uint32_t backoffMs_;
  bool discoveryPublished_;
  uint32_t discoveryRetryMs_;
  bool rememberedPending_;
  uint32_t epoch_;
  uint8_t tagCounter_;

  MQTTCommandCallback cmdCallback_;
  MQTTRtCommandCallback rtCallback_;
  mqttc::CommandMapper mapper_;
  mqttc::CommandMapper::Defaults defaults_;

#ifdef MQTT_ENABLE_OTA
  OTAUpdater* otaUpdater_;
#endif

  void tryConnect();
  void subscribe();

  static void messageCallback(char* topic, byte* payload, unsigned int length);
  void handleMessage(const char* topic, const byte* payload, unsigned int length);

  static bool emitDiscovery(void* ctx, const char* topic, const char* payload);

  static MQTTClient* instance_;
};
