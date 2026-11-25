#include <unity.h>
#include "climate_controller.h"

using namespace wh;

static const uint32_t MIN = 60000UL;

static ClimateController* c;

void setUp() {
  c = new ClimateController();
  ClimateSettings s;
  s.targetC = 20;
  s.durationMin = 120;
  c->configure(s);
}
void tearDown() { delete c; }

static ClimateInputs in(uint32_t now, int16_t t_dC = 100, HeaterPhase ph = HeaterPhase::Off) {
  ClimateInputs i;
  i.now = now;
  i.interiorValid = true;
  i.interior_dC = t_dC;
  i.phase = ph;
  i.phaseTrusted = true;
  i.errorCode = 0;
  i.voltageValid = false;
  i.heaterBusy = false;
  i.transportHealthy = true;
  return i;
}

// Performs one start attempt at time t; returns true if a start was requested.
static bool tryStart(uint32_t t, int16_t temp = 100) { return c->step(in(t, temp)).requestStart; }

void test_start_below_target() {
  c->enable(1000);
  ClimateOutput o = c->step(in(1000, 150));
  TEST_ASSERT_TRUE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Starting, (int)o.status);
}

void test_no_start_at_or_above_target() {
  c->enable(0);
  TEST_ASSERT_FALSE(c->step(in(0, 200)).requestStart);
  ClimateOutput o = c->step(in(100, 215));
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Idle, (int)o.status);
  TEST_ASSERT_TRUE(c->step(in(200, 199)).requestStart);
}

void test_disabled_never_starts() {
  ClimateOutput o = c->step(in(0, 50));
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Off, (int)o.status);
}

void test_hysteresis() {
  ClimateParams p;
  p.hyst_dC = 10;
  ClimateController h(p);
  h.configure(ClimateSettings());
  h.enable(0);
  TEST_ASSERT_FALSE(h.step(in(0, 195)).requestStart);
  TEST_ASSERT_TRUE(h.step(in(100, 189)).requestStart);
}

void test_target_clamps() {
  ClimateSettings s;
  s.targetC = 5;
  c->configure(s);
  TEST_ASSERT_EQUAL(10, c->settings().targetC);
  s.targetC = 30;
  c->configure(s);
  TEST_ASSERT_EQUAL(24, c->settings().targetC);
  s.targetC = 24;
  c->configure(s);
  TEST_ASSERT_EQUAL(24, c->settings().targetC);
  c->enable(0);
  TEST_ASSERT_TRUE(c->step(in(0, 239)).requestStart);
}

void test_duration_clamps() {
  ClimateSettings s;
  s.targetC = 20;
  s.durationMin = 1;
  c->configure(s);
  TEST_ASSERT_EQUAL(10, c->settings().durationMin);
  s.durationMin = 1000;
  c->configure(s);
  TEST_ASSERT_EQUAL(480, c->settings().durationMin);
}

void test_requestStart_exactly_once() {
  c->enable(0);
  TEST_ASSERT_TRUE(c->step(in(0)).requestStart);
  for (uint32_t t = 100; t < 5000; t += 100) {
    ClimateOutput o = c->step(in(t));
    TEST_ASSERT_FALSE(o.requestStart);
    TEST_ASSERT_EQUAL((int)ClimateStatus::Starting, (int)o.status);
  }
  c->notifyStartResult(5000, CmdResult::Ok);
  ClimateOutput o = c->step(in(5100));
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Starting, (int)o.status);
  o = c->step(in(6000, 100, HeaterPhase::Heating));
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::HeaterRunning, (int)o.status);
}

void test_min_pause_after_off() {
  c->enable(0);
  // Heater was running (seen Off before), then turns off after a long run.
  c->step(in(0, 250, HeaterPhase::Off));
  c->step(in(1000, 250, HeaterPhase::Heating));
  uint32_t off = 30 * MIN;
  c->step(in(off - 100, 250, HeaterPhase::Heating));
  ClimateOutput o = c->step(in(off, 100, HeaterPhase::Off));
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Pause, (int)o.status);
  o = c->step(in(off + 2 * MIN - 1, 100));
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Pause, (int)o.status);
  TEST_ASSERT_TRUE(c->step(in(off + 2 * MIN, 100)).requestStart);
}

