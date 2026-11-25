#pragma once
// Collects decoded CAN frames into a HeaterSnapshot and decides when a wake frame is needed.
#include <stdint.h>

#include "can_frames.h"
#include "heater_types.h"

#ifndef CAN_WAKE_INTERVAL_MS
#define CAN_WAKE_INTERVAL_MS 60000
#endif
#ifndef CAN_WAKE_POLL
#define CAN_WAKE_POLL 1
#endif

namespace wh {

constexpr uint32_t kVoltageMaxAgeMs = 600000;
constexpr uint32_t kOutsideMaxAgeMs = 1800000;
constexpr uint32_t kOdometerMaxAgeMs = 1800000;
constexpr uint32_t kEngineMaxAgeMs = 120000;
constexpr uint32_t kIgnitionMaxAgeMs = 120000;
constexpr uint32_t kBusAwakeMs = 1500;
constexpr uint32_t kStatusValidMs = 3000;
constexpr uint32_t kAssumedMaxMs = 35u * 60u * 1000u;
constexpr uint32_t kWakeBusAsleepMs = 2000;
constexpr uint32_t kPanelMaxAgeMs = 40u * 60u * 1000u;

class CanMonitor {
 public:
  void onFrame(const Frame& f, uint32_t now);
  void tick(uint32_t now);
  const HeaterSnapshot& snapshot() const { return snap_; }
  void noteCommandResult(CmdKind k, CmdResult r, uint32_t now);
  bool wantWake(uint32_t now) const;
  void noteWake(uint32_t now) { lastWake_ = now; haveWake_ = true; }

  bool enableWakePoll = (CAN_WAKE_POLL != 0);

 private:
  HeaterPhase evalPhase(uint32_t now, bool& assumed) const;
  void markStart(uint32_t now) { lastStart_ = now; haveStart_ = true; }

  HeaterSnapshot snap_;
  HeaterPhase confirmed_ = HeaterPhase::Unknown;
  bool statusSeen_ = false, busSeen_ = false, haveStart_ = false, haveWake_ = false;
  uint32_t lastStart_ = 0, lastWake_ = 0;
};

}  // namespace wh
