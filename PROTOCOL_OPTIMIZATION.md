# Protocol Optimization Analysis

## Current Protocol Structure

### Packet Layout
```
PacketHeader (10 bytes):
  - magic (4 bytes): 0x574C5231 - validation constant
  - version (1 byte): 3
  - type (1 byte): enum {Command, Status, Ack}
  - src (1 byte): source address
  - dst (1 byte): destination address
  - seq (2 bytes): sequence number

Payload Union (32 bytes - FIXED):
  Command variant (2 bytes):
    - kind (1 byte): enum {Stop, Start, RunMinutes}
    - minutes (1 byte): duration
    
  Status variant (14+ bytes, wasted 18+ bytes):
    - state (1 byte): enum {Unknown, Off, Running, Error}
    - minutesRemaining (1 byte): 0-255 minutes
    - lastRssiDbm (1 byte): signed RSSI (-157 to 0 dBm)
    - lastSnrDb (1 byte): signed SNR (-20 to ~10 dB)
    - lastWbusOpState (1 byte): raw W-BUS operating state
    - lastErrorCode (1 byte): error code
    - lastCmdSeq (2 bytes): sequence ACK correlation
    - temperatureC (2 bytes): signed, range ~-50 to +120°C
    - voltage_mV (2 bytes): unsigned, typical 9000-15000 mV
    - power (2 bytes): unsigned, in watts

CRC (2 bytes)

TOTAL: 10 + 32 + 2 = 44 bytes per packet
```

### Time-on-Air Impact (433 MHz, SF7, BW=125kHz)
- Current: 44 bytes × 8 bits/byte = 352 bits ≈ 12.5 ms per packet
- **Every 1 byte saved = ~286 μs reduction in airtime**
- Range improvement: LoRa sensitivity improves ~0.75 dB per SF increase or ~0.5 dB per BW halving

---

## Optimization Opportunities

### 1. **Eliminate Fixed 32-Byte Payload Union** ⭐ MAJOR IMPACT
**Current waste: 18+ bytes per status packet**

**Analysis:**
- Command payload: only 2 bytes used (1 unused byte if minutes not needed)
- Status payload: only 14 bytes used out of 32
- Wasted: ~50% of every packet on average

**Solution: Variable-length payload**
```c
Packet header (unchanged 10 bytes)
  + actual_payload_len (1 byte, 0-255 indicates true payload size)
  + payload (N bytes, matched to message type)
CRC (2 bytes)
```

**Expected savings:**
- Command: 10 + 1 + 2 + 2 = 15 bytes (vs 44) = **29 bytes saved (66%)**
- Status: 10 + 1 + 14 + 2 = 27 bytes (vs 44) = **17 bytes saved (39%)**

**Compatibility impact:** Protocol version must bump to 4

---

### 2. **Bit-Pack Status Fields** ⭐ MAJOR IMPACT
**Current: 14 bytes → Potential: 6-8 bytes**

#### Field Analysis & Bit Packing Strategy

| Field | Current | Range/Notes | Bits Needed | Optimization |
|-------|---------|-------------|------------|--------------|
| `state` | 1 byte | 4 values (Unknown, Off, Running, Error) | 2 | 2 bits |
| `minutesRemaining` | 1 byte | 0-255 minutes | 5-8 | 8 bits (fits typical 0-240 min range) |
| `lastRssiDbm` | 1 byte signed | -157 to 0 dBm | 8 | Could reduce to 7 bits (practical -128 to -1) |
| `lastSnrDb` | 1 byte signed | -20 to +10 dB | 5-6 | 5 bits with offset (-20 to +12 range) |
| `lastWbusOpState` | 1 byte | 0-255 (raw opstate) | 8 | Keep 8 bits (W-BUS opstate values) |
| `lastErrorCode` | 1 byte | 0-255 error codes | 4-8 | Could compress if errors are < 16 common codes |
| `lastCmdSeq` | 2 bytes | Seq correlation | 8-10 | Reduce to 8 bits (256 unique seqs enough, or use counter modulo) |
| `temperatureC` | 2 bytes signed | -50 to +120°C range | 8 | **1 byte: temp_C + 50 (offset), covers -50 to +205°C, lossless** |
| `voltage_mV` | 2 bytes unsigned | 9000-15000 typical | 10-11 | **1 byte: (voltage_mV - 8000) / 32, ~32mV steps, covers 8000-16160mV** |
| `power` | 2 bytes unsigned | 0-3000W typical | 12 | **1 byte: power_W / 16, ~16W steps, covers 0-4080W** |

