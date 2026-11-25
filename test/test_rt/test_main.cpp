#include <unity.h>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include "../../lib/rt/snapshot_buffer.h"
#include "../../lib/rt/command_queue.h"
#include "../../lib/rt/commands.h"

struct Big { uint32_t a, b, c, d, e; uint8_t f; };

void setUp() {}
void tearDown() {}

void test_snapshot_roundtrip() {
  rt::SnapshotBuffer<Big> sb;
  Big out{9, 9, 9, 9, 9, 9};
  TEST_ASSERT_FALSE(sb.read(out));  // never written
  TEST_ASSERT_EQUAL_UINT32(9, out.a);
  Big in{1, 2, 3, 4, 5, 6};
  sb.write(in);
  TEST_ASSERT_TRUE(sb.read(out));
  TEST_ASSERT_EQUAL_UINT32(1, out.a);
  TEST_ASSERT_EQUAL_UINT32(5, out.e);
  TEST_ASSERT_EQUAL_UINT8(6, out.f);
}

void test_snapshot_sequence_monotonic() {
  rt::SnapshotBuffer<uint16_t> sb;
  uint32_t last = sb.sequence();
  for (uint16_t i = 0; i < 50; i++) {
    sb.write(i);
    uint32_t s = sb.sequence();
    TEST_ASSERT_TRUE(s > last);
    TEST_ASSERT_EQUAL_UINT32(0, s & 1u);
    last = s;
  }
  uint16_t v = 0;
  TEST_ASSERT_TRUE(sb.read(v));
  TEST_ASSERT_EQUAL_UINT16(49, v);
}

struct Wide { uint32_t v[16]; };

void test_snapshot_torture() {
  rt::SnapshotBuffer<Wide> sb;
  std::atomic<bool> stop{false};
  std::atomic<long> bad{0}, good{0};
  std::thread w([&] {
    uint32_t c = 1;
    while (!stop) {
      Wide x;
      for (auto& e : x.v) e = c;
      sb.write(x);
      c++;
    }
  });
  std::vector<std::thread> rs;
  for (int i = 0; i < 3; i++)
    rs.emplace_back([&] {
      uint32_t lastSeen = 0;
      while (!stop) {
        Wide x;
        if (!sb.read(x)) continue;
        good++;
        for (auto e : x.v)
          if (e != x.v[0]) { bad++; break; }
        if (x.v[0] < lastSeen) bad++;  // must not go backwards
        lastSeen = x.v[0];
      }
    });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop = true;
  w.join();
  for (auto& r : rs) r.join();
  TEST_ASSERT_EQUAL_INT(0, (int)bad.load());
  TEST_ASSERT_TRUE(good.load() > 0);
}

void test_queue_full_fifo_wrap_clear() {
  rt::CommandQueue<rt::Command, 4> q;
  TEST_ASSERT_EQUAL_UINT32(4, q.capacity());
  rt::Command c{};
  for (uint8_t i = 0; i < 4; i++) {
    c.tag = i;
    TEST_ASSERT_TRUE(q.push(c));
  }
  c.tag = 99;
  TEST_ASSERT_FALSE(q.push(c));
  TEST_ASSERT_EQUAL_UINT32(4, q.size());
  rt::Command o{};
  for (uint8_t i = 0; i < 4; i++) {
    TEST_ASSERT_TRUE(q.pop(o));
    TEST_ASSERT_EQUAL_UINT8(i, o.tag);
  }
  TEST_ASSERT_FALSE(q.pop(o));
  // wrap around several times
  uint8_t next = 0, expect = 0;
  for (int round = 0; round < 20; round++) {
    for (int k = 0; k < 3; k++) { c.tag = next++; TEST_ASSERT_TRUE(q.push(c)); }
    for (int k = 0; k < 3; k++) { TEST_ASSERT_TRUE(q.pop(o)); TEST_ASSERT_EQUAL_UINT8(expect++, o.tag); }
  }
  q.push(c); q.push(c);
  q.clear();
  TEST_ASSERT_EQUAL_UINT32(0, q.size());
  TEST_ASSERT_FALSE(q.pop(o));
  TEST_ASSERT_TRUE(q.push(c));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_snapshot_roundtrip);
  RUN_TEST(test_snapshot_sequence_monotonic);
  RUN_TEST(test_snapshot_torture);
  RUN_TEST(test_queue_full_fifo_wrap_clear);
  return UNITY_END();
}
