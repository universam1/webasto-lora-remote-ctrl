#pragma once
// Fixed-capacity, single-reader multi-writer queue. push()/pop() never block.
//  ESP32: wrapper over a FreeRTOS queue (timeout 0 for push/pop).
//  Host:  plain ring buffer, HOST-ONLY and single-threaded (for tests, no locking).
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace rt {

template <class T, size_t N>
class CommandQueue {
  static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

 public:
  CommandQueue() : q_(xQueueCreate(N, sizeof(T))) {}
  ~CommandQueue() { if (q_) vQueueDelete(q_); }
  CommandQueue(const CommandQueue&) = delete;
  CommandQueue& operator=(const CommandQueue&) = delete;

  bool valid() const { return q_ != nullptr; }
  // Any task; false when full (caller reports Busy).
  bool push(const T& item) { return q_ && xQueueSend(q_, &item, 0) == pdTRUE; }
  // Owner only; non-blocking.
  bool pop(T& out) { return q_ && xQueueReceive(q_, &out, 0) == pdTRUE; }
  // Owner only; blocks at most timeoutMs (never forever).
  bool waitPop(T& out, uint32_t timeoutMs) {
    return q_ && xQueueReceive(q_, &out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
  }
  size_t size() const { return q_ ? uxQueueMessagesWaiting(q_) : 0; }
  size_t capacity() const { return N; }
  void clear() { if (q_) xQueueReset(q_); }

 private:
  QueueHandle_t q_;
};

}  // namespace rt

#else  // host

namespace rt {

template <class T, size_t N>
class CommandQueue {
  static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
  static_assert(N > 0, "N must be > 0");

 public:
  bool valid() const { return true; }
  bool push(const T& item) {
    if (count_ == N) return false;
    buf_[(head_ + count_) % N] = item;
    count_++;
    return true;
  }
  bool pop(T& out) {
    if (count_ == 0) return false;
    out = buf_[head_];
    head_ = (head_ + 1) % N;
    count_--;
    return true;
  }
  // Host: never waits.
  bool waitPop(T& out, uint32_t /*timeoutMs*/) { return pop(out); }
  size_t size() const { return count_; }
  size_t capacity() const { return N; }
  void clear() { head_ = 0; count_ = 0; }

 private:
  T buf_[N];
  size_t head_ = 0;
  size_t count_ = 0;
};

}  // namespace rt

#endif
