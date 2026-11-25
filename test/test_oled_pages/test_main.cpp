#include <unity.h>
#include <string.h>
#include "oled_page_logic.h"

using namespace wh;

static OledPageInput base() {
  OledPageInput in;
  in.phase = HeaterPhase::Heating;
  in.coolant_dC = 712;
  return in;
}
void setUp() {}
void tearDown() {}

static void test_skip_pages() {
  OledPageInput in = base();  // no voltage, no climate
  TEST_ASSERT_FALSE(pageAvailable(OledPage::Voltage, in));
  TEST_ASSERT_FALSE(pageAvailable(OledPage::Climate, in));
  OledPageCursor c;
  OledPage want1[] = {OledPage::Fuel, OledPage::Temps, OledPage::Power, OledPage::Fuel};
  for (OledPage w : want1) TEST_ASSERT_EQUAL((int)w, (int)c.advance(in));
  in.voltageValid = true; in.voltage_mV = 12800; in.climateActive = true;
  c = OledPageCursor();
  OledPage want[] = {OledPage::Fuel, OledPage::Temps, OledPage::Voltage, OledPage::Climate, OledPage::Power};
  for (OledPage w : want) TEST_ASSERT_EQUAL((int)w, (int)c.advance(in));
}

static void test_temps_skipped_without_data() {
  OledPageInput in = base();
  in.coolant_dC = kUnknownDC;
  TEST_ASSERT_FALSE(pageAvailable(OledPage::Temps, in));
  in.outsideValid = true; in.outside_dC = 85;
  TEST_ASSERT_TRUE(pageAvailable(OledPage::Temps, in));
}

static void test_error_every_second_step() {
  OledPageInput in = base();
  in.errorCode = 2;
  in.voltageValid = true; in.voltage_mV = 12000;
  OledPageCursor c;
  OledPage want[] = {OledPage::Error, OledPage::Fuel, OledPage::Error, OledPage::Temps, OledPage::Error,
                     OledPage::Voltage, OledPage::Error, OledPage::Power, OledPage::Error, OledPage::Fuel};
  for (int i = 0; i < 10; i++) TEST_ASSERT_EQUAL_MESSAGE((int)want[i], (int)c.advance(in), "step");
  OledPageInput e = base();
  e.phase = HeaterPhase::Error;
  TEST_ASSERT_TRUE(pageAvailable(OledPage::Error, e));
}

static void test_minimal_pages() {
  OledPageInput in;  // only Power and Fuel have data
  TEST_ASSERT_EQUAL(2, pageCount(in));
  TEST_ASSERT_EQUAL((int)OledPage::Fuel, (int)nextPage(OledPage::Power, in));
  TEST_ASSERT_EQUAL((int)OledPage::Power, (int)nextPage(OledPage::Fuel, in));
  TEST_ASSERT_EQUAL((int)OledPage::Power, (int)nextPage(OledPage::Count, in));
}

static void test_page_index_and_wrap() {
  OledPageInput in = base();
  TEST_ASSERT_EQUAL(3, pageCount(in));
  TEST_ASSERT_EQUAL(0, pageIndex(OledPage::Power, in));
  TEST_ASSERT_EQUAL(2, pageIndex(OledPage::Temps, in));
  OledPage p = OledPage::Power;
  for (int i = 0; i < 3; i++) p = nextPage(p, in);
  TEST_ASSERT_EQUAL((int)OledPage::Power, (int)p);
}

static void test_power() {
  OledPageInput in = base();
  in.fuelRate_mlph = 580;
  in.powerW = 4989;
  PageText t = formatPage(OledPage::Power, in);
  TEST_ASSERT_EQUAL_STRING("LEISTUNG", t.title);
  TEST_ASSERT_EQUAL_STRING("5,0", t.value);
  TEST_ASSERT_EQUAL_STRING("kW", t.unit);
  TEST_ASSERT_EQUAL_STRING("0,58 l/h", t.sub);
  in.powerW = 1200; t = formatPage(OledPage::Power, in);
  TEST_ASSERT_EQUAL_STRING("1,2", t.value);
  in.powerW = 850; t = formatPage(OledPage::Power, in);
  TEST_ASSERT_EQUAL_STRING("850", t.value);
  TEST_ASSERT_EQUAL_STRING("W", t.unit);
  in.powerW = 4800; t = formatPage(OledPage::Power, in);
  TEST_ASSERT_EQUAL_STRING("4,8", t.value);
}

static void test_fuel_voltage() {
  OledPageInput in = base();
  in.fuelUsedMl = 123; in.fuelRate_mlph = 580;
  PageText t = formatPage(OledPage::Fuel, in);
  TEST_ASSERT_EQUAL_STRING("VERBRAUCH", t.title);
  TEST_ASSERT_EQUAL_STRING("123", t.value);
  TEST_ASSERT_EQUAL_STRING("ml", t.unit);
  in.voltage_mV = 12800; in.voltageValid = true;
  t = formatPage(OledPage::Voltage, in);
  TEST_ASSERT_EQUAL_STRING("SPANNUNG", t.title);
  TEST_ASSERT_EQUAL_STRING("12,8", t.value);
  TEST_ASSERT_EQUAL_STRING("V", t.unit);
}

