#pragma once
#include "project_config.h"

#if !defined(HEATER_TRANSPORT_WBUS) || !HEATER_TRANSPORT_WBUS

#include <atomic>

#include "can_command_fsm.h"
#include "can_monitor.h"
#include "command_queue.h"
#include "heater_iface.h"
#include "snapshot_buffer.h"

namespace heater {

// CAN (TWAI) transport. begin() starts canTask (core 1, prio 5, 2 ms tick). That task is the
// only code that touches TWAI after begin() returns. Everything else uses submit(), snapshot()
// and popEvent().
class HeaterCan : public IHeater, public wh::CanPort {
 public:
  HeaterCan();

  // IHeater
  bool begin() override;
  bool submit(const HeaterRequest& r) override;
  bool busy() const override { return mb_.load(std::memory_order_acquire) != kMbEmpty; }
  wh::HeaterSnapshot snapshot() const override;
  bool popEvent(HeaterEvent& e) override { return events_.pop(e); }
  const char* transport() const override { return "can"; }
  bool supportsVentilation() const override { return false; }
  bool supportsRunMinutes() const override { return false; }

  // wh::CanPort (called only from canTask, through the FSM)
  bool setNormalMode(bool on) override;
  bool send(const wh::Frame& f) override;

  // Diagnostics (any task, approximate)
  uint32_t droppedEvents() const { return droppedEvents_.load(); }
  uint32_t busOffTotal() const { return busOffTotal_.load(); }
  uint32_t maxLoopGapMs() const { return maxGapMs_.load(); }

 private:
  static constexpr uint8_t kMbEmpty = 0, kMbPending = 1, kMbRunning = 2, kMbWriting = 3;

  static void taskEntry(void* arg);
  void run();
  bool install(bool normal);
  void uninstall();
  void drainRx(uint32_t now, bool& gotFrame);
  void handleAlerts(uint32_t now);
  void startPending(uint32_t now);
  void onFsmResult(wh::CmdResult r, uint32_t now);
  void finishRequest(wh::CmdResult r, uint32_t now);
  void updateHealth(uint32_t now);
  void publish(uint32_t now);
  void pushEvent(const HeaterEvent& e);

  wh::CanCommandFsm fsm_;
  wh::CanMonitor monitor_;
  rt::SnapshotBuffer<wh::HeaterSnapshot> snap_;
  rt::CommandQueue<HeaterEvent, 4> events_;

  // single slot mailbox: kMbWriting while a submitter fills req_
  std::atomic<uint8_t> mb_;
  HeaterRequest req_;

  // canTask state
  bool installed_ = false;
  bool normal_ = false;
  bool started_ = false;
  uint32_t lastInstallTry_ = 0;
  bool requestActive_ = false;   // a HeaterRequest is being executed (FSM or retry wait)
  bool wakeActive_ = false;      // FSM is running our own wake
  bool retryWait_ = false;
  uint32_t retryAt_ = 0;
  uint8_t retriesLeft_ = 0;
  uint32_t lastPublish_ = 0;

  // health
  uint32_t busOffAt_[4] = {0, 0, 0, 0};
  uint8_t busOffN_ = 0;
  bool overflowSeen_ = false;
  uint32_t overflowAt_ = 0;
  bool healthy_ = true;
  uint8_t healthDetail_ = 0;

  std::atomic<uint32_t> droppedEvents_;
  std::atomic<uint32_t> busOffTotal_;
  std::atomic<uint32_t> maxGapMs_;
};

}  // namespace heater

#endif  // !HEATER_TRANSPORT_WBUS
