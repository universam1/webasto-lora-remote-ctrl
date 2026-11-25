#include <unity.h>
#include <string.h>
#include "mqtt_codec.h"

using namespace mqttc;

void setUp() {}
void tearDown() {}

static wh::SystemSnapshot full() {
  wh::SystemSnapshot s;
  s.nowMs = 100000;
  s.heater.phase = wh::HeaterPhase::Heating;
  s.heater.busAwake = true;
  s.heater.statusAtMs = 99000;
  s.heater.powerRaw = 232;
  s.heater.fuelUsedMl = 123;
  s.heater.coolant_dC = 712;
  s.heater.outside_dC.set(85, 99000);
  s.heater.voltage_mV.set(12800, 99000);
  s.heater.odometer_km.set(296314, 99000);
  s.heater.ignitionOn.set(false, 99000);
  s.interiorValid = true;
  s.interior_dC = 143;
  s.climateStatus = wh::ClimateStatus::HeaterRunning;
  s.climateActive = true;
  s.climateTargetC = 20;
  s.climateDurationMin = 120;
  s.climateRemainingMin = 97;
  return s;
}

void test_state_json_full() {
  char b[400];
  size_t n = buildStateJson(full(), b, sizeof b);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"phase\":\"heating\",\"power_w\":4802,\"fuel_ml\":123,\"fuel_lph\":0.56,\"coolant\":71.2,"
      "\"outside\":8.5,\"interior\":14.3,\"voltage\":12.80,\"odometer\":296314,\"error\":\"none\","
      "\"bus_awake\":true,\"ign\":false,\"climate\":\"heater_running\",\"climate_active\":true,"
      "\"target\":20,\"duration\":120,\"remaining\":97,\"assumed\":false}",
      b);
}

void test_state_json_unknowns_omitted() {
  wh::SystemSnapshot s;
  char b[400];
  size_t n = buildStateJson(s, b, sizeof b);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_NULL(strstr(b, "phase"));
  TEST_ASSERT_NULL(strstr(b, "power_w"));
  TEST_ASSERT_NULL(strstr(b, "coolant"));
  TEST_ASSERT_NULL(strstr(b, "interior"));
  TEST_ASSERT_NULL(strstr(b, "voltage"));
  TEST_ASSERT_NULL(strstr(b, "odometer"));
  TEST_ASSERT_NOT_NULL(strstr(b, "\"bus_awake\":false"));
}

void test_state_json_stale_and_negative() {
  wh::SystemSnapshot s = full();
  s.nowMs = 99000 + kFreshMs + 1;
  s.heater.coolant_dC = -35;
  char b[400];
  buildStateJson(s, b, sizeof b);
  TEST_ASSERT_NULL(strstr(b, "voltage"));
  TEST_ASSERT_NULL(strstr(b, "outside"));
  TEST_ASSERT_NOT_NULL(strstr(b, "\"coolant\":-3.5"));
  TEST_ASSERT_NOT_NULL(strstr(b, "odometer"));
}

void test_state_json_overflow() {
  char b[40];
  TEST_ASSERT_EQUAL(0, buildStateJson(full(), b, sizeof b));
}

void test_current_temp() {
  wh::SystemSnapshot s = full();
  TEST_ASSERT_EQUAL(143, currentTempDC(s));
  s.interiorValid = false;
  TEST_ASSERT_EQUAL(712, currentTempDC(s));
  s.heater.coolant_dC = wh::kUnknownDC;
  TEST_ASSERT_EQUAL(wh::kUnknownDC, currentTempDC(s));
}

void test_last_cmd_json() {
  char b[80];
  buildLastCmdJson(rt::Source::Mqtt, (uint8_t)wh::CmdResult::NotAdopted, 7, b, sizeof b);
  TEST_ASSERT_EQUAL_STRING("{\"src\":\"mqtt\",\"result\":\"not_adopted\",\"tag\":7}", b);
}

static CommandMapper::Defaults defs(bool active = false) {
  CommandMapper::Defaults d;
  d.targetC = 18;
  d.durationMin = 90;
  d.climateActive = active;
  return d;
}

