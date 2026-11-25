#pragma once
// Home Assistant MQTT discovery: topic names and the entity table. Builds the payloads with
// ArduinoJson and hands each one to an emit callback, so it does not depend on PubSubClient
// and can be size-checked on the host. Needs MQTT_TOPIC_BASE, MQTT_DISCOVERY_PREFIX and
// MQTT_CLIENT_ID (project_config.h / credentials.h) to be defined before inclusion.
#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- topics (string literals, concatenated at compile time) ----------------------------
#define MQTT_T_AVAILABILITY MQTT_TOPIC_BASE "/availability"
#define MQTT_T_STATE MQTT_TOPIC_BASE "/state"
#define MQTT_T_LAST_CMD MQTT_TOPIC_BASE "/last_cmd/state"
#define MQTT_T_MODE_SET MQTT_TOPIC_BASE "/mode/set"
#define MQTT_T_MODE_STATE MQTT_TOPIC_BASE "/mode/state"
#define MQTT_T_MINUTES_SET MQTT_TOPIC_BASE "/minutes/set"
#define MQTT_T_QUERY MQTT_TOPIC_BASE "/query"
#define MQTT_T_TEMP_STATE MQTT_TOPIC_BASE "/temperature/state"
#define MQTT_T_VOLTAGE_STATE MQTT_TOPIC_BASE "/voltage/state"
#define MQTT_T_POWER_STATE MQTT_TOPIC_BASE "/power/state"
#define MQTT_T_TARGET_SET MQTT_TOPIC_BASE "/climate/target/set"
#define MQTT_T_DURATION_SET MQTT_TOPIC_BASE "/climate/duration/set"
#define MQTT_T_ENABLE_SET MQTT_TOPIC_BASE "/climate/enable/set"
#define MQTT_T_LORA_RSSI MQTT_TOPIC_BASE "/lora_rssi/state"
#define MQTT_T_LORA_SNR MQTT_TOPIC_BASE "/lora_snr/state"
#define MQTT_T_LAST_CMD_SOURCE MQTT_TOPIC_BASE "/last_cmd_source/state"
#define MQTT_T_WBUS_HEALTH MQTT_TOPIC_BASE "/wbus_health/state"
#define MQTT_T_BUS_HEALTH_ALIAS MQTT_TOPIC_BASE "/bus_health/state"

