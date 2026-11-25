#pragma once
// BLE GATT profile shared by sender, receiver and the PWA (pwa/parser.js mirrors these layouts).
// Pure constants and offsets, no includes beyond stdint, so native tests can include it.
// UUIDs are generated once, do not change them without changing pwa/ble.js.
#include <stdint.h>

namespace ble {

// Service 7e5a0001-..., characteristics 0002..0005
#define BLE_SERVICE_UUID "7e5a0001-4b1d-4c5e-9a3f-2f6d0c1b8a10"
#define BLE_CHR_INFO_UUID "7e5a0002-4b1d-4c5e-9a3f-2f6d0c1b8a10"    // read, encrypted
#define BLE_CHR_STATUS_UUID "7e5a0003-4b1d-4c5e-9a3f-2f6d0c1b8a10"  // read + notify, encrypted
#define BLE_CHR_COMMAND_UUID "7e5a0004-4b1d-4c5e-9a3f-2f6d0c1b8a10" // write with response, encrypted + authenticated
#define BLE_CHR_RESULT_UUID "7e5a0005-4b1d-4c5e-9a3f-2f6d0c1b8a10"  // notify, encrypted

constexpr uint8_t kStatusVersion = 1;
constexpr uint8_t kInfoVersion = 1;

// Info (read): [ver u8][role u8: 0 receiver, 1 sender][transport u8: 0 can, 1 wbus, 2 none][fw: ASCII, up to 16 bytes]
enum Role : uint8_t { RoleReceiver = 0, RoleSender = 1 };
enum Transport : uint8_t { TransportCan = 0, TransportWbus = 1, TransportNone = 2 };

// Status (34 bytes, little endian). Sentinels: temperatures INT16_MIN, voltage 0, odometer 0xFFFFFFFF.
constexpr uint8_t kStatusLen = 34;
enum StatusOff : uint8_t {
  S_VER = 0,            // u8 = 1
  S_FLAGS = 1,          // b0-2 phase, b3 bus awake, b4 climate active, b5 interior valid, b6 transport healthy, b7 ignition on
  S_ERR = 2,            // b0-3 error code, b4 stored, b5 kl30 lost, b6 assumed, b7 stale
  S_CLIMATE = 3,        // wh::ClimateStatus
  S_POWER_W = 4,        // u16
  S_FUEL_ML = 6,        // u16
  S_FUEL_RATE_MLPH = 8, // u16
  S_INTERIOR_DC = 10,   // i16, 0.1 degC
  S_OUTSIDE_DC = 12,    // i16
  S_COOLANT_DC = 14,    // i16 (heater coolant)
  S_ENGINE_DC = 16,     // i16 (engine coolant, 0x35B)
  S_VOLTAGE_MV = 18,    // u16
  S_ODOMETER_KM = 20,   // u32
  S_TARGET_C = 24,      // u8
  S_DURATION_MIN = 25,  // u16 (configured)
  S_REMAINING_MIN = 27, // u16
  S_RSSI = 29,          // i8 (sender only, receiver 0)
  S_SNR = 30,           // i8
  S_LINK_AGE_S = 31,    // u8, age of the last LoRa status on the sender, 255 saturates; receiver 0
  S_LAST_CMD_ID = 32,   // u8
  S_LAST_CMD_RESULT = 33,  // wh::CmdResult
};

// Command (write): [op u8][cmdId u8][args...]
enum Op : uint8_t {
  OP_HEAT = 1,         // no args
  OP_STOP = 2,         // no args
  OP_CLIMATE_SET = 3,  // [targetC u8][durationMin u16 LE], also enables the session
  OP_CLIMATE_OFF = 4,  // no args
  OP_QUERY = 5,        // no args
  // 0x20.. reserved: trip logger, battery control
};

// Result (notify): [cmdId u8][stage u8][result u8][detail u8]
enum Stage : uint8_t { STAGE_QUEUED = 0, STAGE_SENT = 1, STAGE_ACKED = 2, STAGE_DONE = 3, STAGE_FAILED = 4 };

}  // namespace ble
