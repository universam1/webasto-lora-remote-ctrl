// CAN sniffer for the VW T5.2 Webasto (WWZH) reverse engineering.
//
// Hardware: LOLIN ESP32-S2 mini + SN65HVD230 transceiver (pins via build flags).
//   CAN_TX_PIN -> transceiver D/CTX (TXD), CAN_RX_PIN <- transceiver R/CRX (RXD),
//   CAN_RS_PIN -> transceiver Rs (HIGH = standby/driver off, LOW = normal),
//   -1 if Rs is not wired. Without Rs the controller's listen-only mode is the
//   only guard; TX is held recessive (HIGH) from the first instruction.
//   Do NOT add a 120 ohm terminator when tapping the vehicle bus.
//
// Host protocol: GVRET (SavvyCAN compatible). On the S2 this runs over native
//   USB CDC (baud rate ignored, host must assert DTR). On a classic ESP32 with
//   a CP2102 use 921600 baud (the CP2102 garbles 1 Mbaud).
//   Before the host sends E7 E7 the device is in text mode (human readable,
//   type "help"). After E7 E7 it speaks binary GVRET until reset.
//
// Safety: the bus is opened listen-only (no ACK, no error frames). Transmit
// is only compiled in with -D CAN_ALLOW_TX=1 and must additionally be enabled
// at runtime (text "tx on" or GVRET vendor command F1 41 01).

#include <Arduino.h>
#include <driver/twai.h>
#include <esp_timer.h>

#ifndef CAN_TX_PIN
#define CAN_TX_PIN 21
#endif
#ifndef CAN_RX_PIN
#define CAN_RX_PIN 22
#endif
#ifndef CAN_RS_PIN
#define CAN_RS_PIN -1  // -1: Rs not wired (module ties Rs to GND)
#endif
#ifndef CAN_BITRATE
#define CAN_BITRATE 100000
#endif
#ifndef CAN_ALLOW_TX
#define CAN_ALLOW_TX 0
#endif
#ifndef SNIFFER_SERIAL_BAUD
#define SNIFFER_SERIAL_BAUD 921600
#endif

static constexpr uint16_t kBuildNumber = 1;

// ---------------------------------------------------------------------------
// TWAI handling
// ---------------------------------------------------------------------------

enum class BusState : uint8_t { Stopped = 0, Running = 1, BusOff = 2, Recovering = 3 };

struct Stats {
  uint32_t rxFrames = 0;
  uint32_t rxMissed = 0;    // driver queue overruns
  uint32_t busErrors = 0;   // bus error alerts (bit/stuff/form/CRC)
  uint32_t txFrames = 0;
  uint32_t txFailed = 0;
};

static Stats gStats;
static BusState gState = BusState::Stopped;
static uint32_t gBitrate = CAN_BITRATE;
static bool gTxEnabled = false;   // runtime switch, only effective if CAN_ALLOW_TX
static bool gInstalled = false;

static bool timingFor(uint32_t bitrate, twai_timing_config_t &t) {
  switch (bitrate) {
    case 1000000: t = TWAI_TIMING_CONFIG_1MBITS(); return true;
    case 500000:  t = TWAI_TIMING_CONFIG_500KBITS(); return true;
    case 250000:  t = TWAI_TIMING_CONFIG_250KBITS(); return true;
    case 125000:  t = TWAI_TIMING_CONFIG_125KBITS(); return true;
    case 100000:  t = TWAI_TIMING_CONFIG_100KBITS(); return true;
    case 50000:   t = TWAI_TIMING_CONFIG_50KBITS(); return true;
    case 25000:   t = TWAI_TIMING_CONFIG_25KBITS(); return true;
    case 83333:   // 80 MHz / 32 = 2.5 MHz, 30 tq
      t = {.brp = 32, .tseg_1 = 22, .tseg_2 = 7, .sjw = 3, .triple_sampling = false};
      return true;
    case 33333:   // 80 MHz / 80 = 1 MHz, 30 tq
      t = {.brp = 80, .tseg_1 = 22, .tseg_2 = 7, .sjw = 3, .triple_sampling = false};
      return true;
    default:
      return false;
  }
}

static void setTransceiverStandby(bool standby) {
  if (CAN_RS_PIN < 0) return;
  pinMode(CAN_RS_PIN, OUTPUT);
  digitalWrite(CAN_RS_PIN, standby ? HIGH : LOW);
}

static bool txActive() { return CAN_ALLOW_TX && gTxEnabled; }

static void canStop() {
  if (!gInstalled) return;
  twai_stop();
  twai_driver_uninstall();
  gInstalled = false;
  gState = BusState::Stopped;
}

