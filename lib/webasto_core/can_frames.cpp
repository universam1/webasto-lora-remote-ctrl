#include "can_frames.h"

namespace wh {

Frame make5D3(uint8_t byte0) {
  Frame f;
  f.id = kIdMode;
  f.dlc = 2;
  f.d[0] = byte0;
  f.d[1] = 0;
  return f;
}

bool decodeStatus(const Frame& f, Status3E5& o) {
  if (f.id != kIdStatus || f.dlc < 8) return false;
  o.radioBits = f.d[0];
  o.blower = f.d[1] & 0x01;
  o.controlLed = f.d[1] & 0x02;
  o.combustion = f.d[1] & 0x04;
  o.waterPump = f.d[1] & 0x08;
  o.errorCode = (f.d[1] >> 4) & 0x07;
  o.errorStored = f.d[1] & 0x80;
  uint16_t w = (uint16_t)(f.d[2] | (f.d[3] << 8));
  o.fuelMl = w & 0x3FFF;
  o.parkingHeater = w & 0x4000;
  o.kl30Lost = w & 0x8000;
  o.powerRaw = f.d[4];
  o.coolant_dC = f.d[5] == 0xFF ? kUnknownDC : (int16_t)(((int32_t)f.d[5] * 75 - 4800) / 10);
  o.fuelPump = f.d[6] & 0x01;
  o.waterWarmed = f.d[6] & 0x02;
  o.ventActive = f.d[7] & 0x01;
  return true;
}

HeaterPhase derivePhase(const Status3E5& s) {
  if (s.errorCode >= 3 && s.errorCode <= 6) return HeaterPhase::Error;
  if (s.ventActive) return HeaterPhase::Ventilating;
  if (s.controlLed || s.combustion || s.powerRaw != 0) return HeaterPhase::Heating;
  if (s.waterPump) return HeaterPhase::RunOn;
  return HeaterPhase::Off;
}

bool decodeVoltage(const Frame& f, uint16_t& mV) {
  if (f.id != kIdVoltage || f.dlc < 1) return false;
  mV = (uint16_t)(f.d[0] * 50u + 5000u);
  return true;
}

bool decodeOdometer(const Frame& f, uint32_t& km) {
  if (f.id != kIdOdometer || f.dlc < 4) return false;
  km = ((uint32_t)(f.d[3] & 0x0F) << 16) | ((uint32_t)f.d[2] << 8) | f.d[1];
  return true;
}

bool decodeIgnition(const Frame& f, bool& on) {
  if (f.id != kIdIgnition || f.dlc < 1) return false;
  if (f.d[0] == 0x87) { on = true; return true; }
  if (f.d[0] == 0x00) { on = false; return true; }
  return false;
}

bool decodeEngineCoolant(const Frame& f, int16_t& dC) {
  if (f.id != kIdEngine || f.dlc < 4) return false;
  dC = (int16_t)(((int)f.d[3] - 64) * 10);
  return true;
}

bool decodeOutside(const Frame& f, int16_t& dC) {
  if (f.id != kIdOutside || f.dlc < 6) return false;
  dC = (int16_t)((int)f.d[5] * 5 - 500);
  return true;
}

}  // namespace wh
