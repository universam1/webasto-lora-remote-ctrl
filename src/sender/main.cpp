#include <Arduino.h>

#include "project_config.h"
#include "lora_link.h"
#include "oled_ui.h"
#include "protocol.h"
#include "status_led.h"
#include "encryption.h"
#include "menu_handler.h"
#include "status_codec.h"

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

// User activity: set by button/menu/serial activity and by senderNoteUserActivity() (BLE later).
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

// Job result hook. Null now; the BLE package connects it.
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

  if ((uint32_t)(now - gJob.startMs) >= kSenderJobTimeoutMs)
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
      // Retry interval with +-25 % jitter to avoid colliding with receiver status packets.
      int32_t jitter = ((int32_t)random(51) - 25) * (int32_t)SENDER_CMD_RETRY_INTERVAL_MS / 100;
      gJob.nextTxMs = now + (uint32_t)((int32_t)SENDER_CMD_RETRY_INTERVAL_MS + jitter);
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
  case MenuItem::Run10min: return "Klima an";
  case MenuItem::Run20min: return "Klima aus";
  case MenuItem::QueryStatus: return "Abfrage";
  default: return "-";  // Run30min, Run90min: unused
  }
}

static void renderOled(uint32_t now)
{
  if (menu.getState() == MenuState::Visible)
  {
    MenuItem selected = menu.getSelectedItem();
    int selectedIdx = static_cast<int>(selected);
    int totalItems = static_cast<int>(MenuItem::Count);
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
        MenuItem item = static_cast<MenuItem>(itemIdx);
        String label = menuLabel(item);
        if (item == MenuItem::Run10min)
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

// ---- Commands from menu / serial (and later BLE) ------------------------------------------------

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
  case MenuItem::Run10min: submit(proto::CommandKind::ClimateSet, gClimateTargetC, gClimateMinutes); break;
  case MenuItem::Run20min: submit(proto::CommandKind::ClimateOff, 0, 0); break;
  case MenuItem::QueryStatus: submit(proto::CommandKind::QueryStatus, 0, 0); break;
  default: break;
  }
}

static void handleSerialLine(const String &line)
{
  if (line.equalsIgnoreCase("stop"))
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
    Serial.println("Unknown command. Use: start | stop | run <min> | climate <C> <min> | climateoff | query");
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
  if (!gUserActive || jobBusy())
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
  if (gUserActive && (now - gUserActivityMs) > kSenderUserActiveMs)
    gUserActive = false;

  MenuItem activatedItem;
  if (menu.isItemActivated(activatedItem))
    handleMenuSelection(activatedItem);

  serviceJob(now);
  serviceKeepalive(now);
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
