#include <string.h>
#include <unity.h>
#include "ble_name.h"

void setUp() {}
void tearDown() {}

static const uint8_t kMac[6] = {0x24, 0x6F, 0x28, 0x01, 0xAB, 0x0C};

void test_receiver_name() {
  char b[ble::kNameLen];
  TEST_ASSERT_EQUAL(15, (int)ble::makeBleName(b, sizeof(b), ble::RoleReceiver, kMac));
  TEST_ASSERT_EQUAL_STRING("Webasto-RX-AB0C", b);
}

void test_sender_name() {
  char b[ble::kNameLen];
  TEST_ASSERT_EQUAL(15, (int)ble::makeBleName(b, sizeof(b), ble::RoleSender, kMac));
  TEST_ASSERT_EQUAL_STRING("Webasto-TX-AB0C", b);
}

void test_uppercase_and_leading_zero() {
  const uint8_t m[6] = {0, 0, 0, 0, 0x0A, 0xFF};
  char b[ble::kNameLen];
  ble::makeBleName(b, sizeof(b), ble::RoleReceiver, m);
  TEST_ASSERT_EQUAL_STRING("Webasto-RX-0AFF", b);
}

void test_only_last_two_bytes_matter() {
  const uint8_t m[6] = {1, 2, 3, 4, 0xAB, 0x0C};
  char a[ble::kNameLen], b[ble::kNameLen];
  ble::makeBleName(a, sizeof(a), ble::RoleReceiver, kMac);
  ble::makeBleName(b, sizeof(b), ble::RoleReceiver, m);
  TEST_ASSERT_EQUAL_STRING(a, b);
}

void test_roles_differ() {
  char a[ble::kNameLen], b[ble::kNameLen];
  ble::makeBleName(a, sizeof(a), ble::RoleReceiver, kMac);
  ble::makeBleName(b, sizeof(b), ble::RoleSender, kMac);
  TEST_ASSERT_NOT_EQUAL(0, strcmp(a, b));
  TEST_ASSERT_EQUAL(0, strncmp(a, "Webasto-RX", 10));
  TEST_ASSERT_EQUAL(0, strncmp(b, "Webasto-TX", 10));
}

void test_small_buffer() {
  char b[15];
  memset(b, 'x', sizeof(b));
  TEST_ASSERT_EQUAL(0, (int)ble::makeBleName(b, sizeof(b), ble::RoleReceiver, kMac));
  TEST_ASSERT_EQUAL_STRING("", b);
  TEST_ASSERT_EQUAL(0, (int)ble::makeBleName(nullptr, 16, ble::RoleReceiver, kMac));
}

void test_fits_scan_response() {
  // AD structure: length byte + type byte + name must fit 31 bytes.
  TEST_ASSERT_TRUE(2 + (ble::kNameLen - 1) <= 31);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_receiver_name);
  RUN_TEST(test_sender_name);
  RUN_TEST(test_uppercase_and_leading_zero);
  RUN_TEST(test_only_last_two_bytes_matter);
  RUN_TEST(test_roles_differ);
  RUN_TEST(test_small_buffer);
  RUN_TEST(test_fits_scan_response);
  return UNITY_END();
}
