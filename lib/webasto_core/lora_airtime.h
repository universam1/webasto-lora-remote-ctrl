#pragma once
// LoRa time on air (Semtech SX1276 datasheet / AN1200.13 formula) and the sender's command
// retry interval derived from it. Pure, host-testable.
//
// Why: the sender used to resend every 1.3 s +-25 %. At SF11/BW125 a 12-byte command takes about
// 0.68 s and the receiver's 27-byte status reply about 1.0 s on air. The receiver answers about
// 0.1 s after a command, so its reply is still on air when the sender's next retry starts: both
// radios transmit at the same time, the sender is deaf while it transmits, and the reply is lost.
// Captured on 2026-11-27: 17 retries in 15 s, the receiver answered every one, none arrived.
#include <stdint.h>

namespace lora {

// Time on air in microseconds. sf 6..12, bwHz e.g. 125000, cr4 = 5..8 (4/5..4/8), explicit header,
// CRC on, low data rate optimisation on for SF >= 11 at 125 kHz (as the radio does automatically).
inline uint32_t airtimeUs(uint16_t payloadBytes, uint8_t sf, uint32_t bwHz, uint8_t cr4,
                          uint16_t preamble = 8, bool crc = true) {
  if (sf < 6 || sf > 12 || bwHz == 0 || cr4 < 5 || cr4 > 8) return 0;
  const bool de = sf >= 11 && bwHz <= 125000;
  const uint64_t tsymUs = ((uint64_t)1000000 << sf) / bwHz;  // symbol time
  const int32_t num = 8 * (int32_t)payloadBytes - 4 * sf + 28 + (crc ? 16 : 0);  // explicit header
  const int32_t den = 4 * (sf - (de ? 2 : 0));
  int32_t blocks = num > 0 ? (num + den - 1) / den : 0;
  const uint32_t payloadSym = 8 + (uint32_t)blocks * cr4;
  // preamble + 4.25 symbols, kept in quarter symbols to stay integer
  const uint64_t quarterSyms = (uint64_t)(preamble * 4 + 17) + (uint64_t)payloadSym * 4;
  return (uint32_t)(tsymUs * quarterSyms / 4);
}

// Minimum time between two sender command transmissions so the receiver's reply fits in between:
// own airtime (the TX call returns at once, the radio is busy), the receiver's processing, the
// reply's airtime, plus a guard. Jitter is added on top by the caller and must only lengthen it.
inline uint32_t commandRetryMs(uint32_t cmdAirUs, uint32_t replyAirUs, uint32_t receiverTurnaroundMs = 250,
                               uint32_t guardMs = 300) {
  return (cmdAirUs + replyAirUs + 999) / 1000 + receiverTurnaroundMs + guardMs;
}

}  // namespace lora