namespace mqttd {

// Called once per entity with the discovery topic and the serialized JSON.
typedef bool (*EmitFn)(void* ctx, const char* topic, const char* payload);

// JSON value template for a field of the state document; missing field -> unknown.
#define MQTT_VT(field) "{{ value_json." field " if value_json." field " is defined else none }}"

struct SensorDef {
  const char* key;          // unique_id = <MQTT_CLIENT_ID>_<key>
  const char* name;
  const char* stateTopic;
  const char* deviceClass;  // nullable
  const char* unit;         // nullable
  const char* stateClass;   // nullable
  const char* valueTemplate;  // nullable
  const char* entityCategory;  // nullable ("diagnostic")
  const char* icon;         // nullable
  const char* options;      // nullable, comma separated, for device_class enum
  bool attributesFromState; // json_attributes_topic = stateTopic
  bool diagnosticGroup;     // only with MQTT_ENABLE_DIAGNOSTIC_SENSORS
};

// clang-format off
static const SensorDef kSensors[] = {
  {"heater_state",  "Heater state",   MQTT_T_STATE, "enum",        nullptr, nullptr,          MQTT_VT("phase"),     nullptr,      "mdi:fire", "off,heating,run_on,ventilating,error", false, false},
  {"heating_power", "Heating power",  MQTT_T_STATE, "power",       "W",     "measurement",    MQTT_VT("power_w"),   nullptr,      nullptr, nullptr, false, false},
  {"fuel_rate",     "Fuel rate",      MQTT_T_STATE, "volume_flow_rate", "L/h", "measurement", MQTT_VT("fuel_lph"),  nullptr,      "mdi:gas-station", nullptr, false, false},
  {"fuel_used",     "Fuel used",      MQTT_T_STATE, "volume",      "mL",    "total_increasing", MQTT_VT("fuel_ml"), nullptr,      nullptr, nullptr, false, false},
  {"coolant",       "Coolant temperature", MQTT_T_STATE, "temperature", "\xC2\xB0""C", "measurement", MQTT_VT("coolant"),  nullptr, nullptr, nullptr, false, false},
  {"interior",      "Interior temperature", MQTT_T_STATE, "temperature", "\xC2\xB0""C", "measurement", MQTT_VT("interior"), nullptr, nullptr, nullptr, false, false},
  {"outside",       "Outside temperature",  MQTT_T_STATE, "temperature", "\xC2\xB0""C", "measurement", MQTT_VT("outside"),  nullptr, nullptr, nullptr, false, false},
  {"voltage",       "Board voltage",  MQTT_T_STATE, "voltage",     "V",     "measurement",    MQTT_VT("voltage"),   nullptr,      nullptr, nullptr, false, false},
  {"odometer",      "Odometer",       MQTT_T_STATE, "distance",    "km",    "total_increasing", MQTT_VT("odometer"), nullptr,     nullptr, nullptr, false, false},
  {"error",         "Heater error",   MQTT_T_STATE, nullptr,       nullptr, nullptr,          MQTT_VT("error"),     "diagnostic", "mdi:alert-circle-outline", nullptr, false, false},
  // Existing diagnostic sensors: unique ids and state topics unchanged.
  {"lora_rssi",       "LoRa RSSI",           MQTT_T_LORA_RSSI,       "signal_strength", "dBm", nullptr, nullptr, "diagnostic", nullptr, nullptr, false, true},
  {"lora_snr",        "LoRa SNR",            MQTT_T_LORA_SNR,        nullptr,           "dB",  nullptr, nullptr, "diagnostic", nullptr, nullptr, false, true},
  {"last_cmd_source", "Last Command Source", MQTT_T_LAST_CMD_SOURCE, nullptr, nullptr, nullptr, nullptr, "diagnostic", "mdi:source-branch", nullptr, false, true},
  {"wbus_health",     "Heater bus health",   MQTT_T_WBUS_HEALTH,     nullptr, nullptr, nullptr, "{{ value_json.status }}", "diagnostic", "mdi:heart-pulse", nullptr, true, true},
};
// clang-format on

inline void addDevice(JsonDocument& doc) {
  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"].add(MQTT_CLIENT_ID);
  device["name"] = "Webasto ThermoTop C";
  device["manufacturer"] = "Custom";
  device["model"] = "TTGO LoRa32 + W-BUS";
  device["sw_version"] = "1.0.0";
}

// Common part of every entity: device block, availability, unique id. Serializes and emits.
// objectId: discovery topic segment, uniqueId: unique_id.
inline bool emitEntity(EmitFn emit, void* ctx, const char* component, const char* objectId,
                       const char* uniqueId, JsonDocument& doc) {
  doc["unique_id"] = uniqueId;
  doc["availability_topic"] = MQTT_T_AVAILABILITY;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
  addDevice(doc);

  char topic[128];
  snprintf(topic, sizeof topic, "%s/%s/%s/config", MQTT_DISCOVERY_PREFIX, component, objectId);
  const size_t n = measureJson(doc);
  char* buf = (char*)malloc(n + 1);
  if (!buf) return false;
  serializeJson(doc, buf, n + 1);
  const bool ok = emit(ctx, topic, buf);
  free(buf);
  return ok;
}

inline bool emitKeyed(EmitFn emit, void* ctx, const char* component, const char* key,
                      JsonDocument& doc) {
  char id[64];
  snprintf(id, sizeof id, "%s_%s", MQTT_CLIENT_ID, key);
  return emitEntity(emit, ctx, component, id, id, doc);
}

// The one helper for sensor entities.
inline bool publishSensorDiscovery(EmitFn emit, void* ctx, const SensorDef& s) {
  JsonDocument doc;
  doc["name"] = s.name;
  doc["state_topic"] = s.stateTopic;
  if (s.deviceClass) doc["device_class"] = s.deviceClass;
  if (s.unit) doc["unit_of_measurement"] = s.unit;
  if (s.stateClass) doc["state_class"] = s.stateClass;
  if (s.valueTemplate) doc["value_template"] = s.valueTemplate;
  if (s.attributesFromState) doc["json_attributes_topic"] = s.stateTopic;
  if (s.entityCategory) doc["entity_category"] = s.entityCategory;
  if (s.icon) doc["icon"] = s.icon;
  if (s.options) {
    JsonArray arr = doc["options"].to<JsonArray>();
    const char* p = s.options;
    while (*p) {
      const char* e = strchr(p, ',');
      const size_t len = e ? (size_t)(e - p) : strlen(p);
      char tmp[24];
      snprintf(tmp, sizeof tmp, "%.*s", (int)len, p);
      arr.add(tmp);  // copied into the document
      p += len;
      if (*p == ',') p++;
    }
  }
  return emitKeyed(emit, ctx, "sensor", s.key, doc);
}

inline bool publishClimateDiscovery(EmitFn emit, void* ctx) {
  JsonDocument doc;
  JsonObject origin = doc["origin"].to<JsonObject>();
  origin["name"] = "Webasto LoRa Controller";
  origin["sw"] = "1.0.0";
  origin["url"] = "https://github.com/yourusername/webastolora";
  doc["name"] = "Webasto Heater";
  doc["modes"].add("off");
  doc["modes"].add("heat");
  doc["mode_command_topic"] = MQTT_T_MODE_SET;
  doc["mode_state_topic"] = MQTT_T_MODE_STATE;
  doc["temperature_command_topic"] = MQTT_T_TARGET_SET;
  doc["temperature_state_topic"] = MQTT_T_STATE;
  doc["temperature_state_template"] = MQTT_VT("target");
  doc["current_temperature_topic"] = MQTT_T_TEMP_STATE;
  doc["min_temp"] = 10;
  doc["max_temp"] = 24;
  doc["temp_step"] = 1;
  doc["temperature_unit"] = "C";
  // Existing unique id and discovery topic stay unchanged.
  return emitEntity(emit, ctx, "climate", MQTT_CLIENT_ID, "webasto_ttgo_receiver", doc);
}

inline bool publishControlDiscovery(EmitFn emit, void* ctx) {
  bool ok = true;
  {
    JsonDocument doc;
    doc["name"] = "Bus awake";
    doc["state_topic"] = MQTT_T_STATE;
    doc["value_template"] = "{{ 'ON' if value_json.bus_awake else 'OFF' }}";
    doc["entity_category"] = "diagnostic";
    ok &= emitKeyed(emit, ctx, "binary_sensor", "bus_awake", doc);
  }
  {
    JsonDocument doc;
    doc["name"] = "Climate duration";
    doc["command_topic"] = MQTT_T_DURATION_SET;
    doc["state_topic"] = MQTT_T_STATE;
    doc["value_template"] = MQTT_VT("duration");
    doc["min"] = 10;
    doc["max"] = 480;
    doc["step"] = 10;
    doc["unit_of_measurement"] = "min";
    doc["icon"] = "mdi:timer-outline";
    ok &= emitKeyed(emit, ctx, "number", "climate_duration", doc);
  }
  {
    JsonDocument doc;
    doc["name"] = "Climate automation";
    doc["command_topic"] = MQTT_T_ENABLE_SET;
    doc["state_topic"] = MQTT_T_STATE;
    doc["value_template"] = "{{ 'ON' if value_json.climate_active else 'OFF' }}";
    doc["payload_on"] = "ON";
    doc["payload_off"] = "OFF";
    doc["icon"] = "mdi:thermostat-auto";
    ok &= emitKeyed(emit, ctx, "switch", "climate_enable", doc);
  }
  {
    JsonDocument doc;
    doc["name"] = "Query status";
    doc["command_topic"] = MQTT_T_QUERY;
    doc["payload_press"] = "PRESS";
    doc["icon"] = "mdi:refresh";
    ok &= emitKeyed(emit, ctx, "button", "query", doc);
  }
  return ok;
}

// All entities. Stops at the first emit failure.
inline bool publishAllDiscovery(EmitFn emit, void* ctx, bool withDiagnosticSensors) {
  if (!publishClimateDiscovery(emit, ctx)) return false;
  for (const SensorDef& s : kSensors) {
    if (s.diagnosticGroup && !withDiagnosticSensors) continue;
    if (!publishSensorDiscovery(emit, ctx, s)) return false;
  }
  return publishControlDiscovery(emit, ctx);
}

}  // namespace mqttd