static bool canStart(uint32_t bitrate) {
  canStop();
  twai_timing_config_t timing;
  if (!timingFor(bitrate, timing)) return false;

  const bool normal = txActive();
  setTransceiverStandby(!normal);

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN,
      normal ? TWAI_MODE_NORMAL : TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = 256;
  g.tx_queue_len = normal ? 16 : 0;
  g.alerts_enabled = TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL |
                     TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                     TWAI_ALERT_ERR_PASS | TWAI_ALERT_TX_FAILED;
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g, &timing, &f) != ESP_OK) return false;
  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    return false;
  }
  gInstalled = true;
  gBitrate = bitrate;
  gState = BusState::Running;
  return true;
}

static void handleAlerts() {
  if (!gInstalled) return;
  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) != ESP_OK) return;
  if (alerts & TWAI_ALERT_BUS_ERROR) {
    twai_status_info_t s;
    if (twai_get_status_info(&s) == ESP_OK) gStats.busErrors = s.bus_error_count;
    else gStats.busErrors++;
  }
  if (alerts & TWAI_ALERT_TX_FAILED) gStats.txFailed++;
  if (alerts & TWAI_ALERT_BUS_OFF) {
    gState = BusState::Recovering;
    twai_initiate_recovery();
  }
  if (alerts & TWAI_ALERT_BUS_RECOVERED) {
    twai_start();
    gState = BusState::Running;
  }
  twai_status_info_t s;
  if (twai_get_status_info(&s) == ESP_OK) gStats.rxMissed = s.rx_missed_count;
}

// ---------------------------------------------------------------------------
// Host side: text mode and GVRET binary mode
// ---------------------------------------------------------------------------

static bool gBinary = false;
static bool gMonitor = false;  // text mode frame printing

static void sendBytes(const uint8_t *b, size_t n) { Serial.write(b, n); }

static void putU32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static uint32_t getU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void emitFrame(const twai_message_t &m, uint32_t ts) {
  if (gBinary) {
    uint8_t buf[4 + 4 + 4 + 1 + 8 + 1];
    size_t n = 0;
    buf[n++] = 0xF1;
    buf[n++] = 0x00;
    putU32(&buf[n], ts); n += 4;
    uint32_t id = m.identifier | (m.extd ? 0x80000000UL : 0);
    putU32(&buf[n], id); n += 4;
    uint8_t len = m.rtr ? 0 : (m.data_length_code > 8 ? 8 : m.data_length_code);
    buf[n++] = (0 << 4) | len;  // bus 0
    for (uint8_t i = 0; i < len; i++) buf[n++] = m.data[i];
    buf[n++] = 0x00;
    sendBytes(buf, n);
  } else if (gMonitor) {
    char line[64];
    int p = snprintf(line, sizeof(line), "%10lu %s%03lX [%u]", (unsigned long)ts,
                     m.extd ? "x" : " ", (unsigned long)m.identifier, m.data_length_code);
    for (uint8_t i = 0; i < m.data_length_code && i < 8 && p < (int)sizeof(line) - 4; i++)
      p += snprintf(line + p, sizeof(line) - p, " %02X", m.data[i]);
    Serial.println(line);
  }
}

static void sendStatus() {  // vendor extension F1 40
  uint8_t b[2 + 1 + 12 + 1];
  b[0] = 0xF1; b[1] = 0x40;
  b[2] = (CAN_ALLOW_TX ? 0x01 : 0) | (txActive() ? 0x02 : 0);
  putU32(&b[3], gStats.rxFrames);
  putU32(&b[7], gStats.rxMissed);
  putU32(&b[11], gStats.busErrors);
  b[15] = (uint8_t)gState;
  sendBytes(b, sizeof(b));
}

static bool transmit(uint32_t id, bool ext, const uint8_t *data, uint8_t len) {
  if (!txActive() || !gInstalled) return false;
  twai_message_t m = {};
  m.identifier = id;
  m.extd = ext;
  m.data_length_code = len > 8 ? 8 : len;
  memcpy(m.data, data, m.data_length_code);
  if (twai_transmit(&m, pdMS_TO_TICKS(20)) == ESP_OK) {
    gStats.txFrames++;
    return true;
  }
  gStats.txFailed++;
  return false;
}

static void setTxEnabled(bool on) {
  if (!CAN_ALLOW_TX) on = false;
  if (on == gTxEnabled) return;
  gTxEnabled = on;
  canStart(gBitrate);  // reinstall in the new mode
}