void test_early_off_pause_session_stays_active() {
  c->enable(0);
  c->step(in(0, 100, HeaterPhase::Off));
  c->step(in(1000, 100, HeaterPhase::Heating));
  uint32_t off = 10 * MIN;  // shorter than 25 min, not our stop
  ClimateOutput o = c->step(in(off, 100, HeaterPhase::Off));
  TEST_ASSERT_EQUAL((int)ClimateStatus::Pause, (int)o.status);
  // after the 2 min minimum pause still paused
  TEST_ASSERT_FALSE(c->step(in(off + 5 * MIN, 100)).requestStart);
  TEST_ASSERT_FALSE(c->step(in(off + 59 * MIN, 100)).requestStart);
  TEST_ASSERT_TRUE(c->enabled());
  // 60 min over: session (120 min) still active at 70 min
  o = c->step(in(off + 60 * MIN, 100));
  TEST_ASSERT_TRUE(o.requestStart);
  TEST_ASSERT_TRUE(c->enabled());
}

void test_long_run_off_is_no_early_stop() {
  c->enable(0);
  c->step(in(0, 100, HeaterPhase::Off));
  c->step(in(1000, 100, HeaterPhase::Heating));
  uint32_t off = 31 * MIN;
  c->step(in(off, 100, HeaterPhase::Off));
  TEST_ASSERT_TRUE(c->step(in(off + 2 * MIN, 100)).requestStart);
}

void test_run_on_counts_as_running() {
  c->enable(0);
  c->step(in(0, 100, HeaterPhase::Off));
  c->step(in(1000, 100, HeaterPhase::Heating));
  c->step(in(20 * MIN, 100, HeaterPhase::RunOn));
  ClimateOutput o = c->step(in(21 * MIN, 100, HeaterPhase::Off));  // 20 min run: early
  TEST_ASSERT_FALSE(c->step(in(30 * MIN, 100)).requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Pause, (int)o.status);
}

void test_user_stop_no_external_pause() {
  c->enable(0);
  c->step(in(0, 100, HeaterPhase::Off));
  c->step(in(1000, 100, HeaterPhase::Heating));
  c->notifyUserStop(5 * MIN);
  TEST_ASSERT_FALSE(c->enabled());
  c->step(in(5 * MIN + 1000, 100, HeaterPhase::Off));
  c->enable(5 * MIN + 2000);
  // only the 2 min minimum pause applies
  TEST_ASSERT_FALSE(c->step(in(5 * MIN + 3000, 100)).requestStart);
  TEST_ASSERT_TRUE(c->step(in(7 * MIN + 1000, 100)).requestStart);
}

void test_failures_backoff_and_latch() {
  c->enable(0);
  uint32_t t = 0;
  TEST_ASSERT_TRUE(tryStart(t));
  c->notifyStartResult(t + 500, CmdResult::NotAdopted);
  t += 500;
  ClimateOutput o = c->step(in(t));
  TEST_ASSERT_EQUAL((int)ClimateStatus::Pause, (int)o.status);
  TEST_ASSERT_FALSE(c->step(in(t + 2 * MIN - 1)).requestStart);
  t += 2 * MIN;
  TEST_ASSERT_TRUE(tryStart(t));
  c->notifyStartResult(t, CmdResult::Timeout);
  TEST_ASSERT_FALSE(c->step(in(t + 5 * MIN - 1)).requestStart);
  t += 5 * MIN;
  TEST_ASSERT_TRUE(tryStart(t));
  c->notifyStartResult(t, CmdResult::NoBus);
  o = c->step(in(t + 1));
  TEST_ASSERT_EQUAL((int)ClimateStatus::Failed, (int)o.status);
  TEST_ASSERT_FALSE(c->step(in(t + 60 * MIN)).requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Failed, (int)c->step(in(t + 61 * MIN)).status);
  // cleared by disable + enable
  c->disable();
  c->enable(t + 62 * MIN);
  TEST_ASSERT_TRUE(c->step(in(t + 62 * MIN)).requestStart);
}

void test_busoff_counts_as_failure() {
  c->enable(0);
  TEST_ASSERT_TRUE(tryStart(0));
  c->notifyStartResult(10, CmdResult::BusOff);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Pause, (int)c->step(in(20)).status);
}

