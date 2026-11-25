#include "can_monitor.h"

namespace wh {

void CanMonitor::onFrame(const Frame& f, uint32_t now) {
  busSeen_ = true;
  snap_.lastBusFrameMs = now;
  snap_.busAwake = true;
  switch (f.id) {
    case kIdStatus: {
      Status3E5 s;
      if (!decodeStatus(f, s)) break;
      HeaterPhase p = derivePhase(s);
      if ((p == HeaterPhase::Heating && confirmed_ != HeaterPhase::Heating) ||
          (p == HeaterPhase::RunOn && !haveStart_))
        markStart(now);
      confirmed_ = p;
      statusSeen_ = true;
      snap_.errorCode = s.errorCode;
      snap_.errorStored = s.errorStored;
      snap_.kl30Lost = s.kl30Lost;
      snap_.fuelUsedMl = s.fuelMl;
      snap_.powerRaw = s.powerRaw;
      snap_.coolant_dC = s.coolant_dC;
      snap_.statusAtMs = now;
      snap_.phase = p;
      snap_.phaseAssumed = false;
      break;
    }
    case kIdMode:
      if (f.dlc >= 1) snap_.panelByte0.set(f.d[0], now);
      break;
    case kIdVoltage: {
      uint16_t v;
      if (decodeVoltage(f, v)) snap_.voltage_mV.set(v, now);
      break;
    }
    case kIdOdometer: {
      uint32_t km;
      if (decodeOdometer(f, km)) snap_.odometer_km.set(km, now);
      break;
    }
    case kIdIgnition: {
      bool on;
      if (decodeIgnition(f, on)) snap_.ignitionOn.set(on, now);
      break;
    }
    case kIdEngine: {
      int16_t t;
      if (decodeEngineCoolant(f, t)) snap_.engineCoolant_dC.set(t, now);
      break;
    }
    case kIdOutside: {
      int16_t t;
      if (decodeOutside(f, t)) snap_.outside_dC.set(t, now);
      break;
    }
    default:
      break;
  }
}

HeaterPhase CanMonitor::evalPhase(uint32_t now, bool& assumed) const {
  assumed = false;
  if (!statusSeen_) return HeaterPhase::Unknown;
  if ((uint32_t)(now - snap_.statusAtMs) <= kStatusValidMs) return confirmed_;
  if (confirmed_ == HeaterPhase::Heating || confirmed_ == HeaterPhase::RunOn) {
    if (!haveStart_ || (uint32_t)(now - lastStart_) > kAssumedMaxMs) return HeaterPhase::Unknown;
  }
  assumed = true;
  return confirmed_;
}

void CanMonitor::tick(uint32_t now) {
  snap_.busAwake = busSeen_ && (uint32_t)(now - snap_.lastBusFrameMs) < kBusAwakeMs;
  bool assumed;
  snap_.phase = evalPhase(now, assumed);
  snap_.phaseAssumed = assumed;
}

void CanMonitor::noteCommandResult(CmdKind k, CmdResult r, uint32_t now) {
  if (k == CmdKind::Heat && (r == CmdResult::Ok || r == CmdResult::OkUnconfirmed)) markStart(now);
}

bool CanMonitor::wantWake(uint32_t now) const {
  if (!enableWakePoll) return false;
  bool assumed;
  HeaterPhase p = evalPhase(now, assumed);
  if (p != HeaterPhase::Heating && p != HeaterPhase::RunOn) return false;
  if (busSeen_ && (uint32_t)(now - snap_.lastBusFrameMs) <= kWakeBusAsleepMs) return false;
  if (haveWake_ && (uint32_t)(now - lastWake_) < (uint32_t)CAN_WAKE_INTERVAL_MS) return false;
  if (!snap_.panelByte0.fresh(now, kPanelMaxAgeMs)) return false;
  if ((snap_.panelByte0.v & 0x7F) == 0) return false;
  return true;
}

}  // namespace wh
