// Generates pwa/test/vectors.json from the C++ codec (single source of truth).
//   g++ -std=gnu++17 -Ilib/webasto_core tools/gen_vectors.cpp -o /tmp/gen_vectors && /tmp/gen_vectors > pwa/test/vectors.json
#include <stdio.h>

#include "../lib/webasto_core/status_codec.h"

static void hex(const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}

struct Case {
  const char* name;
  wh::DecodedStatus d;
  wh::BleExtra x;
};

int main() {
  Case cases[3];

  // 1: heater off, bus awake, ignition on, climate off
  cases[0].name = "off_idle";
  cases[0].d.phase = wh::HeaterPhase::Off;
  cases[0].d.busAwake = true;
  cases[0].d.transportHealthy = true;
  cases[0].d.ignitionOn = true;
  cases[0].d.coolant_dC = 90;
  cases[0].d.interior_dC = 143;
  cases[0].d.interiorValid = true;
  cases[0].d.outside_dC = 85;
  cases[0].d.voltage_mV = 12800;
  cases[0].d.odometer_km = 296314;
  cases[0].x.engineCoolant_dC = 90;

  // 2: heating at full load with climate control active
  cases[1].name = "heating_climate";
  cases[1].d.phase = wh::HeaterPhase::Heating;
  cases[1].d.busAwake = true;
  cases[1].d.transportHealthy = true;
  cases[1].d.climateActive = true;
  cases[1].d.interiorValid = true;
  cases[1].d.climateStatus = wh::ClimateStatus::HeaterRunning;
  cases[1].d.powerW = wh::heatingPowerW(241);
  cases[1].d.fuelRate_mlph = wh::fuelRate_mlph(241);
  cases[1].d.fuelUsedMl = 123;
  cases[1].d.coolant_dC = 712;
  cases[1].d.interior_dC = 143;
  cases[1].d.outside_dC = 85;
  cases[1].d.voltage_mV = 13150;
  cases[1].d.odometer_km = 296314;
  cases[1].d.climateTargetC = 20;
  cases[1].d.rssi = -87;
  cases[1].d.snr = 7;
  cases[1].x.durationMin = 120;
  cases[1].x.remainingMin = 97;
  cases[1].x.engineCoolant_dC = 640;
  cases[1].x.lastCmdId = 7;
  cases[1].x.lastCmdResult = 1;

  // 3: everything unknown, error 3 (heater defective), assumed state, stale
  cases[2].name = "unknown_error";
  cases[2].d.phase = wh::HeaterPhase::Error;
  cases[2].d.errorCode = 3;
  cases[2].d.errorStored = true;
  cases[2].d.assumed = true;
  cases[2].x.stale = true;
  cases[2].x.kl30Lost = true;
  cases[2].x.lastCmdId = 255;
  cases[2].x.lastCmdResult = 3;

  printf("{\n  \"bleStatusV1\": [\n");
  for (int i = 0; i < 3; i++) {
    uint8_t b[ble::kStatusLen];
    wh::packBleStatus(cases[i].d, cases[i].x, b);
    printf("    {\"name\": \"%s\", \"hex\": \"", cases[i].name);
    hex(b, sizeof(b));
    const wh::DecodedStatus& d = cases[i].d;
    const wh::BleExtra& x = cases[i].x;
    printf("\",\n     \"expect\": {\"phase\": %u, \"busAwake\": %s, \"climateActive\": %s, \"interiorValid\": %s, "
           "\"transportHealthy\": %s, \"ignitionOn\": %s, \"errorCode\": %u, \"errorStored\": %s, \"kl30Lost\": %s, "
           "\"assumed\": %s, \"stale\": %s, \"climateStatus\": %u, \"powerW\": %u, \"fuelUsedMl\": %u, "
           "\"fuelRate_mlph\": %u, \"interior_dC\": %d, \"outside_dC\": %d, \"coolant_dC\": %d, "
           "\"engineCoolant_dC\": %d, \"voltage_mV\": %u, \"odometer_km\": %u, \"targetC\": %u, "
           "\"durationMin\": %u, \"remainingMin\": %u, \"rssi\": %d, \"snr\": %d, \"linkAgeS\": %u, "
           "\"lastCmdId\": %u, \"lastCmdResult\": %u}}%s\n",
           (unsigned)d.phase, d.busAwake ? "true" : "false", d.climateActive ? "true" : "false",
           d.interiorValid ? "true" : "false", d.transportHealthy ? "true" : "false", d.ignitionOn ? "true" : "false",
           d.errorCode, d.errorStored ? "true" : "false", x.kl30Lost ? "true" : "false", d.assumed ? "true" : "false",
           x.stale ? "true" : "false", (unsigned)d.climateStatus, d.powerW, d.fuelUsedMl, d.fuelRate_mlph,
           d.interior_dC, d.outside_dC, d.coolant_dC, x.engineCoolant_dC, d.voltage_mV, d.odometer_km, d.climateTargetC,
           x.durationMin, x.remainingMin, d.rssi, d.snr, x.linkAgeS, x.lastCmdId, x.lastCmdResult, i < 2 ? "," : "");
  }
  printf("  ],\n  \"commands\": [\n");
  printf("    {\"name\": \"heat\", \"op\": 1, \"cmdId\": 1, \"hex\": \"0101\"},\n");
  printf("    {\"name\": \"stop\", \"op\": 2, \"cmdId\": 2, \"hex\": \"0202\"},\n");
  printf("    {\"name\": \"climateSet 20C 120min\", \"op\": 3, \"cmdId\": 3, \"hex\": \"03031478" "00\"},\n");
  printf("    {\"name\": \"climateOff\", \"op\": 4, \"cmdId\": 4, \"hex\": \"0404\"},\n");
  printf("    {\"name\": \"query\", \"op\": 5, \"cmdId\": 5, \"hex\": \"0505\"}\n");
  printf("  ],\n");
  printf("  \"result\": {\"hex\": \"07030100\", \"cmdId\": 7, \"stage\": 3, \"result\": 1, \"detail\": 0},\n");

  // LoRa status v5 round trip, so the sender decoding is covered by the same file
  wh::SystemSnapshot s;
  s.heater.phase = wh::HeaterPhase::Heating;
  s.heater.busAwake = true;
  s.heater.healthy = true;
  s.heater.powerRaw = 241;
  s.heater.fuelUsedMl = 123;
  s.heater.coolant_dC = 712;
  s.heater.ignitionOn.set(false, 0);
  s.heater.outside_dC.set(85, 0);
  s.heater.voltage_mV.set(13150, 0);
  s.heater.odometer_km.set(296314, 0);
  s.interiorValid = true;
  s.interior_dC = 143;
  s.climateActive = true;
  s.climateStatus = wh::ClimateStatus::HeaterRunning;
  s.climateTargetC = 20;
  proto::StatusPayload p;
  wh::packLoraStatus(s, 4242, 0, p);
  printf("  \"loraStatusV5\": {\"hex\": \"");
  hex(reinterpret_cast<const uint8_t*>(&p), sizeof(p));
  wh::DecodedStatus d = wh::unpackLoraStatus(p);
  printf("\", \"expect\": {\"phase\": %u, \"powerW\": %u, \"fuelRate_mlph\": %u, \"fuelUsedMl\": %u, "
         "\"coolant_dC\": %d, \"interior_dC\": %d, \"outside_dC\": %d, \"voltage_mV\": %u, \"odometer_km\": %u, "
         "\"targetC\": %u, \"lastCmdSeq\": %u}}\n}\n",
         (unsigned)d.phase, d.powerW, d.fuelRate_mlph, d.fuelUsedMl, d.coolant_dC, d.interior_dC, d.outside_dC,
         d.voltage_mV, d.odometer_km, d.climateTargetC, d.lastCmdSeq);
  return 0;
}