// GVRET parser state
enum class GState { Idle, Cmd, Payload };
static GState gs = GState::Idle;
static uint8_t gCmd = 0;
static uint8_t gPay[32];
static uint8_t gPayLen = 0;
static uint8_t gPayNeed = 0;

static void gvretExec() {
  switch (gCmd) {
    case 0x00: {  // TX frame: id[4] bus len data[len]
      uint32_t raw = getU32(gPay);
      uint8_t len = gPay[5] & 0x0F;
      transmit(raw & 0x1FFFFFFF, raw & 0x80000000UL, &gPay[6], len);
      break;
    }
    case 0x01: {  // time sync
      uint8_t b[6] = {0xF1, 0x01};
      putU32(&b[2], (uint32_t)esp_timer_get_time());
      sendBytes(b, sizeof(b));
      break;
    }
    case 0x05: {  // set bus config
      uint32_t v = getU32(gPay);
      uint32_t rate = v & 0x0FFFFFFF;
      bool listenOnly = v & 0x20000000UL;
      if (listenOnly && gTxEnabled) gTxEnabled = false;
      if (rate > 0 && (v & 0x40000000UL)) canStart(rate);
      break;
    }
    case 0x06: {  // get bus params
      uint8_t b[12] = {0xF1, 0x06};
      b[2] = (gInstalled ? 0x01 : 0) | (txActive() ? 0 : 0x10);
      putU32(&b[3], gBitrate);
      b[7] = 0;
      putU32(&b[8], 0);
      sendBytes(b, sizeof(b));
      break;
    }
    case 0x07: {  // device info
      uint8_t b[8] = {0xF1, 0x07, (uint8_t)(kBuildNumber & 0xFF), (uint8_t)(kBuildNumber >> 8),
                      0x20, 0, 0, 0};
      sendBytes(b, sizeof(b));
      break;
    }
    case 0x09: {  // keepalive
      uint8_t b[4] = {0xF1, 0x09, 0xDE, 0xAD};
      sendBytes(b, sizeof(b));
      break;
    }
    case 0x0C: {  // number of buses
      uint8_t b[3] = {0xF1, 0x0C, 1};
      sendBytes(b, sizeof(b));
      break;
    }
    case 0x40:  // vendor: status
      sendStatus();
      break;
    case 0x41:  // vendor: tx enable
      setTxEnabled(gPay[0] != 0);
      sendStatus();
      break;
    default:
      break;
  }
}

// Number of payload bytes after the command byte. For 0x00 the length depends
// on the len byte, handled in gvretByte().
static int payloadLenFor(uint8_t cmd) {
  switch (cmd) {
    case 0x00: return 6;   // id[4] bus len  (+data, added dynamically)
    case 0x05: return 8;
    case 0x41: return 1;
    case 0x01: case 0x06: case 0x07: case 0x09: case 0x0C: case 0x40: return 0;
    default: return -1;
  }
}

static void gvretByte(uint8_t c) {
  switch (gs) {
    case GState::Idle:
      if (c == 0xF1) gs = GState::Cmd;
      break;  // E7 and anything else ignored
    case GState::Cmd: {
      gCmd = c;
      int need = payloadLenFor(c);
      if (need < 0) { gs = GState::Idle; break; }
      gPayLen = 0;
      gPayNeed = (uint8_t)need;
      if (need == 0) { gvretExec(); gs = GState::Idle; }
      else gs = GState::Payload;
      break;
    }
    case GState::Payload:
      if (gPayLen < sizeof(gPay)) gPay[gPayLen++] = c;
      if (gCmd == 0x00 && gPayLen == 6) gPayNeed = 6 + (gPay[5] & 0x0F);
      if (gPayLen >= gPayNeed) { gvretExec(); gs = GState::Idle; }
      break;
  }
}

// --------------------------- text mode -------------------------------------

static void printStats() {
  Serial.printf("bitrate=%lu state=%u mode=%s rx=%lu missed=%lu buserr=%lu tx=%lu txfail=%lu\n",
                (unsigned long)gBitrate, (unsigned)gState, txActive() ? "NORMAL" : "LISTEN_ONLY",
                (unsigned long)gStats.rxFrames, (unsigned long)gStats.rxMissed,
                (unsigned long)gStats.busErrors, (unsigned long)gStats.txFrames,
                (unsigned long)gStats.txFailed);
}

