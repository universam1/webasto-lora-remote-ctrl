#include <Arduino.h>

#include "project_config.h"
#include "lora_link.h"
#include "oled_ui.h"
#include "protocol.h"
#include "status_led.h"
#include "encryption.h"
#include "menu_handler.h"
#include "status_codec.h"
#include "lora_airtime.h"
#include "range_test.h"
#include "sender_ble_map.h"
#if defined(ENABLE_BLE) && ENABLE_BLE
#include "sender_ble.h"
#endif

// ---------------------------------------------------------------------------------------------
// Sender-only constants (WP4). Defaults here because include/project_config.h is not owned by this
// package. The coordinator may move these into project_config.h:
//   SENDER_JOB_TIMEOUT_MS, SENDER_KEEPALIVE_MS, SENDER_STATUS_INTERVAL_MS, SENDER_USER_ACTIVE_MS,
//   SENDER_INACTIVE_HINT_MS, SENDER_DEFAULT_CLIMATE_C, SENDER_DEFAULT_CLIMATE_MIN
// ---------------------------------------------------------------------------------------------
#ifndef SENDER_JOB_TIMEOUT_MS
#define SENDER_JOB_TIMEOUT_MS 20000       // overall deadline of one command job
#endif
#ifndef SENDER_KEEPALIVE_MS
#define SENDER_KEEPALIVE_MS 600000        // QueryStatus when the last status is older (user active only)
#endif
#ifndef SENDER_STATUS_INTERVAL_MS
#define SENDER_STATUS_INTERVAL_MS 10000   // expected receiver status interval; "veraltet" after 3 of them
#endif
#ifndef SENDER_USER_ACTIVE_MS
#define SENDER_USER_ACTIVE_MS 120000      // gUserActive stays true this long after the last activity
#endif
#ifndef SENDER_INACTIVE_HINT_MS
#define SENDER_INACTIVE_HINT_MS 1800000   // "Sender inaktiv" hint (receiver stops sending after 30 min)
#endif
#ifndef SENDER_DEFAULT_CLIMATE_C
#define SENDER_DEFAULT_CLIMATE_C 20
#endif
#ifndef SENDER_DEFAULT_CLIMATE_MIN
#define SENDER_DEFAULT_CLIMATE_MIN 120
#endif
static constexpr uint32_t kSenderJobTimeoutMs = SENDER_JOB_TIMEOUT_MS;
static constexpr uint32_t kSenderKeepaliveMs = SENDER_KEEPALIVE_MS;
static constexpr uint32_t kSenderStatusIntervalMs = SENDER_STATUS_INTERVAL_MS;
static constexpr uint32_t kSenderUserActiveMs = SENDER_USER_ACTIVE_MS;
static constexpr uint32_t kSenderInactiveHintMs = SENDER_INACTIVE_HINT_MS;

// Job result reporting (values match the BLE result characteristic: ble::Stage and wh::CmdResult).
enum : uint8_t { kStageSent = 1, kStageAcked = 2, kStageDone = 3, kStageFailed = 4 };
enum : uint8_t { kResOk = 1, kResNotAdopted = 3, kResUnsupported = 4, kResNoBus = 6, kResTimeout = 8 };

static OledUi ui;
static LoRaLink loraLink;
static StatusLed statusLed;
static MenuHandler menu;

static uint16_t gSeq = 1;
static uint16_t gLastMinutes = DEFAULT_RUN_MINUTES;            // Start / run N
static uint8_t gClimateTargetC = SENDER_DEFAULT_CLIMATE_C;     // last used climate target
static uint16_t gClimateMinutes = SENDER_DEFAULT_CLIMATE_MIN;  // last used climate duration

// Last received status (decoded) and its receive time. gStatusRxMs == 0: none yet.
static wh::DecodedStatus gStatus;
static uint32_t gStatusRxMs = 0;

// User activity: set by button/menu/serial activity and by senderNoteUserActivity() (also called for BLE activity).
bool gUserActive = false;
static uint32_t gUserActivityMs = 0;
void senderNoteUserActivity()
{
  gUserActive = true;
  gUserActivityMs = millis();
}

// Battery monitoring state
static float gBattV = 0.0f;
static uint32_t gLastBattUpdateMs = 0;

// Job result hook (set by the BLE glue below, null without BLE).
void (*gJobDone)(uint8_t bleCmdId, uint8_t stage, uint8_t result) = nullptr;

// Short result text on the OLED
static String gResultText;
static uint32_t gResultUntilMs = 0;

static String readLineNonBlocking()
{
  static String buf;
  while (Serial.available())
  {
    char c = static_cast<char>(Serial.read());
    if (c == '\r')
      continue;
    if (c == '\n')
    {
      String line = buf;
      buf = "";
      line.trim();
      return line;
    }
    buf += c;
    if (buf.length() > 128)
      buf.remove(0, buf.length() - 128);
  }
  return "";
}

