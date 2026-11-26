#include "heater_wbus.h"

#if defined(HEATER_TRANSPORT_WBUS) && HEATER_TRANSPORT_WBUS

#include <Arduino.h>

#include "task_stacks.h"
#include "task_utils.h"
#include "wbus_simple.h"

namespace heater {

namespace {
constexpr uint32_t kPollMs = 5000;
constexpr uint32_t kPollAfterMs = 60000;

static WBusSimple& wbusPort() {
  static WBusSimple w(Serial2);
  return w;
}

// Coarse mapping as in the former receiver code: 0x04 and 0x00 are off, anything else runs.
wh::HeaterPhase mapOpState(uint8_t opState) {
  if (opState == 0x04) return wh::HeaterPhase::Off;
  if (opState == 0x00) return wh::HeaterPhase::Off;  // burn out / off-ish
  return wh::HeaterPhase::Heating;
}

uint16_t be16(uint8_t hi, uint8_t lo) { return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo); }

// Waits (bounded by timeoutMs) for the 0x50 response of status page `index`.
bool waitForStatusIndex(WBusSimple& wbus, uint8_t index, WBusPacket& out, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(deadline - millis()) > 0) {
    if (!wbus.readPacket(out, timeoutMs)) return false;
    if (out.payloadLen < 3) continue;
    // Response payload begins with cmd|0x80 (typically 0xD0 for cmd 0x50 responses).
    if ((out.payload[0] & 0x7F) != 0x50) continue;
    if ((out.payload[0] & 0x80) == 0) continue;
    if (out.payload[1] != index) continue;
    return true;
  }
  return false;
}

struct Multi {
  int16_t temperatureC = INT16_MIN;
  uint16_t voltage_mV = 0;
  uint16_t power = 0;
};

bool tryPollMultiStatusOnce(WBusSimple& wbus, Multi& out) {
  static const uint8_t kIds[] = {
      0x01, 0x03, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0C, 0x0E, 0x0F, 0x10, 0x11, 0x13,
      0x1E, 0x1F, 0x24, 0x27, 0x29, 0x2A, 0x2C, 0x2D, 0x32, 0x34, 0x3D, 0x52, 0x57,
      0x5F, 0x78, 0x89,
  };
  if (!wbus.requestStatusMulti(kIds, sizeof(kIds))) return false;

  WBusPacket pkt;
  uint32_t deadline = millis() + 250;
  while ((int32_t)(deadline - millis()) > 0) {
    if (!wbus.readPacket(pkt, 250)) break;
    if (pkt.payloadLen < 4) continue;
    if ((pkt.payload[0] & 0x7F) != 0x50) continue;
    if ((pkt.payload[0] & 0x80) == 0) continue;
    if (pkt.payload[1] != 0x30) continue;

    WBusStatus st;
    if (WBusSimple::tryParseStatusTlv(pkt, st) && st.valid) {
      out.temperatureC = st.temperatureC;
      out.voltage_mV = st.voltage_mV;
      out.power = st.power;
      return true;
    }
    break;
  }
  return false;
}

// Simple status page 0x05: temperature (raw - 50) and voltage.
bool readSimple05(WBusSimple& wbus, Multi& out) {
  WBusPacket pkt;
  const uint8_t idx = 0x05;
  if (!(wbus.sendCommand(0x50, &idx, 1) && waitForStatusIndex(wbus, idx, pkt, 250))) return false;
  if (pkt.payloadLen < 5) return false;
  out.temperatureC = static_cast<int16_t>(static_cast<int>(pkt.payload[2]) - 50);
  out.voltage_mV = be16(pkt.payload[3], pkt.payload[4]);
  return true;
}

// Pages 0x0F (glow/pump/fan) and 0x02 (flags) are read for bus compatibility only; the values
// are not part of HeaterSnapshot.
void readSimple0F(WBusSimple& wbus) {
  WBusPacket pkt;
  const uint8_t idx = 0x0F;
  (void)(wbus.sendCommand(0x50, &idx, 1) && waitForStatusIndex(wbus, idx, pkt, 250));
}
void readSimple02(WBusSimple& wbus) {
  WBusPacket pkt;
  const uint8_t idx = 0x02;
  (void)(wbus.sendCommand(0x50, &idx, 1) && waitForStatusIndex(wbus, idx, pkt, 250));
}
}  // namespace

HeaterWBus::HeaterWBus() : mb_(kMbEmpty) {}

bool HeaterWBus::begin() {
  wbus_ = &wbusPort();
  const bool ok = wbus_->begin();  // sets the TX pin idle high before opening the UART
  if (!events_.valid()) return false;
  cur_.healthy = ok;
  cur_.healthDetail = ok ? 0 : 4;
  snap_.write(cur_);
  rt::TaskSpec spec = {"wbusTask", &HeaterWBus::taskEntry, rt::kStackWbus, 4, 1, this};
  return ok && rt::startTask(spec) != nullptr;
}

