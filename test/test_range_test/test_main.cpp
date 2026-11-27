#include <unity.h>

#include "range_test.h"

using wh::RangeParams;
using wh::RangeTest;

void setUp() {}
void tearDown() {}

static RangeParams P() {
  RangeParams p;
  p.intervalMs = 5000;
  p.timeoutMs = 2000;
  return p;
}

void test_first_ping_is_due_at_once_then_after_the_interval() {
  RangeTest t;
  t.start(1000, P());
  TEST_ASSERT_TRUE(t.due(1000));
  t.onSent(10, 1000);
  TEST_ASSERT_FALSE(t.due(1001));  // in flight
  TEST_ASSERT_TRUE(t.onPong(10, -90, 5, -95, 3, 1900));
  TEST_ASSERT_FALSE(t.due(5999));  // start-to-start interval, not end-to-start
  TEST_ASSERT_TRUE(t.due(6000));
}

void test_ok_updates_series_and_rtt() {
  RangeTest t;
  t.start(0, P());
  t.onSent(1, 0);
  TEST_ASSERT_TRUE(t.onPong(1, -100, 4, -90, 6, 1500));
  t.onSent(2, 5000);
  TEST_ASSERT_TRUE(t.onPong(2, -110, -2, -92, 5, 6700));
  const wh::RangeStats& s = t.stats();
  TEST_ASSERT_EQUAL_UINT32(2, s.sent);
  TEST_ASSERT_EQUAL_UINT32(2, s.ok);
  TEST_ASSERT_EQUAL_INT32(-110, s.upRssi.min);
  TEST_ASSERT_EQUAL_INT32(-100, s.upRssi.max);
  TEST_ASSERT_EQUAL_INT32(-105, s.upRssi.avg());
  TEST_ASSERT_EQUAL_INT32(-110, s.upRssi.last);
  TEST_ASSERT_EQUAL_INT32(-2, s.upSnr.min);
  TEST_ASSERT_EQUAL_INT32(-92, s.downRssi.min);
  TEST_ASSERT_EQUAL_INT32(1500, s.rttMs.min);
  TEST_ASSERT_EQUAL_INT32(1700, s.rttMs.max);
  TEST_ASSERT_EQUAL_UINT32(0, s.lossPermille());
}

void test_timeout_counts_lost_and_streak() {
  RangeTest t;
  t.start(0, P());
  t.onSent(1, 0);
  TEST_ASSERT_TRUE(t.poll(1999) == RangeTest::Event::None);
  TEST_ASSERT_TRUE(t.poll(2000) == RangeTest::Event::Lost);
  TEST_ASSERT_FALSE(t.pending());
  t.onSent(2, 5000);
  TEST_ASSERT_TRUE(t.poll(7000) == RangeTest::Event::Lost);
  TEST_ASSERT_EQUAL_UINT32(2, t.stats().lostStreak);
  t.onSent(3, 10000);
  TEST_ASSERT_TRUE(t.onPong(3, -100, 0, -100, 0, 10500));
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().lostStreak);
  TEST_ASSERT_EQUAL_UINT32(2, t.stats().maxLostStreak);
  TEST_ASSERT_EQUAL_UINT32(667, t.stats().lossPermille());  // 2 lost of 3 finished
}

void test_late_pong_is_not_counted_as_ok() {
  RangeTest t;
  t.start(0, P());
  t.onSent(7, 0);
  TEST_ASSERT_TRUE(t.poll(2500) == RangeTest::Event::Lost);
  TEST_ASSERT_FALSE(t.onPong(7, -100, 0, -100, 0, 3000));
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().late);
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().ok);
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().lost);
  // a second copy of the same late pong is stray, not late again
  TEST_ASSERT_FALSE(t.onPong(7, -100, 0, -100, 0, 3100));
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().late);
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().stray);
}

void test_stray_pong_for_an_unknown_seq() {
  RangeTest t;
  t.start(0, P());
  t.onSent(5, 0);
  TEST_ASSERT_FALSE(t.onPong(99, -100, 0, -100, 0, 500));
  TEST_ASSERT_TRUE(t.pending());  // the real ping is still waiting
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().stray);
}

void test_max_pings_finishes_after_the_last_answer() {
  RangeParams p = P();
  p.maxPings = 2;
  RangeTest t;
  t.start(0, p);
  t.onSent(1, 0);
  TEST_ASSERT_TRUE(t.onPong(1, -90, 5, -90, 5, 800));
  TEST_ASSERT_TRUE(t.poll(1000) == RangeTest::Event::None);
  t.onSent(2, 5000);
  TEST_ASSERT_FALSE(t.due(20000));  // no third ping
  TEST_ASSERT_TRUE(t.poll(5100) == RangeTest::Event::None);  // still waiting for the pong
  TEST_ASSERT_TRUE(t.onPong(2, -90, 5, -90, 5, 5800));
  TEST_ASSERT_TRUE(t.poll(5900) == RangeTest::Event::Finished);
  TEST_ASSERT_FALSE(t.active());
}

