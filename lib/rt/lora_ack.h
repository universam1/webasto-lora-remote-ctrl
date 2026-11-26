#pragma once
// ACK correlation of the receiver's LoRa command path (loraTask only). Header only, no Arduino.
//
// The sender runs one job at a time and repeats it with the same seq until a status carries
// lastCmdSeq == seq. This class decides which result events may produce that ACK:
//  - only the result of the CURRENT pending seq counts; the result of an older seq (replaced by a
//    newer command) is ignored and counted, so lastProcessed never moves backwards;
//  - a pending command that gets no result within kPendingTimeoutMs is answered with a failure so
//    the sender can retry with a new seq (result events can be lost: full queue);
//  - "no command processed yet" is a validity flag, not a seq value: every 16 bit seq is valid.
//    On the wire that state is 0, which the sender never uses (it skips 0 on wrap).
#include <stdint.h>

namespace rt {

class LoraAckTracker {
 public:
  // At least the longest heater command (CAN adoption wait plus a parked Heat's kRetryMs of 10 s).
  static constexpr uint32_t kPendingTimeoutMs = 15000;

  enum class Rx : uint8_t {
    Running,  // same seq as the pending command: a repeat, ignore
    Done,     // same seq as the last finished command: answer with a status again
    New,      // anything else: map and push it, then call begin() (or finish() on failure)
  };

  Rx onPacket(uint16_t seq) const {
    if (pending_ && seq == pendingSeq_) return Rx::Running;
    if (haveLast_ && seq == last_) return Rx::Done;
    return Rx::New;
  }

  // The command was pushed to the ctrlTask. A previous pending command is forgotten (the sender
  // runs one job at a time); its late result will be ignored by onResult().
  void begin(uint16_t seq, uint32_t now) {
    pending_ = true;
    pendingSeq_ = seq;
    pendingAt_ = now;
  }

  // The command was answered without the ctrlTask (invalid arguments, queue full).
  void finish(uint16_t seq) {
    if (pending_ && seq == pendingSeq_) pending_ = false;
    last_ = seq;
    haveLast_ = true;
  }

  // A final result event arrived. true = it is the ACK of the pending command.
  bool onResult(uint16_t seq) {
    if (!pending_ || seq != pendingSeq_) {
      staleResults_++;
      return false;
    }
    finish(seq);
    return true;
  }

  // true once when the pending command timed out; it is then finished (ACKed as failed).
  bool checkTimeout(uint32_t now) {
    if (!pending_ || (uint32_t)(now - pendingAt_) < kPendingTimeoutMs) return false;
    timeouts_++;
    finish(pendingSeq_);
    return true;
  }

  bool pending() const { return pending_; }
  uint16_t pendingSeq() const { return pendingSeq_; }
  // Value for StatusPayload.lastCmdSeq: 0 = no command finished yet.
  uint16_t wireLastSeq() const { return haveLast_ ? last_ : 0; }
  uint32_t staleResults() const { return staleResults_; }
  uint32_t timeouts() const { return timeouts_; }

 private:
  bool pending_ = false;
  uint16_t pendingSeq_ = 0;
  uint32_t pendingAt_ = 0;
  bool haveLast_ = false;
  uint16_t last_ = 0;
  uint32_t staleResults_ = 0;
  uint32_t timeouts_ = 0;
};

}  // namespace rt
