#include "can_command_fsm.h"

namespace wh {

namespace {
inline bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
inline uint32_t laterOf(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0 ? a : b; }
inline uint32_t earlierOf(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0 ? a : b; }
}  // namespace

CanCommandFsm::CanCommandFsm(CanPort& port, const FsmTimings& t) : port_(port), t_(t) {}

CmdResult CanCommandFsm::request(CmdKind k, uint32_t now) {
  if (k == CmdKind::Vent) return CmdResult::Unsupported;
  if (state_ != State::Idle) return CmdResult::Busy;
  resultReady_ = false;
  kind_ = k;
  frames_ = 0;
  firstSent_ = false;
  bit7Known_ = false;
  enteredAt_ = now;
  state_ = State::EnterTx;
  return CmdResult::Pending;
}

void CanCommandFsm::finish(CmdResult r) {
  port_.setNormalMode(false);  // always back to listen-only
  result_ = r;
  resultFrames_ = frames_;
  resultReady_ = true;
  state_ = State::Idle;
}

bool CanCommandFsm::takeResult(CmdResult& r, uint8_t& framesSent) {
  if (!resultReady_) return false;
  resultReady_ = false;
  r = result_;
  framesSent = resultFrames_;
  return true;
}

bool CanCommandFsm::sendTarget(uint32_t now) {
  bool ok = port_.send(make5D3(target_));
  frames_++;
  if (!firstSent_) {
    if (!ok) return false;
    firstSent_ = true;
    firstAt_ = now;
    // sleeping bus: the panel value is only known after the wake frame
    nextSend_ = now + (bit7Known_ ? t_.slotMs : (t_.slotMs > t_.panelWaitMs ? t_.slotMs : t_.panelWaitMs));
  } else {
    nextSend_ = now + t_.slotMs;
  }
  return true;
}

void CanCommandFsm::onBusEvent(bool busOff, uint32_t) {
  if (busOff && state_ != State::Idle) finish(CmdResult::BusOff);
}

void CanCommandFsm::onFrame(const Frame& f, uint32_t now) {
  if (f.id == kIdMode && f.dlc >= 1) {
    uint8_t b0 = f.d[0];
    panelSeen_ = true;
    panelByte_ = b0;
    panelAt_ = now;
    if (state_ == State::AwaitPanel && firstSent_) {
      if (!bit7Known_ || (b0 & 0x80) != (target_ & 0x80)) {
        // adopt the panel's bit 7 and send the corrected frame at the next opportunity
        bit7Known_ = true;
        target_ = (uint8_t)(modeBits_ | (b0 & 0x80));
        nextSend_ = earlierOf(nextSend_, laterOf(now, firstAt_ + t_.slotMs));
      }
      if (b0 == target_) {
        adoptedAt_ = now;
        state_ = State::AwaitConfirm;
      }
    }
  } else if (f.id == kIdStatus && state_ == State::AwaitConfirm) {
    Status3E5 s;
    if (!decodeStatus(f, s)) return;
    bool ok = kind_ == CmdKind::Heat ? s.controlLed : (!s.controlLed && !s.ventActive);
    if (ok) finish(CmdResult::Ok);
  }
}

void CanCommandFsm::tick(uint32_t now) {
  switch (state_) {
    case State::Idle:
      break;
    case State::EnterTx:
      if (!port_.setNormalMode(true)) {
        finish(CmdResult::NoBus);
        break;
      }
      enteredAt_ = now;
      state_ = State::SendFirst;
      break;
    case State::SendFirst: {
      if ((uint32_t)(now - enteredAt_) < t_.modeSwitchGuardMs) break;
      if (kind_ == CmdKind::Wake) {
        target_ = panelSeen_ ? panelByte_ : 0x80;
        if (!sendTarget(now)) {
          finish(CmdResult::NoBus);
          break;
        }
        wakeEnd_ = now + t_.wakeListenMs;
        state_ = State::WakeListen;
        break;
      }
      modeBits_ = kind_ == CmdKind::Heat ? kModeHeat : kModeOff;
      bool fresh = panelSeen_ && (uint32_t)(now - panelAt_) <= t_.panelFreshMs;
      uint8_t bit7 = panelSeen_ ? (panelByte_ & 0x80) : 0x80;
      bit7Known_ = fresh;
      target_ = (uint8_t)(modeBits_ | bit7);
      if (!sendTarget(now)) {
        finish(CmdResult::NoBus);
        break;
      }
      state_ = State::AwaitPanel;
      break;
    }
    case State::AwaitPanel:
      if (!reached(now, nextSend_)) break;
      if (frames_ < t_.maxFrames) {
        sendTarget(now);
      } else {
        finish(CmdResult::NotAdopted);  // watched one full slot after the last frame
      }
      break;
    case State::AwaitConfirm:
      if ((uint32_t)(now - adoptedAt_) >= t_.confirmMs) finish(CmdResult::OkUnconfirmed);
      break;
    case State::WakeListen:
      if (reached(now, wakeEnd_)) finish(CmdResult::Ok);
      break;
  }
}

}  // namespace wh
