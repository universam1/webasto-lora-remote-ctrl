#include "command_router.h"

namespace rx {

static_assert((uint8_t)rt::Source::Menu == (uint8_t)heater::Source::Menu &&
                  (uint8_t)rt::Source::Lora == (uint8_t)heater::Source::Lora &&
                  (uint8_t)rt::Source::Mqtt == (uint8_t)heater::Source::Mqtt &&
                  (uint8_t)rt::Source::Ble == (uint8_t)heater::Source::Ble &&
                  (uint8_t)rt::Source::Climate == (uint8_t)heater::Source::Climate &&
                  (uint8_t)rt::Source::Serial == (uint8_t)heater::Source::Serial,
              "rt::Source and heater::Source must stay in the same order");

void CommandRouter::emit(const heater::HeaterRequest& r, uint8_t result, uint8_t stage) {
  rt::CmdEvent ev;
  ev.src = static_cast<rt::Source>(r.src);
  ev.tag = r.tag;
  ev.loraSeq = r.loraSeq;
  ev.result = result;
  ev.stage = stage;
  sink_.emit(ev);
}

void CommandRouter::submitOrPark(const heater::HeaterRequest& r, uint32_t now) {
  if (heater_.submit(r)) {
    if (r.src == heater::Source::Ble) emit(r, (uint8_t)wh::CmdResult::Pending, 0);  // queued
    return;
  }
  // A newer request replaces a parked one (a Stop after a Heat wins, which is what the user wants).
  if (parked_) emit(parkedReq_, (uint8_t)wh::CmdResult::Busy, 4);
  parked_ = true;
  parkedReq_ = r;
  parkedAt_ = now;
}

void CommandRouter::route(const rt::Command& c, uint32_t now) {
  heater::HeaterRequest r;
  r.src = static_cast<heater::Source>(c.src);
  r.tag = c.tag;
  r.loraSeq = c.loraSeq;
  r.minutes = c.minutes;
  switch (c.type) {
    case rt::CmdType::Heat:
      r.kind = wh::CmdKind::Heat;
      submitOrPark(r, now);
      break;
    case rt::CmdType::Stop:
      r.kind = wh::CmdKind::Stop;
      climate_.notifyUserStop(now);  // also ends the climate session
      persist_ = true;
      submitOrPark(r, now);
      break;
    case rt::CmdType::Query:
      // A LoRa query is answered by its own result event (forceNow); do not send twice.
      if (c.src != rt::Source::Lora) sink_.forceStatus();
      emit(r, (uint8_t)wh::CmdResult::Ok, 3);
      break;
    case rt::CmdType::ClimateSet: {
      wh::ClimateSettings s = climate_.settings();
      if (c.targetC != 0) s.targetC = c.targetC;
      if (c.minutes != 0) s.durationMin = c.minutes;
      climate_.configure(s);
      climate_.enable(now);
      persist_ = true;
      emit(r, (uint8_t)wh::CmdResult::Ok, 3);
      break;
    }
    case rt::CmdType::ClimateOff:
      climate_.disable();
      persist_ = true;
      emit(r, (uint8_t)wh::CmdResult::Ok, 3);
      break;
  }
}

void CommandRouter::service(uint32_t now) {
  if (!parked_) return;
  if (heater_.submit(parkedReq_)) {
    parked_ = false;
    if (parkedReq_.src == heater::Source::Ble) emit(parkedReq_, (uint8_t)wh::CmdResult::Pending, 0);
    return;
  }
  if ((uint32_t)(now - parkedAt_) >= kRetryMs) {
    parked_ = false;
    emit(parkedReq_, (uint8_t)wh::CmdResult::Busy, 4);
  }
}

void CommandRouter::onHeaterEvent(const heater::HeaterEvent& ev, uint32_t now) {
  if (ev.req.kind == wh::CmdKind::Wake) return;  // internal, nobody waits for it
  if (ev.req.src == heater::Source::Climate) {
    climate_.notifyStartResult(now, ev.result);
    return;
  }
  const bool ok = ev.result == wh::CmdResult::Ok || ev.result == wh::CmdResult::OkUnconfirmed;
  emit(ev.req, (uint8_t)ev.result, ok ? 3 : 4);
}

void CommandRouter::startClimate(uint32_t now) {
  heater::HeaterRequest r;
  r.kind = wh::CmdKind::Heat;
  r.src = heater::Source::Climate;
  if (!heater_.submit(r)) climate_.notifyStartResult(now, wh::CmdResult::Busy);
}

}  // namespace rx