void test_unsupported_is_no_failure() {
  c->enable(0);
  TEST_ASSERT_TRUE(tryStart(0));
  for (int n = 1; n <= 5; n++) {
    // The result clears the attempt; the next step may start again (not a start, not a failure).
    c->notifyStartResult(n * 1000 - 500, CmdResult::Unsupported);
    ClimateOutput o = c->step(in(n * 1000));
    TEST_ASSERT_TRUE(o.requestStart);
    TEST_ASSERT_NOT_EQUAL((int)ClimateStatus::Failed, (int)o.status);
    TEST_ASSERT_NOT_EQUAL((int)ClimateStatus::Pause, (int)o.status);
  }
}

void test_success_resets_counters() {
  c->enable(0);
  uint32_t t = 0;
  tryStart(t);
  c->notifyStartResult(t, CmdResult::NotAdopted);
  t += 2 * MIN;
  tryStart(t);
  c->notifyStartResult(t, CmdResult::NotAdopted);  // 2 failures
  t += 5 * MIN;
  TEST_ASSERT_TRUE(tryStart(t));
  c->notifyStartResult(t, CmdResult::Ok);
  c->step(in(t + 1000, 100, HeaterPhase::Heating));  // counter reset
  t += 40 * MIN;
  c->step(in(t, 100, HeaterPhase::Off));
  t += 2 * MIN;
  TEST_ASSERT_TRUE(tryStart(t));
  c->notifyStartResult(t, CmdResult::NotAdopted);  // failure 1 again: back-off 2 min, no latch
  TEST_ASSERT_EQUAL((int)ClimateStatus::Pause, (int)c->step(in(t + 1)).status);
  t += 2 * MIN;
  TEST_ASSERT_TRUE(tryStart(t));
}

void test_ok_without_heating_is_failure() {
  c->enable(0);
  TEST_ASSERT_TRUE(tryStart(0));
  c->notifyStartResult(1000, CmdResult::Ok);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Starting, (int)c->step(in(80000)).status);
  ClimateOutput o = c->step(in(91000));
  TEST_ASSERT_EQUAL((int)ClimateStatus::Pause, (int)o.status);
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_TRUE(c->step(in(91000 + 2 * MIN)).requestStart);
}

void test_ok_unconfirmed_without_heating_is_failure() {
  c->enable(0);
  tryStart(0);
  c->notifyStartResult(0, CmdResult::OkUnconfirmed);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Pause, (int)c->step(in(90000)).status);
}

void test_ok_then_heating_in_time() {
  c->enable(0);
  tryStart(0);
  c->notifyStartResult(0, CmdResult::Ok);
  ClimateOutput o = c->step(in(30000, 100, HeaterPhase::Heating));
  TEST_ASSERT_EQUAL((int)ClimateStatus::HeaterRunning, (int)o.status);
  o = c->step(in(200000, 100, HeaterPhase::Heating));
  TEST_ASSERT_EQUAL((int)ClimateStatus::HeaterRunning, (int)o.status);
}

void test_heater_error_blocks() {
  c->enable(0);
  ClimateInputs i = in(0);
  i.errorCode = 3;
  ClimateOutput o = c->step(i);
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Idle, (int)o.status);
  i.now = 1000;
  i.errorCode = 0;
  TEST_ASSERT_TRUE(c->step(i).requestStart);
}

void test_error_during_start_attempt_fails() {
  c->enable(0);
  TEST_ASSERT_TRUE(tryStart(0));
  ClimateInputs i = in(1000);
  i.errorCode = 2;
  TEST_ASSERT_EQUAL((int)ClimateStatus::Failed, (int)c->step(i).status);
}

void test_blockers() {
  c->enable(0);
  ClimateInputs i = in(0);
  i.heaterBusy = true;
  TEST_ASSERT_FALSE(c->step(i).requestStart);
  i = in(100);
  i.transportHealthy = false;
  TEST_ASSERT_FALSE(c->step(i).requestStart);
  i = in(200);
  i.phaseTrusted = false;
  TEST_ASSERT_FALSE(c->step(i).requestStart);
  i = in(300, 100, HeaterPhase::Unknown);
  TEST_ASSERT_FALSE(c->step(i).requestStart);
  i = in(400, 100, HeaterPhase::Ventilating);
  TEST_ASSERT_FALSE(c->step(i).requestStart);
  TEST_ASSERT_TRUE(c->step(in(500)).requestStart);
}