#### Proposed Bit-Packed Layout (6 bytes)
```
Byte 0: [state(2)] [lastSnrDb(5)] [rssi_byte_0(1)]
Byte 1: [rssi_byte_1(7)] [minutesRemaining(1)]
Byte 2: [minutesRemaining(7)] [padding(1)]
Byte 3: [lastWbusOpState(8)]
Byte 4: [lastCmdSeq(8)]
Byte 5: [lastErrorCode(8)]
Byte 6-7: [temperatureC(8)] [voltage_10bits(2)][power_10bits(6)]
Byte 8-9: [voltage_10bits(8)][power_10bits(8)]

Total: 10 bytes (vs 14)
```

**Quantization Strategy (Single-Byte Fields):**
```
Temperature (1 byte):
  encoding: temp_C + 50
  range: -50°C to +205°C
  resolution: 1°C (lossless for integer temps)
  typical loss: none (already in 1°C increments)

Voltage (1 byte):
  encoding: (voltage_mV - 8000) / 32
  range: 8000 to 16160 mV
  resolution: ~32 mV steps
  typical value 12V = 12000 mV → (12000-8000)/32 = 125 (recovers as 12000)
  typical value 10V = 10000 mV → (10000-8000)/32 = 62 (recovers as 9984, ±16 mV error)
  typical loss: ±16 mV (acceptable for battery monitoring)

Power (1 byte):
  encoding: power_W / 16
  range: 0 to 4080 W
  resolution: 16W steps
  typical value 1800W → 1800/16 = 112 (recovers as 1792, ±8W error)
  typical value 2500W → 2500/16 = 156 (recovers as 2496, ±4W error)
  typical loss: ±8W (acceptable for heater power monitoring)
```

**Better approach using bit fields with optimized 1-byte quantized fields:**
```cpp
struct PackedStatus {
  uint8_t state : 2;              // 2 bits (4 states)
  int8_t  snrDb_offset : 5;       // 5 bits (-20 to +12 dB, stored as val+20)
  uint8_t rssi_high : 1;          // 1 bit (MSB of RSSI)
  uint8_t rssi_low;               // 8 bits (LSB of RSSI, signed)
  uint8_t minutesRemaining;       // 8 bits (0-255 minutes)
  uint8_t lastWbusOpState;        // 8 bits (W-BUS operating state)
  uint8_t lastCmdSeq;             // 8 bits (wrap at 256)
  uint8_t lastErrorCode;          // 8 bits (error code)
  
  // Quantized sensor data (1 byte each - was 2 bytes each = 6 bytes saved!)
  uint8_t temperatureC_offset;    // temp_C + 50: covers -50°C to +205°C
  uint8_t voltage_quantized;      // (voltage_mV - 8000) / 32: ~32mV steps
  uint8_t power_quantized;        // power_W / 16: ~16W steps
};

Total: 1 + 1 + 1 + 8 + 8 + 8 + 8 + 8 + 1 + 1 + 1 = 10 bytes (was 14)

Breakdown:
  - Fixed/bitfield fields: 7 bytes (same as before)
  - Quantized sensor fields: 3 bytes (was 6 bytes) ← 50% reduction!
  - Total: 10 bytes (was 14) ← 28% reduction in status payload
```

**Pack/Unpack Helper Examples:**
```cpp
// Pack temperature: -50 to +205°C → 0-255
inline uint8_t packTemp(int16_t tempC) {
  return (uint8_t)(tempC + 50);
}
inline int16_t unpackTemp(uint8_t packed) {
  return (int16_t)packed - 50;
}

// Pack voltage: 8000-16160 mV → 0-255
inline uint8_t packVoltage(uint16_t voltage_mV) {
  return (uint8_t)((voltage_mV - 8000) / 32);
}
inline uint16_t unpackVoltage(uint8_t packed) {
  return (uint16_t)packed * 32 + 8000;
}

// Pack power: 0-4080 W → 0-255
inline uint8_t packPower(uint16_t power_W) {
  return (uint8_t)(power_W / 16);
}
inline uint16_t unpackPower(uint8_t packed) {
  return (uint16_t)packed * 16;
}
```

---

### 3. **Remove Magic Constant** ⚠️ MEDIUM IMPACT
**Current: 4 bytes → Save 4 bytes**

**Analysis:**
- Magic is only for validation (0x574C5231 = "WLR1")
- In constrained radio link, packet loss/corruption is rare
- Nonce calculation uses seq+src+dst anyway
- Type field (Command/Status) implicit from context in bidirectional link

**Alternative validation:**
- Rely on CRC for validation only
- Rely on packet length matching expected type

**Trade-off:** Reduces robustness slightly, but CRC is strong anyway
**Benefit: 4 bytes saved per packet (9% reduction)**

