#include <unity.h>

#include "can_frames.h"

using namespace wh;

void setUp() {}
void tearDown() {}

static Frame f3e5(const uint8_t (&b)[8]) {
  Frame f;
  f.id = kIdStatus;
  f.dlc = 8;
  for (int i = 0; i < 8; i++) f.d[i] = b[i];
  return f;
}

static Status3E5 dec(const uint8_t (&b)[8]) {
  Status3E5 s;
  TEST_ASSERT_TRUE(decodeStatus(f3e5(b), s));
  return s;
}

void test_status_off() {
  const uint8_t b[8] = {0x00, 0x00, 0x00, 0x40, 0x00, 0x4C, 0x00, 0x00};
  Status3E5 s = dec(b);
  TEST_ASSERT_EQUAL_INT16(90, s.coolant_dC);
  TEST_ASSERT_EQUAL(HeaterPhase::Off, derivePhase(s));
  TEST_ASSERT_EQUAL_UINT16(0, s.fuelMl);
  TEST_ASSERT_TRUE(s.parkingHeater);
  TEST_ASSERT_FALSE(s.kl30Lost);
}

void test_status_heating() {
  const uint8_t b[8] = {0x00, 0x0A, 0x01, 0x40, 0x44, 0x65, 0x02, 0x00};
  Status3E5 s = dec(b);
  TEST_ASSERT_EQUAL_INT16(277, s.coolant_dC);
  TEST_ASSERT_EQUAL_UINT16(1, s.fuelMl);
  TEST_ASSERT_EQUAL_UINT8(0x44, s.powerRaw);
  TEST_ASSERT_EQUAL(HeaterPhase::Heating, derivePhase(s));
  TEST_ASSERT_TRUE(s.controlLed);
  TEST_ASSERT_TRUE(s.waterPump);
  TEST_ASSERT_TRUE(s.waterWarmed);
}

void test_status_runon() {
  const uint8_t b[8] = {0x00, 0x08, 0x0A, 0x40, 0x00, 0x7C, 0x02, 0x00};
  Status3E5 s = dec(b);
  TEST_ASSERT_EQUAL_INT16(450, s.coolant_dC);
  TEST_ASSERT_EQUAL_UINT16(10, s.fuelMl);
  TEST_ASSERT_EQUAL(HeaterPhase::RunOn, derivePhase(s));
}

void test_status_vent() {
  const uint8_t b[8] = {0x00, 0x09, 0x08, 0x40, 0x00, 0x70, 0x02, 0x01};
  Status3E5 s = dec(b);
  TEST_ASSERT_EQUAL_INT16(360, s.coolant_dC);
  TEST_ASSERT_TRUE(s.blower);
  TEST_ASSERT_EQUAL(HeaterPhase::Ventilating, derivePhase(s));
}

void test_status_overheat() {
  const uint8_t b[8] = {0x00, 0xE0, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00};
  Status3E5 s = dec(b);
  TEST_ASSERT_EQUAL_UINT8(6, s.errorCode);
  TEST_ASSERT_TRUE(s.errorStored);
  TEST_ASSERT_EQUAL_INT16(kUnknownDC, s.coolant_dC);
  TEST_ASSERT_EQUAL(HeaterPhase::Error, derivePhase(s));
}

void test_status_rejects_short_or_wrong_id() {
  Status3E5 s;
  Frame f;
  f.id = kIdStatus;
  f.dlc = 7;
  TEST_ASSERT_FALSE(decodeStatus(f, s));
  f.dlc = 8;
  f.id = 0x123;
  TEST_ASSERT_FALSE(decodeStatus(f, s));
}

void test_voltage() {
  Frame f;
  f.id = kIdVoltage;
  f.dlc = 8;
  f.d[0] = 0x00;
  uint16_t mV = 0;
  TEST_ASSERT_TRUE(decodeVoltage(f, mV));
  TEST_ASSERT_EQUAL_UINT16(5000, mV);
  f.d[0] = 164;
  TEST_ASSERT_TRUE(decodeVoltage(f, mV));
  TEST_ASSERT_EQUAL_UINT16(13200, mV);
}