void test_mode_and_query() {
  CommandMapper m;
  rt::Command c;
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Command, (int)m.map(CmdTopic::Mode, "off", defs(), 1, c));
  TEST_ASSERT_EQUAL((int)rt::CmdType::Stop, (int)c.type);
  TEST_ASSERT_EQUAL((int)rt::Source::Mqtt, (int)c.src);
  TEST_ASSERT_EQUAL(1, c.tag);
  m.map(CmdTopic::Mode, "HEAT", defs(), 2, c);
  TEST_ASSERT_EQUAL((int)rt::CmdType::Heat, (int)c.type);
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Ignored, (int)m.map(CmdTopic::Mode, "cool", defs(), 3, c));
  m.map(CmdTopic::Query, "PRESS", defs(), 4, c);
  TEST_ASSERT_EQUAL((int)rt::CmdType::Query, (int)c.type);
}

void test_minutes() {
  CommandMapper m;
  rt::Command c;
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Command, (int)m.map(CmdTopic::Minutes, "120", defs(), 1, c));
  TEST_ASSERT_EQUAL((int)rt::CmdType::Heat, (int)c.type);
  TEST_ASSERT_EQUAL(120, c.minutes);
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Ignored, (int)m.map(CmdTopic::Minutes, "5", defs(), 1, c));
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Ignored, (int)m.map(CmdTopic::Minutes, "abc", defs(), 1, c));
}

void test_target_rounds_and_uses_default_duration() {
  CommandMapper m;
  rt::Command c;
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Command, (int)m.map(CmdTopic::ClimateTarget, "19.6", defs(), 5, c));
  TEST_ASSERT_EQUAL((int)rt::CmdType::ClimateSet, (int)c.type);
  TEST_ASSERT_EQUAL(20, c.targetC);
  TEST_ASSERT_EQUAL(90, c.minutes);
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Ignored, (int)m.map(CmdTopic::ClimateTarget, "25", defs(), 5, c));
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Ignored, (int)m.map(CmdTopic::ClimateTarget, "9.4", defs(), 5, c));
}

void test_duration_inactive_is_remembered() {
  CommandMapper m;
  rt::Command c;
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Remembered, (int)m.map(CmdTopic::ClimateDuration, "60.0", defs(false), 1, c));
  TEST_ASSERT_EQUAL(60, m.effectiveDuration(defs()));
  m.map(CmdTopic::ClimateTarget, "21", defs(), 2, c);
  TEST_ASSERT_EQUAL(60, c.minutes);
  TEST_ASSERT_EQUAL(21, c.targetC);
}

void test_duration_active_sends_climate_set() {
  CommandMapper m;
  rt::Command c;
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Command, (int)m.map(CmdTopic::ClimateDuration, "240", defs(true), 1, c));
  TEST_ASSERT_EQUAL((int)rt::CmdType::ClimateSet, (int)c.type);
  TEST_ASSERT_EQUAL(240, c.minutes);
  TEST_ASSERT_EQUAL(18, c.targetC);
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Ignored, (int)m.map(CmdTopic::ClimateDuration, "500", defs(true), 1, c));
}

void test_enable() {
  CommandMapper m;
  rt::Command c;
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Command, (int)m.map(CmdTopic::ClimateEnable, "ON", defs(), 1, c));
  TEST_ASSERT_EQUAL((int)rt::CmdType::ClimateSet, (int)c.type);
  TEST_ASSERT_EQUAL(18, c.targetC);
  TEST_ASSERT_EQUAL(90, c.minutes);
  m.map(CmdTopic::ClimateEnable, "OFF", defs(), 2, c);
  TEST_ASSERT_EQUAL((int)rt::CmdType::ClimateOff, (int)c.type);
  TEST_ASSERT_EQUAL((int)CommandMapper::Out::Ignored, (int)m.map(CmdTopic::ClimateEnable, "maybe", defs(), 3, c));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_state_json_full);
  RUN_TEST(test_state_json_unknowns_omitted);
  RUN_TEST(test_state_json_stale_and_negative);
  RUN_TEST(test_state_json_overflow);
  RUN_TEST(test_current_temp);
  RUN_TEST(test_last_cmd_json);
  RUN_TEST(test_mode_and_query);
  RUN_TEST(test_minutes);
  RUN_TEST(test_target_rounds_and_uses_default_duration);
  RUN_TEST(test_duration_inactive_is_remembered);
  RUN_TEST(test_duration_active_sends_climate_set);
  RUN_TEST(test_enable);
  return UNITY_END();
}