// ---- Command job (non-blocking replacement of sendCommandWithAck) ----------------------------

enum class JobState : uint8_t { Idle, Sending, WaitAck };

struct CmdJob
{
  JobState state = JobState::Idle;
  proto::CommandPayload cmd{};
  uint8_t bleCmdId = 0;  // 0 = none
  uint16_t seq = 0;
  uint32_t startMs = 0;
  uint32_t nextTxMs = 0;
  uint8_t txCount = 0;
};
static CmdJob gJob;

static const char *kindToStr(proto::CommandKind k)
{
  switch (k)
  {
  case proto::CommandKind::Stop: return "STOP";
  case proto::CommandKind::Start: return "START";
  case proto::CommandKind::RunMinutes: return "RUN";
  case proto::CommandKind::QueryStatus: return "QUERY";
  case proto::CommandKind::ClimateSet: return "KLIMA";
  case proto::CommandKind::ClimateOff: return "KLIMA AUS";
  default: return "?";
  }
}

static bool jobBusy() { return gJob.state != JobState::Idle; }

static void jobReport(uint8_t stage, uint8_t result)
{
  if (gJobDone && gJob.bleCmdId != 0)
    gJobDone(gJob.bleCmdId, stage, result);
}

static void jobFinish(bool ok, uint8_t result, const char *text)
{
  Serial.printf("[JOB] %s seq=%u after %u tx: %s\n", ok ? "done" : "FAILED", gJob.seq, gJob.txCount, text);
  jobReport(ok ? kStageDone : kStageFailed, result);
  // Query jobs are silent on the display unless they fail.
  if (!(ok && gJob.cmd.kind == proto::CommandKind::QueryStatus))
  {
    gResultText = String(kindToStr(gJob.cmd.kind)) + (ok ? " ok" : String(" ") + text);
    gResultUntilMs = millis() + 5000;
  }
  gJob.state = JobState::Idle;
  gJob.bleCmdId = 0;
  statusLed.setOff();
}

// Returns false when a job is already running.
static bool startJob(const proto::CommandPayload &cmd, uint8_t bleCmdId)
{
  if (jobBusy())
    return false;
  gJob = CmdJob{};
  gJob.cmd = cmd;
  gJob.bleCmdId = bleCmdId;
  gJob.seq = gSeq++;
  if (gSeq == 0)
    gSeq = 1;  // seq 0 is the receiver's "no command yet" value
  gJob.startMs = millis();
  gJob.nextTxMs = gJob.startMs;
  gJob.state = JobState::Sending;
  statusLed.setBlink(200);
  Serial.printf("[JOB] start %s target=%u minutes=%u seq=%u\n", kindToStr(cmd.kind), cmd.targetC, cmd.minutes, gJob.seq);
  return true;
}

static proto::Packet makeCommandPacket(const proto::CommandPayload &cmd, uint16_t seq)
{
  proto::Packet pkt{};
  pkt.h.magic_version = proto::kMagicVersion;
  pkt.h.type = proto::MsgType::Command;
  pkt.h.src = LORA_NODE_SENDER;
  pkt.h.dst = LORA_NODE_RECEIVER;
  pkt.h.seq = seq;
  pkt.p.cmd = cmd;
  pkt.crc = proto::calcCrc(pkt);
  return pkt;
}

// Call every loop iteration. Never blocks.
static void serviceJob(uint32_t now)
{
  if (!jobBusy())
    return;

  // `now` is taken at the top of the loop; a job started later in the same pass (serial, BLE,
  // menu) has startMs > now. Signed difference so that case counts as "just started", not as
  // a wrapped 49-day age that failed the job at once with Timeout.
  if ((int32_t)(now - gJob.startMs) >= (int32_t)kSenderJobTimeoutMs)
  {
    jobFinish(false, kResTimeout, "Timeout");
    return;
  }

  if (gJob.state == JobState::WaitAck && (int32_t)(now - gJob.nextTxMs) >= 0)
    gJob.state = JobState::Sending;

  if (gJob.state == JobState::Sending && (int32_t)(now - gJob.nextTxMs) >= 0 && !loraLink.txBusy())
  {
    // Same sequence number on every retry. The ACK is the status with lastCmdSeq == seq.
    if (loraLink.send(makeCommandPacket(gJob.cmd, gJob.seq)))
    {
      statusLed.toggle();
      gJob.txCount++;
      if (gJob.txCount == 1)
        jobReport(kStageSent, 0);
      // Retry interval: own airtime + the receiver's reply airtime + margin, so the reply is not
      // lost while we transmit again (lora_airtime.h). Jitter 0..+25 % only lengthens it.
      static const uint32_t kRetryMs = []() {
        const uint32_t cmdAir = lora::airtimeUs(sizeof(proto::PacketHeader) + sizeof(proto::CommandPayload) + 2,
                                                LORA_SF, (uint32_t)LORA_BW, LORA_CR);
        const uint32_t replyAir = lora::airtimeUs(sizeof(proto::PacketHeader) + sizeof(proto::StatusPayload) + 2,
                                                  LORA_SF, (uint32_t)LORA_BW, LORA_CR);
        const uint32_t derived = lora::commandRetryMs(cmdAir, replyAir);
        return derived > (uint32_t)SENDER_CMD_RETRY_INTERVAL_MS ? derived : (uint32_t)SENDER_CMD_RETRY_INTERVAL_MS;
      }();
      const uint32_t jitter = (uint32_t)random(26) * kRetryMs / 100;
      gJob.nextTxMs = now + kRetryMs + jitter;
      gJob.state = JobState::WaitAck;
    }
    else
    {
      gJob.nextTxMs = now + 100;  // radio not ready, try again shortly
    }
  }
}

