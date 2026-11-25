#pragma once
// Pure page selection and text formatting for the full-screen OLED pages shown while the heater
// runs. No Arduino or U8g2 includes, no float printf. Timing and drawing live in oled_pages.cpp.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "heater_types.h"

namespace wh {

enum class OledPage : uint8_t { Power, Fuel, Temps, Voltage, Climate, Error, Count };

struct OledPageInput {
  HeaterPhase phase = HeaterPhase::Unknown;
  uint16_t powerW = 0;
  uint16_t fuelUsedMl = 0;
  uint16_t fuelRate_mlph = 0;
  int16_t coolant_dC = kUnknownDC;
  int16_t interior_dC = kUnknownDC;
  bool interiorValid = false;
  int16_t outside_dC = kUnknownDC;
  bool outsideValid = false;
  uint16_t voltage_mV = 0;
  bool voltageValid = false;
  bool climateActive = false;
  ClimateStatus climateStatus = ClimateStatus::Off;
  uint8_t climateTargetC = 20;
  uint16_t climateRemainingMin = 0;
  uint8_t errorCode = 0;
  bool senderActive = false;
};

struct PageText {
  char title[12];
  char value[16];  // long enough for "Heizg. defekt"
  char unit[8];
  char sub[28];
  bool alert;
};

inline bool oledHeaterOn(HeaterPhase p) {
  return p == HeaterPhase::Heating || p == HeaterPhase::RunOn || p == HeaterPhase::Ventilating;
}

// Builds the page input from a snapshot.
inline OledPageInput makeOledInput(const SystemSnapshot& s) {
  OledPageInput in;
  const HeaterSnapshot& h = s.heater;
  in.phase = h.phase;
  in.powerW = heatingPowerW(h.powerRaw);
  in.fuelUsedMl = h.fuelUsedMl;
  in.fuelRate_mlph = wh::fuelRate_mlph(h.powerRaw);
  in.coolant_dC = h.coolant_dC;
  in.interior_dC = s.interior_dC;
  in.interiorValid = s.interiorValid && s.interior_dC != kUnknownDC;
  in.outside_dC = h.outside_dC.v;
  in.outsideValid = h.outside_dC.seen && h.outside_dC.v != kUnknownDC;
  in.voltage_mV = h.voltage_mV.v;
  in.voltageValid = h.voltage_mV.seen && h.voltage_mV.v != 0;
  in.climateActive = s.climateActive;
  in.climateStatus = s.climateStatus;
  in.climateTargetC = s.climateTargetC;
  in.climateRemainingMin = s.climateRemainingMin;
  in.errorCode = h.errorCode;
  in.senderActive = s.senderActive;
  return in;
}

inline bool oledErrorActive(const OledPageInput& in) {
  return in.errorCode != 0 || in.phase == HeaterPhase::Error;
}

inline bool pageAvailable(OledPage p, const OledPageInput& in) {
  switch (p) {
    case OledPage::Power:
    case OledPage::Fuel: return true;
    case OledPage::Temps: return in.coolant_dC != kUnknownDC || in.interiorValid || in.outsideValid;
    case OledPage::Voltage: return in.voltageValid;
    case OledPage::Climate: return in.climateActive;
    case OledPage::Error: return oledErrorActive(in);
    default: return false;
  }
}

inline uint8_t pageCount(const OledPageInput& in) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < (uint8_t)OledPage::Count; i++) n += pageAvailable((OledPage)i, in) ? 1 : 0;
  return n;
}

// Position of p among the available pages (0 based), for the dot row. 0 if p is unavailable.
inline uint8_t pageIndex(OledPage p, const OledPageInput& in) {
  uint8_t idx = 0;
  for (uint8_t i = 0; i < (uint8_t)OledPage::Count; i++) {
    if ((OledPage)i == p) return idx;
    if (pageAvailable((OledPage)i, in)) idx++;
  }
  return 0;
}

// One step. Regular pages cycle in enum order and skip pages without data. With an active error the
// Error page is shown on every second step: after any regular page comes Error, after Error comes the
// regular page following lastRegular (the regular page shown before the Error page).
inline OledPage nextPage(OledPage current, const OledPageInput& in, OledPage lastRegular = OledPage::Power) {
  const bool err = oledErrorActive(in);
  if (err && current != OledPage::Error) return OledPage::Error;
  OledPage from = (current == OledPage::Error) ? lastRegular : current;
  if ((uint8_t)from >= (uint8_t)OledPage::Error) from = OledPage::Count;  // so that Power is next
  const uint8_t nReg = (uint8_t)OledPage::Error;
  for (uint8_t step = 1; step <= nReg; step++) {
    uint8_t base = (from == OledPage::Count) ? (uint8_t)(nReg - 1) : (uint8_t)from;
    OledPage c = (OledPage)((base + step) % nReg);
    if (pageAvailable(c, in)) return c;
  }
  return OledPage::Power;
}