static void scanBitrates() {
  static const uint32_t rates[] = {100000, 125000, 83333, 50000, 500000, 33333, 250000};
  const bool hadTx = gTxEnabled;
  gTxEnabled = false;  // scan always listen-only
  Serial.println("scan: listen-only, 1.5 s per bitrate");
  for (uint32_t r : rates) {
    if (!canStart(r)) { Serial.printf("  %7lu: start failed\n", (unsigned long)r); continue; }
    uint32_t frames = 0, errs = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 1500) {
      twai_message_t m;
      while (twai_receive(&m, 0) == ESP_OK) frames++;
      uint32_t a;
      if (twai_read_alerts(&a, pdMS_TO_TICKS(5)) == ESP_OK && (a & TWAI_ALERT_BUS_ERROR)) errs++;
    }
    twai_status_info_t s = {};
    twai_get_status_info(&s);
    Serial.printf("  %7lu: frames=%lu bus_errors=%lu (alerts=%lu)\n", (unsigned long)r,
                  (unsigned long)frames, (unsigned long)s.bus_error_count, (unsigned long)errs);
  }
  gTxEnabled = hadTx;
  canStart(gBitrate);
  Serial.printf("scan done, back on %lu\n", (unsigned long)gBitrate);
}

static void printHelp() {
  Serial.println("commands: help | stats | mon on|off | rate <bps> | scan | tx on|off");
  Serial.println("binary GVRET: send E7 E7 (SavvyCAN / mcp-can-capture do this)");
}

static char gLine[64];
static uint8_t gLineLen = 0;

static void textCommand(const char *cmd) {
  if (!strcmp(cmd, "help") || !*cmd) { printHelp(); return; }
  if (!strcmp(cmd, "stats")) { printStats(); return; }
  if (!strcmp(cmd, "mon on")) { gMonitor = true; return; }
  if (!strcmp(cmd, "mon off")) { gMonitor = false; return; }
  if (!strcmp(cmd, "scan")) { scanBitrates(); return; }
  if (!strncmp(cmd, "rate ", 5)) {
    uint32_t r = strtoul(cmd + 5, nullptr, 10);
    Serial.println(canStart(r) ? "ok" : "unsupported bitrate");
    return;
  }
  if (!strcmp(cmd, "tx on")) {
    if (!CAN_ALLOW_TX) { Serial.println("tx not compiled in (CAN_ALLOW_TX=0)"); return; }
    setTxEnabled(true); printStats(); return;
  }
  if (!strcmp(cmd, "tx off")) { setTxEnabled(false); printStats(); return; }
  Serial.println("unknown command, type help");
}

static uint8_t gLastByte = 0;

static void hostInput() {
  while (Serial.available()) {
    uint8_t c = (uint8_t)Serial.read();
    if (gBinary) { gvretByte(c); continue; }
    if (c == 0xE7 && gLastByte == 0xE7) {
      gBinary = true;
      gMonitor = false;
      gs = GState::Idle;
      continue;
    }
    gLastByte = c;
    if (c == 0xE7) continue;
    if (c == 0xF1) { gBinary = true; gs = GState::Cmd; continue; }  // host skipped E7 E7
    if (c == '\r' || c == '\n') {
      gLine[gLineLen] = 0;
      if (gLineLen) textCommand(gLine);
      gLineLen = 0;
    } else if (gLineLen < sizeof(gLine) - 1 && c >= 0x20 && c < 0x7F) {
      gLine[gLineLen++] = (char)c;
    }
  }
}

// ---------------------------------------------------------------------------

void setup() {
  // Keep the transceiver's TXD recessive until the TWAI driver owns the pin.
  pinMode(CAN_TX_PIN, OUTPUT);
  digitalWrite(CAN_TX_PIN, HIGH);
  Serial.setRxBufferSize(1024);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);  // never block the CAN loop when no host is attached
#else
  Serial.setTxBufferSize(8192);
#endif
  Serial.begin(SNIFFER_SERIAL_BAUD);
  setTransceiverStandby(true);
  delay(200);
  Serial.printf("\ncansniffer build %u, tx=%s rx=%s rs=%d, tx_compiled=%d\n", kBuildNumber,
                String(CAN_TX_PIN).c_str(), String(CAN_RX_PIN).c_str(), CAN_RS_PIN, CAN_ALLOW_TX);
  if (canStart(gBitrate)) Serial.printf("TWAI listen-only @ %lu bit/s\n", (unsigned long)gBitrate);
  else Serial.println("TWAI start FAILED");
  printHelp();
}

void loop() {
  hostInput();
  if (gInstalled) {
    twai_message_t m;
    // Drain up to 64 frames per loop so host input stays responsive.
    for (int i = 0; i < 64 && twai_receive(&m, 0) == ESP_OK; i++) {
      gStats.rxFrames++;
      emitFrame(m, (uint32_t)esp_timer_get_time());
    }
  }
  handleAlerts();
  delay(0);
}