// Called for every valid status from the receiver.
static void onStatusReceived(const wh::DecodedStatus &st)
{
  if (jobBusy() && gJob.txCount > 0 && st.lastCmdSeq == gJob.seq)
  {
    jobReport(kStageAcked, 0);
    switch (st.errorCode)
    {
    case wh::kErrNotAdopted: jobFinish(false, kResNotAdopted, "nicht uebernommen"); break;
    case wh::kErrUnsupported: jobFinish(false, kResUnsupported, "nicht unterstuetzt"); break;
    case wh::kErrNoBus: jobFinish(false, kResNoBus, "kein Bus"); break;
    default: jobFinish(true, kResOk, "ok"); break;
    }
  }
}

// ---- Display text helpers ---------------------------------------------------------------------
// The OLED font (u8g2_font_6x12_tr) has no umlauts, so the display uses ae/ue/oe.

static const char *phaseToStr(wh::HeaterPhase p)
{
  switch (p)
  {
  case wh::HeaterPhase::Off: return "Aus";
  case wh::HeaterPhase::Heating: return "Heizt";
  case wh::HeaterPhase::RunOn: return "Nachlauf";
  case wh::HeaterPhase::Ventilating: return "Lueftet";
  case wh::HeaterPhase::Error: return "Fehler";
  default: return "?";
  }
}

static const char *climateStatusToStr(wh::ClimateStatus s)
{
  switch (s)
  {
  case wh::ClimateStatus::Off: return "aus";
  case wh::ClimateStatus::Idle: return "warm genug";
  case wh::ClimateStatus::Starting: return "startet";
  case wh::ClimateStatus::HeaterRunning: return "heizt";
  case wh::ClimateStatus::Pause: return "Pause";
  case wh::ClimateStatus::SensorFault: return "Sensor def.";
  case wh::ClimateStatus::LowVoltage: return "Spannung";
  case wh::ClimateStatus::Failed: return "fehlgeschl.";
  default: return "?";
  }
}

static String fmtDC(int16_t dC)
{
  if (dC == wh::kUnknownDC)
    return "--";
  return String(dC / 10.0f, 1) + "C";
}

// ---- OLED -------------------------------------------------------------------------------------

static const char *menuLabel(MenuItem item)
{
  switch (item)
  {
  case MenuItem::Start: return "Heizen";
  case MenuItem::Stop: return "Aus";
  case MenuItem::ClimateOn: return "Klima an";
  case MenuItem::ClimateOff: return "Klima aus";
  case MenuItem::QueryStatus: return "Abfrage";
  case MenuItem::BlePair: return "BLE koppeln";
  case MenuItem::BleClear: return "Bindungen loesch.";
  case MenuItem::RangeTest: return "Ping-Pong";
  default: return "-";
  }
}

// ---- Range test (ping-pong) -------------------------------------------------------------------
// Menu "Ping-Pong" or serial "ping [n]" / "pingstop". While it runs the OLED shows the live
// statistics and no heater command is sent: the receiver answers every ping with a pong that
// carries the RSSI/SNR it measured. The test needs no heater, only two radios.
static wh::RangeTest gRange;
static uint16_t gRangeLastPongSeq = 0;
static uint32_t gRangeShowUntilMs = 0;  // keep the result on the OLED for a while after the test ended

static void rangeStart(uint32_t maxPings)
{
  if (jobBusy())
  {
    Serial.println("[PING] busy: a command is still running");
    return;
  }
  wh::RangeParams p = wh::rangeParamsFor(
      LORA_SF, (uint32_t)LORA_BW, LORA_CR,
      sizeof(proto::PacketHeader) + 2,                               // ping: header + crc
      sizeof(proto::PacketHeader) + sizeof(proto::PongPayload) + 2);  // pong
  p.maxPings = maxPings;
  gRange.start(millis(), p);
  gRangeShowUntilMs = 0;
  senderNoteUserActivity();
  Serial.printf("[PING] start: interval %u ms, timeout %u ms, %s pings\n", (unsigned)p.intervalMs,
                (unsigned)p.timeoutMs, maxPings ? String(maxPings).c_str() : "unlimited");
}

