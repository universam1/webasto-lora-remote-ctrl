#include <unity.h>
#include <string.h>
#include "../../lib/webasto_core/sender_ble_map.h"

void setUp() {}
void tearDown() {}

static rt::Command cmd(rt::CmdType t, uint8_t target = 0, uint16_t min = 0) {
  rt::Command c;
  memset(&c, 0, sizeof(c));
  c.src = rt::Source::Ble;
  c.type = t;
  c.targetC = target;
  c.minutes = min;
  c.tag = 5;
  return c;
}

void test_command_mapping() {
  proto::CommandPayload p;
  TEST_ASSERT_TRUE(wh::bleCommandToPayload(cmd(rt::CmdType::Heat), 45, p));
  TEST_ASSERT_TRUE(p.kind == proto::CommandKind::Start);
  TEST_ASSERT_EQUAL_UINT16(45, p.minutes);
  TEST_ASSERT_TRUE(wh::bleCommandToPayload(cmd(rt::CmdType::Stop), 45, p));
  TEST_ASSERT_TRUE(p.kind == proto::CommandKind::Stop);
  TEST_ASSERT_EQUAL_UINT16(0, p.minutes);
  TEST_ASSERT_TRUE(wh::bleCommandToPayload(cmd(rt::CmdType::Query), 45, p));
  TEST_ASSERT_TRUE(p.kind == proto::CommandKind::QueryStatus);
  TEST_ASSERT_TRUE(wh::bleCommandToPayload(cmd(rt::CmdType::ClimateOff), 45, p));
  TEST_ASSERT_TRUE(p.kind == proto::CommandKind::ClimateOff);
  TEST_ASSERT_TRUE(wh::bleCommandToPayload(cmd(rt::CmdType::ClimateSet, 21, 90), 45, p));
  TEST_ASSERT_TRUE(p.kind == proto::CommandKind::ClimateSet);
  TEST_ASSERT_EQUAL_UINT8(21, p.targetC);
  TEST_ASSERT_EQUAL_UINT16(90, p.minutes);
}

void test_command_rejects() {
  proto::CommandPayload p;
  TEST_ASSERT_FALSE(wh::bleCommandToPayload(cmd(rt::CmdType::ClimateSet, 9, 90), 45, p));
  TEST_ASSERT_FALSE(wh::bleCommandToPayload(cmd(rt::CmdType::ClimateSet, 25, 90), 45, p));
  TEST_ASSERT_FALSE(wh::bleCommandToPayload(cmd(rt::CmdType::ClimateSet, 20, 9), 45, p));
  TEST_ASSERT_FALSE(wh::bleCommandToPayload(cmd(rt::CmdType::ClimateSet, 20, 481), 45, p));
  rt::Command bad = cmd(rt::CmdType::Heat);
  bad.type = (rt::CmdType)99;
  TEST_ASSERT_FALSE(wh::bleCommandToPayload(bad, 45, p));
}

void test_decode_to_payload_chain() {
  const uint8_t b[] = {3, 7, 20, 120, 0};
  rt::Command c;
  proto::CommandPayload p;
  TEST_ASSERT_TRUE(decodeBleCommand(b, sizeof(b), c));
  TEST_ASSERT_TRUE(wh::bleCommandToPayload(c, 30, p));
  TEST_ASSERT_TRUE(p.kind == proto::CommandKind::ClimateSet);
  TEST_ASSERT_EQUAL_UINT8(7, c.tag);
}

void test_result_mapping() {
  TEST_ASSERT_TRUE(wh::cmdResultFromStatusError(0) == wh::CmdResult::Ok);
  TEST_ASSERT_TRUE(wh::cmdResultFromStatusError(3) == wh::CmdResult::Ok);  // heater error code, command still ok
  TEST_ASSERT_TRUE(wh::cmdResultFromStatusError(wh::kErrNotAdopted) == wh::CmdResult::NotAdopted);
  TEST_ASSERT_TRUE(wh::cmdResultFromStatusError(wh::kErrUnsupported) == wh::CmdResult::Unsupported);
  TEST_ASSERT_TRUE(wh::cmdResultFromStatusError(wh::kErrNoBus) == wh::CmdResult::NoBus);
  TEST_ASSERT_TRUE(wh::cmdResultIsOk(wh::CmdResult::OkUnconfirmed));
  TEST_ASSERT_FALSE(wh::cmdResultIsOk(wh::CmdResult::Busy));

  rt::CmdEvent ev = wh::makeBleJobEvent(9, ble::STAGE_FAILED, (uint8_t)wh::CmdResult::Busy);
  uint8_t out[4];
  TEST_ASSERT_EQUAL(4, encodeBleResult(ev, out));
  TEST_ASSERT_EQUAL_UINT8(9, out[0]);
  TEST_ASSERT_EQUAL_UINT8(4, out[1]);
  TEST_ASSERT_EQUAL_UINT8(5, out[2]);
}

