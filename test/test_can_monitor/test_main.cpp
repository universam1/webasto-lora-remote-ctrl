#include <unity.h>

#include "can_monitor.h"

using namespace wh;

void setUp() {}
void tearDown() {}

static Frame status(const uint8_t (&b)[8]) {
  Frame f;
  f.id = kIdStatus;
  f.dlc = 8;
  for (int i = 0; i < 8; i++) f.d[i] = b[i];
  return f;
}
static const uint8_t kHeating[8] = {0x00, 0x0A, 0x01, 0x40, 0x44, 0x65, 0x02, 0x00};
static const uint8_t kOff[8] = {0x00, 0x00, 0x00, 0x40, 0x00, 0x4C, 0x00, 0x00};

static Frame simple(uint32_t id, uint8_t d0, uint8_t d1 = 0, uint8_t d2 = 0, uint8_t d3 = 0, uint8_t d5 = 0) {
  Frame f;
  f.id = id;
  f.dlc = 8;
  f.d[0] = d0; f.d[1] = d1; f.d[2] = d2; f.d[3] = d3; f.d[5] = d5;
  return f;
}

static const uint32_t kMin = 60000;

void test_dispatch_status_and_signals() {
  CanMonitor m;
  m.onFrame(status(kHeating), 1000);
  m.tick(1000);
  const HeaterSnapshot& s = m.snapshot();
  TEST_ASSERT_EQUAL(HeaterPhase::Heating, s.phase);
  TEST_ASSERT_FALSE(s.phaseAssumed);
  TEST_ASSERT_TRUE(s.busAwake);
  TEST_ASSERT_EQUAL_INT16(277, s.coolant_dC);
  TEST_ASSERT_EQUAL_UINT8(0x44, s.powerRaw);
  m.onFrame(simple(kIdVoltage, 164), 1000);
  m.onFrame(simple(kIdOdometer, 0, 0x7A, 0x85, 0xF4), 1000);
  m.onFrame(simple(kIdIgnition, 0x87), 1000);
  m.onFrame(simple(kIdEngine, 0, 0, 0, 149), 1000);
  m.onFrame(simple(kIdOutside, 0, 0, 0, 0, 120), 1000);
  TEST_ASSERT_EQUAL_UINT16(13200, s.voltage_mV.v);
  TEST_ASSERT_EQUAL_UINT32(296314, s.odometer_km.v);
  TEST_ASSERT_TRUE(s.ignitionOn.v);
  TEST_ASSERT_EQUAL_INT16(850, s.engineCoolant_dC.v);
  TEST_ASSERT_EQUAL_INT16(100, s.outside_dC.v);
  m.onFrame(simple(kIdIgnition, 0x42), 1100);
  TEST_ASSERT_TRUE(s.ignitionOn.v);
  TEST_ASSERT_EQUAL_UINT32(1000, s.ignitionOn.atMs);
}

void test_staleness_windows() {
  CanMonitor m;
  m.onFrame(simple(kIdVoltage, 100), 0);
  m.onFrame(simple(kIdOutside, 0, 0, 0, 0, 120), 0);
  m.onFrame(simple(kIdOdometer, 0, 1, 2, 3), 0);
  m.onFrame(simple(kIdEngine, 0, 0, 0, 100), 0);
  m.onFrame(simple(kIdIgnition, 0x87), 0);
  const HeaterSnapshot& s = m.snapshot();
  TEST_ASSERT_TRUE(s.voltage_mV.fresh(kVoltageMaxAgeMs, kVoltageMaxAgeMs));
  TEST_ASSERT_FALSE(s.voltage_mV.fresh(kVoltageMaxAgeMs + 1, kVoltageMaxAgeMs));
  TEST_ASSERT_TRUE(s.outside_dC.fresh(kOutsideMaxAgeMs, kOutsideMaxAgeMs));
  TEST_ASSERT_FALSE(s.outside_dC.fresh(kOutsideMaxAgeMs + 1, kOutsideMaxAgeMs));
  TEST_ASSERT_FALSE(s.odometer_km.fresh(kOdometerMaxAgeMs + 1, kOdometerMaxAgeMs));
  TEST_ASSERT_TRUE(s.engineCoolant_dC.fresh(kEngineMaxAgeMs, kEngineMaxAgeMs));
  TEST_ASSERT_FALSE(s.engineCoolant_dC.fresh(kEngineMaxAgeMs + 1, kEngineMaxAgeMs));
  TEST_ASSERT_FALSE(s.ignitionOn.fresh(kIgnitionMaxAgeMs + 1, kIgnitionMaxAgeMs));
  TEST_ASSERT_EQUAL_UINT32(600000, kVoltageMaxAgeMs);
  TEST_ASSERT_EQUAL_UINT32(1800000, kOutsideMaxAgeMs);
  TEST_ASSERT_EQUAL_UINT32(120000, kEngineMaxAgeMs);
}