static void rangeStop(const char *why)
{
  if (!gRange.active())
    return;
  gRange.stop();
  gRangeShowUntilMs = millis() + 20000;  // result stays visible for 20 s
  const wh::RangeStats &s = gRange.stats();
  Serial.printf("[PING] %s: sent=%u ok=%u lost=%u late=%u loss=%u.%u%% maxLostStreak=%u\n", why, (unsigned)s.sent,
                (unsigned)s.ok, (unsigned)s.lost, (unsigned)s.late, (unsigned)(s.lossPermille() / 10),
                (unsigned)(s.lossPermille() % 10), (unsigned)s.maxLostStreak);
  if (s.ok)
    Serial.printf("[PING] uplink rssi min/avg/max=%d/%d/%d snr min/avg=%d/%d | downlink rssi min/avg/max=%d/%d/%d snr min/avg=%d/%d | rtt min/avg/max=%d/%d/%d ms\n",
                  (int)s.upRssi.min, (int)s.upRssi.avg(), (int)s.upRssi.max, (int)s.upSnr.min, (int)s.upSnr.avg(),
                  (int)s.downRssi.min, (int)s.downRssi.avg(), (int)s.downRssi.max, (int)s.downSnr.min,
                  (int)s.downSnr.avg(), (int)s.rttMs.min, (int)s.rttMs.avg(), (int)s.rttMs.max);
}

// Sender side of the test: transmit the next ping when it is due, give up on a missing pong.
static void serviceRange(uint32_t now)
{
  if (!gRange.active())
    return;
  // A heater command has priority: it would collide with the pings on the air.
  if (jobBusy())
  {
    rangeStop("aborted by a command");
    return;
  }
  const wh::RangeTest::Event ev = gRange.poll(now);
  if (ev == wh::RangeTest::Event::Lost || ev == wh::RangeTest::Event::Finished)
  {
    const wh::RangeStats &s = gRange.stats();
    Serial.printf("[PING] seq lost (timeout), %u in a row, loss %u.%u%%\n", (unsigned)s.lostStreak,
                  (unsigned)(s.lossPermille() / 10), (unsigned)(s.lossPermille() % 10));
    if (ev == wh::RangeTest::Event::Finished)
    {
      gRangeShowUntilMs = now + 20000;
      Serial.printf("[PING] finished: sent=%u ok=%u lost=%u\n", (unsigned)s.sent, (unsigned)s.ok, (unsigned)s.lost);
    }
    return;
  }
  if (gRange.due(now) && !loraLink.txBusy())
  {
    proto::Packet pkt{};
    pkt.h.magic_version = proto::kMagicVersion;
    pkt.h.type = proto::MsgType::Ping;
    pkt.h.src = LORA_NODE_SENDER;
    pkt.h.dst = LORA_NODE_RECEIVER;
    pkt.h.seq = gSeq;
    pkt.crc = proto::calcCrc(pkt);
    if (loraLink.send(pkt))
    {
      statusLed.toggle();
      gRange.onSent(gSeq, now);
      Serial.printf("[PING] -> seq=%u\n", gSeq);
      gSeq++;
      if (gSeq == 0)
        gSeq = 1;
    }
  }
}

// A pong arrived (called from the receive path).
static void onPongReceived(const proto::Packet &pkt, int rssi, float snr, uint32_t now)
{
  const bool ok = gRange.onPong(pkt.p.pong.pingSeq, pkt.p.pong.rssiDbm, pkt.p.pong.snrDb, rssi, (int)snr, now);
  Serial.printf("[PING] <- pong for seq=%u %s up rssi=%d snr=%d | down rssi=%d snr=%.1f\n", pkt.p.pong.pingSeq,
                ok ? "OK" : "(late/stray)", (int)pkt.p.pong.rssiDbm, (int)pkt.p.pong.snrDb, rssi, snr);
  gRangeLastPongSeq = pkt.p.pong.pingSeq;
}

static void renderRangeTest(uint32_t now)
{
  const wh::RangeStats &s = gRange.stats();
  ui.drawProgressBar(0, 0, 0.0f);
  ui.setLine(0, String("PING-PONG ") + (gRange.active() ? (gRange.pending() ? "..." : "") : "(Ende)"));
  ui.setLine(1, String("Ping ") + String(s.sent) + " ok " + String(s.ok) + " v " + String(s.lost));
  ui.setLine(2, String("Verlust ") + String(s.lossPermille() / 10) + "." + String(s.lossPermille() % 10) + "%");
  if (s.ok == 0)
  {
    ui.setLine(3, "warte auf Pong");
    ui.setLine(4, "");
  }
  else
  {
    ui.setLine(3, String("Hin  ") + String(s.upRssi.last) + "dBm " + String(s.upSnr.last) + "dB");
    ui.setLine(4, String("Rueck ") + String(s.downRssi.last) + "dBm " + String(s.downSnr.last) + "dB");
  }
  if (s.ok == 0)
    ui.setLine(5, s.lostStreak ? String("Keine Antwort ") + String(s.lostStreak) + "x" : String("Taste: Ende"));
  else
    ui.setLine(5, String("RTT ") + String(s.rttMs.last) + "ms min " + String(s.upRssi.min));
  (void)now;
  ui.render();
}