static void test_temps() {
  OledPageInput in = base();  // coolant 71.2 -> "71" (integer degrees, one decimal is too wide)
  in.interiorValid = true; in.interior_dC = 143;
  in.outsideValid = true; in.outside_dC = 85;
  PageText t = formatPage(OledPage::Temps, in);
  TEST_ASSERT_EQUAL_STRING("KUEHLMITTEL", t.title);
  TEST_ASSERT_EQUAL_STRING("71", t.value);
  TEST_ASSERT_EQUAL_STRING("C", t.unit);
  TEST_ASSERT_EQUAL_STRING("Innen 14,3  Aussen 8,5", t.sub);
  in.outside_dC = -35;
  t = formatPage(OledPage::Temps, in);
  TEST_ASSERT_EQUAL_STRING("Innen 14,3  Aussen -3,5", t.sub);
  in.interiorValid = false;
  t = formatPage(OledPage::Temps, in);
  TEST_ASSERT_EQUAL_STRING("Aussen -3,5", t.sub);
  in.outsideValid = false;
  t = formatPage(OledPage::Temps, in);
  TEST_ASSERT_EQUAL_STRING("", t.sub);
  in.coolant_dC = -35;
  t = formatPage(OledPage::Temps, in);
  TEST_ASSERT_EQUAL_STRING("-4", t.value);  // -3.5 rounds away from zero
  in.coolant_dC = kUnknownDC;
  t = formatPage(OledPage::Temps, in);
  TEST_ASSERT_EQUAL_STRING("--", t.value);
}

static void test_climate() {
  OledPageInput in = base();
  in.climateActive = true; in.climateTargetC = 20; in.climateRemainingMin = 97;
  in.interiorValid = true; in.interior_dC = 143;
  PageText t = formatPage(OledPage::Climate, in);
  TEST_ASSERT_EQUAL_STRING("KLIMA", t.title);
  TEST_ASSERT_EQUAL_STRING("20", t.value);
  TEST_ASSERT_EQUAL_STRING("Innen 14,3  Rest 1h37", t.sub);
  in.interiorValid = false; in.climateRemainingMin = 0;
  TEST_ASSERT_EQUAL_STRING("", formatPage(OledPage::Climate, in).sub);
}

static void test_errors() {
  const char* want[] = {"Stoerung", "Batt leer", "Tank leer", "Heizg. defekt", "Crash", "Stoerung", "Ueberhitzung"};
  for (uint8_t c = 1; c <= 6; c++) {
    OledPageInput in = base(); in.errorCode = c;
    PageText t = formatPage(OledPage::Error, in);
    TEST_ASSERT_EQUAL_STRING("FEHLER", t.title);
    TEST_ASSERT_EQUAL_STRING(want[c], t.value);
    TEST_ASSERT_TRUE(t.alert);
  }
  OledPageInput in = base(); in.phase = HeaterPhase::Error;
  TEST_ASSERT_EQUAL_STRING("Stoerung", formatPage(OledPage::Error, in).value);
  TEST_ASSERT_FALSE(formatPage(OledPage::Power, in).alert);
}

static void test_bounds() {
  OledPageInput in = base();
  in.powerW = 65535; in.fuelUsedMl = 65535; in.fuelRate_mlph = 65535; in.voltage_mV = 65535;
  in.coolant_dC = 32767; in.interiorValid = true; in.interior_dC = -32767;
  in.outsideValid = true; in.outside_dC = -32767; in.climateActive = true; in.climateTargetC = 255;
  in.climateRemainingMin = 65535; in.errorCode = 255;
  for (uint8_t p = 0; p < (uint8_t)OledPage::Count; p++) {
    PageText t = formatPage((OledPage)p, in);
    TEST_ASSERT_TRUE(strlen(t.title) < sizeof(t.title));
    TEST_ASSERT_TRUE(strlen(t.value) < sizeof(t.value));
    TEST_ASSERT_TRUE(strlen(t.unit) < sizeof(t.unit));
    TEST_ASSERT_TRUE(strlen(t.sub) < sizeof(t.sub));
  }
  // Longest temperature line is cut at the buffer end instead of overflowing.
  PageText t = formatPage(OledPage::Temps, in);
  TEST_ASSERT_EQUAL(sizeof(t.sub) - 1, strlen(t.sub));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_skip_pages);
  RUN_TEST(test_temps_skipped_without_data);
  RUN_TEST(test_error_every_second_step);
  RUN_TEST(test_minimal_pages);
  RUN_TEST(test_page_index_and_wrap);
  RUN_TEST(test_power);
  RUN_TEST(test_fuel_voltage);
  RUN_TEST(test_temps);
  RUN_TEST(test_climate);
  RUN_TEST(test_errors);
  RUN_TEST(test_bounds);
  return UNITY_END();
}
