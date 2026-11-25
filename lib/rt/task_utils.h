#pragma once
// ESP32 only. Compiles to nothing on the host.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace rt {

struct TaskSpec {
  const char* name;
  TaskFunction_t fn;
  uint32_t stackWords;  // stack size in bytes on ESP32 (xTaskCreatePinnedToCore unit)
  UBaseType_t prio;
  int core;  // 0, 1 or tskNO_AFFINITY
  void* arg;
};

// Creates the task, logs name and stack, registers it for logStackWatermarks().
// Returns nullptr on failure.
TaskHandle_t startTask(const TaskSpec& spec);

// Logs uxTaskGetStackHighWaterMark of every task started via startTask().
void logStackWatermarks();

// Drift-free periodic loop helper. Use from inside one task only.
class LoopTimer {
 public:
  explicit LoopTimer(uint32_t periodMs);
  void waitNext();  // vTaskDelayUntil; records lateness
  uint32_t maxJitterMs() const { return maxJitterMs_; }
  void resetJitter() { maxJitterMs_ = 0; }

 private:
  TickType_t period_;
  TickType_t last_;
  TickType_t expected_;
  uint32_t maxJitterMs_;
};

}  // namespace rt
#endif
