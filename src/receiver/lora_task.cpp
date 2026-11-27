// loraTask (core 1, prio 4, 10 ms): the only task that touches the SX1276 / SPI.
// Receives sender packets, dedupes them, turns them into rt::Command for the ctrlTask and sends
// status packets only inside the sender-active window (wh::shouldSendStatus).
#include <Arduino.h>

#include "encryption.h"
#include "lora_link.h"
#include "net_task.h"
#include "project_config.h"
#include "lora_ack.h"
#include "protocol.h"
#include "receiver_ctx.h"
#include "send_policy.h"
#include "status_codec.h"
#include "task_stacks.h"
#include "task_utils.h"

namespace rx {
namespace {

constexpr uint32_t kPeriodMs = 10;
constexpr uint32_t kDiagPeriodMs = 1000;
constexpr uint32_t kStackLogPeriodMs = 60000;

LoRaLink g_lora;
uint16_t g_seq = 1;

// ACK correlation (pending seq, last finished seq with validity flag, timeout). Deep sleep is not
// supported any more, so nothing has to survive a reset.
rt::LoraAckTracker g_ack;

uint8_t mapError(wh::CmdResult r) {
  switch (r) {
    case wh::CmdResult::Ok:
    case wh::CmdResult::OkUnconfirmed:
    case wh::CmdResult::Pending:
      return 0;
    case wh::CmdResult::Unsupported:
      return wh::kErrUnsupported;
    case wh::CmdResult::NoBus:
    case wh::CmdResult::BusOff:
      return wh::kErrNoBus;
    case wh::CmdResult::NotAdopted:
    case wh::CmdResult::Busy:
    case wh::CmdResult::Timeout:
    default:
      return wh::kErrNotAdopted;
  }
}

const char* sourceName(uint8_t s) {
  switch (s) {
    case (uint8_t)rt::Source::Menu: return "menu";
    case (uint8_t)rt::Source::Lora: return "lora";
    case (uint8_t)rt::Source::Mqtt: return "mqtt";
    case (uint8_t)rt::Source::Ble: return "ble";
    case (uint8_t)rt::Source::Climate: return "climate";
    default: return "none";
  }
}

bool heaterOn(wh::HeaterPhase p) {
  return p == wh::HeaterPhase::Heating || p == wh::HeaterPhase::RunOn || p == wh::HeaterPhase::Ventilating;
}

// Maps a wire command to an rt::Command. false = invalid arguments.
bool mapCommand(const proto::Packet& pkt, rt::Command& c) {
  c = rt::Command{};
  c.src = rt::Source::Lora;
  c.loraSeq = pkt.h.seq;
  const proto::CommandPayload& p = pkt.p.cmd;
  switch (p.kind) {
    case proto::CommandKind::Stop:
      c.type = rt::CmdType::Stop;
      return true;
    case proto::CommandKind::Start:
    case proto::CommandKind::RunMinutes:
      c.type = rt::CmdType::Heat;
      c.minutes = p.minutes;
      return true;
    case proto::CommandKind::QueryStatus:
      c.type = rt::CmdType::Query;
      return true;
    case proto::CommandKind::ClimateSet:
      if (p.targetC < 10 || p.targetC > 24 || p.minutes < 10 || p.minutes > 480) return false;
      c.type = rt::CmdType::ClimateSet;
      c.targetC = p.targetC;
      c.minutes = p.minutes;
      return true;
    case proto::CommandKind::ClimateOff:
      c.type = rt::CmdType::ClimateOff;
      return true;
    default:
      return false;
  }
}

void loraTaskFn(void*) {
  Ctx& c = ctx();
  rt::LoopTimer timer(kPeriodMs);

  wh::SystemSnapshot snap;
  bool haveSnap = false;
  int rssi = 0;
  float snr = 0;

  // Status that is waiting for the radio (send() returns false while TX is on air).
  bool forcePending = false;
  uint8_t errOverride = 0;

  // Range test: the Pong for the last Ping. A newer ping replaces an unsent pong. A pong goes out
  // before any status, so the sender's round trip time stays short.
  bool pongPending = false;
  uint16_t pongPingSeq = 0;
  int8_t pongRssi = 0, pongSnr = 0;

  bool everSent = false;
  uint32_t lastTxMs = 0;
  wh::HeaterPhase sentPhase = wh::HeaterPhase::Unknown;
  uint8_t sentErr = 0;
  wh::ClimateStatus sentClimate = wh::ClimateStatus::Off;
  uint32_t lastDiagMs = 0;
  uint32_t lastStackLogMs = 0;
  uint32_t lastLoggedDrops = 0, lastLoggedStale = 0, lastLoggedTimeouts = 0;

  wh::SendPolicyParams params;
  params.senderActiveMs = LORA_SENDER_ACTIVE_MS;
  params.runningMs = LORA_STATUS_INTERVAL_RUNNING_MS;
  params.climateMs = LORA_STATUS_INTERVAL_CLIMATE_MS;
  params.idleMs = LORA_STATUS_INTERVAL_IDLE_MS;
  params.minGapMs = LORA_STATUS_MIN_GAP_MS;

  for (;;) {
    const uint32_t now = millis();
    g_lora.service();

    if (c.snapshot.read(snap)) haveSnap = true;

    // 1. Receive. A valid sender packet opens the window BEFORE anything is answered.
    proto::Packet pkt{};
    int r = 0;
    float s = 0;
    while (g_lora.recv(pkt, r, s)) {
      c.loraRxCount.fetch_add(1);
      if (pkt.h.type == proto::MsgType::Ping && pkt.h.dst == LORA_NODE_RECEIVER &&
          pkt.h.src == LORA_NODE_SENDER) {
        // Range test: answer at once, with the RSSI/SNR of this ping. A ping is not a command: it
        // does not open the sender-active window and never reaches the heater.
        pongPending = true;
        pongPingSeq = pkt.h.seq;
        pongRssi = (int8_t)r;
        pongSnr = (int8_t)s;
        c.pingCount.fetch_add(1);
        Serial.printf("[lora] ping seq=%u rssi=%d snr=%.1f\n", pkt.h.seq, r, s);
        continue;
      }
      if (pkt.h.type != proto::MsgType::Command || pkt.h.dst != LORA_NODE_RECEIVER ||
          pkt.h.src != LORA_NODE_SENDER)
        continue;
      rssi = r;
      snr = s;
      c.loraRssi.store(r);
      c.loraSnr.store(s);
      c.lastSenderRequestMs.store(now);
      c.everRequested.store(true);

      Serial.printf("[lora] cmd kind=%d min=%u seq=%u rssi=%d snr=%.1f\n", (int)pkt.p.cmd.kind,
                    pkt.p.cmd.minutes, pkt.h.seq, r, s);

      const rt::LoraAckTracker::Rx rx = g_ack.onPacket(pkt.h.seq);
      if (rx == rt::LoraAckTracker::Rx::Running) continue;  // repeat of a running command
      if (rx == rt::LoraAckTracker::Rx::Done) {             // repeat of a finished command
        forcePending = true;
        errOverride = 0;
        continue;
      }
      rt::Command cmd;
      if (!mapCommand(pkt, cmd)) {
        g_ack.finish(pkt.h.seq);
        forcePending = true;
        errOverride = wh::kErrUnsupported;
        continue;
      }
      // The sender runs one job at a time; a new seq replaces a pending one (its late result is
      // ignored by the tracker).
      if (c.cmdQueue.push(cmd)) {
        g_ack.begin(pkt.h.seq, now);
      } else {  // queue full: answer with an error status at once
        g_ack.finish(pkt.h.seq);
        forcePending = true;
        errOverride = wh::kErrNotAdopted;
      }
    }

    // 2. Command results from the ctrlTask: the next status is the ACK.
    rt::CmdEvent ev;
    while (c.loraEvents.pop(ev)) {
      if (ev.stage < 3) continue;
      if (!g_ack.onResult(ev.loraSeq)) continue;  // result of an older seq: not the ACK
      forcePending = true;
      errOverride = ev.stage == 4 ? mapError((wh::CmdResult)ev.result) : 0;
    }
    // A lost result event must not block the ACK forever: answer failed, the sender retries.
    if (g_ack.checkTimeout(now)) {
      Serial.println("[lora] pending command timed out, answering failed");
      forcePending = true;
      errOverride = wh::kErrNotAdopted;
    }
    if (c.forceStatus.exchange(false)) forcePending = true;

    // 2b. Range test answer. send() fails while the radio is on air: it is tried again in the next
    // pass (10 ms); once the sender's timeout has passed the ping counts as lost there anyway.
    if (pongPending) {
      proto::Packet po{};
      po.h.magic_version = proto::kMagicVersion;
      po.h.type = proto::MsgType::Pong;
      po.h.src = LORA_NODE_RECEIVER;
      po.h.dst = LORA_NODE_SENDER;
      po.h.seq = g_seq;  // own counter (shared with status): the AES-CTR nonce is never reused
      po.p.pong.pingSeq = pongPingSeq;
      po.p.pong.rssiDbm = pongRssi;
      po.p.pong.snrDb = pongSnr;
      po.crc = proto::calcCrc(po);
      if (g_lora.send(po)) {
        g_seq++;
        pongPending = false;
        c.pongCount.fetch_add(1);
      }
    }

    // 3. Status sending according to the send window.
    if (haveSnap && !pongPending) {
      wh::SendInputs in;
      in.now = now;
      in.everRequested = c.everRequested.load();
      in.lastSenderRequestMs = c.lastSenderRequestMs.load();
      in.lastTxMs = lastTxMs;
      in.everSent = everSent;
      in.heaterRunning = heaterOn(snap.heater.phase) || snap.climateStatus == wh::ClimateStatus::Starting;
      in.climateActive = snap.climateActive;
      in.eventPending = everSent && (snap.heater.phase != sentPhase || snap.heater.errorCode != sentErr ||
                                     snap.climateStatus != sentClimate);
      in.forceNow = forcePending;
      if (wh::shouldSendStatus(in, params) != wh::SendReason::None) {
        proto::Packet st{};
        st.h.magic_version = proto::kMagicVersion;
        st.h.type = proto::MsgType::Status;
        st.h.src = LORA_NODE_RECEIVER;
        st.h.dst = LORA_NODE_SENDER;
        st.h.seq = g_seq;
        wh::packLoraStatus(snap, g_ack.wireLastSeq(), errOverride, st.p.status);
        st.p.status.lastRssiDbm = (int8_t)rssi;
        st.p.status.lastSnrDb = (int8_t)snr;
        st.crc = proto::calcCrc(st);
        if (g_lora.send(st)) {  // returns at once; false while the radio is still on air
          g_seq++;
          everSent = true;
          lastTxMs = now;
          sentPhase = snap.heater.phase;
          sentErr = snap.heater.errorCode;
          sentClimate = snap.climateStatus;
          forcePending = false;
          errOverride = 0;
          c.loraTxCount.fetch_add(1);
        }
      } else if (forcePending && in.everRequested == false) {
        forcePending = false;  // nobody asked: nothing to answer
      } else if (forcePending && (uint32_t)(now - in.lastSenderRequestMs) >= params.senderActiveMs) {
        forcePending = false;  // window closed (e.g. a Query from BLE long after the sender)
      }
    }

    // Diagnostics: counters only when they change, stack watermarks every minute.
    {
      const uint32_t drops = c.eventsDropped.load() + c.eventsUnrouted.load();
      if (drops != lastLoggedDrops || g_ack.staleResults() != lastLoggedStale ||
          g_ack.timeouts() != lastLoggedTimeouts) {
        lastLoggedDrops = drops;
        lastLoggedStale = g_ack.staleResults();
        lastLoggedTimeouts = g_ack.timeouts();
        Serial.printf("[diag] events dropped=%u (stop=%u) unrouted=%u stale_lora=%u lora_timeouts=%u\n",
                      (unsigned)c.eventsDropped.load(), (unsigned)c.stopEventsDropped.load(),
                      (unsigned)c.eventsUnrouted.load(), (unsigned)lastLoggedStale,
                      (unsigned)lastLoggedTimeouts);
      }
      if ((uint32_t)(now - lastStackLogMs) >= kStackLogPeriodMs) {
        lastStackLogMs = now;
        rt::logStackWatermarks();
      }
    }

#ifdef ENABLE_MQTT_CONTROL
    if ((uint32_t)(now - lastDiagMs) >= kDiagPeriodMs) {
      lastDiagMs = now;
      netTaskSetDiagnostics(c.loraRssi.load(), c.loraSnr.load(), sourceName(c.lastCmdSource.load()),
                            haveSnap ? snap.heater.healthy : true);
    }
#else
    (void)lastDiagMs;
    (void)sourceName;
#endif

    timer.waitNext();
  }
}

}  // namespace

bool startLoraTask() {
  crypto::AES128CTR::setKey(proto::kDefaultPSK);
  const bool ok = g_lora.begin();
  if (ok) g_lora.enableInterrupt();
  ctx().loraOk.store(ok);
  Serial.printf("[setup] LoRa %s\n", ok ? "OK (IRQ)" : "FAIL");
  if (!ok) return false;
  rt::TaskSpec spec = {"loraTask", loraTaskFn, rt::kStackLora, 4, 1, nullptr};
  return rt::startTask(spec) != nullptr;
}

}  // namespace rx