bool HeaterWBus::submit(const HeaterRequest& r) {
  uint8_t expected = kMbEmpty;
  if (!mb_.compare_exchange_strong(expected, kMbWriting, std::memory_order_acq_rel)) return false;
  req_ = r;
  mb_.store(kMbPending, std::memory_order_release);
  return true;
}

wh::HeaterSnapshot HeaterWBus::snapshot() const {
  wh::HeaterSnapshot s;
  snap_.read(s);
  return s;
}

void HeaterWBus::pushEvent(const HeaterEvent& e) {
  if (events_.push(e)) return;
  HeaterEvent dropped;
  events_.pop(dropped);  // drop the oldest
  events_.push(e);
}

void HeaterWBus::pollOnce(uint32_t now) {
  uint8_t op = 0;
  bool any = false;
  if (wbus_->readOperatingState(op)) {
    cur_.phase = mapOpState(op);
    cur_.phaseAssumed = false;
    cur_.statusAtMs = now;
    any = true;
  }

  Multi m;
  bool got = false;
  if (tlvSupport_ == 0) {
    got = tryPollMultiStatusOnce(*wbus_, m);  // probe once, as the receiver did at setup
    tlvSupport_ = got ? 2 : 1;
  } else if (tlvSupport_ == 2) {
    got = tryPollMultiStatusOnce(*wbus_, m);
  }
  if (!got) {
    got = readSimple05(*wbus_, m);
    readSimple0F(*wbus_);
    readSimple02(*wbus_);
  }
  if (got) {
    if (m.temperatureC != INT16_MIN) cur_.coolant_dC = static_cast<int16_t>(m.temperatureC * 10);
    if (m.voltage_mV != 0) cur_.voltage_mV.set(m.voltage_mV, now);
    // TLV power has no documented scale: not mapped, powerRaw stays 0.
    cur_.powerRaw = 0;
    any = true;
  }

  if (any) {
    failStreak_ = 0;
    cur_.busAwake = true;
    cur_.lastBusFrameMs = now;
    cur_.healthy = true;
    cur_.healthDetail = 0;
  } else {
    if (failStreak_ < 255) failStreak_++;
    if (failStreak_ >= 3) {
      cur_.healthy = false;
      cur_.healthDetail = 4;
      cur_.busAwake = false;
    }
  }
}

void HeaterWBus::taskEntry(void* arg) {
  static_cast<HeaterWBus*>(arg)->run();
  vTaskDelete(nullptr);
}

void HeaterWBus::run() {
  rt::LoopTimer timer(10);
  wh::HeaterPhase prevPhase = wh::HeaterPhase::Unknown;
  pollUntil_ = millis() + kPollAfterMs;  // learn the state after boot
  lastPoll_ = millis() - kPollMs;
  for (;;) {
    uint32_t now = millis();

    if (mb_.load(std::memory_order_acquire) == kMbPending) {
      mb_.store(kMbRunning, std::memory_order_release);
      bool ok = false;
      const uint8_t mins = static_cast<uint8_t>(req_.minutes == 0 ? 30 : (req_.minutes > 255 ? 255 : req_.minutes));
      switch (req_.kind) {
        case wh::CmdKind::Heat: ok = wbus_->startParkingHeater(mins); break;
        case wh::CmdKind::Vent: ok = wbus_->startVentilation(mins); break;  // blocks up to ~1 s: that is the point of this task
        case wh::CmdKind::Stop: ok = wbus_->stop(); break;
        default: break;
      }
      HeaterEvent e;
      e.req = req_;
      e.result = (req_.kind == wh::CmdKind::Wake) ? wh::CmdResult::Unsupported
                                                 : (ok ? wh::CmdResult::Ok : wh::CmdResult::Timeout);
      now = millis();
      pushEvent(e);
      pollUntil_ = now + kPollAfterMs;
      lastPoll_ = now - kPollMs;  // poll immediately after a request
      mb_.store(kMbEmpty, std::memory_order_release);
    }

    const bool running = cur_.phase == wh::HeaterPhase::Heating || cur_.phase == wh::HeaterPhase::Ventilating;
    if (prevPhase != cur_.phase) {
      if (prevPhase != wh::HeaterPhase::Unknown && prevPhase != wh::HeaterPhase::Off && cur_.phase == wh::HeaterPhase::Off)
        pollUntil_ = now + kPollAfterMs;  // 60 s after off
      prevPhase = cur_.phase;
    }
    const bool inWindow = pollUntil_ != 0 && (int32_t)(pollUntil_ - now) > 0;
    if ((running || inWindow) && (uint32_t)(now - lastPoll_) >= kPollMs) {
      lastPoll_ = now;
      pollOnce(now);  // blocks at most a few hundred ms per page
      snap_.write(cur_);
    }
    timer.waitNext();
  }
}

IHeater& instance() {
  static HeaterWBus inst;
  return inst;
}

}  // namespace heater

#endif  // HEATER_TRANSPORT_WBUS
