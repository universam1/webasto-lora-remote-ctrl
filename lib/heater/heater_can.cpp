#include "heater_can.h"

#if !defined(HEATER_TRANSPORT_WBUS) || !HEATER_TRANSPORT_WBUS

#include <Arduino.h>
#include <driver/twai.h>

#include "task_utils.h"

namespace heater {

namespace {
constexpr uint32_t kTickMs = 2;
constexpr uint32_t kPublishMs = 100;
constexpr uint32_t kReinstallMs = 1000;
constexpr uint32_t kRetryGapMs = 2000;
constexpr uint32_t kHealthWindowMs = 600000;
constexpr int kMaxFramesPerTick = 64;
constexpr uint32_t kStackBytes = 6144;

#ifndef CAN_TX_SINGLE_SHOT
// 0 = auto retransmit (same as the proven sniffer firmware). A frame sent to a sleeping bus is
// repeated until a waking node ACKs it. 1 = single shot (no retransmission, no ACK retries).
#define CAN_TX_SINGLE_SHOT 0
#endif
}  // namespace

HeaterCan::HeaterCan() : fsm_(*this), mb_(kMbEmpty), droppedEvents_(0), busOffTotal_(0), maxGapMs_(0) {
  monitor_.enableWakePoll = (CAN_WAKE_POLL != 0);
}

bool HeaterCan::begin() {
  // First thing: keep the transceiver's TXD recessive until the TWAI driver owns the pin.
  pinMode(CAN_TX_PIN, OUTPUT);
  digitalWrite(CAN_TX_PIN, HIGH);
  if (CAN_RS_PIN >= 0) {
    pinMode(CAN_RS_PIN, OUTPUT);
    digitalWrite(CAN_RS_PIN, HIGH);  // standby = listen only
  }
  if (!events_.valid()) return false;
  const bool ok = install(false);
  rt::TaskSpec spec = {"canTask", &HeaterCan::taskEntry, kStackBytes, 5, 1, this};
  const bool task = rt::startTask(spec) != nullptr;
  return ok && task;
}

bool HeaterCan::submit(const HeaterRequest& r) {
  uint8_t expected = kMbEmpty;
  if (!mb_.compare_exchange_strong(expected, kMbWriting, std::memory_order_acq_rel)) return false;
  req_ = r;
  mb_.store(kMbPending, std::memory_order_release);
  return true;
}

wh::HeaterSnapshot HeaterCan::snapshot() const {
  wh::HeaterSnapshot s;
  snap_.read(s);
  return s;
}

// ---------------------------------------------------------------------------------------------
// TWAI driver (canTask only, after begin() returned)
// ---------------------------------------------------------------------------------------------

void HeaterCan::uninstall() {
  if (!installed_) return;
  twai_stop();
  twai_driver_uninstall();
  installed_ = false;
  started_ = false;
}

bool HeaterCan::install(bool normal) {
  uninstall();
  if (CAN_RS_PIN >= 0) digitalWrite(CAN_RS_PIN, normal ? LOW : HIGH);

  twai_timing_config_t timing = TWAI_TIMING_CONFIG_100KBITS();
#if CAN_BITRATE != 100000
#error "HeaterCan supports 100 kbit/s only (Komfort-CAN)"
#endif
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, normal ? TWAI_MODE_NORMAL : TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = 64;
  g.tx_queue_len = normal ? 8 : 0;
  g.alerts_enabled = TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_BUS_OFF |
                     TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_TX_FAILED | TWAI_ALERT_ERR_PASS;
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g, &timing, &f) != ESP_OK) return false;
  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    return false;
  }
  installed_ = true;
  started_ = true;
  normal_ = normal;
  return true;
}

bool HeaterCan::setNormalMode(bool on) {
#if !CAN_ALLOW_TX
  if (on) return false;  // listen-only build: never transmit
#endif
  if (installed_ && started_ && normal_ == on) return true;
  return install(on);
}

bool HeaterCan::send(const wh::Frame& f) {
  // The only transmit path in this firmware.
#if !CAN_ALLOW_TX
  (void)f;
  return false;
#else
  if (!installed_ || !normal_) return false;
  twai_message_t m = {};
  m.identifier = f.id;
  m.extd = 0;
  m.data_length_code = f.dlc > 8 ? 8 : f.dlc;
#if CAN_TX_SINGLE_SHOT
  m.ss = 1;
#endif
  for (uint8_t i = 0; i < m.data_length_code; i++) m.data[i] = f.d[i];
  // At most 20 ms wait for a free TX slot (documented exception to "no blocking").
  return twai_transmit(&m, pdMS_TO_TICKS(20)) == ESP_OK;
#endif
}

void HeaterCan::drainRx(uint32_t now, bool& got) {
  twai_message_t m;
  for (int i = 0; i < kMaxFramesPerTick && twai_receive(&m, 0) == ESP_OK; i++) {
    if (m.extd || m.rtr) continue;
    wh::Frame f;
    f.id = m.identifier;
    f.dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
    for (uint8_t k = 0; k < f.dlc; k++) f.d[k] = m.data[k];
    fsm_.onFrame(f, now);
    monitor_.onFrame(f, now);
    got = true;
  }
}

