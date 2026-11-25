#pragma once
// Komfort-CAN frame definitions and decoders for the T5 parking heater. Pure C++17.
#include <stdint.h>

#include "heater_types.h"

namespace wh {

struct Frame {
  uint32_t id = 0;
  uint8_t dlc = 0;
  uint8_t d[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

constexpr uint32_t kIdMode = 0x5D3;     // roof unit mode command, DLC 2
constexpr uint32_t kIdStatus = 0x3E5;   // heater status, DLC 8
constexpr uint32_t kIdVoltage = 0x571;
constexpr uint32_t kIdOdometer = 0x65D;
constexpr uint32_t kIdIgnition = 0x575;
constexpr uint32_t kIdEngine = 0x35B;
constexpr uint32_t kIdOutside = 0x527;

constexpr uint8_t kModeOff = 0x00;
constexpr uint8_t kModeHeat = 0x02;

// Decoded 0x3E5.
struct Status3E5 {
  uint8_t radioBits = 0;
  bool blower = false;
  bool controlLed = false;
  bool combustion = false;
  bool waterPump = false;
  uint8_t errorCode = 0;  // 0 none, 1 battery low, 2 low fuel, 3 defective, 4 crash, 5 other, 6 overheat
  bool errorStored = false;
  uint16_t fuelMl = 0;
  bool parkingHeater = false;
  bool kl30Lost = false;
  uint8_t powerRaw = 0;
  int16_t coolant_dC = kUnknownDC;
  bool fuelPump = false;
  bool waterWarmed = false;
  bool ventActive = false;
};

Frame make5D3(uint8_t byte0);
bool decodeStatus(const Frame& f, Status3E5& out);  // false if not a complete 0x3E5
HeaterPhase derivePhase(const Status3E5& s);
bool decodeVoltage(const Frame& f, uint16_t& mV);
bool decodeOdometer(const Frame& f, uint32_t& km);
bool decodeIgnition(const Frame& f, bool& on);  // false for values other than 0x87 / 0x00
bool decodeEngineCoolant(const Frame& f, int16_t& dC);
bool decodeOutside(const Frame& f, int16_t& dC);

}  // namespace wh