static void renderOled(uint32_t now)
{
  if (gRange.active() || (gRangeShowUntilMs != 0 && (int32_t)(gRangeShowUntilMs - now) > 0))
  {
    renderRangeTest(now);
    return;
  }
  if (menu.getState() == MenuState::Visible)
  {
    MenuItem selected = menu.getSelectedItem();
    int selectedIdx = menu.getSelectedPos();
    int totalItems = menu.getItemCount();
    const int visibleItems = 4;
    int page = selectedIdx / visibleItems;
    int totalPages = (totalItems + visibleItems - 1) / visibleItems;
    int startIdx = page * visibleItems;

    ui.setLine(0, "=== MENU === (long press)");
    for (int i = 0; i < visibleItems; i++)
    {
      int itemIdx = startIdx + i;
      if (itemIdx < totalItems)
      {
        MenuItem item = menu.getItemAt(itemIdx);
        String label = menuLabel(item);
        if (item == MenuItem::ClimateOn)
          label += String(" ") + String(gClimateTargetC) + "C " + String(gClimateMinutes) + "m";
        ui.setLine(1 + i, String(item == selected ? "> " : "  ") + label);
      }
      else
      {
        ui.setLine(1 + i, "");
      }
    }
    float progress = menu.getLongPressProgress();
    if (progress > 0.0f)
    {
      ui.setLine(5, "Hold...");
      ui.drawProgressBar(55, 60, progress);
    }
    else
    {
      ui.setLine(5, String("Page ") + String(page + 1) + "/" + String(totalPages));
      ui.drawProgressBar(0, 0, 0.0f);
    }
    ui.render();
    return;
  }

  ui.drawProgressBar(0, 0, 0.0f);
  ui.setLine(0, String("Bat ") + String(gBattV, 1) + "V  RSSI " + String(gStatus.rssi));

  if (gStatusRxMs == 0)
  {
    ui.setLine(1, "Status: (keiner)");
    ui.setLine(2, "");
    ui.setLine(3, "");
    ui.setLine(4, "");
  }
  else
  {
    const uint32_t ageMs = now - gStatusRxMs;
    const bool stale = ageMs > 3 * kSenderStatusIntervalMs;
    ui.setLine(1, String("Zustand: ") + phaseToStr(gStatus.phase) + (gStatus.assumed ? " ~" : ""));
    ui.setLine(2, String("Innen ") + fmtDC(gStatus.interior_dC) + " Au " + fmtDC(gStatus.outside_dC));
    ui.setLine(3, String(gStatus.powerW) + "W " + String(gStatus.fuelUsedMl) + "ml");
    String v = gStatus.voltage_mV ? String(gStatus.voltage_mV / 1000.0f, 1) + "V" : String("--V");
    ui.setLine(4, v + " Alter " + String(ageMs / 1000) + "s" + (stale ? " veraltet" : ""));
  }

  // Line 5: job progress / result > inactive hint > rotating detail (3 s each)
  if (jobBusy())
  {
    ui.setLine(5, String("Sende ") + kindToStr(gJob.cmd.kind) + " " + String(gJob.txCount) + "x");
  }
  else if (gResultUntilMs != 0 && (int32_t)(gResultUntilMs - now) > 0)
  {
    ui.setLine(5, gResultText);
  }
  else if (gStatusRxMs != 0 && now - gStatusRxMs > kSenderInactiveHintMs)
  {
    ui.setLine(5, "Sender inaktiv");
  }
  else if (gStatusRxMs == 0)
  {
    ui.setLine(5, "Warte auf Status");
  }
  else
  {
    String detail;
    switch ((now / 3000) % 4)
    {
    case 0: detail = String("Kuehlm. ") + fmtDC(gStatus.coolant_dC); break;
    case 1:
      detail = gStatus.odometer_km == 0xFFFFFFFFu ? String("km --") : String("km ") + String((unsigned long)gStatus.odometer_km);
      break;
    case 2:
      detail = String("Klima ") + climateStatusToStr(gStatus.climateStatus);
      if (gStatus.climateTargetC)
        detail += String(" ") + String(gStatus.climateTargetC) + "C";
      break;
    default:
      detail = gStatus.errorCode ? String("Fehler ") + String(gStatus.errorCode) + (gStatus.errorStored ? " (gesp.)" : "")
                                 : String("Kein Fehler");
      break;
    }
    ui.setLine(5, detail);
  }
  ui.render();
}

