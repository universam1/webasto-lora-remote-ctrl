#pragma once
// Interior climate control. Pure step function, no Arduino includes, no heap, no blocking.
// It only ever requests a START of the heater. It never switches the heater off: at the end of
// the session it disables itself and leaves a running heater alone.
#include <stdint.h>
#include "heater_types.h"

#ifndef CLIMATE_MIN_PAUSE_MS
#define CLIMATE_MIN_PAUSE_MS 120000UL
#endif

namespace wh {

struct ClimateSettings {
  uint8_t targetC = 20;        // 10..24 whole degrees
  uint16_t durationMin = 120;  // 10..480
};

struct ClimateParams {
  uint32_t minPauseAfterOffMs = CLIMATE_MIN_PAUSE_MS;
  uint32_t externalOffPauseMs = 60UL * 60 * 1000;   // roof unit / user stopped the heater early
  uint32_t externalOffMinRunMs = 25UL * 60 * 1000;  // shorter run that we did not stop = early stop
  uint32_t startConfirmMs = 90UL * 1000;            // after Ok the heater must report Heating
  uint8_t maxFailures = 3;
  uint32_t backoffMs[3] = {2 * 60000UL, 5 * 60000UL, 10 * 60000UL};
  uint32_t sensorInvalidGraceMs = 5 * 60000UL;
  uint16_t lowVoltage_mV = 11500, lowVoltageRecover_mV = 11800;
  int16_t hyst_dC = 0;  // start when T < target*10 - hyst
};

struct ClimateInputs {
  uint32_t now = 0;
  bool interiorValid = false;
  int16_t interior_dC = kUnknownDC;
  HeaterPhase phase = HeaterPhase::Unknown;
  bool phaseTrusted = false;
  uint8_t errorCode = 0;
  bool voltageValid = false;  // false = unknown or stale: does not block
  uint16_t voltage_mV = 0;
  bool heaterBusy = false;
  bool transportHealthy = true;
};

struct ClimateOutput {
  bool requestStart = false;  // true for exactly one step per start attempt
  ClimateStatus status = ClimateStatus::Off;
  uint16_t remainingMin = 0;
};

class ClimateController {
 public:
  explicit ClimateController(const ClimateParams& p = ClimateParams()) : p_(p) {}
  void configure(const ClimateSettings& s);
  void enable(uint32_t now);
  void disable();
  void notifyStartResult(uint32_t now, CmdResult r);
  void notifyUserStop(uint32_t now);
  ClimateOutput step(const ClimateInputs& in);
  bool enabled() const { return enabled_; }
  ClimateSettings settings() const { return cfg_; }
  uint16_t remainingMin(uint32_t now) const;
  void restore(const ClimateSettings& s, bool active, uint16_t remainingMin, uint32_t now);

 private:
  struct Timer {
    uint32_t since = 0, dur = 0;
    bool on = false;
    void start(uint32_t now, uint32_t d) { since = now; dur = d; on = true; }
    bool active(uint32_t now) { if (on && (uint32_t)(now - since) >= dur) on = false; return on; }
  };
  void registerFailure(uint32_t now);
  void clearAttempt() { pending_ = false; awaiting_ = false; }

  ClimateParams p_;
  ClimateSettings cfg_;
  bool enabled_ = false;
  uint32_t sessionStart_ = 0;
  bool latched_ = false;
  uint8_t failures_ = 0;
  Timer backoff_, external_;
  bool pending_ = false;   // start requested, result not yet reported
  uint32_t pendingAt_ = 0;
  bool awaiting_ = false;  // result Ok, waiting for Heating
  uint32_t awaitAt_ = 0;
  // phase tracking (also while disabled)
  bool wasRunning_ = false;
  bool runStartKnown_ = false;
  uint32_t runStart_ = 0;
  bool seenOff_ = false;  // an Off phase was observed since boot (run start is then known)
  bool haveOff_ = false;
  uint32_t lastOff_ = 0;
  bool ourStop_ = false;
  bool sensorBad_ = false;
  uint32_t sensorBadSince_ = 0;
  bool lowVolt_ = false;
};

}  // namespace wh