void test_bus_awake_and_asleep() {
  CanMonitor m;
  m.onFrame(simple(kIdVoltage, 100), 1000);
  m.tick(2000);
  TEST_ASSERT_TRUE(m.snapshot().busAwake);
  m.tick(2499);
  TEST_ASSERT_TRUE(m.snapshot().busAwake);
  m.tick(2500);
  TEST_ASSERT_FALSE(m.snapshot().busAwake);
}

void test_phase_assumed_and_35min_cutoff() {
  CanMonitor m;
  m.onFrame(status(kHeating), 1000);
  m.tick(5000);  // status older than 3 s, bus asleep
  TEST_ASSERT_EQUAL(HeaterPhase::Heating, m.snapshot().phase);
  TEST_ASSERT_TRUE(m.snapshot().phaseAssumed);
  m.tick(1000 + 35 * kMin);
  TEST_ASSERT_EQUAL(HeaterPhase::Heating, m.snapshot().phase);
  m.tick(1000 + 35 * kMin + 1);
  TEST_ASSERT_EQUAL(HeaterPhase::Unknown, m.snapshot().phase);
  // a new status heals it
  m.onFrame(status(kHeating), 1000 + 36 * kMin);
  m.tick(1000 + 36 * kMin);
  TEST_ASSERT_EQUAL(HeaterPhase::Heating, m.snapshot().phase);
  TEST_ASSERT_FALSE(m.snapshot().phaseAssumed);
}

void test_off_phase_is_kept_without_cutoff() {
  CanMonitor m;
  m.onFrame(status(kOff), 0);
  m.tick(10 * 60 * kMin);
  TEST_ASSERT_EQUAL(HeaterPhase::Off, m.snapshot().phase);
  TEST_ASSERT_TRUE(m.snapshot().phaseAssumed);
}

void test_heat_command_sets_start_time() {
  CanMonitor m;
  m.noteCommandResult(CmdKind::Heat, CmdResult::OkUnconfirmed, 500);
  m.onFrame(status(kHeating), 0);  // frame older than start by design; start = command time
  m.tick(5000);
  TEST_ASSERT_EQUAL(HeaterPhase::Heating, m.snapshot().phase);
  m.tick(500 + 35 * kMin + 1);
  TEST_ASSERT_EQUAL(HeaterPhase::Unknown, m.snapshot().phase);
  // failed command does not count
  CanMonitor m2;
  m2.noteCommandResult(CmdKind::Heat, CmdResult::NotAdopted, 100);
  m2.noteCommandResult(CmdKind::Stop, CmdResult::Ok, 100);
  m2.onFrame(status(kHeating), 0);
  m2.tick(1000);
  TEST_ASSERT_EQUAL(HeaterPhase::Heating, m2.snapshot().phase);
}

// Heating, bus silent since t=1000, panel byte 0x82 seen at t=1000.
static void armed(CanMonitor& m) {
  m.onFrame(simple(kIdMode, 0x82), 1000);
  m.onFrame(status(kHeating), 1000);
}