// ---- BLE glue (commands from the PWA, results back) -------------------------------------------
#if defined(ENABLE_BLE) && ENABLE_BLE
static uint8_t gBleLastCmdId = 0;
static uint8_t gBleLastCmdResult = 0;

static void bleEmit(uint8_t cmdId, uint8_t stage, uint8_t result)
{
  // Push with timeout 0: never blocks the loop. A full queue drops the event (the PWA times out).
  sbl::shared().events.push(wh::makeBleJobEvent(cmdId, stage, result));
}

// Job hook: sent / acked / done / failed of a job that came from BLE.
static void bleJobDone(uint8_t cmdId, uint8_t stage, uint8_t result)
{
  bleEmit(cmdId, stage, result);
  if (stage == kStageDone || stage == kStageFailed)
  {
    gBleLastCmdId = cmdId;
    gBleLastCmdResult = result;
  }
}

// Takes the commands the NimBLE host queued. One job at a time: while a job runs, further
// commands are answered with Busy (the same code as the receiver).
static void serviceBleCommands()
{
  sbl::Shared &b = sbl::shared();
  rt::Command c;
  while (b.cmdQueue.pop(c))
  {
    senderNoteUserActivity();
    proto::CommandPayload p{};
    if (!wh::bleCommandToPayload(c, gLastMinutes, p))
    {
      bleEmit(c.tag, ble::STAGE_FAILED, (uint8_t)wh::CmdResult::Unsupported);
      continue;
    }
    if (!startJob(p, c.tag))
    {
      bleEmit(c.tag, ble::STAGE_FAILED, (uint8_t)wh::CmdResult::Busy);
      continue;
    }
    bleEmit(c.tag, ble::STAGE_QUEUED, (uint8_t)wh::CmdResult::Pending);
  }
  // BLE connect/subscribe counts as activity: keeps the receiver sending status while the PWA looks.
  static uint32_t seenActivity = 0;
  const uint32_t act = b.lastActivityMs.load();
  if (act != seenActivity)
  {
    seenActivity = act;
    senderNoteUserActivity();
  }
}

// Publish the state for bleTask (single writer: the main loop).
static void publishBleSnapshot(uint32_t now)
{
  static uint32_t lastMs = 0;
  if (now - lastMs < 100)
    return;
  lastMs = now;
  sbl::Snap s;
  s.status = gStatus;
  s.rxMs = gStatusRxMs;
  s.lastCmdId = gBleLastCmdId;
  s.lastCmdResult = gBleLastCmdResult;
  sbl::shared().snap.write(s);
}
#endif

// ---- Commands from menu / serial ------------------------------------------------

static bool submit(proto::CommandKind kind, uint8_t targetC, uint16_t minutes)
{
  proto::CommandPayload c{};
  c.kind = kind;
  c.targetC = targetC;
  c.minutes = minutes;
  if (!startJob(c, 0))
  {
    Serial.println("[JOB] busy, command ignored");
    gResultText = "Beschaeftigt";
    gResultUntilMs = millis() + 2000;
    return false;
  }
  return true;
}

// Menu mapping (menu_handler.h is owned by another package, its enum is unchanged):
//   Start->Heizen, Stop->Aus, Run10min->Klima an, Run20min->Klima aus, QueryStatus->Abfrage,
//   Run30min/Run90min unused (no action).
static uint32_t gMenuFlashStartMs = 0;
static bool gMenuFlashActive = false;

static void handleMenuSelection(MenuItem item)
{
  Serial.printf("[MENU] Activated: %s (%s)\n", menuItemToStr(item), menuLabel(item));
  senderNoteUserActivity();

  gMenuFlashStartMs = millis();
  gMenuFlashActive = true;
  ui.setInverted(true);
  ui.drawProgressBar(0, 0, 0.0f);

  switch (item)
  {
  case MenuItem::Start: submit(proto::CommandKind::Start, 0, gLastMinutes); break;
  case MenuItem::Stop: submit(proto::CommandKind::Stop, 0, 0); break;
  case MenuItem::ClimateOn: submit(proto::CommandKind::ClimateSet, gClimateTargetC, gClimateMinutes); break;
  case MenuItem::ClimateOff: submit(proto::CommandKind::ClimateOff, 0, 0); break;
  case MenuItem::QueryStatus: submit(proto::CommandKind::QueryStatus, 0, 0); break;
  case MenuItem::RangeTest:
    // Toggle: a second activation ends the test and keeps the result on the display for 20 s.
    if (gRange.active())
      rangeStop("stopped by menu");
    else
      rangeStart(0);
    break;
#if defined(ENABLE_BLE) && ENABLE_BLE
  case MenuItem::BlePair: sbl::shared().pairRequest.store(true); break;
  case MenuItem::BleClear: sbl::shared().clearRequest.store(true); break;
#endif
  default: break;
  }
}

