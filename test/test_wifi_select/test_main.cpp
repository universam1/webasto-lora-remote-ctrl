#include <unity.h>

#include "wifi_select.h"

using wsel::Auth;
using wsel::Known;
using wsel::Seen;

static Known kNets[3] = {{"WRES", Auth::Peap}, {"Camper", Auth::Psk}, {"Phone", Auth::Psk}};

void setUp() {}
void tearDown() {}

static void test_strongest_known_wins() {
  Seen seen[] = {{"Other", -30}, {"WRES", -80}, {"Phone", -55}, {"Camper", -70}};
  TEST_ASSERT_EQUAL_INT(2, wsel::pickNetwork(kNets, 3, seen, 4));
}

static void test_none_visible() {
  Seen seen[] = {{"Other", -30}, {"Neighbour", -40}};
  TEST_ASSERT_EQUAL_INT(-1, wsel::pickNetwork(kNets, 3, seen, 2));
  TEST_ASSERT_EQUAL_INT(-1, wsel::pickNetwork(kNets, 3, nullptr, 0));
}

static void test_failed_networks_are_skipped() {
  Seen seen[] = {{"WRES", -50}, {"Camper", -60}};
  TEST_ASSERT_EQUAL_INT(0, wsel::pickNetwork(kNets, 3, seen, 2, 0));
  TEST_ASSERT_EQUAL_INT(1, wsel::pickNetwork(kNets, 3, seen, 2, 0b001));
  TEST_ASSERT_EQUAL_INT(-1, wsel::pickNetwork(kNets, 3, seen, 2, 0b011));
}

static void test_unused_slots_and_exact_match() {
  Known nets[3] = {{"WRES", Auth::Peap}, {"", Auth::Psk}, {nullptr, Auth::Psk}};
  Seen seen[] = {{"", -20}, {"WRES-guest", -30}, {"wres", -35}};
  TEST_ASSERT_EQUAL_INT(-1, wsel::pickNetwork(nets, 3, seen, 3));  // no prefix/case match
  Seen seen2[] = {{"WRES", -90}};
  TEST_ASSERT_EQUAL_INT(0, wsel::pickNetwork(nets, 3, seen2, 1));
}

static void test_same_ssid_several_aps_uses_best_rssi() {
  Seen seen[] = {{"WRES", -85}, {"Camper", -70}, {"WRES", -60}};
  TEST_ASSERT_EQUAL_INT(0, wsel::pickNetwork(kNets, 3, seen, 3));
}

static void test_timeouts() {
  TEST_ASSERT_TRUE(wsel::connectTimeoutMs(Auth::Peap) > wsel::connectTimeoutMs(Auth::Psk));
  TEST_ASSERT_TRUE(wsel::connectTimeoutMs(Auth::Psk) >= 10000u);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_strongest_known_wins);
  RUN_TEST(test_none_visible);
  RUN_TEST(test_failed_networks_are_skipped);
  RUN_TEST(test_unused_slots_and_exact_match);
  RUN_TEST(test_same_ssid_several_aps_uses_best_rssi);
  RUN_TEST(test_timeouts);
  return UNITY_END();
}
