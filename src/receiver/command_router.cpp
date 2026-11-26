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
  ev.isStop = r.kind == wh::CmdKind::Stop;
  sink_.emit(ev);
}

// A Stop that the heater accepted ends the climate session. This happens at acceptance, not at
// the request: while the Stop is parked the climate session keeps managing the heater.
void CommandRouter::stopAccepted(const heater::HeaterRequest& r) {
  climate_.notifyUserStop(now_);
  persist_ = true;
  if (r.src == heater::Source::Ble) emit(r, (uint8_t)wh::CmdResult::Pending, 0);  // queued
}

// Parking rules (one parked request at most):
//  - A parked Stop is sticky. A newer Heat/Vent is rejected with Busy while it is parked, and it
//    must not be submitted ahead of it either.
//  - A newer Stop replaces any parked request (the older one is reported as Busy).
//  - A Stop that is accepted drops a parked Heat/Vent: it must not start the heater afterwards.
void CommandRouter::submitOrPark(const heater::HeaterRequest& r, uint32_t now) {
  now_ = now;
  const bool isStop = r.kind == wh::CmdKind::Stop;
  if (!isStop && parkedIsStop()) {
    emit(r, (uint8_t)wh::CmdResult::Busy, 4);
    return;
  }
  if (heater_.submit(r)) {
    if (isStop) {
      if (parked_) emit(parkedReq_, (uint8_t)wh::CmdResult::Busy, 4);  // superseded
      parked_ = false;
      stopAccepted(r);
    } else if (r.src == heater::Source::Ble) {
      emit(r, (uint8_t)wh::CmdResult::Pending, 0);  // queued
    }
    return;
  }
  if (parked_) emit(parkedReq_, (uint8_t)wh::CmdResult::Busy, 4);  // replaced by a newer request
  parked_ = true;
  parkedReq_ = r;
  parkedAt_ = now;
  parkedReported_ = false;
  if (isStop && r.src == heater::Source::Ble) emit(r, (uint8_t)wh::CmdResult::Pending, 0);
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
      submitOrPark(r, now);  // ends the climate session once the heater accepted the Stop
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
  now_ = now;
  // Retried every tick (a failed submit is one atomic compare-exchange, no bus traffic). Over CAN a
  // Stop to an already stopped heater is harmless, so no extra "heater is off" shortcut is used:
  // the phase is not trusted while a start is still in flight.
  if (heater_.submit(parkedReq_)) {
    const heater::HeaterRequest r = parkedReq_;
    parked_ = false;
    if (r.kind == wh::CmdKind::Stop) {
      stopAccepted(r);
    } else if (r.src == heater::Source::Ble) {
      emit(r, (uint8_t)wh::CmdResult::Pending, 0);
    }
    return;
  }
  if ((uint32_t)(now - parkedAt_) < kRetryMs) return;
  if (parkedReq_.kind == wh::CmdKind::Stop) {
    // Never dropped: tell the source once that the Stop is still pending (stage 1, not final),
    // keep retrying. The final result comes from the heater event of the Stop.
    if (!parkedReported_) {
      parkedReported_ = true;
      emit(parkedReq_, (uint8_t)wh::CmdResult::Pending, 1);
    }
    return;
  }
  parked_ = false;
  emit(parkedReq_, (uint8_t)wh::CmdResult::Busy, 4);
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
  if (parkedIsStop()) {  // a Stop is waiting for the heater: nothing may start in front of it
    climate_.notifyStartResult(now, wh::CmdResult::Busy);
    return;
  }
  heater::HeaterRequest r;
  r.kind = wh::CmdKind::Heat;
  r.src = heater::Source::Climate;
  if (!heater_.submit(r)) climate_.notifyStartResult(now, wh::CmdResult::Busy);
}

}  // namespace rx
