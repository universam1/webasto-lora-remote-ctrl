#pragma once
// Shared, hardware independent types for the heater, the climate controller and the
// system snapshot that UI, MQTT, BLE and LoRa read. No Arduino or ESP-IDF includes.
#include <stdint.h>

namespace wh {

constexpr int16_t kUnknownDC = INT16_MIN;  // unknown temperature, in 0.1 degC

enum class HeaterPhase : uint8_t { Unknown = 0, Off = 1, Heating = 2, RunOn = 3, Ventilating = 4, Error = 5 };

enum class CmdKind : uint8_t {
  Stop = 1,
  Heat = 2,
  Vent = 3,  // never supported over CAN: result Unsupported, nothing is sent
  Wake = 4,  // neutral 0x5D3 equal to the panel value, no adoption wait
};

enum class CmdResult : uint8_t {
  Pending = 0,
  Ok = 1,
  OkUnconfirmed = 2,  // adopted by the roof unit, but 0x3E5 did not confirm within 1.5 s
  NotAdopted = 3,
  Unsupported = 4,
  Busy = 5,
  NoBus = 6,
  BusOff = 7,
  Timeout = 8,
};

enum class ClimateStatus : uint8_t {
  Off = 0,
  Idle = 1,           // active, temperature at or above target
  Starting = 2,       // start requested, waiting for the heater
  HeaterRunning = 3,
  Pause = 4,          // minimum pause or external-off pause or back-off
  SensorFault = 5,
  LowVoltage = 6,
  Failed = 7,         // latched after repeated failed starts
};

// A signal with its last update time. fresh() is wrap safe for 32 bit millisecond clocks.
template <class T>
struct Sig {
  T v{};
  uint32_t atMs = 0;
  bool seen = false;
  bool fresh(uint32_t now, uint32_t maxAgeMs) const { return seen && (uint32_t)(now - atMs) <= maxAgeMs; }
  void set(const T& value, uint32_t now) { v = value; atMs = now; seen = true; }
};

// What the CAN monitor (or the W-BUS wrapper) knows about the heater and the vehicle.
struct HeaterSnapshot {
  HeaterPhase phase = HeaterPhase::Unknown;
  bool phaseAssumed = false;   // bus asleep: phase extrapolated from the last confirmed state
  bool busAwake = false;
  uint8_t errorCode = 0;       // 0..6 (0x3E5 byte 1 bits 4-6)
  bool errorStored = false;
  bool kl30Lost = false;
  uint16_t fuelUsedMl = 0;     // 14 bit, resets with every heater run
  uint8_t powerRaw = 0;        // 0..255
  int16_t coolant_dC = kUnknownDC;  // 0.75 * x - 48, in 0.1 degC. kUnknownDC if the sensor reports 0xFF
  uint32_t statusAtMs = 0;     // time of the last 0x3E5
  Sig<uint16_t> voltage_mV;    // 0x571
  Sig<uint32_t> odometer_km;   // 0x65D
  Sig<bool> ignitionOn;        // 0x575
  Sig<int16_t> engineCoolant_dC;  // 0x35B
  Sig<int16_t> outside_dC;     // 0x527
  Sig<uint8_t> panelByte0;     // last Rx 0x5D3 byte 0 from the roof unit
  uint32_t lastBusFrameMs = 0;
  bool healthy = true;         // transport health
  uint8_t healthDetail = 0;    // 0 ok, 1 bus-off, 2 rx overflow, 3 no frames, 4 W-BUS failure
};

inline uint16_t heatingPowerW(uint8_t raw) { return (uint16_t)(((uint32_t)raw * 207u + 5u) / 10u); }  // raw * 20.7
inline uint16_t fuelRate_mlph(uint8_t raw) { return (uint16_t)(((uint32_t)raw * 241u + 50u) / 100u); }  // raw * 2.41

// The complete picture of the receiver. Written by the control task, read by UI, MQTT, BLE and LoRa.
struct SystemSnapshot {
  HeaterSnapshot heater;
  bool interiorValid = false;
  int16_t interior_dC = kUnknownDC;  // DS18B20, 0.1 degC
  ClimateStatus climateStatus = ClimateStatus::Off;
  bool climateActive = false;
  uint8_t climateTargetC = 20;       // 10..24
  uint16_t climateDurationMin = 120; // configured session duration, 10..480
  uint16_t climateRemainingMin = 0;
  uint8_t lastCmdId = 0;             // last command tag and its result, for the BLE result notification
  CmdResult lastCmdResult = CmdResult::Ok;
  uint16_t lastCmdSeq = 0;           // last LoRa command sequence number that was answered
  bool senderActive = false;         // LoRa: a sender asked within LORA_SENDER_ACTIVE_MS
  uint32_t nowMs = 0;
};

}  // namespace wh
