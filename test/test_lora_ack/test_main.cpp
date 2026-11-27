#include <unity.h>
#include "lora_ack.h"
#include "menu_clamp.h"

using rt::LoraAckTracker;

void setUp() {}
void tearDown() {}

void test_new_pending_done_cycle() {
  LoraAckTracker a;
  TEST_ASSERT_EQUAL(0, a.wireLastSeq());
  TEST_ASSERT_TRUE(a.onPacket(5) == LoraAckTracker::Rx::New);
  a.begin(5, 1000);
  TEST_ASSERT_TRUE(a.onPacket(5) == LoraAckTracker::Rx::Running);
  TEST_ASSERT_TRUE(a.onResult(5));
  TEST_ASSERT_EQUAL(5, a.wireLastSeq());
  TEST_ASSERT_TRUE(a.onPacket(5) == LoraAckTracker::Rx::Done);
}

// A(seq 5) pending, B(seq 6) replaces it and finishes first, A's Busy event arrives after.
void test_old_result_never_moves_ack_backwards() {
  LoraAckTracker a;
  a.begin(5, 0);
  a.begin(6, 100);
  TEST_ASSERT_TRUE(a.onResult(6));
  TEST_ASSERT_EQUAL(6, a.wireLastSeq());
  TEST_ASSERT_FALSE(a.onResult(5));  // late Busy of A
  TEST_ASSERT_EQUAL(6, a.wireLastSeq());
  TEST_ASSERT_EQUAL(1, a.staleResults());
  // A's result before B finished is ignored too, B stays pending.
  LoraAckTracker b;
  b.begin(5, 0);
  b.begin(6, 100);
  TEST_ASSERT_FALSE(b.onResult(5));
  TEST_ASSERT_TRUE(b.pending());
  TEST_ASSERT_EQUAL(0, b.wireLastSeq());
}

void test_seq_zero_valid_after_boot() {
  LoraAckTracker a;
  TEST_ASSERT_TRUE(a.onPacket(0) == LoraAckTracker::Rx::New);  // not "already processed"
  a.begin(0, 0);
  TEST_ASSERT_TRUE(a.onResult(0));
  TEST_ASSERT_TRUE(a.onPacket(0) == LoraAckTracker::Rx::Done);
}

void test_seq_wrap() {
  LoraAckTracker a;
  a.begin(0xFFFF, 0);
  TEST_ASSERT_TRUE(a.onResult(0xFFFF));
  TEST_ASSERT_TRUE(a.onPacket(0xFFFF) == LoraAckTracker::Rx::Done);
  TEST_ASSERT_TRUE(a.onPacket(1) == LoraAckTracker::Rx::New);
  a.begin(1, 10);
  TEST_ASSERT_TRUE(a.onResult(1));
  TEST_ASSERT_EQUAL(1, a.wireLastSeq());
}

void test_timeout_answers_failed_once() {
  LoraAckTracker a;
  a.begin(7, 4000000000u);  // near the millis() wrap
  TEST_ASSERT_FALSE(a.checkTimeout(4000000000u + LoraAckTracker::kPendingTimeoutMs - 1));
  TEST_ASSERT_TRUE(a.checkTimeout(4000000000u + LoraAckTracker::kPendingTimeoutMs));
  TEST_ASSERT_FALSE(a.pending());
  TEST_ASSERT_EQUAL(7, a.wireLastSeq());
  TEST_ASSERT_FALSE(a.checkTimeout(4000000000u + 99999));
  TEST_ASSERT_EQUAL(1, a.timeouts());
  TEST_ASSERT_FALSE(a.onResult(7));  // result after the timeout is stale
  TEST_ASSERT_EQUAL(1, a.staleResults());
}

void test_finish_without_pending() {
  LoraAckTracker a;
  a.finish(9);  // invalid command answered at once
  TEST_ASSERT_EQUAL(9, a.wireLastSeq());
  TEST_ASSERT_TRUE(a.onPacket(9) == LoraAckTracker::Rx::Done);
}

void test_menu_clamp() {
  TEST_ASSERT_EQUAL(7, rt::menuClampCount(0, 7));
  TEST_ASSERT_EQUAL(7, rt::menuClampCount(9, 7));
  TEST_ASSERT_EQUAL(5, rt::menuClampCount(5, 7));
  TEST_ASSERT_EQUAL(4, rt::menuClampSelected(6, 5));  // BLE item selected, BLE disappears
  TEST_ASSERT_EQUAL(4, rt::menuClampSelected(4, 5));
  TEST_ASSERT_EQUAL(2, rt::menuClampSelected(2, 5));
}

// Item numbers as in MenuItem: 0 Start .. 4 QueryStatus, 5 BlePair, 6 BleClear, 7 RangeTest.
void test_menu_mask() {
  const uint8_t kMax = 8;
  const uint16_t common = 0x1F, ble = 0x60, range = 0x80;
  // Sender without BLE: five common items plus the range test, in item order.
  const uint16_t m = common | range;
  TEST_ASSERT_EQUAL(6, rt::menuMaskCount(m, kMax));
  TEST_ASSERT_EQUAL(7, rt::menuMaskItemAt(m, kMax, 5));  // last visible = RangeTest
  TEST_ASSERT_EQUAL(0xFF, rt::menuMaskItemAt(m, kMax, 6));
  TEST_ASSERT_EQUAL(5, rt::menuMaskPosOf(m, kMax, 7));
  TEST_ASSERT_EQUAL(0xFF, rt::menuMaskPosOf(m, kMax, 5));  // BLE hidden
  // The next press wraps around over the hidden BLE items.
  TEST_ASSERT_EQUAL(7, rt::menuMaskNext(m, kMax, 4));
  TEST_ASSERT_EQUAL(0, rt::menuMaskNext(m, kMax, 7));
  // Receiver: common + BLE, no range test.
  const uint16_t r = common | ble;
  TEST_ASSERT_EQUAL(7, rt::menuMaskCount(r, kMax));
  TEST_ASSERT_EQUAL(0, rt::menuMaskNext(r, kMax, 6));
  TEST_ASSERT_EQUAL(0xFF, rt::menuMaskPosOf(r, kMax, 7));
  // An item that becomes hidden while it is selected falls back to the first visible one.
  TEST_ASSERT_EQUAL(0, rt::menuMaskFix(common, kMax, 6));
  TEST_ASSERT_EQUAL(3, rt::menuMaskFix(common, kMax, 3));
  // A mask without any valid bit shows everything; bits above the item count are ignored.
  TEST_ASSERT_EQUAL(8, rt::menuMaskCount(0, kMax));
  TEST_ASSERT_EQUAL(8, rt::menuMaskCount(0xFF00, kMax));
  TEST_ASSERT_EQUAL(1, rt::menuMaskCount(0xFF01, kMax));
  // A hidden item as the start of "next" gives the first visible item.
  TEST_ASSERT_EQUAL(0, rt::menuMaskNext(common, kMax, 6));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_new_pending_done_cycle);
  RUN_TEST(test_old_result_never_moves_ack_backwards);
  RUN_TEST(test_seq_zero_valid_after_boot);
  RUN_TEST(test_seq_wrap);
  RUN_TEST(test_timeout_answers_failed_once);
  RUN_TEST(test_finish_without_pending);
  RUN_TEST(test_menu_clamp);
  RUN_TEST(test_menu_mask);
  return UNITY_END();
}