void test_status_none_received() {
  uint8_t out[ble::kStatusLen];
  wh::buildSenderBleStatus(false, wh::DecodedStatus(), 0, 30000, 0, 0, out);
  wh::DecodedStatus d;
  wh::BleExtra x;
  TEST_ASSERT_TRUE(wh::unpackBleStatus(out, sizeof(out), d, x));
  TEST_ASSERT_TRUE(x.stale);
  TEST_ASSERT_EQUAL_UINT8(255, x.linkAgeS);
  TEST_ASSERT_TRUE(d.phase == wh::HeaterPhase::Unknown);
  TEST_ASSERT_EQUAL_INT16(wh::kUnknownDC, d.interior_dC);
  TEST_ASSERT_EQUAL_INT16(wh::kUnknownDC, d.outside_dC);
  TEST_ASSERT_EQUAL_INT16(wh::kUnknownDC, d.coolant_dC);
  TEST_ASSERT_EQUAL_INT16(wh::kUnknownDC, x.engineCoolant_dC);
  TEST_ASSERT_EQUAL_UINT16(0, d.voltage_mV);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, d.odometer_km);
}

void test_status_fresh_and_stale() {
  wh::DecodedStatus s;
  s.phase = wh::HeaterPhase::Heating;
  s.busAwake = true;
  s.transportHealthy = true;
  s.powerW = 1800;
  s.interior_dC = 215;
  s.rssi = -90;
  s.snr = 7;
  s.climateActive = true;
  s.climateTargetC = 21;
  uint8_t out[ble::kStatusLen];
  wh::DecodedStatus d;
  wh::BleExtra x;

  wh::buildSenderBleStatus(true, s, 12500, 30000, 3, 1, out);
  TEST_ASSERT_TRUE(wh::unpackBleStatus(out, sizeof(out), d, x));
  TEST_ASSERT_FALSE(x.stale);
  TEST_ASSERT_EQUAL_UINT8(12, x.linkAgeS);
  TEST_ASSERT_TRUE(d.phase == wh::HeaterPhase::Heating);
  TEST_ASSERT_EQUAL_UINT16(1800, d.powerW);
  TEST_ASSERT_EQUAL_INT16(215, d.interior_dC);
  TEST_ASSERT_EQUAL_INT8(-90, d.rssi);
  TEST_ASSERT_EQUAL_INT8(7, d.snr);
  TEST_ASSERT_EQUAL_UINT8(21, d.climateTargetC);
  TEST_ASSERT_EQUAL_UINT8(3, x.lastCmdId);
  TEST_ASSERT_EQUAL_UINT8(1, x.lastCmdResult);

  wh::buildSenderBleStatus(true, s, 30000, 30000, 0, 0, out);  // exactly at the limit: still fresh
  wh::unpackBleStatus(out, sizeof(out), d, x);
  TEST_ASSERT_FALSE(x.stale);
  wh::buildSenderBleStatus(true, s, 30001, 30000, 0, 0, out);
  wh::unpackBleStatus(out, sizeof(out), d, x);
  TEST_ASSERT_TRUE(x.stale);
  TEST_ASSERT_TRUE(d.phase == wh::HeaterPhase::Heating);  // values are kept
  TEST_ASSERT_EQUAL_UINT8(0x80, out[ble::S_ERR] & 0x80);

  wh::buildSenderBleStatus(true, s, 4000000, 30000, 0, 0, out);  // saturates
  wh::unpackBleStatus(out, sizeof(out), d, x);
  TEST_ASSERT_EQUAL_UINT8(255, x.linkAgeS);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_command_mapping);
  RUN_TEST(test_command_rejects);
  RUN_TEST(test_decode_to_payload_chain);
  RUN_TEST(test_result_mapping);
  RUN_TEST(test_status_none_received);
  RUN_TEST(test_status_fresh_and_stale);
  return UNITY_END();
}
