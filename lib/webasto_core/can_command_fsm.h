#pragma once
// Non-blocking port of CanSniffer.set_heater_mode (mcp-can-capture). Driven by onFrame() and tick().
#include <stdint.h>

#include "can_frames.h"
#include "heater_types.h"

namespace wh {

struct CanPort {
  virtual bool setNormalMode(bool on) = 0;  // false = listen-only
  virtual bool send(const Frame&) = 0;
  virtual ~CanPort() {}
};

struct FsmTimings {
  uint32_t slotMs = 500, panelFreshMs = 2000, panelWaitMs = 600, confirmMs = 1500, modeSwitchGuardMs = 50;
  uint8_t maxFrames = 10;
  uint32_t wakeListenMs = 3000;
};

class CanCommandFsm {
 public:
  CanCommandFsm(CanPort& port, const FsmTimings& t = FsmTimings());
  CmdResult request(CmdKind k, uint32_t now);  // Pending = accepted; Unsupported (Vent) and Busy at once
  void onFrame(const Frame& f, uint32_t now);
  void onBusEvent(bool busOff, uint32_t now);
  void tick(uint32_t now);
  bool busy() const { return state_ != State::Idle; }
  bool takeResult(CmdResult& r, uint8_t& framesSent);  // true exactly once per finished command

 private:
  enum class State : uint8_t { Idle, EnterTx, SendFirst, AwaitPanel, AwaitConfirm, WakeListen };
  void finish(CmdResult r);
  bool sendTarget(uint32_t now);

  CanPort& port_;
  FsmTimings t_;
  State state_ = State::Idle;
  CmdKind kind_ = CmdKind::Stop;
  uint8_t modeBits_ = 0, target_ = 0;
  bool bit7Known_ = false;
  uint8_t frames_ = 0;
  uint32_t enteredAt_ = 0, firstAt_ = 0, nextSend_ = 0, adoptedAt_ = 0, wakeEnd_ = 0;
  bool firstSent_ = false;
  // own copy of the roof unit's last 0x5D3 byte 0
  bool panelSeen_ = false;
  uint8_t panelByte_ = 0x80;
  uint32_t panelAt_ = 0;
  // result
  bool resultReady_ = false;
  CmdResult result_ = CmdResult::Pending;
  uint8_t resultFrames_ = 0;
};

}  // namespace wh
