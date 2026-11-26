// bleTask of the sender (core 0, prio 2, 100 ms): the only caller of BleServer::service() and
// notifyResult(). See sender_ble.h for the data flow.
#include "sender_ble.h"

#if defined(ENABLE_BLE) && ENABLE_BLE

#include <Arduino.h>
#include <esp_mac.h>

#include "ble_server.h"
#include "project_config.h"
#include "sender_ble_map.h"
#include "task_stacks.h"
#include "task_utils.h"

#ifndef FW_VERSION
#define FW_VERSION "phase-d"
#endif
#ifndef BLE_PASSKEY
#define BLE_PASSKEY BLE_DEFAULT_PASSKEY
#endif

namespace sbl {
namespace {

constexpr uint32_t kPeriodMs = 100;

BleServer g_ble;
uint32_t g_staleAfterMs = 30000;

// Called from service() inside bleTask. Reads the latest snapshot, never blocks.
bool provideStatus(uint8_t out[ble::kStatusLen]) {
  Snap s;
  if (!shared().snap.read(s)) return false;
  const uint32_t now = millis();
  wh::buildSenderBleStatus(s.rxMs != 0, s.status, s.rxMs ? now - s.rxMs : 0, g_staleAfterMs,
                           s.lastCmdId, s.lastCmdResult, out);
  return true;
}

void bleTaskFn(void*) {
  Shared& c = shared();
  rt::LoopTimer timer(kPeriodMs);
  for (;;) {
    const uint32_t now = millis();
    if (c.pairRequest.exchange(false)) g_ble.openPairingWindow(now);
    if (c.clearRequest.exchange(false)) g_ble.clearBonds();
    g_ble.service(now);
    rt::CmdEvent ev;
    while (c.events.pop(ev)) g_ble.notifyResult(ev);
    c.connected.store(g_ble.connected());
    c.lastActivityMs.store(g_ble.lastActivityMs());
    timer.waitNext();
  }
}

}  // namespace

Shared& shared() {
  static Shared s;
  return s;
}

bool start(uint32_t staleAfterMs) {
  Shared& c = shared();
  g_staleAfterMs = staleAfterMs;
  if (!c.cmdQueue.valid() || !c.events.valid()) {
    Serial.println("[setup] BLE queues failed");
    return false;
  }
  c.defaultPasskey.store((uint32_t)BLE_PASSKEY == (uint32_t)BLE_DEFAULT_PASSKEY);
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  if (!ble::makeBleName(c.name, sizeof(c.name), ble::RoleSender, mac)) {
    Serial.println("[setup] BLE name failed");
    return false;
  }
  Serial.printf("[setup] BLE name: %s\n", c.name);
  if (c.defaultPasskey.load()) Serial.println("[setup] BLE: default passkey in use, set -D BLE_PASSKEY=...");
  // Transport "none": the sender has no heater transport, it relays over LoRa.
  if (!g_ble.begin(c.cmdQueue, ble::RoleSender, ble::TransportNone, FW_VERSION, c.name,
                   (uint32_t)BLE_PASSKEY)) {
    Serial.println("[setup] BLE begin failed");
    return false;
  }
  g_ble.setStatusProvider(provideStatus);
  rt::TaskSpec spec = {"bleTask", bleTaskFn, rt::kStackBleSender, 2, 0, nullptr};
  if (!rt::startTask(spec)) return false;
  c.ok.store(true);
  return true;
}

}  // namespace sbl

#endif  // ENABLE_BLE
