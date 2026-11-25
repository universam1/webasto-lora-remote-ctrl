#pragma once
#include "project_config.h"

#if defined(HEATER_TRANSPORT_WBUS) && HEATER_TRANSPORT_WBUS

#include <atomic>

#include "command_queue.h"
#include "heater_iface.h"
#include "snapshot_buffer.h"

class HardwareSerial;
class WBusSimple;

namespace heater {

// W-BUS transport around WBusSimple. wbusTask (core 1, prio 4) owns Serial2 and executes the
// blocking start/stop commands (break pulse, up to 3 tries, about 1 s) and the status polling.
class HeaterWBus : public IHeater {
 public:
  HeaterWBus();
  bool begin() override;
  bool submit(const HeaterRequest& r) override;
  bool busy() const override { return mb_.load(std::memory_order_acquire) != kMbEmpty; }
  wh::HeaterSnapshot snapshot() const override;
  bool popEvent(HeaterEvent& e) override { return events_.pop(e); }
  const char* transport() const override { return "wbus"; }
  bool supportsVentilation() const override { return true; }
  bool supportsRunMinutes() const override { return true; }

 private:
  static constexpr uint8_t kMbEmpty = 0, kMbPending = 1, kMbRunning = 2, kMbWriting = 3;
  static void taskEntry(void* arg);
  void run();
  void pollOnce(uint32_t now);
  void pushEvent(const HeaterEvent& e);

  WBusSimple* wbus_ = nullptr;
  rt::SnapshotBuffer<wh::HeaterSnapshot> snap_;
  rt::CommandQueue<HeaterEvent, 4> events_;
  std::atomic<uint8_t> mb_;
  HeaterRequest req_;

  // wbusTask state
  wh::HeaterSnapshot cur_;
  uint8_t tlvSupport_ = 0;  // 0 unknown, 1 no, 2 yes
  uint32_t lastPoll_ = 0;
  uint32_t pollUntil_ = 0;  // keep polling until this time (60 s after off or after a request)
  bool pollWindow_ = false;
  uint8_t failStreak_ = 0;
};

}  // namespace heater

#endif  // HEATER_TRANSPORT_WBUS
