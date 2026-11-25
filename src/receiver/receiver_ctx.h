#pragma once
// Shared objects of the receiver tasks: queues, the snapshot buffer and a few atomics.
// Every hardware has exactly one owner task (see main.cpp); everything here is thread safe
// and never blocks.
#include <atomic>
#include <stdint.h>

#include "command_queue.h"
#include "commands.h"
#include "heater_types.h"
#include "snapshot_buffer.h"

namespace rx {

struct Ctx {
  rt::CommandQueue<rt::Command, 8> cmdQueue;       // all sources -> ctrlTask
  rt::CommandQueue<rt::CmdEvent, 8> loraEvents;    // ctrlTask -> loraTask
  rt::CommandQueue<rt::CmdEvent, 8> bleEvents;     // ctrlTask -> bleTask
  rt::CommandQueue<rt::CmdEvent, 8> netEvents;     // ctrlTask -> netTask
  rt::SnapshotBuffer<wh::SystemSnapshot> snapshot;  // ctrlTask -> everybody

  // LoRa send window: written by loraTask on every valid sender packet, read by ctrlTask.
  std::atomic<uint32_t> lastSenderRequestMs{0};
  std::atomic<bool> everRequested{false};
  // Any task other than loraTask asks for an immediate status packet (Query from MQTT, BLE, menu).
  std::atomic<bool> forceStatus{false};
  std::atomic<uint8_t> lastCmdSource{0xFF};  // rt::Source of the last routed command
  // Activity counters for the LED (uiTask looks at changes).
  std::atomic<uint32_t> loraRxCount{0};
  std::atomic<uint32_t> loraTxCount{0};
  // uiTask -> bleTask requests (the BLE owner task executes them).
  std::atomic<bool> blePairRequest{false};
  std::atomic<bool> bleClearRequest{false};
  std::atomic<bool> bleDefaultPasskey{false};
  std::atomic<bool> bleConnected{false};
  // loraTask -> ctrlTask/uiTask: last received packet and radio state.
  std::atomic<int32_t> loraRssi{-157};
  std::atomic<float> loraSnr{-20.0f};
  std::atomic<bool> loraOk{false};
};

// Created by the first call (from setup()), never destroyed.
Ctx& ctx();

// Each start function initialises its hardware and creates its task. Call from setup().
void startControlTask();
bool startLoraTask();
void startUiTask();
#if defined(ENABLE_BLE) && ENABLE_BLE
void startBleTask();
#endif

}  // namespace rx
