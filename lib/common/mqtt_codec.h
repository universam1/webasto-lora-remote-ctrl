#pragma once
// Pure (host testable) part of the MQTT interface: JSON state building and mapping of
// command topics/payloads to rt::Command. Plain C++, no Arduino or ArduinoJson.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "heater_types.h"

namespace mqttc {

constexpr uint32_t kFreshMs = 10UL * 60UL * 1000UL;  // vehicle signals older than this are omitted

inline const char* phaseName(wh::HeaterPhase p) {
  switch (p) {
    case wh::HeaterPhase::Off: return "off";
    case wh::HeaterPhase::Heating: return "heating";
    case wh::HeaterPhase::RunOn: return "run_on";
    case wh::HeaterPhase::Ventilating: return "ventilating";
    case wh::HeaterPhase::Error: return "error";
    default: return nullptr;  // unknown: omitted
  }
}

inline const char* errorName(uint8_t code) {
  static const char* const k[] = {"none", "code_1", "code_2", "code_3", "code_4", "code_5", "code_6"};
  return code < 7 ? k[code] : "code_unknown";
}

inline const char* climateName(wh::ClimateStatus s) {
  switch (s) {
    case wh::ClimateStatus::Off: return "off";
    case wh::ClimateStatus::Idle: return "idle";
    case wh::ClimateStatus::Starting: return "starting";
    case wh::ClimateStatus::HeaterRunning: return "heater_running";
    case wh::ClimateStatus::Pause: return "pause";
    case wh::ClimateStatus::SensorFault: return "sensor_fault";
    case wh::ClimateStatus::LowVoltage: return "low_voltage";
    case wh::ClimateStatus::Failed: return "failed";
  }
  return "off";
}

inline const char* resultName(uint8_t r) {
  switch ((wh::CmdResult)r) {
    case wh::CmdResult::Ok: return "ok";
    case wh::CmdResult::OkUnconfirmed: return "ok_unconfirmed";
    case wh::CmdResult::NotAdopted: return "not_adopted";
    case wh::CmdResult::Unsupported: return "unsupported";
    case wh::CmdResult::Busy: return "busy";
    case wh::CmdResult::NoBus: return "no_bus";
    case wh::CmdResult::BusOff: return "bus_off";
    case wh::CmdResult::Timeout: return "timeout";
    default: return "pending";
  }
}

inline const char* sourceName(rt::Source s) {
  switch (s) {
    case rt::Source::Menu: return "menu";
    case rt::Source::Lora: return "lora";
    case rt::Source::Mqtt: return "mqtt";
    case rt::Source::Ble: return "ble";
    case rt::Source::Climate: return "climate";
    case rt::Source::Serial: return "serial";
  }
  return "?";
}

// Temperature for the climate entity and the legacy topic: interior if valid, else coolant.
// Returns wh::kUnknownDC if neither is known.
inline int16_t currentTempDC(const wh::SystemSnapshot& s) {
  if (s.interiorValid && s.interior_dC != wh::kUnknownDC) return s.interior_dC;
  return s.heater.coolant_dC;
}

// "-3.5", "21.0" from 0.1 degC units.
inline void fmtDC(char* out, size_t n, int dC) {
  const bool neg = dC < 0;
  const unsigned a = (unsigned)(neg ? -dC : dC);
  snprintf(out, n, "%s%u.%u", neg ? "-" : "", a / 10, a % 10);
}

// "12.80" from a value in thousandths (mV -> V, ml/h -> l/h).
inline void fmtMilli2(char* out, size_t n, uint32_t milli) {
  const uint32_t c = (milli + 5) / 10;  // hundredths
  snprintf(out, n, "%u.%02u", (unsigned)(c / 100), (unsigned)(c % 100));
}

// Minimal JSON object writer into a caller buffer. Values given to str() must not need escaping.
class JsonOut {
 public:
  JsonOut(char* b, size_t n) : b_(b), n_(n) { put("{"); }
  void str(const char* k, const char* v) { key(k); put("\""); put(v); put("\""); }
  void num(const char* k, unsigned long v) {
    key(k);
    char t[16];
    snprintf(t, sizeof t, "%lu", v);
    put(t);
  }
  void boolean(const char* k, bool v) { key(k); put(v ? "true" : "false"); }
  void raw(const char* k, const char* text) { key(k); put(text); }
  // Returns the length, or 0 if the buffer was too small.
  size_t finish() { put("}"); return ok_ ? len_ : 0; }

