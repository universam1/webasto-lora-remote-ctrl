#pragma once
// Command router (runs in ctrlTask only). Every source (menu, LoRa, MQTT, BLE, climate) reaches
// the heater through route(). Heat and Stop go to heater::submit(), a Stop also switches the
// climate session off. ClimateSet/ClimateOff go to the controller. Query only marks a status.
// A request that the heater cannot take right now (one request in flight) is parked and retried
// every tick for up to kRetryMs, so a Stop is not lost while a start is still running.
#include <stdint.h>

#include "climate_controller.h"
#include "commands.h"
#include "heater_iface.h"

namespace rx {

class EventSink {
 public:
  virtual void emit(const rt::CmdEvent& ev) = 0;  // result for the originating source
  virtual void forceStatus() = 0;                 // send a LoRa status now
  virtual ~EventSink() {}
};

class CommandRouter {
 public:
  static constexpr uint32_t kRetryMs = 10000;

  CommandRouter(wh::ClimateController& climate, heater::IHeater& heater, EventSink& sink)
      : climate_(climate), heater_(heater), sink_(sink) {}

  void route(const rt::Command& c, uint32_t now);
  // Retries the parked request. Call once per tick.
  void service(uint32_t now);
  // The heater finished a request.
  void onHeaterEvent(const heater::HeaterEvent& ev, uint32_t now);
  // The climate controller asked for a start.
  void startClimate(uint32_t now);

  // True once after the climate settings or the active flag changed (persist to NVS).
  bool takePersist() {
    const bool p = persist_;
    persist_ = false;
    return p;
  }

 private:
  void submitOrPark(const heater::HeaterRequest& r, uint32_t now);
  void emit(const heater::HeaterRequest& r, uint8_t result, uint8_t stage);

  wh::ClimateController& climate_;
  heater::IHeater& heater_;
  EventSink& sink_;
  bool persist_ = false;
  bool parked_ = false;
  heater::HeaterRequest parkedReq_;
  uint32_t parkedAt_ = 0;
};

}  // namespace rx
