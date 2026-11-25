#pragma once
// Transport neutral heater interface. The implementation owns its hardware in its own task;
// every other task talks to it only through submit(), snapshot() and popEvent().
#include <stdint.h>

#include "heater_types.h"

namespace heater {

enum class Source : uint8_t { Menu, Lora, Mqtt, Ble, Climate, Serial };

struct HeaterRequest {
  wh::CmdKind kind = wh::CmdKind::Stop;
  Source src = Source::Menu;
  uint8_t tag = 0;       // caller's correlation id (e.g. BLE cmdId)
  uint16_t loraSeq = 0;  // LoRa command sequence number, 0 if none
  uint16_t minutes = 0;  // W-BUS only: run time, 0 = 30. Ignored over CAN.
};

// Pushed to the event queue when a request finishes.
struct HeaterEvent {
  HeaterRequest req;
  wh::CmdResult result = wh::CmdResult::Pending;
};

class IHeater {
 public:
  // Call EARLY in setup(): TX pin recessive, driver installed listen-only, task started.
  virtual bool begin() = 0;
  // Never blocks. false = busy (one request in flight, none queued).
  virtual bool submit(const HeaterRequest&) = 0;
  virtual bool busy() const = 0;
  // Thread safe copy.
  virtual wh::HeaterSnapshot snapshot() const = 0;
  // Non blocking, from the event queue (capacity 4). Single consumer.
  virtual bool popEvent(HeaterEvent&) = 0;
  virtual const char* transport() const = 0;  // "can" / "wbus"
  virtual bool supportsVentilation() const = 0;
  virtual bool supportsRunMinutes() const = 0;
  virtual ~IHeater() {}
};

// HeaterCan or HeaterWBus singleton depending on HEATER_TRANSPORT_WBUS.
IHeater& instance();

}  // namespace heater
