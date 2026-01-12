# Phase 2 Quantization Strategy: 1-Byte Sensor Fields

## Overview
Reducing three 2-byte sensor fields to 1-byte each saves 3 bytes per status packet. Using smart quantization based on realistic ranges ensures minimal data loss.

---

## Field-by-Field Analysis

### 1. Temperature (temperatureC)

**Current:** 2 bytes signed (-50 to +120°C typical, -128 to +127 in practice)

**Optimized:** 1 byte with offset encoding
```
Encoding: temp_C + 50
Range: -50°C to +205°C (covers entire operating range)
Resolution: 1°C (lossless for integer values)
```

**Rationale:**
- Webasto ThermoTop C typically operates 0-100°C
- Safety margin: covers -50°C (extreme cold storage) to +205°C (overheat protection)
- Sensor readings are already in 1°C increments
- **Zero data loss** - pure offset encoding

**Pack/Unpack:**
```cpp
uint8_t pack(int16_t tempC) { return (uint8_t)(tempC + 50); }
int16_t unpack(uint8_t val) { return (int16_t)val - 50; }
```

**Examples:**
```
-50°C → 0 (minimum)
  0°C → 50
 20°C → 70 (typical indoor)
 50°C → 100 (warm outlet)
100°C → 150 (typical operating)
205°C → 255 (maximum)
```

---

### 2. Voltage (voltage_mV)

**Current:** 2 bytes unsigned (9000-15000 mV typical)

**Optimized:** 1 byte with quantization
```
Encoding: (voltage_mV - 8000) / 32
Range: 8000 to 16160 mV (256 × 32)
Resolution: ~32 mV steps
```

**Rationale:**
- Typical car battery: 9.6V (9600 mV) to 14.4V (14400 mV)
- Margin to 8.0V (8000 mV) protects against low-battery cutoff
- Margin to 16.16V (16160 mV) covers alternator peaks + surge protection
- Battery monitoring tolerates ±32 mV error (0.3% typical)

**Pack/Unpack:**
```cpp
uint8_t pack(uint16_t voltage_mV) { 
  return (uint8_t)((voltage_mV - 8000) / 32); 
}
uint16_t unpack(uint8_t val) { 
  return (uint16_t)val * 32 + 8000; 
}
```

**Examples:**
```
8.0V  (8000 mV)  → 0     (recovers as 8000 mV)
10.0V (10000 mV) → 62    (recovers as 9984 mV, -16 mV error)
12.0V (12000 mV) → 125   (recovers as 12000 mV, exact)
14.4V (14400 mV) → 200   (recovers as 14400 mV, exact)
16.16V (16160 mV) → 255  (maximum, exact)
```

**Typical Error Analysis:**
- 12V idle: exact match (125 × 32 + 8000 = 12000)
- 14.4V charging: exact match (200 × 32 + 8000 = 14400)
- 10.5V low battery: ±16 mV error (~0.15%, acceptable)

---

### 3. Power (power_W)

**Current:** 2 bytes unsigned (0-3000W typical)

**Optimized:** 1 byte with quantization
```
Encoding: power_W / 16
Range: 0 to 4080 W (256 × 16)
Resolution: 16W steps
```

**Rationale:**
- Webasto heater peak power: ~2500W
- Margin to 4080W covers oversized aftermarket heaters
- Power monitoring tolerates ±8W error (0.3% typical)
- 16W steps match sensor resolution in W-BUS messages

**Pack/Unpack:**
```cpp
uint8_t pack(uint16_t power_W) { 
  return (uint8_t)(power_W / 16); 
}
uint16_t unpack(uint8_t val) { 
  return (uint16_t)val * 16; 
}
```

**Examples:**
```
0W     → 0    (recovers as 0 W)
400W   → 25   (recovers as 400 W, exact)
1000W  → 62   (recovers as 992 W, -8 W error)
1800W  → 112  (recovers as 1792 W, -8 W error)
2500W  → 156  (recovers as 2496 W, -4 W error)
4080W  → 255  (maximum, exact)
```

**Typical Error Analysis:**
- Idle/standby (0-100W): exact match or ±8W
- Low power (400-800W): exact match or ±16W
- Normal heating (1800-2500W): ±4-8W (0.2-0.4% error, acceptable)

---

## Data Loss Summary

| Field | Before | After | Loss | Acceptable? |
|-------|--------|-------|------|-------------|
| Temperature | 2 bytes | 1 byte | 0% (lossless offset) | ✅ Yes |
| Voltage | 2 bytes | 1 byte | ±16 mV (0.15%) | ✅ Yes |
| Power | 2 bytes | 1 byte | ±8-16W (0.3%) | ✅ Yes |

**Total savings:** 6 bytes → 3 bytes = **50% reduction** in sensor fields

---

## Implementation Strategy

