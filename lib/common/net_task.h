#pragma once
// Net task: owns WiFi and MQTT (PubSubClient is not thread safe). All blocking connection
// set-ups happen here, on core 0. Other tasks never call into wifi_manager/mqtt_client; they
// use the queues and the snapshot buffer passed to netTaskStart().
#ifdef ENABLE_MQTT_CONTROL

#include <stdint.h>

#include "command_queue.h"
#include "commands.h"
#include "heater_types.h"
#include "snapshot_buffer.h"

// Starts the task (core 0, prio 2, 8192 bytes stack, 50 ms period). Call once from setup(),
// after the queues and the snapshot buffer exist. Returns false if the task could not be created.
//  cmdQueue:   MQTT commands are pushed here (non blocking; full queue: counted, logged, dropped).
//  eventQueue: command results (rt::CmdEvent) are popped here and published to last_cmd/state.
//              Events of every source are published; the net task is its only reader.
//  snapshot:   latest SystemSnapshot written by the control task; the net task only reads it.
bool netTaskStart(rt::CommandQueue<rt::Command, 8>& cmdQueue,
                  rt::CommandQueue<rt::CmdEvent, 8>& eventQueue,
                  const rt::SnapshotBuffer<wh::SystemSnapshot>& snapshot);

// Diagnostic values for the HA diagnostic sensors. Any single writer task may call this
// (wait-free, no library calls); the net task publishes them every MQTT_DIAGNOSTIC_INTERVAL_MS.
// cmdSource: "lora", "mqtt", "button", ... (truncated to 15 characters).
void netTaskSetDiagnostics(int loraRssi, float loraSnr, const char* cmdSource, bool transportHealthy);

// Number of MQTT commands dropped because the command queue was full.
uint32_t netTaskDroppedCommands();

#endif  // ENABLE_MQTT_CONTROL
