#pragma once
// SnapshotBuffer: single writer, many readers, wait-free for readers (seqlock).
//
// Memory ordering:
//  * seq is even when stable, odd while the writer is inside write().
//  * Writer: seq=odd (relaxed); release fence; payload words (relaxed); seq=even (release).
//    The fence keeps the payload stores from moving before the odd marker; the release
//    store keeps them from moving after the even marker.
//  * Reader: s1=seq (acquire); payload words (relaxed); acquire fence; s2=seq (relaxed).
//    If s1 is even and s1==s2, no write overlapped the copy, so the copy is consistent.
//  * The payload is stored as relaxed std::atomic<uint32_t> words, so a concurrent
//    read/write is not a C++ data race (no UB, ThreadSanitizer clean); tearing is
//    detected by the sequence check and the torn copy is discarded.
//  * Readers never block and never loop unbounded: after kMaxTries failed attempts
//    read() returns false and leaves `out` untouched, so the caller keeps its own
//    last good copy.
// Exactly one task may call write().
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>

namespace rt {

template <class T>
class SnapshotBuffer {
  static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

 public:
  static constexpr int kMaxTries = 8;

  SnapshotBuffer() : seq_(0) {
    for (size_t i = 0; i < kWords; i++) words_[i].store(0, std::memory_order_relaxed);
  }

  void write(const T& v) {
    uint32_t w[kWords];
    w[kWords - 1] = 0;
    memcpy(w, &v, sizeof(T));
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < kWords; i++) words_[i].store(w[i], std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);
  }

  // true: out holds a consistent copy. false: never written or too much contention;
  // out is unchanged.
  bool read(T& out) const {
    for (int t = 0; t < kMaxTries; t++) {
      const uint32_t s1 = seq_.load(std::memory_order_acquire);
      if (s1 == 0) return false;
      if (s1 & 1u) continue;
      uint32_t w[kWords];
      for (size_t i = 0; i < kWords; i++) w[i] = words_[i].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq_.load(std::memory_order_relaxed) == s1) {
        memcpy(&out, w, sizeof(T));
        return true;
      }
    }
    return false;
  }

  // Increases by 2 per write (odd while a write is in progress).
  uint32_t sequence() const { return seq_.load(std::memory_order_acquire); }

 private:
  static constexpr size_t kWords = (sizeof(T) + 3) / 4;
  std::atomic<uint32_t> seq_;
  std::atomic<uint32_t> words_[kWords];
};

}  // namespace rt