void test_sensor_invalid_grace_then_fault() {
  c->enable(0);
  ClimateInputs i = in(0);
  i.interiorValid = false;
  i.interior_dC = kUnknownDC;
  for (uint32_t t = 0; t < 5 * MIN; t += 1000) {
    i.now = t;
    ClimateOutput o = c->step(i);
    TEST_ASSERT_FALSE(o.requestStart);
    TEST_ASSERT_EQUAL((int)ClimateStatus::Idle, (int)o.status);
  }
  i.now = 5 * MIN;
  ClimateOutput o = c->step(i);
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::SensorFault, (int)o.status);
  i.now = 20 * MIN;
  TEST_ASSERT_FALSE(c->step(i).requestStart);
  // sensor returns: starts again
  TEST_ASSERT_TRUE(c->step(in(21 * MIN)).requestStart);
}

void test_sensor_fault_does_not_stop_running_heater() {
  c->enable(0);
  ClimateInputs i = in(0, kUnknownDC, HeaterPhase::Heating);
  i.interiorValid = false;
  for (uint32_t t = 0; t < 10 * MIN; t += 10000) {
    i.now = t;
    ClimateOutput o = c->step(i);
    TEST_ASSERT_FALSE(o.requestStart);
    TEST_ASSERT_EQUAL((int)ClimateStatus::HeaterRunning, (int)o.status);
  }
}

void test_voltage_hysteresis() {
  c->enable(0);
  ClimateInputs i = in(0);
  i.voltageValid = true;
  i.voltage_mV = 12000;
  TEST_ASSERT_TRUE(c->step(i).requestStart);
  c->notifyStartResult(0, CmdResult::Unsupported);
  i.now = 100;
  i.voltage_mV = 11499;
  ClimateOutput o = c->step(i);
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::LowVoltage, (int)o.status);
  i.now = 200;
  i.voltage_mV = 11500;
  TEST_ASSERT_EQUAL((int)ClimateStatus::LowVoltage, (int)c->step(i).status);
  i.now = 300;
  i.voltage_mV = 11800;
  TEST_ASSERT_EQUAL((int)ClimateStatus::LowVoltage, (int)c->step(i).status);
  i.now = 400;
  i.voltage_mV = 11801;
  TEST_ASSERT_TRUE(c->step(i).requestStart);
}

void test_unknown_voltage_does_not_block() {
  c->enable(0);
  ClimateInputs i = in(0);
  i.voltageValid = false;
  i.voltage_mV = 0;
  TEST_ASSERT_TRUE(c->step(i).requestStart);
}

void test_session_end_disables_no_stop() {
  ClimateSettings s;
  s.targetC = 20;
  s.durationMin = 10;
  c->configure(s);
  c->enable(0);
  c->step(in(0, 100, HeaterPhase::Off));
  c->step(in(1000, 100, HeaterPhase::Heating));
  ClimateOutput o = c->step(in(10 * MIN - 1, 100, HeaterPhase::Heating));
  TEST_ASSERT_EQUAL((int)ClimateStatus::HeaterRunning, (int)o.status);
  TEST_ASSERT_EQUAL(1, o.remainingMin);
  o = c->step(in(10 * MIN, 100, HeaterPhase::Heating));
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Off, (int)o.status);
  TEST_ASSERT_FALSE(c->enabled());
  // heater left alone, no start afterwards
  o = c->step(in(11 * MIN, 100, HeaterPhase::Off));
  TEST_ASSERT_FALSE(o.requestStart);
}

void test_user_stop_disables() {
  c->enable(0);
  c->notifyUserStop(1000);
  TEST_ASSERT_FALSE(c->enabled());
  ClimateOutput o = c->step(in(5 * MIN, 100));
  TEST_ASSERT_FALSE(o.requestStart);
  TEST_ASSERT_EQUAL((int)ClimateStatus::Off, (int)o.status);
}

void test_remaining_minutes() {
  c->enable(1000);
  TEST_ASSERT_EQUAL(120, c->remainingMin(1000));
  TEST_ASSERT_EQUAL(119, c->remainingMin(1000 + MIN));
  TEST_ASSERT_EQUAL(1, c->remainingMin(1000 + 119 * MIN + 1));
  TEST_ASSERT_EQUAL(0, c->remainingMin(1000 + 120 * MIN));
  c->disable();
  TEST_ASSERT_EQUAL(0, c->remainingMin(2000));
}