void test_odometer() {
  Frame f;
  f.id = kIdOdometer;
  f.dlc = 8;
  f.d[1] = 0x7A;
  f.d[2] = 0x85;
  f.d[3] = 0xF4;
  uint32_t km = 0;
  TEST_ASSERT_TRUE(decodeOdometer(f, km));
  TEST_ASSERT_EQUAL_UINT32(296314, km);
}

void test_ignition() {
  Frame f;
  f.id = kIdIgnition;
  f.dlc = 8;
  bool on = false;
  f.d[0] = 0x87;
  TEST_ASSERT_TRUE(decodeIgnition(f, on));
  TEST_ASSERT_TRUE(on);
  f.d[0] = 0x00;
  TEST_ASSERT_TRUE(decodeIgnition(f, on));
  TEST_ASSERT_FALSE(on);
  on = true;
  f.d[0] = 0x42;
  TEST_ASSERT_FALSE(decodeIgnition(f, on));
  TEST_ASSERT_TRUE(on);  // unchanged
}

void test_engine_coolant() {
  Frame f;
  f.id = kIdEngine;
  f.dlc = 8;
  f.d[3] = 64 + 85;
  int16_t t = 0;
  TEST_ASSERT_TRUE(decodeEngineCoolant(f, t));
  TEST_ASSERT_EQUAL_INT16(850, t);
  f.d[3] = 60;
  TEST_ASSERT_TRUE(decodeEngineCoolant(f, t));
  TEST_ASSERT_EQUAL_INT16(-40, t);
}

void test_outside() {
  Frame f;
  f.id = kIdOutside;
  f.dlc = 8;
  f.d[5] = 120;  // 120/2 - 50 = 10 degC
  int16_t t = 0;
  TEST_ASSERT_TRUE(decodeOutside(f, t));
  TEST_ASSERT_EQUAL_INT16(100, t);
  f.d[5] = 91;  // -4.5 degC
  TEST_ASSERT_TRUE(decodeOutside(f, t));
  TEST_ASSERT_EQUAL_INT16(-45, t);
}

void test_make5D3() {
  Frame f = make5D3(0x82);
  TEST_ASSERT_EQUAL_UINT32(0x5D3, f.id);
  TEST_ASSERT_EQUAL_UINT8(2, f.dlc);
  TEST_ASSERT_EQUAL_UINT8(0x82, f.d[0]);
  TEST_ASSERT_EQUAL_UINT8(0, f.d[1]);
}

void test_derive_phase_priorities() {
  Status3E5 s;
  TEST_ASSERT_EQUAL(HeaterPhase::Off, derivePhase(s));
  s.waterPump = true;
  TEST_ASSERT_EQUAL(HeaterPhase::RunOn, derivePhase(s));
  s.powerRaw = 5;
  TEST_ASSERT_EQUAL(HeaterPhase::Heating, derivePhase(s));
  s.ventActive = true;
  TEST_ASSERT_EQUAL(HeaterPhase::Ventilating, derivePhase(s));
  s.errorCode = 1;  // battery low: reported separately, phase kept
  TEST_ASSERT_EQUAL(HeaterPhase::Ventilating, derivePhase(s));
}

void test_power_helpers() {
  TEST_ASSERT_EQUAL_UINT16(4989, heatingPowerW(241));
  TEST_ASSERT_EQUAL_UINT16(581, fuelRate_mlph(241));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_status_off);
  RUN_TEST(test_status_heating);
  RUN_TEST(test_status_runon);
  RUN_TEST(test_status_vent);
  RUN_TEST(test_status_overheat);
  RUN_TEST(test_status_rejects_short_or_wrong_id);
  RUN_TEST(test_voltage);
  RUN_TEST(test_odometer);
  RUN_TEST(test_ignition);
  RUN_TEST(test_engine_coolant);
  RUN_TEST(test_outside);
  RUN_TEST(test_make5D3);
  RUN_TEST(test_derive_phase_priorities);
  RUN_TEST(test_power_helpers);
  return UNITY_END();
}
