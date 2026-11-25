#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include "../../lib/ble_gatt/ble_cmd.h"

void setUp() {}
void tearDown() {}

static bool dec(const std::initializer_list<uint8_t> b, rt::Command& c) {
  uint8_t buf[16];
  size_t n = 0;
  for (uint8_t v : b) buf[n++] = v;
  return decodeBleCommand(buf, n, c);
}

void test_simple_ops() {
  rt::Command c;
  TEST_ASSERT_TRUE(dec({1, 7}, c));
  TEST_ASSERT_TRUE(c.type == rt::CmdType::Heat);
  TEST_ASSERT_TRUE(c.src == rt::Source::Ble);
  TEST_ASSERT_EQUAL_UINT8(7, c.tag);
  TEST_ASSERT_TRUE(dec({2, 8}, c));
  TEST_ASSERT_TRUE(c.type == rt::CmdType::Stop);
  TEST_ASSERT_TRUE(dec({4, 255}, c));
  TEST_ASSERT_TRUE(c.type == rt::CmdType::ClimateOff);
  TEST_ASSERT_EQUAL_UINT8(255, c.tag);
  TEST_ASSERT_TRUE(dec({5, 1}, c));
  TEST_ASSERT_TRUE(c.type == rt::CmdType::Query);
}

void test_climate_set() {
  rt::Command c;
  TEST_ASSERT_TRUE(dec({3, 9, 20, 120, 0}, c));
  TEST_ASSERT_TRUE(c.type == rt::CmdType::ClimateSet);
  TEST_ASSERT_EQUAL_UINT8(20, c.targetC);
  TEST_ASSERT_EQUAL_UINT16(120, c.minutes);
  TEST_ASSERT_EQUAL_UINT8(9, c.tag);
  // boundaries: 10/24 C, 10/480 min (480 = 0x01E0)
  TEST_ASSERT_TRUE(dec({3, 1, 10, 10, 0}, c));
  TEST_ASSERT_TRUE(dec({3, 1, 24, 0xE0, 0x01}, c));
  TEST_ASSERT_EQUAL_UINT16(480, c.minutes);
  // out of range
  TEST_ASSERT_FALSE(dec({3, 1, 9, 120, 0}, c));
  TEST_ASSERT_FALSE(dec({3, 1, 25, 120, 0}, c));
  TEST_ASSERT_FALSE(dec({3, 1, 20, 9, 0}, c));
  TEST_ASSERT_FALSE(dec({3, 1, 20, 0xE1, 0x01}, c));
  TEST_ASSERT_FALSE(dec({3, 1, 20, 0, 0}, c));
  TEST_ASSERT_FALSE(dec({3, 1, 20, 0xFF, 0xFF}, c));
}

void test_malformed() {
  rt::Command c;
  memset(&c, 0xAA, sizeof(c));
  rt::Command before = c;
  TEST_ASSERT_FALSE(decodeBleCommand(nullptr, 2, c));
  uint8_t e[1] = {1};
  TEST_ASSERT_FALSE(decodeBleCommand(e, 0, c));
  TEST_ASSERT_FALSE(decodeBleCommand(e, 1, c));
  TEST_ASSERT_FALSE(dec({1, 0}, c));     // cmdId 0
  TEST_ASSERT_FALSE(dec({0, 1}, c));     // op 0
  TEST_ASSERT_FALSE(dec({6, 1}, c));     // unknown
  TEST_ASSERT_FALSE(dec({0x20, 1}, c));  // reserved
  TEST_ASSERT_FALSE(dec({1, 1, 0}, c));  // extra arg
  TEST_ASSERT_FALSE(dec({2, 1, 0}, c));
  TEST_ASSERT_FALSE(dec({4, 1, 0}, c));
  TEST_ASSERT_FALSE(dec({5, 1, 0}, c));
  TEST_ASSERT_FALSE(dec({3, 1}, c));  // missing args
  TEST_ASSERT_FALSE(dec({3, 1, 20}, c));
  TEST_ASSERT_FALSE(dec({3, 1, 20, 120}, c));
  TEST_ASSERT_FALSE(dec({3, 1, 20, 120, 0, 0}, c));
  TEST_ASSERT_EQUAL_MEMORY(&before, &c, sizeof(c));  // untouched on failure
}

void test_encode_result() {
  rt::CmdEvent ev{};
  ev.tag = 5;
  ev.stage = 4;
  ev.result = 5;
  uint8_t o[4] = {9, 9, 9, 9};
  TEST_ASSERT_EQUAL(4, encodeBleResult(ev, o));
  TEST_ASSERT_EQUAL_UINT8(5, o[0]);
  TEST_ASSERT_EQUAL_UINT8(4, o[1]);
  TEST_ASSERT_EQUAL_UINT8(5, o[2]);
  TEST_ASSERT_EQUAL_UINT8(0, o[3]);
}

// pwa/test/vectors.json "commands": same hex strings as the PWA encoder emits.
void test_vectors() {
  struct V { const char* hex; rt::CmdType t; uint8_t id; uint8_t target; uint16_t min; };
  const V v[] = {
      {"0101", rt::CmdType::Heat, 1, 0, 0},
      {"0202", rt::CmdType::Stop, 2, 0, 0},
      {"0303147800", rt::CmdType::ClimateSet, 3, 20, 120},
      {"0404", rt::CmdType::ClimateOff, 4, 0, 0},
      {"0505", rt::CmdType::Query, 5, 0, 0},
  };
  for (const V& x : v) {
    uint8_t b[8];
    size_t n = strlen(x.hex) / 2;
    for (size_t i = 0; i < n; i++) {
      unsigned h = 0;
      sscanf(x.hex + 2 * i, "%2x", &h);
      b[i] = (uint8_t)h;
    }
    rt::Command c;
    TEST_ASSERT_TRUE_MESSAGE(decodeBleCommand(b, n, c), x.hex);
    TEST_ASSERT_TRUE(c.type == x.t);
    TEST_ASSERT_EQUAL_UINT8(x.id, c.tag);
    TEST_ASSERT_EQUAL_UINT8(x.target, c.targetC);
    TEST_ASSERT_EQUAL_UINT16(x.min, c.minutes);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_simple_ops);
  RUN_TEST(test_climate_set);
  RUN_TEST(test_malformed);
  RUN_TEST(test_encode_result);
  RUN_TEST(test_vectors);
  return UNITY_END();
}
