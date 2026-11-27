#pragma once
// LoRa range test ("ping-pong"): the sender transmits a Ping, the receiver answers with a Pong
// that carries the RSSI/SNR it measured for the Ping. The sender keeps statistics for both
// directions and the round trip time. Pure logic, host-testable (no Arduino, no radio).
//
// Stop-and-wait: one ping in flight at a time. A ping without a pong after timeoutMs counts as
// lost; a pong that arrives after that is counted as "late" and does not change ok/lost.
#include <stdint.h>

#include "lora_airtime.h"

namespace wh {

struct RangeParams {
  uint32_t intervalMs = 7500;  // start-to-start time of two pings
  uint32_t timeoutMs = 2500;   // no pong after this time: lost
  uint32_t maxPings = 0;       // 0: until stopped
};

// Interval and timeout derived from the on-air times, so that the answer fits between two pings
// and neither radio exceeds a 10 % duty cycle (433.05..434.79 MHz SRD band). The busier node is
// the one with the longer packet: interval >= 11 * max(ping airtime, pong airtime).
inline RangeParams rangeParamsFor(uint8_t sf, uint32_t bwHz, uint8_t cr4, uint16_t pingWireBytes,
                                  uint16_t pongWireBytes) {
  RangeParams p;
  const uint32_t pingAir = lora::airtimeUs(pingWireBytes, sf, bwHz, cr4);
  const uint32_t pongAir = lora::airtimeUs(pongWireBytes, sf, bwHz, cr4);
  if (pingAir == 0 || pongAir == 0) return p;  // invalid radio settings: keep the defaults
  // The receiver may still be sending a status when the ping arrives: allow 1 s of delay.
  p.timeoutMs = lora::commandRetryMs(pingAir, pongAir, 250, 1000);
  const uint32_t longest = pingAir > pongAir ? pingAir : pongAir;
  uint32_t duty = (11u * longest + 999) / 1000;
  const uint32_t cycle = p.timeoutMs + 500;
  p.intervalMs = duty > cycle ? duty : cycle;
  return p;
}

// Running statistics of one value: last, min, max, average.
struct Series {
  uint32_t n = 0;
  int32_t last = 0, min = 0, max = 0;
  int64_t sum = 0;
  void add(int32_t v) {
    if (n == 0 || v < min) min = v;
    if (n == 0 || v > max) max = v;
    last = v;
    sum += v;
    n++;
  }
  int32_t avg() const { return n ? (int32_t)(sum / (int64_t)n) : 0; }  // truncated towards zero
};

struct RangeStats {
  uint32_t sent = 0;
  uint32_t ok = 0;
  uint32_t lost = 0;
  uint32_t late = 0;   // pong after the timeout (the ping was already counted as lost)
  uint32_t stray = 0;  // pong that matches no ping at all
  uint32_t lostStreak = 0, maxLostStreak = 0;
  Series upRssi, upSnr;      // receiver heard the sender (from the pong payload)
  Series downRssi, downSnr;  // sender heard the receiver
  Series rttMs;              // from the end of send() to the pong, includes both airtimes

  // Loss in permille of the finished pings (ok + lost). 0 when nothing finished yet.
  uint32_t lossPermille() const {
    const uint32_t done = ok + lost;
    return done ? (uint32_t)(((uint64_t)lost * 1000 + done / 2) / done) : 0;
  }
};

class RangeTest {
 public:
  enum class Event : uint8_t { None, Lost, Finished };

  void start(uint32_t now, const RangeParams& p) {
    params_ = p;
    stats_ = RangeStats{};
    active_ = true;
    pending_ = false;
    haveLost_ = false;
    nextDue_ = now;  // first ping at once
  }
  void stop() {
    active_ = false;
    pending_ = false;
  }

  bool active() const { return active_; }
  bool pending() const { return pending_; }
  uint16_t pendingSeq() const { return pendingSeq_; }
  const RangeStats& stats() const { return stats_; }
  const RangeParams& params() const { return params_; }

  // True when the caller should transmit a ping now (it must still check that the radio is idle
  // and call onSent() only after send() succeeded).
  bool due(uint32_t now) const {
    if (!active_ || pending_) return false;
    if (params_.maxPings != 0 && stats_.sent >= params_.maxPings) return false;
    return (int32_t)(now - nextDue_) >= 0;
  }

  void onSent(uint16_t seq, uint32_t now) {
    pending_ = true;
    pendingSeq_ = seq;
    sentAt_ = now;
    stats_.sent++;
    nextDue_ = now + params_.intervalMs;
  }

  // Call every loop. Lost: a ping timed out. Finished: maxPings reached and nothing in flight.
  Event poll(uint32_t now) {
    if (!active_) return Event::None;
    if (pending_ && (int32_t)(now - sentAt_) >= (int32_t)params_.timeoutMs) {
      pending_ = false;
      stats_.lost++;
      stats_.lostStreak++;
      if (stats_.lostStreak > stats_.maxLostStreak) stats_.maxLostStreak = stats_.lostStreak;
      lostSeq_ = pendingSeq_;
      haveLost_ = true;
      if (params_.maxPings != 0 && stats_.sent >= params_.maxPings) {
        active_ = false;  // that was the last ping
        return Event::Finished;
      }
      return Event::Lost;
    }
    if (!pending_ && params_.maxPings != 0 && stats_.sent >= params_.maxPings) {
      active_ = false;
      return Event::Finished;
    }
    return Event::None;
  }

  // A pong arrived. True: it answers the ping in flight (counted as ok).
  bool onPong(uint16_t pingSeq, int rssiUp, int snrUp, int rssiDown, int snrDown, uint32_t now) {
    if (!active_) return false;
    if (pending_ && pingSeq == pendingSeq_) {
      pending_ = false;
      stats_.ok++;
      stats_.lostStreak = 0;
      stats_.upRssi.add(rssiUp);
      stats_.upSnr.add(snrUp);
      stats_.downRssi.add(rssiDown);
      stats_.downSnr.add(snrDown);
      stats_.rttMs.add((int32_t)(now - sentAt_));
      return true;
    }
    if (haveLost_ && pingSeq == lostSeq_) {
      stats_.late++;
      haveLost_ = false;  // count a repeated pong only once
    } else {
      stats_.stray++;
    }
    return false;
  }

 private:
  RangeParams params_;
  RangeStats stats_;
  bool active_ = false;
  bool pending_ = false;
  bool haveLost_ = false;
  uint16_t pendingSeq_ = 0, lostSeq_ = 0;
  uint32_t sentAt_ = 0, nextDue_ = 0;
};

}  // namespace wh
