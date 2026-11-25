#include "climate_controller.h"

namespace wh {

static inline bool isRunning(HeaterPhase ph) { return ph == HeaterPhase::Heating || ph == HeaterPhase::RunOn; }

void ClimateController::configure(const ClimateSettings& s) {
  cfg_.targetC = s.targetC < 10 ? 10 : (s.targetC > 24 ? 24 : s.targetC);
  cfg_.durationMin = s.durationMin < 10 ? 10 : (s.durationMin > 480 ? 480 : s.durationMin);
}

void ClimateController::enable(uint32_t now) {
  enabled_ = true;
  sessionStart_ = now;
  latched_ = false;
  failures_ = 0;
  backoff_.on = false;
  external_.on = false;
  clearAttempt();
}

void ClimateController::disable() {
  enabled_ = false;
  clearAttempt();
}

void ClimateController::notifyUserStop(uint32_t now) {
  ourStop_ = true;
  haveOff_ = true;
  lastOff_ = now;
  disable();
}

void ClimateController::registerFailure(uint32_t now) {
  clearAttempt();
  if (failures_ < 255) failures_++;
  if (failures_ >= p_.maxFailures) {
    latched_ = true;
  } else {
    uint8_t i = failures_ - 1;
    if (i > 2) i = 2;
    backoff_.start(now, p_.backoffMs[i]);
  }
}

void ClimateController::notifyStartResult(uint32_t now, CmdResult r) {
  if (!pending_) return;
  switch (r) {
    case CmdResult::Ok:
    case CmdResult::OkUnconfirmed:
      pending_ = false;
      awaiting_ = true;
      awaitAt_ = now;
      break;
    case CmdResult::NotAdopted:
    case CmdResult::NoBus:
    case CmdResult::BusOff:
    case CmdResult::Timeout:
      registerFailure(now);
      break;
    case CmdResult::Pending:
      break;
    default:  // Unsupported, Busy: no failure, no start
      clearAttempt();
      break;
  }
}

uint16_t ClimateController::remainingMin(uint32_t now) const {
  if (!enabled_) return 0;
  const uint32_t dur = (uint32_t)cfg_.durationMin * 60000UL;
  const uint32_t el = (uint32_t)(now - sessionStart_);
  if (el >= dur) return 0;
  return (uint16_t)((dur - el + 59999UL) / 60000UL);
}

void ClimateController::restore(const ClimateSettings& s, bool active, uint16_t remMin, uint32_t now) {
  configure(s);
  disable();
  if (!active || remMin == 0) return;
  if (remMin > cfg_.durationMin) remMin = cfg_.durationMin;
  enable(now);
  sessionStart_ = now - ((uint32_t)cfg_.durationMin - remMin) * 60000UL;
}

ClimateOutput ClimateController::step(const ClimateInputs& in) {
  ClimateOutput out;
  const uint32_t now = in.now;
  const HeaterPhase ph = in.phase;
  const bool running = isRunning(ph);

  // Phase edge tracking, also while disabled.
  if (running) {
    if (!wasRunning_) {
      wasRunning_ = true;
      runStart_ = now;
      runStartKnown_ = seenOff_;
      ourStop_ = false;
    }
    if (ph == HeaterPhase::Heating) {
      failures_ = 0;
      clearAttempt();
      backoff_.on = false;
    }
  } else if (ph == HeaterPhase::Off) {
    if (wasRunning_) {
      wasRunning_ = false;
      haveOff_ = true;
      lastOff_ = now;
      if (enabled_ && !ourStop_ && runStartKnown_ && (uint32_t)(now - runStart_) < p_.externalOffMinRunMs)
        external_.start(now, p_.externalOffPauseMs);
      ourStop_ = false;
    }
    seenOff_ = true;
  } else if (ph != HeaterPhase::Unknown) {
    wasRunning_ = false;  // Error or Ventilating: no off edge
  }

  if (!enabled_) {
    out.status = ClimateStatus::Off;
    return out;
  }
  if ((uint32_t)(now - sessionStart_) >= (uint32_t)cfg_.durationMin * 60000UL) {
    disable();
    out.status = ClimateStatus::Off;
    return out;
  }
  out.remainingMin = remainingMin(now);

  // Attempt supervision.
  if (pending_ && (uint32_t)(now - pendingAt_) >= p_.startConfirmMs) registerFailure(now);
  if (awaiting_ && !running && (uint32_t)(now - awaitAt_) >= p_.startConfirmMs) registerFailure(now);
  if (in.errorCode != 0 && (pending_ || awaiting_)) {
    clearAttempt();
    latched_ = true;
  }

  // Sensor and voltage state.
  if (!in.interiorValid) {
    if (!sensorBad_) { sensorBad_ = true; sensorBadSince_ = now; }
  } else {
    sensorBad_ = false;
  }
  if (!in.voltageValid) lowVolt_ = false;
  else if (in.voltage_mV < p_.lowVoltage_mV) lowVolt_ = true;
  else if (in.voltage_mV > p_.lowVoltageRecover_mV) lowVolt_ = false;

  if (latched_) { out.status = ClimateStatus::Failed; return out; }
  if (running) { out.status = ClimateStatus::HeaterRunning; return out; }
  if (pending_ || awaiting_) { out.status = ClimateStatus::Starting; return out; }

  if (sensorBad_) {
    out.status = (uint32_t)(now - sensorBadSince_) >= p_.sensorInvalidGraceMs ? ClimateStatus::SensorFault
                                                                              : ClimateStatus::Idle;
    return out;
  }
  if (lowVolt_) { out.status = ClimateStatus::LowVoltage; return out; }

  const bool pauseOn = backoff_.active(now) || external_.active(now) ||
                       (haveOff_ && (uint32_t)(now - lastOff_) < p_.minPauseAfterOffMs);
  if (pauseOn) { out.status = ClimateStatus::Pause; return out; }

  const int target_dC = (int)cfg_.targetC * 10 - p_.hyst_dC;
  if (!(in.interior_dC < target_dC)) { out.status = ClimateStatus::Idle; return out; }

  if (ph == HeaterPhase::Off && in.phaseTrusted && in.errorCode == 0 && !in.heaterBusy && in.transportHealthy) {
    pending_ = true;
    pendingAt_ = now;
    out.requestStart = true;
    out.status = ClimateStatus::Starting;
  } else {
    out.status = ClimateStatus::Idle;
  }
  return out;
}

}  // namespace wh