void test_wake_all_conditions_true() {
  CanMonitor m;
  armed(m);
  TEST_ASSERT_FALSE(m.wantWake(2500));  // bus quiet for only 1.5 s
  TEST_ASSERT_FALSE(m.wantWake(3000));  // exactly 2 s: not yet
  TEST_ASSERT_TRUE(m.wantWake(3001));
}

void test_wake_needs_running_phase() {
  CanMonitor m;
  m.onFrame(simple(kIdMode, 0x82), 1000);
  m.onFrame(status(kOff), 1000);
  TEST_ASSERT_FALSE(m.wantWake(10000));
  CanMonitor m2;
  m2.onFrame(simple(kIdMode, 0x82), 1000);
  TEST_ASSERT_FALSE(m2.wantWake(10000));  // phase unknown
  CanMonitor m3;
  armed(m3);
  TEST_ASSERT_TRUE(m3.wantWake(10000));
  m3.tick(1000 + 35 * kMin + 5);  // cut-off passed, phase unknown
  TEST_ASSERT_FALSE(m3.wantWake(1000 + 35 * kMin + 5));
}

void test_wake_needs_bus_asleep() {
  CanMonitor m;
  armed(m);
  m.onFrame(simple(kIdVoltage, 100), 9500);
  TEST_ASSERT_FALSE(m.wantWake(10000));
  TEST_ASSERT_TRUE(m.wantWake(11600));
}

void test_wake_interval() {
  CanMonitor m;
  armed(m);
  TEST_ASSERT_TRUE(m.wantWake(10000));
  m.noteWake(10000);
  TEST_ASSERT_FALSE(m.wantWake(10000 + 59999));
  TEST_ASSERT_TRUE(m.wantWake(10000 + 60000));
  TEST_ASSERT_EQUAL_INT(60000, CAN_WAKE_INTERVAL_MS);
}

void test_wake_needs_fresh_panel_byte() {
  CanMonitor m;
  m.onFrame(status(kHeating), 1000);  // never saw the panel
  TEST_ASSERT_FALSE(m.wantWake(10000));
  CanMonitor m2;
  armed(m2);
  m2.noteCommandResult(CmdKind::Heat, CmdResult::Ok, 1000 + 38 * kMin);  // keeps the assumed phase alive
  TEST_ASSERT_TRUE(m2.wantWake(1000 + 40 * kMin));
  TEST_ASSERT_FALSE(m2.wantWake(1000 + 40 * kMin + 1));
}

void test_wake_needs_panel_not_off() {
  CanMonitor m;
  m.onFrame(simple(kIdMode, 0x80), 1000);  // bit 7 only, mode off
  m.onFrame(status(kHeating), 1000);
  TEST_ASSERT_FALSE(m.wantWake(10000));
  m.onFrame(simple(kIdMode, 0x00), 1000);
  TEST_ASSERT_FALSE(m.wantWake(10000));
  m.onFrame(simple(kIdMode, 0x02), 1000);
  m.onFrame(status(kHeating), 1000);
  TEST_ASSERT_TRUE(m.wantWake(10000));
}

void test_wake_poll_switch() {
  CanMonitor m;
  armed(m);
  m.enableWakePoll = false;
  TEST_ASSERT_FALSE(m.wantWake(10000));
  m.enableWakePoll = true;
  TEST_ASSERT_TRUE(m.wantWake(10000));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_dispatch_status_and_signals);
  RUN_TEST(test_staleness_windows);
  RUN_TEST(test_bus_awake_and_asleep);
  RUN_TEST(test_phase_assumed_and_35min_cutoff);
  RUN_TEST(test_off_phase_is_kept_without_cutoff);
  RUN_TEST(test_heat_command_sets_start_time);
  RUN_TEST(test_wake_all_conditions_true);
  RUN_TEST(test_wake_needs_running_phase);
  RUN_TEST(test_wake_needs_bus_asleep);
  RUN_TEST(test_wake_interval);
  RUN_TEST(test_wake_needs_fresh_panel_byte);
  RUN_TEST(test_wake_needs_panel_not_off);
  RUN_TEST(test_wake_poll_switch);
  return UNITY_END();
}