### Phase 2A: Prepare Helpers (5 minutes)
```cpp
// In protocol.h namespace
namespace proto {

// Temperature helpers
inline uint8_t packTemperature(int16_t tempC) {
  // Clamp to valid range
  if (tempC < -50) tempC = -50;
  if (tempC > 205) tempC = 205;
  return (uint8_t)(tempC + 50);
}

inline int16_t unpackTemperature(uint8_t packed) {
  return (int16_t)packed - 50;
}

// Voltage helpers (in mV)
inline uint8_t packVoltage(uint16_t voltage_mV) {
  // Clamp to valid range
  if (voltage_mV < 8000) voltage_mV = 8000;
  if (voltage_mV > 16160) voltage_mV = 16160;
  return (uint8_t)((voltage_mV - 8000) / 32);
}

inline uint16_t unpackVoltage(uint8_t packed) {
  return (uint16_t)packed * 32 + 8000;
}

// Power helpers (in watts)
inline uint8_t packPower(uint16_t power_W) {
  // Clamp to valid range
  if (power_W > 4080) power_W = 4080;
  return (uint8_t)(power_W / 16);
}

inline uint16_t unpackPower(uint8_t packed) {
  return (uint16_t)packed * 16;
}

} // namespace proto
```

### Phase 2B: Update StatusPayload (use new fields)
```cpp
struct CompactStatusPayload {
  // ... existing fields ...
  uint8_t temperatureC_packed;  // was int16_t temperatureC
  uint8_t voltage_mV_packed;    // was uint16_t voltage_mV
  uint8_t power_packed;         // was uint16_t power
};
```

### Phase 2C: Update Sender Display Logic
```cpp
// In sender/main.cpp formatMeasurements():
int16_t tempC = proto::unpackTemperature(status.temperatureC_packed);
uint16_t voltage_mV = proto::unpackVoltage(status.voltage_mV_packed);
uint16_t power_W = proto::unpackPower(status.power_packed);

// Format and display as before...
```

### Phase 2D: Update Receiver Status Update
```cpp
// In receiver/main.cpp when receiving W-BUS data:
gStatus.temperatureC_packed = proto::packTemperature(wbus_temp);
gStatus.voltage_mV_packed = proto::packVoltage(wbus_voltage);
gStatus.power_packed = proto::packPower(wbus_power);
```

---

## Testing Plan

### Unit Tests (for pack/unpack functions)
```cpp
// Test temperature
assert(unpackTemperature(packTemperature(-50)) == -50);
assert(unpackTemperature(packTemperature(0)) == 0);
assert(unpackTemperature(packTemperature(100)) == 100);
assert(unpackTemperature(packTemperature(205)) == 205);

// Test voltage (allow ±32mV tolerance)
auto v1 = unpackVoltage(packVoltage(12000));
assert(v1 >= 11968 && v1 <= 12032); // 12000 ± 32

// Test power (allow ±16W tolerance)
auto p1 = unpackPower(packPower(2500));
assert(p1 >= 2484 && p1 <= 2516); // 2500 ± 16
```

### Integration Tests
1. Send/receive status packets with packed values
2. Verify sender displays unpacked values correctly
3. Monitor typical range of values (12V, 1800W heating)
4. Check error margins in real driving conditions

### Margin Analysis
- Temperature: -50 to +205°C (covers all extremes)
- Voltage: 8.0 to 16.16V (typical car range with margin)
- Power: 0 to 4080W (typical + oversized heaters)

---

## Migration Path

1. **Phase 1 (Done):** Variable-length payloads + fused magic/version
2. **Phase 2A (Next):** Add pack/unpack helper functions
3. **Phase 2B (Next):** Create new `CompactStatusPayload` alongside old one
4. **Phase 2C (Next):** Dual support - both formats accepted
5. **Phase 2D (Final):** Remove old format, use new compact format exclusively

This staged approach allows testing without breaking deployed receivers.

---

## Final Size Comparison

```
Status Payload Breakdown:

Before (v3):
  state, minutesRemaining, lastRssiDbm, lastSnrDb: 4 bytes
  lastWbusOpState, lastErrorCode: 2 bytes
  lastCmdSeq: 2 bytes
  temperatureC (2 bytes), voltage_mV (2 bytes), power (2 bytes): 6 bytes
  ─────────────────────────────────────
  Total: 14 bytes

After Phase 1 (v4):
  Same fixed fields: 8 bytes
  temperatureC (2), voltage_mV (2), power (2): 6 bytes
  ─────────────────────────────────────
  Total: 14 bytes (same as v3)

After Phase 1+2 (v4 optimized):
  Same fixed fields: 8 bytes
  temperatureC_packed (1), voltage_mV_packed (1), power_packed (1): 3 bytes
  ─────────────────────────────────────
  Total: 11 bytes ← 28% reduction from Phase 1
```

**Combined with fused magic/version in header:**
- Status wire packet: 6 (header) + 11 (payload) + 2 (crc) = **19 bytes** (vs 44 original)
- **57% total reduction vs Protocol v3**
