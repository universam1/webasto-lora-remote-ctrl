#include "net_task.h"

#ifdef ENABLE_MQTT_CONTROL

#include <Arduino.h>
#include <string.h>

#include "mqtt_client.h"
#include "mqtt_codec.h"
#include "project_config.h"
#include "task_utils.h"
#include "wifi_manager.h"

namespace {

constexpr uint32_t kPeriodMs = 50;
constexpr uint32_t kMinStateIntervalMs = 10000;  // on change, at most this often
constexpr uint32_t kMaxStateIntervalMs = 60000;  // at least this often
constexpr size_t kStateBuf = 512;

#ifndef MQTT_DIAGNOSTIC_INTERVAL_MS
#define MQTT_DIAGNOSTIC_INTERVAL_MS 60000
#endif

struct NetDiag {
  int32_t rssi;
  float snr;
  char src[16];
  bool healthy;
  bool valid;
};

struct NetCtx {
  rt::CommandQueue<rt::Command, 8>* cmdQueue;
  rt::CommandQueue<rt::CmdEvent, 8>* eventQueue;
  const rt::SnapshotBuffer<wh::SystemSnapshot>* snapshot;
};

NetCtx g_ctx;
WiFiManager g_wifi;
MQTTClient g_mqtt(g_wifi);
rt::SnapshotBuffer<NetDiag> g_diag;
volatile uint32_t g_dropped = 0;
volatile uint8_t g_linkState = 0;

void onCommand(const rt::Command& cmd) {
  if (g_ctx.cmdQueue && g_ctx.cmdQueue->push(cmd)) return;
  g_dropped = g_dropped + 1;
  Serial.printf("[net] command queue full, MQTT command type %u dropped (total %lu)\n",
                (unsigned)cmd.type, (unsigned long)g_dropped);
}

// True when 'now' is at or after 'deadline' (wrap safe).
inline bool reached(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

void netTaskFn(void*) {
  rt::LoopTimer timer(kPeriodMs);

  wh::SystemSnapshot snap;  // last good copy
  bool haveSnap = false;
  char lastJson[kStateBuf] = {0};
  uint32_t lastStateMs = 0;
  bool everPublished = false;
  bool forcePublish = false;
  uint32_t lastEpoch = g_mqtt.connectionEpoch();
  uint32_t lastDiagMs = 0;
  bool diagDue = true;

  for (;;) {
    const uint32_t now = millis();

    // 1. Newest snapshot (wait-free); keep the old copy when the read fails.
    if (g_ctx.snapshot && g_ctx.snapshot->read(snap)) haveSnap = true;
    // A duration set over MQTT while the climate is off is shown as state until the controller
    // takes it over.
    if (haveSnap && !snap.climateActive && g_mqtt.commandMapper().durationSet()) {
      snap.climateDurationMin = g_mqtt.commandMapper().duration();
    }
    g_mqtt.setSystemView(snap);

    // 2. Connection state machines and incoming commands (may block up to the 2 s socket timeout).
    g_wifi.update();
    g_mqtt.update();

    g_linkState = g_mqtt.isConnected() ? 2 : (g_wifi.isConnected() ? 1 : 0);

    if (g_mqtt.connectionEpoch() != lastEpoch) {
      lastEpoch = g_mqtt.connectionEpoch();
      forcePublish = true;
    }
    if (g_mqtt.takeRememberedFlag()) forcePublish = true;

    // 3. Command results.
    rt::CmdEvent ev;
    while (g_ctx.eventQueue && g_ctx.eventQueue->pop(ev)) {
      // Only terminal stages carry a final result.
      if (ev.stage >= 3) {
        g_mqtt.publishCommandResult(ev.src, ev.result, ev.tag);
        forcePublish = true;
      }
    }

    // 4. State publishing.
    if (haveSnap && g_mqtt.isConnected()) {
      char json[kStateBuf];
      if (mqttc::buildStateJson(snap, json, sizeof json) > 0) {
        const bool changed = strcmp(json, lastJson) != 0;
        const uint32_t age = now - lastStateMs;
        const bool due = !everPublished || forcePublish ||
                         (changed && age >= kMinStateIntervalMs) || age >= kMaxStateIntervalMs;
        if (due && g_mqtt.publishSnapshot(snap, json)) {
          strcpy(lastJson, json);
          lastStateMs = now;
          everPublished = true;
          forcePublish = false;
        }
      }
    } else if (!g_mqtt.isConnected()) {
      everPublished = false;  // publish at once after reconnect
    }

#ifdef MQTT_ENABLE_DIAGNOSTIC_SENSORS
    if (g_mqtt.isConnected() && (diagDue || reached(now, lastDiagMs + MQTT_DIAGNOSTIC_INTERVAL_MS))) {
      NetDiag d;
      if (g_diag.read(d) && d.valid) {
        if (g_mqtt.publishDiagnostics(d.rssi, d.snr, d.src, d.healthy, d.healthy ? "" : "transport")) {
          diagDue = false;
          lastDiagMs = now;
        }
      }
    }
#else
    (void)lastDiagMs;
    (void)diagDue;
#endif

    timer.waitNext();
  }
}

}  // namespace

bool netTaskStart(rt::CommandQueue<rt::Command, 8>& cmdQueue,
                  rt::CommandQueue<rt::CmdEvent, 8>& eventQueue,
                  const rt::SnapshotBuffer<wh::SystemSnapshot>& snapshot) {
  g_ctx = {&cmdQueue, &eventQueue, &snapshot};
  {
    // Network 0: WPA2-Enterprise (PEAP). Networks 1 and 2: WPA2-PSK (empty SSID = unused).
    WiFiManager::Network nets[wsel::kMaxNetworks];
    nets[0].ssid = MQTT_WIFI_SSID;
    nets[0].auth = wsel::Auth::Peap;
    nets[0].user = MQTT_WIFI_USERNAME;
    nets[0].password = MQTT_WIFI_PASSWORD;
    nets[0].anonymousId = MQTT_WIFI_ANONYMOUS_ID;
    nets[1].ssid = WIFI_PSK1_SSID;
    nets[1].password = WIFI_PSK1_PASSWORD;
    nets[2].ssid = WIFI_PSK2_SSID;
    nets[2].password = WIFI_PSK2_PASSWORD;
    g_wifi.begin(nets, wsel::kMaxNetworks);
  }
  g_mqtt.begin(MQTT_BROKER, MQTT_PORT, MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
  g_mqtt.setRtCommandCallback(onCommand);

  rt::TaskSpec spec = {"netTask", netTaskFn, 8192, 2, 0, nullptr};
  return rt::startTask(spec) != nullptr;
}

uint8_t netTaskLinkState() { return g_linkState; }

void netTaskSetDiagnostics(int loraRssi, float loraSnr, const char* cmdSource, bool transportHealthy) {
  NetDiag d;
  memset(&d, 0, sizeof d);
  d.rssi = loraRssi;
  d.snr = loraSnr;
  if (cmdSource) strncpy(d.src, cmdSource, sizeof(d.src) - 1);
  d.healthy = transportHealthy;
  d.valid = true;
  g_diag.write(d);
}

uint32_t netTaskDroppedCommands() { return g_dropped; }

#endif  // ENABLE_MQTT_CONTROL