static void handleSerialLine(const String &line)
{
  if (line.equalsIgnoreCase("pingstop"))
  {
    if (gRange.active())
      rangeStop("stopped by serial");
    else
      Serial.println("[PING] not running");
  }
  else if (line.startsWith("ping") || line.startsWith("PING"))
  {
    // ping [count]: range test, 0 or no count = until "pingstop" or the menu stops it
    int space = line.indexOf(' ');
    long n = space < 0 ? 0 : line.substring(space + 1).toInt();
    if (n < 0 || n > 100000)
    {
      Serial.println("Usage: ping [count 1..100000]  (no count: until pingstop)");
      return;
    }
    rangeStart((uint32_t)n);
  }
  else if (line.equalsIgnoreCase("stop"))
  {
    submit(proto::CommandKind::Stop, 0, 0);
  }
  else if (line.equalsIgnoreCase("start"))
  {
    submit(proto::CommandKind::Start, 0, gLastMinutes);
  }
  else if (line.equalsIgnoreCase("query"))
  {
    submit(proto::CommandKind::QueryStatus, 0, 0);
  }
  else if (line.equalsIgnoreCase("climateoff"))
  {
    submit(proto::CommandKind::ClimateOff, 0, 0);
  }
  else if (line.startsWith("climate") || line.startsWith("CLIMATE"))
  {
    int s1 = line.indexOf(' ');
    int s2 = s1 < 0 ? -1 : line.indexOf(' ', s1 + 1);
    if (s1 < 0 || s2 < 0)
    {
      Serial.println("Usage: climate <targetC 10..24> <minutes 10..480>");
      return;
    }
    int t = line.substring(s1 + 1, s2).toInt();
    int m = line.substring(s2 + 1).toInt();
    if (t < 10 || t > 24 || m < 10 || m > 480)
    {
      Serial.println("climate: target 10..24, minutes 10..480");
      return;
    }
    gClimateTargetC = (uint8_t)t;
    gClimateMinutes = (uint16_t)m;
    submit(proto::CommandKind::ClimateSet, gClimateTargetC, gClimateMinutes);
  }
  else if (line.startsWith("run") || line.startsWith("RUN"))
  {
    int space = line.indexOf(' ');
    if (space < 0)
    {
      Serial.println("Usage: run <minutes>");
      return;
    }
    int minutes = line.substring(space + 1).toInt();
    if (minutes <= 0 || minutes > 480)
    {
      Serial.println("Minutes must be 1..480");
      return;
    }
    gLastMinutes = static_cast<uint16_t>(minutes);
    submit(proto::CommandKind::RunMinutes, 0, gLastMinutes);
  }
  else
  {
    Serial.println("Unknown command. Use: start | stop | run <min> | climate <C> <min> | climateoff | query | ping [n] | pingstop");
  }
}

void setup()
{
  Serial.begin(115200);
  while (!Serial && millis() < 1000) {
    yield();  // Wait up to 1s for serial, allow RTOS tasks
  }

  Serial.println("\n\n==================================");
  Serial.println("  WEBASTO LORA SENDER");
  Serial.println("  Device ID: SENDER");
  Serial.println("==================================");

  statusLed.begin();
  statusLed.setOff();

  ui.begin();
  ui.setLine(0, "Webasto LoRa Sender");
  ui.setLine(1, "Init LoRa...");
  ui.render();

  bool ok = loraLink.begin();
  ui.setLine(1, ok ? "LoRa OK" : "LoRa FAIL");

  ui.setLine(2, String("Freq ") + String((uint32_t)LORA_FREQUENCY_HZ));
  ui.setLine(3, "Cmd via Serial:");
  ui.setLine(4, "start|stop|climate");
  ui.render();

  // Initialize encryption with default PSK
  crypto::AES128CTR::setKey(proto::kDefaultPSK);
  Serial.println("[SETUP] AES-128-CTR encryption initialized");

  // Initialize button/menu on GPIO0
  menu.begin(MENU_BUTTON_PIN);
#if defined(ENABLE_BLE) && ENABLE_BLE
  // Start BLE before the menu is sized: the BLE items (pairing window, clear bonds) need the stack.
  gJobDone = bleJobDone;
  sbl::start(3 * kSenderStatusIntervalMs);
  menu.setVisibleMask(kMenuCommonMask | kMenuRangeMask | (sbl::shared().ok.load() ? kMenuBleMask : 0));
#else
  menu.setVisibleMask(kMenuCommonMask | kMenuRangeMask);  // no BLE items
#endif
  Serial.println("[SETUP] Menu button initialized on GPIO0");

  // Initialize battery ADC (GPIO35 on TTGO LoRa32 v1.0)
#ifdef ARDUINO_ARCH_ESP32
  adcAttachPin(VBAT_ADC_PIN);
  analogSetPinAttenuation(VBAT_ADC_PIN, ADC_11db);
#endif

  // Enable LoRa interrupt LAST to avoid any interference from other setup
  if (ok) {
    loraLink.enableInterrupt();
    ui.setLine(1, "LoRa OK (IRQ)");
    ui.render();
  }

  Serial.println("Sender ready. Commands: start | stop | run <min> | climate <C> <min> | climateoff | query");
}