void test_restore_resumes_remaining() {
  ClimateSettings s;
  s.targetC = 22;
  s.durationMin = 120;
  c->restore(s, true, 45, 500);
  TEST_ASSERT_TRUE(c->enabled());
  TEST_ASSERT_EQUAL(22, c->settings().targetC);
  TEST_ASSERT_EQUAL(45, c->remainingMin(500));
  TEST_ASSERT_TRUE(c->step(in(500, 100)).requestStart);
  c->notifyStartResult(500, CmdResult::Unsupported);
  ClimateOutput o = c->step(in(500 + 44 * MIN, 100));
  TEST_ASSERT_TRUE(c->enabled());
  TEST_ASSERT_EQUAL(1, o.remainingMin);
  o = c->step(in(500 + 45 * MIN, 100));
  TEST_ASSERT_FALSE(c->enabled());
  TEST_ASSERT_EQUAL((int)ClimateStatus::Off, (int)o.status);
}

void test_restore_inactive_or_zero() {
  c->restore(ClimateSettings(), false, 45, 0);
  TEST_ASSERT_FALSE(c->enabled());
  c->restore(ClimateSettings(), true, 0, 0);
  TEST_ASSERT_FALSE(c->enabled());
  c->restore(ClimateSettings(), true, 9999, 0);  // capped at the duration
  TEST_ASSERT_EQUAL(120, c->remainingMin(0));
}

void test_time_wrap() {
  const uint32_t t0 = 0xFFFFFFFFUL - 30000UL;
  c->enable(t0);
  c->step(in(t0, 100, HeaterPhase::Off));
  c->step(in(t0 + 1000, 100, HeaterPhase::Heating));  // wraps in the next few seconds
  uint32_t off = t0 + 40 * MIN;                        // wrapped past 0
  TEST_ASSERT_TRUE(off < t0);
  c->step(in(off, 100, HeaterPhase::Off));
  TEST_ASSERT_FALSE(c->step(in(off + 2 * MIN - 1, 100)).requestStart);
  TEST_ASSERT_TRUE(c->step(in(off + 2 * MIN, 100)).requestStart);
  TEST_ASSERT_EQUAL(120 - 42, c->remainingMin(off + 2 * MIN));
}

void test_time_wrap_failure_backoff() {
  uint32_t t = 0xFFFFFFFFUL - 1000UL;
  c->enable(t);
  TEST_ASSERT_TRUE(tryStart(t));
  c->notifyStartResult(t, CmdResult::NotAdopted);
  TEST_ASSERT_FALSE(c->step(in(t + 2 * MIN - 1)).requestStart);
  TEST_ASSERT_TRUE(c->step(in(t + 2 * MIN)).requestStart);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_start_below_target);
  RUN_TEST(test_no_start_at_or_above_target);
  RUN_TEST(test_disabled_never_starts);
  RUN_TEST(test_hysteresis);
  RUN_TEST(test_target_clamps);
  RUN_TEST(test_duration_clamps);
  RUN_TEST(test_requestStart_exactly_once);
  RUN_TEST(test_min_pause_after_off);
  RUN_TEST(test_early_off_pause_session_stays_active);
  RUN_TEST(test_long_run_off_is_no_early_stop);
  RUN_TEST(test_run_on_counts_as_running);
  RUN_TEST(test_user_stop_no_external_pause);
  RUN_TEST(test_failures_backoff_and_latch);
  RUN_TEST(test_busoff_counts_as_failure);
  RUN_TEST(test_unsupported_is_no_failure);
  RUN_TEST(test_success_resets_counters);
  RUN_TEST(test_ok_without_heating_is_failure);
  RUN_TEST(test_ok_unconfirmed_without_heating_is_failure);
  RUN_TEST(test_ok_then_heating_in_time);
  RUN_TEST(test_heater_error_blocks);
  RUN_TEST(test_error_during_start_attempt_fails);
  RUN_TEST(test_blockers);
  RUN_TEST(test_sensor_invalid_grace_then_fault);
  RUN_TEST(test_sensor_fault_does_not_stop_running_heater);
  RUN_TEST(test_voltage_hysteresis);
  RUN_TEST(test_unknown_voltage_does_not_block);
  RUN_TEST(test_session_end_disables_no_stop);
  RUN_TEST(test_user_stop_disables);
  RUN_TEST(test_remaining_minutes);
  RUN_TEST(test_restore_resumes_remaining);
  RUN_TEST(test_restore_inactive_or_zero);
  RUN_TEST(test_time_wrap);
  RUN_TEST(test_time_wrap_failure_backoff);
  return UNITY_END();
}