 private:
  void key(const char* k) {
    if (!first_) put(",");
    first_ = false;
    put("\"");
    put(k);
    put("\":");
  }
  void put(const char* s) {
    const size_t l = strlen(s);
    if (!ok_ || len_ + l + 1 > n_) { ok_ = false; return; }
    memcpy(b_ + len_, s, l + 1);
    len_ += l;
  }
  char* b_;
  size_t n_;
  size_t len_ = 0;
  bool first_ = true;
  bool ok_ = true;
};

// Builds the JSON of <base>/state. Unknown values are omitted.
inline size_t buildStateJson(const wh::SystemSnapshot& s, char* buf, size_t n) {
  JsonOut j(buf, n);
  char t[16];
  const wh::HeaterSnapshot& h = s.heater;
  if (const char* p = phaseName(h.phase)) j.str("phase", p);
  if (h.statusAtMs != 0) {
    j.num("power_w", wh::heatingPowerW(h.powerRaw));
    j.num("fuel_ml", h.fuelUsedMl);
    fmtMilli2(t, sizeof t, wh::fuelRate_mlph(h.powerRaw));
    j.raw("fuel_lph", t);
    if (h.coolant_dC != wh::kUnknownDC) {
      fmtDC(t, sizeof t, h.coolant_dC);
      j.raw("coolant", t);
    }
  }
  if (h.outside_dC.fresh(s.nowMs, kFreshMs)) {
    fmtDC(t, sizeof t, h.outside_dC.v);
    j.raw("outside", t);
  }
  if (s.interiorValid && s.interior_dC != wh::kUnknownDC) {
    fmtDC(t, sizeof t, s.interior_dC);
    j.raw("interior", t);
  }
  if (h.voltage_mV.fresh(s.nowMs, kFreshMs)) {
    fmtMilli2(t, sizeof t, h.voltage_mV.v);
    j.raw("voltage", t);
  }
  if (h.odometer_km.seen) j.num("odometer", h.odometer_km.v);
  j.str("error", errorName(h.errorCode));
  j.boolean("bus_awake", h.busAwake);
  if (h.ignitionOn.fresh(s.nowMs, kFreshMs)) j.boolean("ign", h.ignitionOn.v);
  j.str("climate", climateName(s.climateStatus));
  j.boolean("climate_active", s.climateActive);
  j.num("target", s.climateTargetC);
  j.num("duration", s.climateDurationMin);
  j.num("remaining", s.climateRemainingMin);
  j.boolean("assumed", h.phaseAssumed);
  return j.finish();
}

// {"src":"mqtt","result":"ok","tag":3}
inline size_t buildLastCmdJson(rt::Source src, uint8_t result, uint8_t tag, char* buf, size_t n) {
  JsonOut j(buf, n);
  j.str("src", sourceName(src));
  j.str("result", resultName(result));
  j.num("tag", tag);
  return j.finish();
}

// ---- command topics -> rt::Command ----------------------------------------------------

enum class CmdTopic : uint8_t { Mode, Minutes, Query, ClimateTarget, ClimateDuration, ClimateEnable };

inline bool parseNumber(const char* s, double& out) {
  char* end = nullptr;
  out = strtod(s, &end);
  if (end == s) return false;
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
  return *end == '\0' && isfinite(out);
}

// Case-insensitive compare against a lower case literal, ignoring surrounding blanks.
inline bool equalsNoCase(const char* s, const char* lit) {
  while (*s == ' ') s++;
  size_t i = 0;
  for (; lit[i]; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (c != lit[i]) return false;
  }
  while (s[i] == ' ' || s[i] == '\r' || s[i] == '\n') i++;
  return s[i] == '\0';
}

// Remembers the last target and duration received over MQTT. Until one is received, the
// values from the system snapshot apply.
class CommandMapper {
 public:
  enum class Out : uint8_t { Ignored, Command, Remembered };
  struct Defaults {
    uint8_t targetC = 20;
    uint16_t durationMin = 120;
    bool climateActive = false;
  };

  bool durationSet() const { return durSet_; }
  uint16_t duration() const { return duration_; }
  uint8_t effectiveTarget(const Defaults& d) const { return targetSet_ ? target_ : d.targetC; }
  uint16_t effectiveDuration(const Defaults& d) const { return durSet_ ? duration_ : d.durationMin; }

  Out map(CmdTopic t, const char* payload, const Defaults& d, uint8_t tag, rt::Command& out) {
    out = rt::Command{rt::Source::Mqtt, rt::CmdType::Query, 0, tag, 0, 0};
    double v;
    switch (t) {
      case CmdTopic::Mode:
        if (equalsNoCase(payload, "off")) { out.type = rt::CmdType::Stop; return Out::Command; }
        if (equalsNoCase(payload, "heat")) { out.type = rt::CmdType::Heat; out.minutes = 30; return Out::Command; }
        return Out::Ignored;
      case CmdTopic::Minutes:
        if (!parseNumber(payload, v)) return Out::Ignored;
        v = floor(v + 0.5);
        if (v < 10 || v > 480) return Out::Ignored;
        out.type = rt::CmdType::Heat;
        out.minutes = (uint16_t)v;
        return Out::Command;
      case CmdTopic::Query:
        out.type = rt::CmdType::Query;
        return Out::Command;
      case CmdTopic::ClimateTarget:
        if (!parseNumber(payload, v)) return Out::Ignored;
        v = floor(v + 0.5);
        if (v < 10 || v > 24) return Out::Ignored;
        target_ = (uint8_t)v;
        targetSet_ = true;
        return climateSet(d, out);
      case CmdTopic::ClimateDuration:
        if (!parseNumber(payload, v)) return Out::Ignored;
        v = floor(v + 0.5);
        if (v < 10 || v > 480) return Out::Ignored;
        duration_ = (uint16_t)v;
        durSet_ = true;
        return d.climateActive ? climateSet(d, out) : Out::Remembered;
      case CmdTopic::ClimateEnable:
        if (equalsNoCase(payload, "on") || equalsNoCase(payload, "true")) return climateSet(d, out);
        if (equalsNoCase(payload, "off") || equalsNoCase(payload, "false")) {
          out.type = rt::CmdType::ClimateOff;
          return Out::Command;
        }
        return Out::Ignored;
    }
    return Out::Ignored;
  }

 private:
  Out climateSet(const Defaults& d, rt::Command& out) const {
    out.type = rt::CmdType::ClimateSet;
    out.targetC = effectiveTarget(d);
    out.minutes = effectiveDuration(d);
    return Out::Command;
  }
  uint8_t target_ = 20;
  uint16_t duration_ = 120;
  bool targetSet_ = false;
  bool durSet_ = false;
};

}  // namespace mqttc
