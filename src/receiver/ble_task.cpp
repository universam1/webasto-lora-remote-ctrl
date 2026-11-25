// bleTask (core 0, prio 2, 100 ms): the only caller of BleServer::service() and notifyResult().
// The NimBLE host callbacks only push into cmdQueue and an internal ring (see ble_server.h).
#include "receiver_ctx.h"

#if defined(ENABLE_BLE) && ENABLE_BLE

#include <Arduino.h>

#include "ble_server.h"
#include "can_monitor.h"
#include "project_config.h"
#include "status_codec.h"
#include "task_utils.h"

#ifndef FW_VERSION
#define FW_VERSION "phase-d"
#endif
#ifndef BLE_PASSKEY
#define BLE_PASSKEY BLE_DEFAULT_PASSKEY
#endif

namespace rx {
namespace {

constexpr uint32_t kPeriodMs = 100;
constexpr uint32_t kStackBytes = 4096;

BleServer g_ble;

// Called from service() inside bleTask: reads the latest SystemSnapshot, never blocks.
bool provideStatus(uint8_t out[ble::kStatusLen]) {
  wh::SystemSnapshot s;
  if (!ctx().snapshot.read(s)) return false;
  const wh::HeaterSnapshot& h = s.heater;

  wh::DecodedStatus d;
  d.phase = h.phase;
  d.busAwake = h.busAwake;
  d.climateActive = s.climateActive;
  d.interiorValid = s.interiorValid;
  d.transportHealthy = h.healthy;
  d.ignitionOn = h.ignitionOn.seen && h.ignitionOn.v;
  d.errorCode = h.errorCode;
  d.errorStored = h.errorStored;
  d.assumed = h.phaseAssumed;
  d.climateStatus = s.climateStatus;
  d.powerW = wh::heatingPowerW(h.powerRaw);
  d.fuelUsedMl = h.fuelUsedMl;
  d.fuelRate_mlph = wh::fuelRate_mlph(h.powerRaw);
  d.coolant_dC = h.coolant_dC;
  d.interior_dC = s.interiorValid ? s.interior_dC : wh::kUnknownDC;
  d.outside_dC = h.outside_dC.seen ? h.outside_dC.v : wh::kUnknownDC;
  d.voltage_mV = h.voltage_mV.seen ? h.voltage_mV.v : 0;
  d.odometer_km = h.odometer_km.seen ? h.odometer_km.v : 0xFFFFFFFFu;
  d.climateTargetC = s.climateActive ? s.climateTargetC : 0;

  wh::BleExtra x;
  x.engineCoolant_dC = h.engineCoolant_dC.seen ? h.engineCoolant_dC.v : wh::kUnknownDC;
  x.durationMin = s.climateDurationMin;
  x.remainingMin = s.climateRemainingMin;
  x.lastCmdId = s.lastCmdId;
  x.lastCmdResult = (uint8_t)s.lastCmdResult;
  x.kl30Lost = h.kl30Lost;
  wh::packBleStatus(d, x, out);
  return true;
}

void bleTaskFn(void*) {
  Ctx& c = ctx();
  rt::LoopTimer timer(kPeriodMs);
  for (;;) {
    const uint32_t now = millis();
    if (c.blePairRequest.exchange(false)) g_ble.openPairingWindow(now);
    if (c.bleClearRequest.exchange(false)) g_ble.clearBonds();
    g_ble.service(now);
    rt::CmdEvent ev;
    while (c.bleEvents.pop(ev)) g_ble.notifyResult(ev);
    c.bleConnected.store(g_ble.connected());
    timer.waitNext();
  }
}

}  // namespace

void startBleTask() {
#if defined(HEATER_TRANSPORT_WBUS) && HEATER_TRANSPORT_WBUS
  const uint8_t transport = ble::TransportWbus;
#else
  const uint8_t transport = ble::TransportCan;
#endif
  ctx().bleDefaultPasskey.store((uint32_t)BLE_PASSKEY == (uint32_t)BLE_DEFAULT_PASSKEY);
  if (!g_ble.begin(ctx().cmdQueue, ble::RoleReceiver, transport, FW_VERSION, "Webasto-RX",
                   (uint32_t)BLE_PASSKEY)) {
    Serial.println("[setup] BLE begin failed");
    return;
  }
  g_ble.setStatusProvider(provideStatus);
  rt::TaskSpec spec = {"bleTask", bleTaskFn, kStackBytes, 2, 0, nullptr};
  rt::startTask(spec);
}

}  // namespace rx

#endif  // ENABLE_BLE
