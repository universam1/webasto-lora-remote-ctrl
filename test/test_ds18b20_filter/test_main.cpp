#include <unity.h>

#include "ds18b20_filter.h"

using namespace wh;

void setUp() {}
void tearDown() {}

void test_rejects_implausible() {
  TEST_ASSERT_FALSE(Ds18b20Filter::plausible(-127.0f));
  TEST_ASSERT_FALSE(Ds18b20Filter::plausible(85.0f));
  TEST_ASSERT_FALSE(Ds18b20Filter::plausible(-40.5f));
  TEST_ASSERT_FALSE(Ds18b20Filter::plausible(60.5f));
  TEST_ASSERT_FALSE(Ds18b20Filter::plausible(__builtin_nanf("")));
  TEST_ASSERT_TRUE(Ds18b20Filter::plausible(-40.0f));
  TEST_ASSERT_TRUE(Ds18b20Filter::plausible(60.0f));
  TEST_ASSERT_TRUE(Ds18b20Filter::plausible(21.5f));
}

void test_valid_after_two_good() {
  Ds18b20Filter f;
  TEST_ASSERT_FALSE(f.valid(0));
  TEST_ASSERT_TRUE(f.addSample(20.0f, 1000));
  TEST_ASSERT_FALSE(f.valid(1000));
  TEST_ASSERT_TRUE(f.addSample(20.0f, 6000));
  TEST_ASSERT_TRUE(f.valid(6000));
  TEST_ASSERT_EQUAL_INT16(200, f.temp_dC());
}

void test_bad_samples_do_not_count() {
  Ds18b20Filter f;
  TEST_ASSERT_FALSE(f.addSample(85.0f, 0));
  TEST_ASSERT_FALSE(f.addSample(-127.0f, 0));
  TEST_ASSERT_TRUE(f.addSample(20.0f, 0));
  TEST_ASSERT_FALSE(f.valid(0));
  TEST_ASSERT_TRUE(f.addSample(20.0f, 0));
  TEST_ASSERT_TRUE(f.valid(0));
}

void test_median_removes_spike() {
  Ds18b20Filter f;
  f.addSample(20.0f, 0);
  f.addSample(20.0f, 5000);
  f.addSample(30.0f, 10000);  // median of (20,20,30) = 20
  TEST_ASSERT_EQUAL_INT16(200, f.temp_dC());
}

void test_ema_smooths_step() {
  Ds18b20Filter f;
  for (int i = 0; i < 3; i++) f.addSample(20.0f, i * 5000);
  f.addSample(24.0f, 15000);  // median(20,20,24)=20 -> ema 20
  TEST_ASSERT_EQUAL_INT16(200, f.temp_dC());
  f.addSample(24.0f, 20000);  // median(20,24,24)=24 -> ema 21.2
  TEST_ASSERT_EQUAL_INT16(212, f.temp_dC());
  f.addSample(24.0f, 25000);  // 22.04
  TEST_ASSERT_EQUAL_INT16(220, f.temp_dC());
}

void test_three_failures_invalidate_and_need_two_good() {
  Ds18b20Filter f;
  f.addSample(20.0f, 0);
  f.addSample(20.0f, 5000);
  TEST_ASSERT_TRUE(f.valid(5000));
  f.addFailure(10000);
  f.addFailure(15000);
  TEST_ASSERT_TRUE(f.valid(15000));
  f.addFailure(20000);
  TEST_ASSERT_FALSE(f.valid(20000));
  f.addSample(22.0f, 25000);
  TEST_ASSERT_FALSE(f.valid(25000));
  f.addSample(22.0f, 30000);
  TEST_ASSERT_TRUE(f.valid(30000));
  TEST_ASSERT_EQUAL_INT16(220, f.temp_dC());  // history was cleared
}

void test_good_sample_resets_failure_count() {
  Ds18b20Filter f;
  f.addSample(20.0f, 0);
  f.addSample(20.0f, 5000);
  f.addFailure(10000);
  f.addFailure(15000);
  f.addSample(20.0f, 20000);
  f.addFailure(25000);
  f.addFailure(30000);
  TEST_ASSERT_TRUE(f.valid(30000));
}

void test_invalid_after_60s_without_good() {
  Ds18b20Filter f;
  f.addSample(20.0f, 1000);
  f.addSample(20.0f, 6000);
  TEST_ASSERT_TRUE(f.valid(66000));
  TEST_ASSERT_FALSE(f.valid(66001));
}

void test_clock_wrap() {
  Ds18b20Filter f;
  const uint32_t t0 = 0xFFFFFFFFu - 2000u;
  f.addSample(20.0f, t0);
  f.addSample(20.0f, t0 + 1000u);
  TEST_ASSERT_TRUE(f.valid(t0 + 20000u));
  TEST_ASSERT_FALSE(f.valid(t0 + 70000u));
}

void test_negative_rounding() {
  Ds18b20Filter f;
  f.addSample(-5.26f, 0);
  f.addSample(-5.26f, 5000);
  TEST_ASSERT_EQUAL_INT16(-53, f.temp_dC());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_rejects_implausible);
  RUN_TEST(test_valid_after_two_good);
  RUN_TEST(test_bad_samples_do_not_count);
  RUN_TEST(test_median_removes_spike);
  RUN_TEST(test_ema_smooths_step);
  RUN_TEST(test_three_failures_invalidate_and_need_two_good);
  RUN_TEST(test_good_sample_resets_failure_count);
  RUN_TEST(test_invalid_after_60s_without_good);
  RUN_TEST(test_clock_wrap);
  RUN_TEST(test_negative_rounding);
  return UNITY_END();
}