void HeaterCan::handleAlerts(uint32_t now) {
  uint32_t a = 0;
  if (twai_read_alerts(&a, 0) != ESP_OK) return;
  if (a & TWAI_ALERT_RX_QUEUE_FULL) {
    overflowSeen_ = true;
    overflowAt_ = now;
  }
  if (a & TWAI_ALERT_BUS_OFF) {
    busOffTotal_++;
    // keep the last 4 bus-off times, newest at the end
    for (int i = 0; i < 3; i++) busOffAt_[i] = busOffAt_[i + 1];
    busOffAt_[3] = now;
    if (busOffN_ < 4) busOffN_++;
    fsm_.onBusEvent(true, now);  // finishes a running command with BusOff, back to listen-only
    if (installed_) twai_initiate_recovery();
  }
  if (a & TWAI_ALERT_BUS_RECOVERED) {
    if (installed_) twai_start();
    started_ = true;
    fsm_.onBusEvent(false, now);
  }
}

void HeaterCan::updateHealth(uint32_t now) {
  bool busOffBad = busOffN_ >= 4 && (uint32_t)(now - busOffAt_[0]) <= kHealthWindowMs;
  bool overflowBad = overflowSeen_ && (uint32_t)(now - overflowAt_) <= kHealthWindowMs;
  if (busOffBad) {
    healthy_ = false;
    healthDetail_ = 1;
  } else if (overflowBad) {
    healthy_ = false;
    healthDetail_ = 2;
  } else if (!installed_) {
    healthy_ = false;
    healthDetail_ = 3;  // driver not running: no frames can be received
  } else {
    healthy_ = true;
    healthDetail_ = 0;
  }
}

// ---------------------------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------------------------

void HeaterCan::pushEvent(const HeaterEvent& e) {
  if (events_.push(e)) return;
  HeaterEvent dropped;
  events_.pop(dropped);  // drop the oldest, count it
  droppedEvents_++;
  events_.push(e);
}

void HeaterCan::finishRequest(wh::CmdResult r, uint32_t now) {
  HeaterEvent e;
  e.req = req_;
  e.result = r;
  monitor_.noteCommandResult(req_.kind, r, now);
  requestActive_ = false;
  retryWait_ = false;
  pushEvent(e);
  mb_.store(kMbEmpty, std::memory_order_release);  // after the event: owner sees it when !busy()
}

void HeaterCan::startPending(uint32_t now) {
  if (mb_.load(std::memory_order_acquire) != kMbPending) return;
  if (fsm_.busy()) return;  // our own wake is still running; the request waits at most 3 s
  mb_.store(kMbRunning, std::memory_order_release);
  requestActive_ = true;
  retryWait_ = false;
  retriesLeft_ = req_.kind == wh::CmdKind::Stop ? 2 : (req_.kind == wh::CmdKind::Heat ? 1 : 0);
  const wh::CmdResult r = fsm_.request(req_.kind, now);
  if (r != wh::CmdResult::Pending) finishRequest(r, now);  // Unsupported (Vent) or Busy
}

void HeaterCan::onFsmResult(wh::CmdResult r, uint32_t now) {
  if (r == wh::CmdResult::NotAdopted && retriesLeft_ > 0) {
    retriesLeft_--;
    retryWait_ = true;
    retryAt_ = now + kRetryGapMs;
    return;
  }
  finishRequest(r, now);
}

void HeaterCan::publish(uint32_t now) {
  wh::HeaterSnapshot s = monitor_.snapshot();
  s.healthy = healthy_;
  s.healthDetail = healthDetail_;
  snap_.write(s);
  lastPublish_ = now;
}

// ---------------------------------------------------------------------------------------------
// canTask
// ---------------------------------------------------------------------------------------------

void HeaterCan::taskEntry(void* arg) {
  static_cast<HeaterCan*>(arg)->run();
  vTaskDelete(nullptr);
}

void HeaterCan::run() {
  rt::LoopTimer timer(kTickMs);
  uint32_t lastRun = millis();
  for (;;) {
    const uint32_t now = millis();
    const uint32_t gap = now - lastRun;
    lastRun = now;
    if (gap > maxGapMs_.load(std::memory_order_relaxed)) maxGapMs_.store(gap, std::memory_order_relaxed);

    if (!installed_) {
      // driver failed or is gone: retry in listen-only mode, no frames are sent
      if ((uint32_t)(now - lastInstallTry_) >= kReinstallMs) {
        lastInstallTry_ = now;
        install(false);
      }
    }

    bool got = false;
    if (installed_) {
      drainRx(now, got);
      handleAlerts(now);
    }

    startPending(now);
    if (retryWait_ && (int32_t)(now - retryAt_) >= 0) {
      retryWait_ = false;
      const wh::CmdResult r = fsm_.request(req_.kind, now);
      if (r != wh::CmdResult::Pending) finishRequest(r, now);
    }

    fsm_.tick(now);
    wh::CmdResult res;
    uint8_t frames;
    bool fsmDone = false;
    if (fsm_.takeResult(res, frames)) {
      fsmDone = true;
      if (requestActive_) {
        onFsmResult(res, now);
      } else {
        wakeActive_ = false;  // result of our own wake: nothing to report
      }
    }
    (void)fsmDone;

    monitor_.tick(now);

    // Wake policy: only when nothing else is running.
    if (CAN_WAKE_POLL && !requestActive_ && !fsm_.busy() && mb_.load(std::memory_order_acquire) == kMbEmpty &&
        monitor_.wantWake(now)) {
      if (fsm_.request(wh::CmdKind::Wake, now) == wh::CmdResult::Pending) {
        wakeActive_ = true;
        monitor_.noteWake(now);
      }
    }

    updateHealth(now);
    if (got || (uint32_t)(now - lastPublish_) >= kPublishMs) publish(now);

    timer.waitNext();
  }
}

// ---------------------------------------------------------------------------------------------

IHeater& instance() {
  static HeaterCan inst;
  return inst;
}

}  // namespace heater

#endif  // !HEATER_TRANSPORT_WBUS