void test_max_pings_with_a_lost_last_ping_finishes_once() {
  RangeParams p = P();
  p.maxPings = 1;
  RangeTest t;
  t.start(0, p);
  t.onSent(1, 0);
  TEST_ASSERT_TRUE(t.poll(2000) == RangeTest::Event::Finished);
  TEST_ASSERT_EQUAL_UINT32(1, t.stats().lost);
  TEST_ASSERT_TRUE(t.poll(2100) == RangeTest::Event::None);
}

void test_stop_clears_the_ping_in_flight() {
  RangeTest t;
  t.start(0, P());
  t.onSent(1, 0);
  t.stop();
  TEST_ASSERT_FALSE(t.active());
  TEST_ASSERT_FALSE(t.pending());
  TEST_ASSERT_FALSE(t.due(100000));
  TEST_ASSERT_FALSE(t.onPong(1, -90, 5, -90, 5, 100));  // ignored while stopped
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().ok);
}

void test_millis_wrap() {
  RangeTest t;
  const uint32_t t0 = 0xFFFFFFFFu - 1000;
  t.start(t0, P());
  t.onSent(1, t0);
  TEST_ASSERT_TRUE(t.poll(t0 + 2000) == RangeTest::Event::Lost);  // crosses the wrap
  TEST_ASSERT_FALSE(t.due(t0 + 4999));
  TEST_ASSERT_TRUE(t.due(t0 + 5000));
}

void test_restart_resets_statistics() {
  RangeTest t;
  t.start(0, P());
  t.onSent(1, 0);
  t.onPong(1, -90, 5, -90, 5, 100);
  t.start(50000, P());
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().sent);
  TEST_ASSERT_EQUAL_UINT32(0, t.stats().ok);
  TEST_ASSERT_TRUE(t.due(50000));
}

void test_params_for_the_long_range_profile() {
  // SF11, BW125, CR4/7: ping 8 bytes on air (0.56 s), pong 12 bytes (0.68 s)
  const RangeParams p = wh::rangeParamsFor(11, 125000, 7, 8, 12);
  TEST_ASSERT_UINT32_WITHIN(60, 2500, p.timeoutMs);
  // duty cycle: interval >= 11 x longest airtime, about 7.4 s
  TEST_ASSERT_TRUE(p.intervalMs >= 7400);
  TEST_ASSERT_TRUE(p.intervalMs <= 7600);
  TEST_ASSERT_TRUE(p.intervalMs > p.timeoutMs);
}

void test_params_for_a_fast_profile_are_limited_by_the_cycle() {
  // SF7: the airtime is tiny, so the cycle time (timeout + 0.5 s) decides
  const RangeParams p = wh::rangeParamsFor(7, 125000, 5, 8, 12);
  TEST_ASSERT_EQUAL_UINT32(p.timeoutMs + 500, p.intervalMs);
}

void test_params_for_invalid_radio_settings_keep_the_defaults() {
  const RangeParams d;
  const RangeParams p = wh::rangeParamsFor(3, 125000, 7, 8, 12);
  TEST_ASSERT_EQUAL_UINT32(d.intervalMs, p.intervalMs);
  TEST_ASSERT_EQUAL_UINT32(d.timeoutMs, p.timeoutMs);
}

void test_series_with_no_samples() {
  wh::Series s;
  TEST_ASSERT_EQUAL_INT32(0, s.avg());
  TEST_ASSERT_EQUAL_UINT32(0, wh::RangeStats{}.lossPermille());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_first_ping_is_due_at_once_then_after_the_interval);
  RUN_TEST(test_ok_updates_series_and_rtt);
  RUN_TEST(test_timeout_counts_lost_and_streak);
  RUN_TEST(test_late_pong_is_not_counted_as_ok);
  RUN_TEST(test_stray_pong_for_an_unknown_seq);
  RUN_TEST(test_max_pings_finishes_after_the_last_answer);
  RUN_TEST(test_max_pings_with_a_lost_last_ping_finishes_once);
  RUN_TEST(test_stop_clears_the_ping_in_flight);
  RUN_TEST(test_millis_wrap);
  RUN_TEST(test_restart_resets_statistics);
  RUN_TEST(test_params_for_the_long_range_profile);
  RUN_TEST(test_params_for_a_fast_profile_are_limited_by_the_cycle);
  RUN_TEST(test_params_for_invalid_radio_settings_keep_the_defaults);
  RUN_TEST(test_series_with_no_samples);
  return UNITY_END();
}
