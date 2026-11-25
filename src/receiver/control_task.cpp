// ctrlTask (core 1, prio 3, 100 ms): the only task that touches OneWire (DS18B20) and NVS.
// It owns the climate controller and the command router, reads the heater snapshot and writes
// the SystemSnapshot that every other task reads.
#include <Arduino.h>
#include <Preferences.h>

#include "command_router.h"
#include "climate_controller.h"
#include "ds18b20_sensor.h"
#include "heater_iface.h"
#include "project_config.h"
#include "receiver_ctx.h"
#include "task_utils.h"

namespace rx {
namespace {

constexpr uint32_t kPeriodMs = 100;
constexpr uint32_t kVoltageMaxAgeMs = 600000;      // 10 min
constexpr uint32_t kRemainingSaveMs = 15UL * 60 * 1000;
constexpr uint32_t kStackBytes = 6144;

// Result of the last finished command, for the ACK status (LoRa) and the BLE status.
struct LastCmd {
  uint8_t id = 0;
  wh::CmdResult result = wh::CmdResult::Ok;
};

class Sink : public EventSink {
 public:
  void emit(const rt::CmdEvent& ev) override {
    Ctx& c = ctx();
    if (ev.stage >= 3) {
      lastCmd.id = ev.tag;
      lastCmd.result = (wh::CmdResult)ev.result;
      c.lastCmdSource.store((uint8_t)ev.src);
    }
    // Every event goes to the queue of its own source only. A full queue drops the event: the
    // sender repeats its command and the BLE client times out after 25 s.
    switch (ev.src) {
      case rt::Source::Lora:
        c.loraEvents.push(ev);
        break;
      case rt::Source::Ble:
        c.bleEvents.push(ev);
        break;
      case rt::Source::Mqtt:
        c.netEvents.push(ev);
        break;
      default:
        break;
    }
  }
  void forceStatus() override { ctx().forceStatus.store(true); }
  LastCmd lastCmd;
};

Sink g_sink;
wh::ClimateController g_climate;
Ds18b20 g_ds;
Preferences g_prefs;

void loadClimate(uint32_t now) {
  wh::ClimateSettings s;
  g_prefs.begin("climate", true);
  s.targetC = g_prefs.getUChar("target", 20);
  s.durationMin = g_prefs.getUShort("duration", 120);
  const bool active = g_prefs.getBool("active", false);
  const uint16_t remaining = g_prefs.getUShort("remaining", 0);
  g_prefs.end();
  g_climate.restore(s, active, remaining, now);
  Serial.printf("[ctrl] climate restored: target=%u duration=%u active=%d remaining=%u\n", s.targetC,
                s.durationMin, active, remaining);
}

void saveClimate(uint32_t now) {
  const wh::ClimateSettings s = g_climate.settings();
  g_prefs.begin("climate", false);
  g_prefs.putUChar("target", s.targetC);
  g_prefs.putUShort("duration", s.durationMin);
  g_prefs.putBool("active", g_climate.enabled());
  g_prefs.putUShort("remaining", g_climate.remainingMin(now));
  g_prefs.end();
}

void controlTaskFn(void*) {
  Ctx& c = ctx();
  heater::IHeater& h = heater::instance();
  CommandRouter router(g_climate, h, g_sink);
  rt::LoopTimer timer(kPeriodMs);

  g_ds.begin();
  uint32_t now = millis();
  loadClimate(now);
  uint32_t lastSave = now;
  bool lastEnabled = g_climate.enabled();

  for (;;) {
    now = millis();
    const wh::HeaterSnapshot hs = h.snapshot();
    g_ds.step(now);

    // 1. Commands from all sources, then the retry of a parked request.
    rt::Command cmd;
    while (c.cmdQueue.pop(cmd)) {
      c.lastCmdSource.store((uint8_t)cmd.src);
      router.route(cmd, now);
    }
    router.service(now);

    // 2. Results from the heater.
    heater::HeaterEvent hev;
    while (h.popEvent(hev)) router.onHeaterEvent(hev, now);

    // 3. Climate controller.
    wh::ClimateInputs in;
    in.now = now;
    in.interiorValid = g_ds.valid();
    in.interior_dC = g_ds.temp_dC();
    in.phase = hs.phase;
    in.phaseTrusted = hs.phase != wh::HeaterPhase::Unknown && !hs.phaseAssumed;
    in.errorCode = hs.errorCode;
    in.voltageValid = hs.voltage_mV.fresh(now, kVoltageMaxAgeMs);
    in.voltage_mV = hs.voltage_mV.v;
    in.heaterBusy = h.busy();
    in.transportHealthy = hs.healthy;
    const wh::ClimateOutput out = g_climate.step(in);
    if (out.requestStart) router.startClimate(now);

    // 4. Persist: on every settings/active change, and the remaining time every 15 minutes.
    const bool enabled = g_climate.enabled();
    if (enabled != lastEnabled) {  // session ended by itself or by a command
      lastEnabled = enabled;
      saveClimate(now);
      lastSave = now;
    } else if (router.takePersist()) {
      saveClimate(now);
      lastSave = now;
    } else if (enabled && (uint32_t)(now - lastSave) >= kRemainingSaveMs) {
      saveClimate(now);
      lastSave = now;
    }

    // 5. System snapshot for everybody else.
    wh::SystemSnapshot s;
    s.heater = hs;
    s.interiorValid = g_ds.valid();
    s.interior_dC = g_ds.valid() ? g_ds.temp_dC() : wh::kUnknownDC;
    s.climateStatus = out.status;
    s.climateActive = enabled;
    s.climateTargetC = g_climate.settings().targetC;
    s.climateDurationMin = g_climate.settings().durationMin;
    s.climateRemainingMin = out.remainingMin;
    s.lastCmdId = g_sink.lastCmd.id;
    s.lastCmdResult = g_sink.lastCmd.result;
    s.senderActive = c.everRequested.load() &&
                     (uint32_t)(now - c.lastSenderRequestMs.load()) < (uint32_t)LORA_SENDER_ACTIVE_MS;
    s.nowMs = now;
    c.snapshot.write(s);

    timer.waitNext();
  }
}

}  // namespace

void startControlTask() {
  rt::TaskSpec spec = {"ctrlTask", controlTaskFn, kStackBytes, 3, 1, nullptr};
  rt::startTask(spec);
}

}  // namespace rx