// Receive path: parse every valid status, overwrite RSSI/SNR with our own values.
static void pumpLoRaRx(uint32_t now)
{
  loraLink.service();
  if (!loraLink.hasPacket())
    return;
  proto::Packet pkt{};
  int rssi = 0;
  float snr = 0;
  if (!loraLink.recv(pkt, rssi, snr))
    return;
  statusLed.toggle();
  if (pkt.h.type == proto::MsgType::Pong && pkt.h.src == LORA_NODE_RECEIVER && pkt.h.dst == LORA_NODE_SENDER)
  {
    onPongReceived(pkt, rssi, snr, now);
    return;
  }
  if (pkt.h.type == proto::MsgType::Status && pkt.h.src == LORA_NODE_RECEIVER)
  {
    wh::DecodedStatus st = wh::unpackLoraStatus(pkt.p.status);
    st.rssi = (int8_t)rssi;
    st.snr = (int8_t)snr;
    gStatus = st;
    gStatusRxMs = now ? now : 1;
    Serial.printf("[LOOP] status phase=%u lastCmdSeq=%u job=%u\n", (unsigned)st.phase, st.lastCmdSeq, gJob.seq);
    onStatusReceived(st);
  }
}

// Keep-alive: the receiver transmits only while a sender is active (packet younger than 30 min).
// While the user is active, query when the last status is older than kSenderKeepaliveMs.
// Without any status yet, retry once a minute.
static void serviceKeepalive(uint32_t now)
{
  static uint32_t lastTryMs = 0;
  static bool tried = false;
  if (!gUserActive || jobBusy() || gRange.active())
    return;
  const bool statusOld = gStatusRxMs == 0 || (now - gStatusRxMs) > kSenderKeepaliveMs;
  const uint32_t retryMs = gStatusRxMs == 0 ? 60000UL : kSenderKeepaliveMs;
  if (statusOld && !(tried && (now - lastTryMs) < retryMs))
  {
    proto::CommandPayload c{};
    c.kind = proto::CommandKind::QueryStatus;
    if (startJob(c, 0))
    {
      tried = true;
      lastTryMs = now;
    }
  }
}

static void updateBattery(uint32_t now)
{
  if (now - gLastBattUpdateMs <= VBAT_UPDATE_INTERVAL_MS)
    return;
  gLastBattUpdateMs = now;
  int raw = analogRead(VBAT_ADC_PIN);
  float vpin = (float)raw / 4095.0f * 3.3f;  // approx. ADC scale to volts
  float vbat = vpin * VBAT_DIVIDER_RATIO * VBAT_CALIBRATION;
  gBattV = (gBattV <= 0.01f) ? vbat : (gBattV * 0.8f) + (vbat * 0.2f);
}

// One pass of the sender main loop. Never blocks (no delay, no waiting loops).
static void senderLoopOnce()
{
  const uint32_t now = millis();

  pumpLoRaRx(now);

  String line = readLineNonBlocking();
  if (line.length() > 0)
  {
    senderNoteUserActivity();
    handleSerialLine(line);
  }

  statusLed.update();

  if (gMenuFlashActive && now - gMenuFlashStartMs >= 150)
  {
    ui.setInverted(false);
    gMenuFlashActive = false;
  }

  menu.update();
  if (menu.getState() == MenuState::Visible || digitalRead(MENU_BUTTON_PIN) == LOW)
    senderNoteUserActivity();
  // signed: gUserActivityMs may be newer than `now` (set by millis() earlier in this pass)
  if (gUserActive && (int32_t)(now - gUserActivityMs) > (int32_t)kSenderUserActiveMs)
    gUserActive = false;

  MenuItem activatedItem;
  if (menu.isItemActivated(activatedItem))
    handleMenuSelection(activatedItem);

#if defined(ENABLE_BLE) && ENABLE_BLE
  serviceBleCommands();
#endif
  serviceJob(now);
  serviceRange(now);
  serviceKeepalive(now);
#if defined(ENABLE_BLE) && ENABLE_BLE
  publishBleSnapshot(now);
#endif
  updateBattery(now);

  // OLED refresh: faster in the menu (progress bar), slower otherwise. I2C, short.
  static uint32_t lastUiMs = 0;
  const uint32_t uiInterval = (menu.getState() == MenuState::Visible) ? 50 : 250;
  if (now - lastUiMs > uiInterval)
  {
    lastUiMs = now;
    renderOled(now);
  }
}

void loop()
{
  senderLoopOnce();
}
