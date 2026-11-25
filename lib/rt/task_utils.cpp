#include "task_utils.h"
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <Arduino.h>

namespace rt {

namespace {
constexpr int kMaxTasks = 12;
struct Entry { const char* name; TaskHandle_t h; uint32_t stack; };
Entry g_tasks[kMaxTasks];
int g_count = 0;
}  // namespace

TaskHandle_t startTask(const TaskSpec& s) {
  TaskHandle_t h = nullptr;
  BaseType_t ok = xTaskCreatePinnedToCore(s.fn, s.name, s.stackWords, s.arg, s.prio, &h,
                                          s.core);
  if (ok != pdPASS) {
    Serial.printf("[rt] task %s FAILED (stack %u)\n", s.name, (unsigned)s.stackWords);
    return nullptr;
  }
  Serial.printf("[rt] task %s started: stack %u, prio %u, core %d\n", s.name,
                (unsigned)s.stackWords, (unsigned)s.prio, s.core);
  if (g_count < kMaxTasks) g_tasks[g_count++] = {s.name, h, s.stackWords};
  return h;
}

void logStackWatermarks() {
  for (int i = 0; i < g_count; i++) {
    Serial.printf("[rt] stack %s: min free %u of %u\n", g_tasks[i].name,
                  (unsigned)uxTaskGetStackHighWaterMark(g_tasks[i].h),
                  (unsigned)g_tasks[i].stack);
  }
}

LoopTimer::LoopTimer(uint32_t periodMs)
    : period_(pdMS_TO_TICKS(periodMs) ? pdMS_TO_TICKS(periodMs) : 1),
      last_(xTaskGetTickCount()),
      expected_(last_),
      maxJitterMs_(0) {}

void LoopTimer::waitNext() {
  expected_ = last_ + period_;
  vTaskDelayUntil(&last_, period_);
  const TickType_t late = xTaskGetTickCount() - expected_;  // unsigned, wrap safe
  if (late < (TickType_t)0x80000000u) {
    const uint32_t ms = (uint32_t)late * portTICK_PERIOD_MS;
    if (ms > maxJitterMs_) maxJitterMs_ = ms;
  }
}

}  // namespace rt
#endif
