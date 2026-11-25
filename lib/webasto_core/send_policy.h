#pragma once
// Pure decision for the receiver's LoRa status sending. No Arduino includes.
//
// Semantics:
//  - The receiver only transmits while a sender is "active": the last valid sender packet
//    (command or query, after CRC and decryption) is younger than senderActiveMs (30 min).
//  - The caller sets everRequested/lastSenderRequestMs on every valid sender packet BEFORE
//    calling, so the ACK to a just received command is always inside the window.
//  - !everRequested (no sender packet since boot): never send, except forceNow (ACK/Query reply).
//  - Window expired (now - lastSenderRequestMs >= senderActiveMs): never send, not even events
//    or forceNow. A valid sender packet reopens the window.
//  - Inside the window: forceNow sends at once and ignores the other limits. Events (state
//    change, error, climate event) send once minGapMs have passed since the last send.
//    Periodic: runningMs while heaterRunning (the caller also sets it while the climate
//    controller is Starting), climateMs while climateActive and not running, else idleMs.
//    If nothing was ever sent, the first periodic send is allowed at once.
//  - All time arithmetic is wrap safe for 32 bit millisecond clocks.
#include <stdint.h>

namespace wh {

struct SendPolicyParams {
  uint32_t senderActiveMs = 1800000;
  uint32_t runningMs = 15000;
  uint32_t climateMs = 30000;
  uint32_t idleMs = 120000;
  uint32_t minGapMs = 5000;
};

enum class SendReason : uint8_t { None, Periodic, Event };

struct SendInputs {
  uint32_t now = 0;
  bool everRequested = false;
  uint32_t lastSenderRequestMs = 0;
  uint32_t lastTxMs = 0;
  bool everSent = false;
  bool heaterRunning = false;
  bool climateActive = false;
  bool eventPending = false;
  bool forceNow = false;
};

inline SendReason shouldSendStatus(const SendInputs& in, const SendPolicyParams& p = SendPolicyParams()) {
  if (!in.everRequested) return in.forceNow ? SendReason::Event : SendReason::None;
  if ((uint32_t)(in.now - in.lastSenderRequestMs) >= p.senderActiveMs) return SendReason::None;
  if (in.forceNow) return SendReason::Event;
  const uint32_t sinceTx = (uint32_t)(in.now - in.lastTxMs);
  if (in.eventPending && (!in.everSent || sinceTx >= p.minGapMs)) return SendReason::Event;
  if (!in.everSent) return SendReason::Periodic;
  const uint32_t interval = in.heaterRunning ? p.runningMs : (in.climateActive ? p.climateMs : p.idleMs);
  return sinceTx >= interval ? SendReason::Periodic : SendReason::None;
}

}  // namespace wh
