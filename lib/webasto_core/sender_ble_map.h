#pragma once
// Host-testable logic of the sender's BLE bridge (no Arduino, no NimBLE):
//  * BLE command (rt::Command, already decoded by decodeBleCommand) -> LoRa proto::CommandPayload
//  * last LoRa status (wh::DecodedStatus) + age -> BLE status characteristic (34 bytes)
//  * job result mapping and result events for the BLE result characteristic
//
// BLE op            rt::CmdType    proto::CommandKind
//  OP_HEAT (1)      Heat           Start (minutes = the sender's last run time)
//  OP_STOP (2)      Stop           Stop
//  OP_CLIMATE_SET   ClimateSet     ClimateSet (targetC 10..24, minutes 10..480)
//  OP_CLIMATE_OFF   ClimateOff     ClimateOff
//  OP_QUERY (5)     Query          QueryStatus
// RunMinutes has no BLE op (serial "run <min>" only). Anything else: Unsupported.
#include <stdint.h>

#include "../common/protocol.h"
#include "../rt/commands.h"
#include "../ble_gatt/ble_cmd.h"
#include "status_codec.h"

namespace wh {

// Returns false for command types the sender cannot relay (caller answers Unsupported).
inline bool bleCommandToPayload(const rt::Command& c, uint16_t defaultRunMinutes, proto::CommandPayload& out) {
  proto::CommandPayload p{};
  switch (c.type) {
    case rt::CmdType::Heat:
      p.kind = proto::CommandKind::Start;
      p.minutes = defaultRunMinutes;
      break;
    case rt::CmdType::Stop:
      p.kind = proto::CommandKind::Stop;
      break;
    case rt::CmdType::Query:
      p.kind = proto::CommandKind::QueryStatus;
      break;
    case rt::CmdType::ClimateSet:
      if (c.targetC < kBleClimateMinC || c.targetC > kBleClimateMaxC) return false;
      if (c.minutes < kBleClimateMinMin || c.minutes > kBleClimateMaxMin) return false;
      p.kind = proto::CommandKind::ClimateSet;
      p.targetC = c.targetC;
      p.minutes = c.minutes;
      break;
    case rt::CmdType::ClimateOff:
      p.kind = proto::CommandKind::ClimateOff;
      break;
    default:
      return false;
  }
  out = p;
  return true;
}

// Result of a command job from the error code of the status that acknowledged it.
inline CmdResult cmdResultFromStatusError(uint8_t errorCode) {
  switch (errorCode) {
    case kErrNotAdopted: return CmdResult::NotAdopted;
    case kErrUnsupported: return CmdResult::Unsupported;
    case kErrNoBus: return CmdResult::NoBus;
    default: return CmdResult::Ok;
  }
}

inline bool cmdResultIsOk(CmdResult r) { return r == CmdResult::Ok || r == CmdResult::OkUnconfirmed; }

// Event for the BLE result characteristic. stage: ble::Stage, result: wh::CmdResult.
inline rt::CmdEvent makeBleJobEvent(uint8_t cmdId, uint8_t stage, uint8_t result) {
  rt::CmdEvent ev{};
  ev.src = rt::Source::Ble;
  ev.tag = cmdId;
  ev.stage = stage;
  ev.result = result;
  return ev;
}

// BLE status of the sender: the last status received over LoRa.
//  have == false (nothing received yet): all values unknown (sentinels), stale flag set, link age 255.
//  ageMs > staleAfterMs: values kept, stale flag set. S_LINK_AGE_S saturates at 255.
//  lastCmdId / lastCmdResult: the last BLE command of this sender and its final result.
inline void buildSenderBleStatus(bool have, const DecodedStatus& last, uint32_t ageMs, uint32_t staleAfterMs,
                                 uint8_t lastCmdId, uint8_t lastCmdResult, uint8_t out[ble::kStatusLen]) {
  const DecodedStatus d = have ? last : DecodedStatus();
  BleExtra x;
  x.stale = !have || ageMs > staleAfterMs;
  x.linkAgeS = !have ? 255 : (uint8_t)(ageMs / 1000u > 255u ? 255u : ageMs / 1000u);
  x.lastCmdId = lastCmdId;
  x.lastCmdResult = lastCmdResult;
  packBleStatus(d, x, out);
}

}  // namespace wh