**Recommendation: OPTIONAL - combine with other changes for better ROI**

---

### 4. **Reduce Sequence Number Size** ⭐ MINOR IMPACT
**Current: 2 bytes → 1 byte**

**Analysis:**
- Sender retries commands with ACK-based deduplication
- With sub-10s latency, receiver never sees duplicates
- 256 unique seq values is plenty (wrap around naturally)
- lastCmdSeq also only needs 8 bits for ACK correlation

**Savings: 1 byte in header + 1 byte in status (already included above)**

---

## Optimized Protocol Design (Protocol v4)

### Compact Packet Structure
```cpp
#pragma pack(push, 1)
struct PacketHeader {
  uint8_t version;           // 1 byte (v4)
  MsgType type : 2;          // 2 bits
  uint8_t flags : 6;         // 6 bits (reserved/future)
  uint8_t src;               // 1 byte
  uint8_t dst;               // 1 byte
  uint8_t seq;               // 1 byte (reduced from 2)
  uint16_t payloadLen;       // 2 bytes (actual payload length + CRC)
  // Total: 7 bytes (was 10)
};

struct CompactCommand {
  CommandKind kind;          // 1 byte
  uint8_t minutes;           // 1 byte (0 if not needed)
  // Total: 2 bytes (same as before)
};

struct CompactStatus {
  // Bit-packed as above
  // Total: 11-12 bytes (was 14)
};

struct CompactPacket {
  PacketHeader h;            // 7 bytes
  union {
    CompactCommand cmd;      // 2 bytes
    CompactStatus status;    // 12 bytes
    uint8_t raw[12];
  } p;
  uint16_t crc;              // 2 bytes
};
#pragma pack(pop)

// Total: 7 + 12 + 2 = 21 bytes for status (vs 44)
// Total: 7 + 2 + 2 = 11 bytes for command (vs 44)
// Savings: 52% for status, 75% for command
```

---

## Implementation Checklist

### Phase 1: Variable-Length Payload (Immediate Impact)
- [ ] Add `payloadLen` field to header
- [ ] Implement variable serialization/deserialization
- [ ] Bump protocol version to 4
- [ ] Update sender and receiver
- [ ] Test backward compatibility (optional failsafe)

### Phase 2: Bit-Pack Status (Medium Effort)
- [ ] Define new `PackedStatus` struct with 1-byte quantized fields
- [ ] Implement pack/unpack helpers for temperature, voltage, power
- [ ] Convert all field assignments to use helpers
- [ ] Test quantization accuracy over typical ranges
- [ ] Verify error margins acceptable (<1°C, <32mV, <16W)

### Phase 3: Reduce Sequence to 8-bit (Low Risk)
- [ ] Update header seq field
- [ ] Audit seq comparison logic in sender/receiver
- [ ] Verify wrap-around behavior at 255→0

### Phase 4: Remove Magic (Optional, Lower Priority)
- [ ] If combined with above changes, defer until v5

---

## Expected Results

### Before & After (Status Packet)
| Metric | Before | After v4 Phase 1 | After v4 Phase 1+2 | Total Savings |
|--------|--------|-----------------|-------------------|---------------|
| **Packet size** | 44 bytes | 22 bytes | **18 bytes** | **59%** |
| **Airtime (SF7, 125kHz)** | ~12.5 ms | ~7.9 ms | **~6.4 ms** | **49%** |
| **Link budget** | baseline | +1.5 dB | **+1.8 dB** | **~30m range** |

### Before & After (Command Packet)
| Metric | Before | After (v4) | Savings |
|--------|--------|-----------|---------|
| **Packet size** | 44 bytes | 10 bytes | 77% |
| **Airtime** | ~12.5 ms | ~3.6 ms | 71% |

---

## Risks & Mitigation

| Risk | Mitigation |
|------|-----------|
| Bit-packing complexity | Encapsulate in pack/unpack helpers; unit test thoroughly |
| Quantization loss (voltage, power) | Already done in W-BUS; verify precision adequate |
| Temperature range assumptions | Support -50 to +205°C (offset +50 in uint8) |
| Sequence wrap-around | Test seq 254, 255, 0, 1 transitions in sender ACK loop |
| CRC without magic | CRC is 16-bit CCITT, collision probability ~1:65536 |

---

## Recommendation

**Start with Phase 1 (Variable-Length) immediately:**
- Low risk, high return (~40% airtime savings)
- No complex bit-packing logic
- Easy to test and debug
- Reserve Phase 2 for follow-up if more range is needed

**Phase 2 (Bit-Pack) can follow once Phase 1 is stable** to achieve ~52% total savings.
