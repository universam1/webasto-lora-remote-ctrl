#pragma once
// BLE bridge of the sender (compiled only with -D ENABLE_BLE=1).
// Same GATT profile as the receiver (role = sender, transport = none: the heater is reached over
// LoRa, there is no heater transport on the sender itself).
//
// Threads and data flow (nothing here blocks the main loop):
//   main loop (core 1)  --snap (SnapshotBuffer, single writer)-->  bleTask (core 0): status characteristic
//   NimBLE host task    --cmdQueue (push, timeout 0)------------->  main loop: startJob()
//   main loop           --events (push, timeout 0)--------------->  bleTask: result characteristic
//   main loop (menu)    --pairRequest / clearRequest (atomics)--->  bleTask
// bleTask is the only caller of BleServer::service() and notifyResult().
#include <atomic>
#include <stdint.h>

#include "ble_name.h"
#include "command_queue.h"
#include "commands.h"
#include "snapshot_buffer.h"
#include "status_codec.h"

namespace sbl {

// Written by the main loop only (single writer), read by bleTask.
struct Snap {
  wh::DecodedStatus status;  // last status received over LoRa
  uint32_t rxMs;             // millis() of its reception, 0 = none yet
  uint8_t lastCmdId;         // last finished BLE command of this sender
  uint8_t lastCmdResult;     // its wh::CmdResult
};

struct Shared {
  rt::CommandQueue<rt::Command, 8> cmdQueue;   // NimBLE host -> main loop
  rt::CommandQueue<rt::CmdEvent, 8> events;    // main loop -> bleTask
  rt::SnapshotBuffer<Snap> snap;               // main loop -> bleTask
  std::atomic<bool> pairRequest{false};
  std::atomic<bool> clearRequest{false};
  std::atomic<bool> connected{false};
  std::atomic<uint32_t> lastActivityMs{0};     // BLE connect/subscribe/command, for the keep-alive
  std::atomic<bool> ok{false};                 // stack started (menu items need it)
  std::atomic<bool> defaultPasskey{false};
  char name[ble::kNameLen] = {0};              // written once before ok is set
};

Shared& shared();

// Starts NimBLE and bleTask. staleAfterMs: age after which the status is marked stale.
// Logs the BLE name once. Returns false (ok stays false) when the stack did not start.
bool start(uint32_t staleAfterMs);

}  // namespace sbl
