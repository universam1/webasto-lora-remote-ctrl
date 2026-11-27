#include <unity.h>

#include "lora_airtime.h"

void setUp() {}
void tearDown() {}

// Reference values: Semtech LoRa calculator, explicit header, CRC on, preamble 8.
static void test_airtime_sf11_bw125_cr47() {
  // 12-byte command and 27-byte status at SF11/BW125/CR4-7 (the LORA_RANGE_LONG profile)
  TEST_ASSERT_UINT32_WITHIN(2000, 676000, lora::airtimeUs(12, 11, 125000, 7));
  TEST_ASSERT_UINT32_WITHIN(2000, 1020000, lora::airtimeUs(27, 11, 125000, 7));
}

static void test_airtime_sf7_bw125_cr45() {
  // SF7 without low data rate optimisation: 12 bytes = 41.2 ms
  TEST_ASSERT_UINT32_WITHIN(500, 41216, lora::airtimeUs(12, 7, 125000, 5));
}

static void test_airtime_invalid() {
  TEST_ASSERT_EQUAL_UINT32(0, lora::airtimeUs(12, 5, 125000, 7));
  TEST_ASSERT_EQUAL_UINT32(0, lora::airtimeUs(12, 11, 0, 7));
  TEST_ASSERT_EQUAL_UINT32(0, lora::airtimeUs(12, 11, 125000, 4));
}

static void test_retry_leaves_room_for_reply() {
  const uint32_t cmd = lora::airtimeUs(12, 11, 125000, 7), reply = lora::airtimeUs(27, 11, 125000, 7);
  const uint32_t retry = lora::commandRetryMs(cmd, reply);
  // the old fixed value (1.3 s, minus 25 % jitter = 0.975 s) is shorter than command + reply
  TEST_ASSERT_TRUE(975 < (cmd + reply) / 1000);
  TEST_ASSERT_TRUE(retry >= (cmd + reply) / 1000 + 250);
  TEST_ASSERT_UINT32_WITHIN(20, 2246, retry);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_airtime_sf11_bw125_cr47);
  RUN_TEST(test_airtime_sf7_bw125_cr45);
  RUN_TEST(test_airtime_invalid);
  RUN_TEST(test_retry_leaves_room_for_reply);
  return UNITY_END();
}