// Keeps the current page and the last regular page. advance() is one 1 s step.
struct OledPageCursor {
  OledPage page = OledPage::Power;
  OledPage lastRegular = OledPage::Power;
  OledPage advance(const OledPageInput& in) {
    if (page != OledPage::Error) lastRegular = page;
    page = nextPage(page, in, lastRegular);
    return page;
  }
};

namespace oled_detail {

inline void setStr(char* dst, size_t n, const char* src) {
  size_t i = 0;
  for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

// Tenths -> "-12,5" (decimal comma).
inline void fmtTenths(char* dst, size_t n, int tenths) {
  unsigned a = (unsigned)(tenths < 0 ? -tenths : tenths);
  snprintf(dst, n, "%s%u,%u", tenths < 0 ? "-" : "", a / 10u, a % 10u);
}

// Integer degrees from 0.1 degC, rounded half away from zero.
inline int roundDeg(int dC) { return dC >= 0 ? (dC + 5) / 10 : -((-dC + 5) / 10); }

inline const char* errorText(uint8_t code) {
  switch (code) {
    case 1: return "Batt leer";
    case 2: return "Tank leer";
    case 3: return "Heizg. defekt";
    case 4: return "Crash";
    case 5: return "Stoerung";
    case 6: return "Ueberhitzung";
    default: return "Stoerung";
  }
}

inline void fmtRate(char* dst, size_t n, uint16_t mlph) {
  unsigned h = ((unsigned)mlph + 5u) / 10u;  // hundredths of l/h
  snprintf(dst, n, "%u,%02u l/h", h / 100u, h % 100u);
}

}  // namespace oled_detail

// Text of one page. Value, unit and sub are always NUL terminated and bounded.
inline PageText formatPage(OledPage page, const OledPageInput& in) {
  using namespace oled_detail;
  PageText t;
  memset(&t, 0, sizeof(t));
  char tmp[16];
  size_t len = 0;
  switch (page) {
    case OledPage::Power:
      setStr(t.title, sizeof(t.title), "LEISTUNG");
      if (in.powerW >= 1000) {
        unsigned tenths = ((unsigned)in.powerW + 50u) / 100u;
        snprintf(t.value, sizeof(t.value), "%u,%u", tenths / 10u, tenths % 10u);
        setStr(t.unit, sizeof(t.unit), "kW");
      } else {
        snprintf(t.value, sizeof(t.value), "%u", (unsigned)in.powerW);
        setStr(t.unit, sizeof(t.unit), "W");
      }
      fmtRate(t.sub, sizeof(t.sub), in.fuelRate_mlph);
      break;
    case OledPage::Fuel:
      setStr(t.title, sizeof(t.title), "VERBRAUCH");
      snprintf(t.value, sizeof(t.value), "%u", (unsigned)in.fuelUsedMl);
      setStr(t.unit, sizeof(t.unit), "ml");
      fmtRate(t.sub, sizeof(t.sub), in.fuelRate_mlph);
      break;
    case OledPage::Temps:
      setStr(t.title, sizeof(t.title), "KUEHLMITTEL");
      if (in.coolant_dC != kUnknownDC) {
        snprintf(t.value, sizeof(t.value), "%d", roundDeg(in.coolant_dC));
      } else {
        setStr(t.value, sizeof(t.value), "--");
      }
      setStr(t.unit, sizeof(t.unit), "C");
      if (in.interiorValid) {
        fmtTenths(tmp, sizeof(tmp), in.interior_dC);
        snprintf(t.sub, sizeof(t.sub), "Innen %s", tmp);
        len = strlen(t.sub);
      }
      if (in.outsideValid) {
        fmtTenths(tmp, sizeof(tmp), in.outside_dC);
        snprintf(t.sub + len, sizeof(t.sub) - len, "%sAussen %s", len ? "  " : "", tmp);
      }
      break;
    case OledPage::Voltage: {
      setStr(t.title, sizeof(t.title), "SPANNUNG");
      unsigned tenths = ((unsigned)in.voltage_mV + 50u) / 100u;
      snprintf(t.value, sizeof(t.value), "%u,%u", tenths / 10u, tenths % 10u);
      setStr(t.unit, sizeof(t.unit), "V");
      break;
    }
    case OledPage::Climate:
      setStr(t.title, sizeof(t.title), "KLIMA");
      snprintf(t.value, sizeof(t.value), "%u", (unsigned)in.climateTargetC);
      setStr(t.unit, sizeof(t.unit), "C");
      if (in.interiorValid) {
        fmtTenths(tmp, sizeof(tmp), in.interior_dC);
        snprintf(t.sub, sizeof(t.sub), "Innen %s", tmp);
        len = strlen(t.sub);
      }
      if (in.climateRemainingMin > 0) {
        unsigned m = in.climateRemainingMin;
        snprintf(t.sub + len, sizeof(t.sub) - len, "%sRest %uh%02u", len ? "  " : "", m / 60u, m % 60u);
      }
      break;
    case OledPage::Error:
      setStr(t.title, sizeof(t.title), "FEHLER");
      setStr(t.value, sizeof(t.value), errorText(in.errorCode));
      t.alert = true;
      break;
    default: break;
  }
  return t;
}

}  // namespace wh
