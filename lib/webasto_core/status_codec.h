#pragma once
// Pack and unpack the LoRa status v5 and the BLE status v1. Pure C++, no Arduino.
// The same byte layouts are mirrored in pwa/parser.js and checked with pwa/test/vectors.json.
#include <stdint.h>
#include <string.h>

#include "../common/protocol.h"
#include "ble_profile.h"
#include "heater_types.h"

namespace wh {

// Error codes in StatusPayload.errClimate b0-3 (0..6 come from the heater, 8..10 from our side).
enum : uint8_t { kErrNotAdopted = 8, kErrUnsupported = 9, kErrNoBus = 10 };

// Values in display units. Unknown values use the sentinels: temperatures kUnknownDC, voltage 0,
// odometer 0xFFFFFFFF.
struct DecodedStatus {
  HeaterPhase phase = HeaterPhase::Unknown;
  bool busAwake = false, climateActive = false, interiorValid = false, transportHealthy = false, ignitionOn = false;
  uint8_t errorCode = 0;
  bool errorStored = false, assumed = false;
  ClimateStatus climateStatus = ClimateStatus::Off;
  int8_t rssi = 0, snr = 0;
  uint16_t lastCmdSeq = 0;
  uint16_t powerW = 0;
  uint16_t fuelUsedMl = 0;
  uint16_t fuelRate_mlph = 0;
  int16_t coolant_dC = kUnknownDC;
  int16_t interior_dC = kUnknownDC;
  int16_t outside_dC = kUnknownDC;
  uint16_t voltage_mV = 0;
  uint32_t odometer_km = 0xFFFFFFFFu;
  uint8_t climateTargetC = 0;
};

// --- little endian helpers ---
inline void putU16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void putU32(uint8_t* p, uint32_t v) { putU16(p, (uint16_t)v); putU16(p + 2, (uint16_t)(v >> 16)); }
inline uint16_t getU16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t getU32(const uint8_t* p) { return (uint32_t)getU16(p) | ((uint32_t)getU16(p + 2) << 16); }

// --- scaling to and from the wire encodings ---
inline uint8_t encCoolant(int16_t dC) {  // 0.75 * x - 48 degC
  if (dC == kUnknownDC) return 0xFF;
  int32_t x = ((int32_t)dC + 480) * 4 / 30;  // (dC/10 + 48) / 0.75
  if (x < 0) x = 0;
  if (x > 254) x = 254;
  return (uint8_t)x;
}
inline int16_t decCoolant(uint8_t raw) { return raw == 0xFF ? kUnknownDC : (int16_t)((raw * 75 - 4800) / 10); }
inline uint8_t encOutside(int16_t dC) {  // x / 2 - 50 degC
  if (dC == kUnknownDC) return 0xFF;
  int32_t x = ((int32_t)dC + 500) / 5;
  if (x < 0) x = 0;
  if (x > 254) x = 254;
  return (uint8_t)x;
}
inline int16_t decOutside(uint8_t raw) { return raw == 0xFF ? kUnknownDC : (int16_t)(raw * 5 - 500); }
inline uint8_t encVoltage(uint16_t mV) {  // x * 0.05 + 5 V
  if (mV == 0) return 0;
  int32_t x = ((int32_t)mV - 5000 + 25) / 50;
  if (x < 1) x = 1;
  if (x > 255) x = 255;
  return (uint8_t)x;
}
inline uint16_t decVoltage(uint8_t raw) { return raw == 0 ? 0 : (uint16_t)(raw * 50 + 5000); }

// --- LoRa status v5: snapshot -> wire (receiver) and wire -> display units (sender) ---
inline void packLoraStatus(const SystemSnapshot& s, uint16_t lastCmdSeq, uint8_t errorOverride, proto::StatusPayload& o) {
  const HeaterSnapshot& h = s.heater;
  memset(&o, 0, sizeof(o));
  o.flags = (uint8_t)((uint8_t)h.phase & 0x07) | (h.busAwake ? 0x08 : 0) | (s.climateActive ? 0x10 : 0) |
            (s.interiorValid ? 0x20 : 0) | (h.healthy ? 0x40 : 0) |
            ((h.ignitionOn.seen && h.ignitionOn.v) ? 0x80 : 0);
  uint8_t err = errorOverride ? errorOverride : h.errorCode;
  o.errClimate = (uint8_t)(err & 0x0F) | (h.errorStored ? 0x10 : 0) | (h.phaseAssumed ? 0x20 : 0);
  o.climateStatus = (uint8_t)s.climateStatus;
  o.lastCmdSeq = lastCmdSeq;
  o.powerRaw = h.powerRaw;
  o.fuelUsedMl = h.fuelUsedMl;
  o.coolantRaw = encCoolant(h.coolant_dC);
  o.interior_dC = s.interiorValid ? s.interior_dC : kUnknownDC;
  o.outsideRaw = h.outside_dC.seen ? encOutside(h.outside_dC.v) : 0xFF;
  o.voltageRaw = h.voltage_mV.seen ? encVoltage(h.voltage_mV.v) : 0;
  uint32_t km = h.odometer_km.seen ? (h.odometer_km.v & 0xFFFFFFu) : 0xFFFFFFu;
  o.odometer_km[0] = (uint8_t)km;
  o.odometer_km[1] = (uint8_t)(km >> 8);
  o.odometer_km[2] = (uint8_t)(km >> 16);
  o.climateTargetC = s.climateActive ? s.climateTargetC : 0;
}

inline DecodedStatus unpackLoraStatus(const proto::StatusPayload& p) {
  DecodedStatus d;
  d.phase = (HeaterPhase)(p.flags & 0x07);
  d.busAwake = p.flags & 0x08;
  d.climateActive = p.flags & 0x10;
  d.interiorValid = p.flags & 0x20;
  d.transportHealthy = p.flags & 0x40;
  d.ignitionOn = p.flags & 0x80;
  d.errorCode = p.errClimate & 0x0F;
  d.errorStored = p.errClimate & 0x10;
  d.assumed = p.errClimate & 0x20;
  d.climateStatus = (ClimateStatus)p.climateStatus;
  d.rssi = p.lastRssiDbm;
  d.snr = p.lastSnrDb;
  d.lastCmdSeq = p.lastCmdSeq;
  d.powerW = heatingPowerW(p.powerRaw);
  d.fuelRate_mlph = fuelRate_mlph(p.powerRaw);
  d.fuelUsedMl = p.fuelUsedMl;
  d.coolant_dC = decCoolant(p.coolantRaw);
  d.interior_dC = p.interior_dC;
  d.outside_dC = decOutside(p.outsideRaw);
  d.voltage_mV = decVoltage(p.voltageRaw);
  uint32_t km = (uint32_t)p.odometer_km[0] | ((uint32_t)p.odometer_km[1] << 8) | ((uint32_t)p.odometer_km[2] << 16);
  d.odometer_km = km == 0xFFFFFFu ? 0xFFFFFFFFu : km;
  d.climateTargetC = p.climateTargetC;
  return d;
}

// --- BLE status v1 (34 bytes) ---
struct BleExtra {            // values that are not part of DecodedStatus
  int16_t engineCoolant_dC = kUnknownDC;
  uint16_t durationMin = 0, remainingMin = 0;
  uint8_t linkAgeS = 0, lastCmdId = 0, lastCmdResult = 0;
  bool kl30Lost = false, stale = false;
};

inline void packBleStatus(const DecodedStatus& d, const BleExtra& x, uint8_t out[ble::kStatusLen]) {
  memset(out, 0, ble::kStatusLen);
  out[ble::S_VER] = ble::kStatusVersion;
  out[ble::S_FLAGS] = (uint8_t)((uint8_t)d.phase & 0x07) | (d.busAwake ? 0x08 : 0) | (d.climateActive ? 0x10 : 0) |
                      (d.interiorValid ? 0x20 : 0) | (d.transportHealthy ? 0x40 : 0) | (d.ignitionOn ? 0x80 : 0);
  out[ble::S_ERR] = (uint8_t)(d.errorCode & 0x0F) | (d.errorStored ? 0x10 : 0) | (x.kl30Lost ? 0x20 : 0) |
                    (d.assumed ? 0x40 : 0) | (x.stale ? 0x80 : 0);
  out[ble::S_CLIMATE] = (uint8_t)d.climateStatus;
  putU16(out + ble::S_POWER_W, d.powerW);
  putU16(out + ble::S_FUEL_ML, d.fuelUsedMl);
  putU16(out + ble::S_FUEL_RATE_MLPH, d.fuelRate_mlph);
  putU16(out + ble::S_INTERIOR_DC, (uint16_t)d.interior_dC);
  putU16(out + ble::S_OUTSIDE_DC, (uint16_t)d.outside_dC);
  putU16(out + ble::S_COOLANT_DC, (uint16_t)d.coolant_dC);
  putU16(out + ble::S_ENGINE_DC, (uint16_t)x.engineCoolant_dC);
  putU16(out + ble::S_VOLTAGE_MV, d.voltage_mV);
  putU32(out + ble::S_ODOMETER_KM, d.odometer_km);
  out[ble::S_TARGET_C] = d.climateTargetC;
  putU16(out + ble::S_DURATION_MIN, x.durationMin);
  putU16(out + ble::S_REMAINING_MIN, x.remainingMin);
  out[ble::S_RSSI] = (uint8_t)d.rssi;
  out[ble::S_SNR] = (uint8_t)d.snr;
  out[ble::S_LINK_AGE_S] = x.linkAgeS;
  out[ble::S_LAST_CMD_ID] = x.lastCmdId;
  out[ble::S_LAST_CMD_RESULT] = x.lastCmdResult;
}

inline bool unpackBleStatus(const uint8_t* in, size_t len, DecodedStatus& d, BleExtra& x) {
  if (len < ble::kStatusLen || in[ble::S_VER] != ble::kStatusVersion) return false;
  uint8_t f = in[ble::S_FLAGS], e = in[ble::S_ERR];
  d = DecodedStatus();
  x = BleExtra();
  d.phase = (HeaterPhase)(f & 0x07);
  d.busAwake = f & 0x08;
  d.climateActive = f & 0x10;
  d.interiorValid = f & 0x20;
  d.transportHealthy = f & 0x40;
  d.ignitionOn = f & 0x80;
  d.errorCode = e & 0x0F;
  d.errorStored = e & 0x10;
  x.kl30Lost = e & 0x20;
  d.assumed = e & 0x40;
  x.stale = e & 0x80;
  d.climateStatus = (ClimateStatus)in[ble::S_CLIMATE];
  d.powerW = getU16(in + ble::S_POWER_W);
  d.fuelUsedMl = getU16(in + ble::S_FUEL_ML);
  d.fuelRate_mlph = getU16(in + ble::S_FUEL_RATE_MLPH);
  d.interior_dC = (int16_t)getU16(in + ble::S_INTERIOR_DC);
  d.outside_dC = (int16_t)getU16(in + ble::S_OUTSIDE_DC);
  d.coolant_dC = (int16_t)getU16(in + ble::S_COOLANT_DC);
  x.engineCoolant_dC = (int16_t)getU16(in + ble::S_ENGINE_DC);
  d.voltage_mV = getU16(in + ble::S_VOLTAGE_MV);
  d.odometer_km = getU32(in + ble::S_ODOMETER_KM);
  d.climateTargetC = in[ble::S_TARGET_C];
  x.durationMin = getU16(in + ble::S_DURATION_MIN);
  x.remainingMin = getU16(in + ble::S_REMAINING_MIN);
  d.rssi = (int8_t)in[ble::S_RSSI];
  d.snr = (int8_t)in[ble::S_SNR];
  x.linkAgeS = in[ble::S_LINK_AGE_S];
  x.lastCmdId = in[ble::S_LAST_CMD_ID];
  x.lastCmdResult = in[ble::S_LAST_CMD_RESULT];
  return true;
}

}  // namespace wh
