#include <unity.h>
#include "send_policy.h"

using namespace wh;

static const uint32_t S = 1000UL;
static const uint32_t MIN = 60000UL;

void setUp() {}
void tearDown() {}

// Window opened at t=0 (a sender packet arrived), nothing sent yet.
static SendInputs base(uint32_t now) {
  SendInputs i;
  i.now = now;
  i.everRequested = true;
  i.lastSenderRequestMs = 0;
  i.lastTxMs = 0;
  i.everSent = true;
  return i;
}

void test_no_send_before_first_request() {
  SendInputs i;
  i.now = 100000;
  i.everRequested = false;
  i.eventPending = true;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
  i.everSent = false;
  i.heaterRunning = true;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
}

void test_first_request_opens_window_force_sends() {
  SendInputs i;
  i.now = 5000;
  i.everRequested = true;
  i.lastSenderRequestMs = 5000;
  i.forceNow = true;
  i.everSent = true;
  i.lastTxMs = 4999;  // just sent: force ignores the gap
  TEST_ASSERT_EQUAL((int)SendReason::Event, (int)shouldSendStatus(i));
}

void test_first_periodic_allowed_at_once_if_never_sent() {
  SendInputs i;
  i.now = 1000;
  i.everRequested = true;
  i.lastSenderRequestMs = 1000;
  i.everSent = false;
  TEST_ASSERT_EQUAL((int)SendReason::Periodic, (int)shouldSendStatus(i));
}

void test_window_boundary() {
  SendInputs i = base(30 * MIN - 1);
  i.lastTxMs = 0;
  i.forceNow = true;
  TEST_ASSERT_EQUAL((int)SendReason::Event, (int)shouldSendStatus(i));
  i.now = 30 * MIN;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
  i.now = 30 * MIN + 1;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
}

void test_no_send_outside_window_even_events() {
  SendInputs i = base(40 * MIN);
  i.eventPending = true;
  i.heaterRunning = true;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
  i.eventPending = false;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
  i.lastTxMs = 0;
  i.everSent = false;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
}

void test_periodic_running_15s() {
  SendInputs i = base(14999);
  i.lastSenderRequestMs = 0;
  i.heaterRunning = true;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
  i.now = 15 * S;
  TEST_ASSERT_EQUAL((int)SendReason::Periodic, (int)shouldSendStatus(i));
}

void test_periodic_climate_30s() {
  SendInputs i = base(29999);
  i.climateActive = true;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
  i.now = 30 * S;
  TEST_ASSERT_EQUAL((int)SendReason::Periodic, (int)shouldSendStatus(i));
}

void test_running_wins_over_climate() {
  SendInputs i = base(15 * S);
  i.climateActive = true;
  i.heaterRunning = true;
  TEST_ASSERT_EQUAL((int)SendReason::Periodic, (int)shouldSendStatus(i));
}

void test_periodic_idle_120s() {
  SendInputs i = base(119999);
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
  i.now = 120 * S;
  TEST_ASSERT_EQUAL((int)SendReason::Periodic, (int)shouldSendStatus(i));
}

void test_event_min_gap() {
  SendInputs i = base(4999);
  i.eventPending = true;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
  i.now = 5000;
  TEST_ASSERT_EQUAL((int)SendReason::Event, (int)shouldSendStatus(i));
}

void test_event_first_send_ignores_gap() {
  SendInputs i = base(1000);
  i.everSent = false;
  i.eventPending = true;
  TEST_ASSERT_EQUAL((int)SendReason::Event, (int)shouldSendStatus(i));
}

void test_force_ignores_gap() {
  SendInputs i = base(1000);
  i.lastTxMs = 900;
  i.forceNow = true;
  TEST_ASSERT_EQUAL((int)SendReason::Event, (int)shouldSendStatus(i));
}

void test_force_without_any_request_sends_ack() {
  SendInputs i;
  i.now = 10;
  i.forceNow = true;
  TEST_ASSERT_EQUAL((int)SendReason::Event, (int)shouldSendStatus(i));
}

void test_custom_params() {
  SendPolicyParams p;
  p.senderActiveMs = 1000;
  p.idleMs = 200;
  SendInputs i = base(200);
  TEST_ASSERT_EQUAL((int)SendReason::Periodic, (int)shouldSendStatus(i, p));
  i.now = 1000;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i, p));
}

void test_wrap_around() {
  const uint32_t t0 = 0xFFFFFFFFUL - 5000UL;
  SendInputs i;
  i.everRequested = true;
  i.lastSenderRequestMs = t0;
  i.lastTxMs = t0;
  i.everSent = true;
  i.heaterRunning = true;
  i.now = t0 + 14999;  // wrapped
  TEST_ASSERT_TRUE(i.now < t0);
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
  i.now = t0 + 15000;
  TEST_ASSERT_EQUAL((int)SendReason::Periodic, (int)shouldSendStatus(i));
  i.now = t0 + 30 * MIN - 1;
  i.lastTxMs = i.now - 20000;
  TEST_ASSERT_EQUAL((int)SendReason::Periodic, (int)shouldSendStatus(i));
  i.now = t0 + 30 * MIN;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
  i.now = t0 + 30 * MIN;
  i.forceNow = true;
  TEST_ASSERT_EQUAL((int)SendReason::None, (int)shouldSendStatus(i));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_send_before_first_request);
  RUN_TEST(test_first_request_opens_window_force_sends);
  RUN_TEST(test_first_periodic_allowed_at_once_if_never_sent);
  RUN_TEST(test_window_boundary);
  RUN_TEST(test_no_send_outside_window_even_events);
  RUN_TEST(test_periodic_running_15s);
  RUN_TEST(test_periodic_climate_30s);
  RUN_TEST(test_running_wins_over_climate);
  RUN_TEST(test_periodic_idle_120s);
  RUN_TEST(test_event_min_gap);
  RUN_TEST(test_event_first_send_ignores_gap);
  RUN_TEST(test_force_ignores_gap);
  RUN_TEST(test_force_without_any_request_sends_ack);
  RUN_TEST(test_custom_params);
  RUN_TEST(test_wrap_around);
  return UNITY_END();
}
