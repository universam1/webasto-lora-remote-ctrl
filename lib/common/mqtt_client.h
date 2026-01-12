#pragma once

#include <PubSubClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "protocol.h"

// Forward declarations
class WiFiManager;

#ifdef MQTT_ENABLE_OTA
class OTAUpdater;
#endif

// MQTT command structure
struct MQTTCommand {
  enum Type { 
    NONE = 0,
    START,
    STOP,
    RUN_MINUTES
  };
  
  Type type;
  uint8_t minutes;
  uint32_t timestampSec;
  
  MQTTCommand() : type(NONE), minutes(0), timestampSec(0) {}
};

// Callback for MQTT commands
typedef void (*MQTTCommandCallback)(const MQTTCommand& cmd);

class MQTTClient {
public:
  MQTTClient(WiFiManager& wifiMgr);

  // Initialize MQTT client
  void begin(const char* broker, uint16_t port, const char* clientId,
             const char* username = "", const char* password = "");

  // Set command callback
  void setCommandCallback(MQTTCommandCallback callback);
  
  #ifdef MQTT_ENABLE_OTA
  // Set OTA updater reference (Phase 7)
  void setOTAUpdater(OTAUpdater* otaUpdater);
  #endif

  // Non-blocking update - call in loop()
  void update();

  // Check if MQTT is connected
  bool isConnected() const;

  // Publish methods
  bool publishState(const char* mode);  // "off" or "heat"
  bool publishTemperature(int16_t tempC);
  bool publishVoltage(uint16_t voltageMv);
  bool publishPower(uint16_t power);
  bool publishAvailability(bool available);
  bool publishStatus(const proto::StatusPayload& status);

  // Phase 6: Diagnostic sensors
  bool publishLoRaRSSI(int rssi);
  bool publishLoRaSNR(float snr);
  bool publishLastCommandSource(const char* source);  // "lora", "mqtt", "button"
  bool publishWBusHealth(bool healthy, const char* details = "");
  bool publishDiagnostics(int loraRssi, float loraSNR, const char* cmdSource, bool wbusHealthy);

  // HomeAssistant discovery
  bool publishDiscovery();
  bool publishDiagnosticDiscovery();  // Phase 6: Discover diagnostic sensors
  
  #ifdef MQTT_ENABLE_OTA
  // Phase 7: OTA status publishing
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
  
  uint32_t lastReconnectMs_;
  uint32_t reconnectIntervalMs_;
  bool discoveryPublished_;
  
  MQTTCommandCallback cmdCallback_;
  
  #ifdef MQTT_ENABLE_OTA
  OTAUpdater* otaUpdater_;  // Phase 7: OTA updater reference
  #endif

  // Connection management
  void tryConnect();
  void subscribe();

  // Message handlers
  static void messageCallback(char* topic, byte* payload, unsigned int length);
  void handleMessage(const char* topic, const byte* payload, unsigned int length);
  
  // Command parsing
  bool parseCommand(const char* payload, unsigned int length, MQTTCommand& cmd);
  bool isCommandFresh(uint32_t timestampSec);

  // Topic helpers
  String getModeCmdTopic();
  String getModeStateTopic();
  String getMinutesCmdTopic();
  String getTempStateTopic();
  String getVoltageStateTopic();
  String getPowerStateTopic();
  String getAvailabilityTopic();
  String getDiscoveryTopic();
  
  // Phase 6: Diagnostic topic helpers
  String getLoRaRSSITopic();
  String getLoRaSNRTopic();
  String getLastCmdSourceTopic();
  String getWBusHealthTopic();

  static MQTTClient* instance_;
};
