#include <Arduino.h>

#include "project_config.h"
#include "lora_link.h"
#include "oled_ui.h"
#include "protocol.h"
#include "status_led.h"
#include "encryption.h"
#include "menu_handler.h"

static OledUi ui;
static LoRaLink loraLink;
static StatusLed statusLed;
static MenuHandler menu;

static uint16_t gSeq = 1;
static uint8_t gLastMinutes = DEFAULT_RUN_MINUTES;
static proto::StatusPayload gLastStatus{};
static uint32_t gLastStatusRxMs = 0;
static uint16_t gAwaitingCmdSeq = 0;

// Battery monitoring state
static float gBattV = 0.0f;
static uint32_t gLastBattUpdateMs = 0;

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

static proto::Packet makeCommandPacket(proto::CommandKind kind, uint8_t minutes, uint16_t seq)
{
  proto::Packet pkt{};
  pkt.h.magic_version = proto::kMagicVersion;
  pkt.h.type = proto::MsgType::Command;
  pkt.h.src = LORA_NODE_SENDER;
  pkt.h.dst = LORA_NODE_RECEIVER;
  pkt.h.seq = seq;
  pkt.p.cmd.kind = kind;
  pkt.p.cmd.minutes = minutes;
  pkt.crc = proto::calcCrc(pkt);

  return pkt;
}

static bool sendCommandWithAck(proto::CommandKind kind, uint8_t minutes)
{
  const uint16_t cmdSeq = gSeq++;
  const proto::Packet cmd = makeCommandPacket(kind, minutes, cmdSeq);
  gAwaitingCmdSeq = cmdSeq;

  statusLed.setBlink(200); // Fast blink while sending command
  Serial.printf("[LORA] Sending command kind=%d minutes=%d seq=%d\n",
                static_cast<int>(kind), minutes, cmdSeq);

  uint32_t start = millis();
  uint32_t nextSend = 0;
  int sendCount = 0;

  while (millis() - start < static_cast<uint32_t>(SENDER_CMD_ACK_TIMEOUT_MS))
  {
    const uint32_t now = millis();
    if (now >= nextSend)
    {
      if (loraLink.send(cmd))
      {
        statusLed.toggle(); // Flash LED on TX
        sendCount++;
        Serial.printf("[LORA] Sent attempt #%d\n", sendCount);
      }
      nextSend = now + static_cast<uint32_t>(SENDER_CMD_RETRY_INTERVAL_MS);
    }

    // Pump RX while we wait.
    proto::Packet pkt{};
    int rssi = 0;
    float snr = 0;
    if (loraLink.recv(pkt, rssi, snr))
    {
      Serial.printf("[LORA] Received packet type=%d src=%d\n",
                    static_cast<int>(pkt.h.type), pkt.h.src);
      if (pkt.h.type == proto::MsgType::Status && pkt.h.src == LORA_NODE_RECEIVER)
      {
        gLastStatus = pkt.p.status;
        gLastStatus.lastRssiDbm = (int8_t)rssi;
        gLastStatus.lastSnrDb = (int8_t)snr;
        gLastStatusRxMs = millis();

        Serial.printf("[LORA] Status lastCmdSeq=%d, expecting=%d\n",
                      gLastStatus.lastCmdSeq, cmdSeq);
        if (gLastStatus.lastCmdSeq == cmdSeq)
        {
          gAwaitingCmdSeq = 0;
          statusLed.setOff(); // Turn off LED on successful ACK
          Serial.println("[LORA] ACK confirmed!");
          return true;
        }
      }
    }

    delay(10);
  }

  // Timed out - no ACK received
  Serial.printf("[LORA] Timeout after %d sends, no ACK\n", sendCount);
  statusLed.setOff();
  gAwaitingCmdSeq = 0;
  return false; // No ACK received = failure
}

static const char *heaterStateToStr(proto::HeaterState s)
{
  switch (s)
  {
  case proto::HeaterState::Off:
    return "OFF";
  case proto::HeaterState::Running:
    return "RUN";
  case proto::HeaterState::Error:
    return "ERR";
  default:
    return "UNK";
  }
}

static String formatMeasurements(const proto::StatusPayload &st)
{
  // Keep it short to fit 128px width.
  String out;
  if (st.temperatureC != INT16_MIN)
  {
    out += String("T:") + String(st.temperatureC) + "C";
  }
  else
  {
    out += "T --";
  }

  if (st.voltage_mV != 0)
  {
    out += String(" V:") + String(st.voltage_mV / 1000.0f, 1) + "V";
  }
  else
  {
    out += " V --";
  }

  if (st.power != 0)
  {
    out += String(" P:") + String(st.power) + "W";
  }

  return out;
}

static void handleMenuSelection(MenuItem item)
{
  Serial.printf("[MENU] Activated: %s\n", menuItemToStr(item));

  switch (item)
  {
  case MenuItem::Start:
    if (sendCommandWithAck(proto::CommandKind::Start, gLastMinutes))
    {
      Serial.printf("Sent START (%u min, ACKed)\n", gLastMinutes);
    }
    else
    {
      Serial.println("Failed to send START");
    }
    break;

  case MenuItem::Stop:
    if (sendCommandWithAck(proto::CommandKind::Stop, 0))
    {
      Serial.println("Sent STOP (ACKed)");
    }
    else
    {
      Serial.println("Failed to send STOP");
    }
    break;

  case MenuItem::Run10min:
    gLastMinutes = 10;
    if (sendCommandWithAck(proto::CommandKind::RunMinutes, gLastMinutes))
    {
      Serial.printf("Sent RUN (10 min, ACKed)\n");
    }
    else
    {
      Serial.println("Failed to send RUN");
    }
    break;

  case MenuItem::Run20min:
    gLastMinutes = 20;
    if (sendCommandWithAck(proto::CommandKind::RunMinutes, gLastMinutes))
    {
      Serial.printf("Sent RUN (20 min, ACKed)\n");
    }
    else
    {
      Serial.println("Failed to send RUN");
    }
    break;

  case MenuItem::Run30min:
    gLastMinutes = 30;
    if (sendCommandWithAck(proto::CommandKind::RunMinutes, gLastMinutes))
    {
      Serial.printf("Sent RUN (30 min, ACKed)\n");
    }
    else
    {
      Serial.println("Failed to send RUN");
    }
    break;

  case MenuItem::Run90min:
    gLastMinutes = 90;
    if (sendCommandWithAck(proto::CommandKind::RunMinutes, gLastMinutes))
    {
      Serial.printf("Sent RUN (90 min, ACKed)\n");
    }
    else
    {
      Serial.println("Failed to send RUN");
    }
    break;

  case MenuItem::QueryStatus:
    if (sendCommandWithAck(proto::CommandKind::QueryStatus, 0))
    {
      Serial.println("Sent QUERY STATUS (ACKed)");
    }
    else
    {
      Serial.println("Failed to send QUERY STATUS");
    }
    break;

  default:
    break;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000); // Longer delay for serial to stabilize

  Serial.println("\n\n==================================");
  Serial.println("  WEBASTO LORA SENDER");
  Serial.println("  Device ID: SENDER");
  Serial.println("==================================");

  statusLed.begin();
  statusLed.setOff(); // Start with LED off, will toggle on packet activity

  ui.begin();
  ui.setLine(0, "Webasto LoRa Sender");
  ui.setLine(1, "Init LoRa...");
  ui.render();

  bool ok = loraLink.begin();
  ui.setLine(1, ok ? "LoRa OK" : "LoRa FAIL");
  ui.setLine(2, String("Freq ") + String((uint32_t)LORA_FREQUENCY_HZ));
  ui.setLine(3, "Cmd via Serial:");
  ui.setLine(4, "start|stop|run N");
  ui.render();

  // Initialize encryption with default PSK
  crypto::AES128CTR::setKey(proto::kDefaultPSK);
  Serial.println("[SETUP] AES-128-CTR encryption initialized");

  // Initialize button/menu on GPIO0
  menu.begin(MENU_BUTTON_PIN);
  Serial.println("[SETUP] Menu button initialized on GPIO0");

  Serial.println("Sender ready. Commands: start | stop | run <minutes>");

  // Initialize battery ADC (GPIO35 on TTGO LoRa32 v1.0)
  // Use 11dB attenuation for better range.
#ifdef ARDUINO_ARCH_ESP32
  adcAttachPin(VBAT_ADC_PIN);
  analogSetPinAttenuation(VBAT_ADC_PIN, ADC_11db);
#endif
}

void loop()
{
  // Receive status from receiver.
  {
    proto::Packet pkt{};
    int rssi = 0;
    float snr = 0;
    if (loraLink.recv(pkt, rssi, snr))
    {
      statusLed.toggle(); // Flash LED on RX
      if (pkt.h.type == proto::MsgType::Status && pkt.h.src == LORA_NODE_RECEIVER)
      {
        gLastStatus = pkt.p.status;
        gLastStatus.lastRssiDbm = (int8_t)rssi;
        gLastStatus.lastSnrDb = (int8_t)snr;
        gLastStatusRxMs = millis();
      }
    }
  }

  // Handle serial UI.
  String line = readLineNonBlocking();
  if (line.length() > 0)
  {
    if (line.equalsIgnoreCase("stop"))
    {
      if (sendCommandWithAck(proto::CommandKind::Stop, 0))
      {
        Serial.println("Sent STOP (ACKed)");
      }
      else
      {
        Serial.println("Failed to send STOP");
      }
    }
    else if (line.equalsIgnoreCase("start"))
    {
      if (sendCommandWithAck(proto::CommandKind::Start, gLastMinutes))
      {
        Serial.printf("Sent START (%u min, ACKed)\n", gLastMinutes);
      }
      else
      {
        Serial.println("Failed to send START");
      }
    }
    else if (line.startsWith("run") || line.startsWith("RUN"))
    {
      int space = line.indexOf(' ');
      if (space < 0)
      {
        Serial.println("Usage: run <minutes>");
      }
      else
      {
        int minutes = line.substring(space + 1).toInt();
        if (minutes <= 0 || minutes > 255)
        {
          Serial.println("Minutes must be 1..255");
        }
        else
        {
          gLastMinutes = static_cast<uint8_t>(minutes);
          if (sendCommandWithAck(proto::CommandKind::RunMinutes, gLastMinutes))
          {
            Serial.printf("Sent RUN (%u min, ACKed)\n", gLastMinutes);
          }
          else
          {
            Serial.println("Failed to send RUN");
          }
        }
      }
    }
    else
    {
      Serial.println("Unknown command. Use: start | stop | run <minutes>");
    }
  }

  // Update LED
  statusLed.update();

  // Handle menu button
  menu.update();

  // Check if menu item was activated (long press)
  MenuItem activatedItem;
  if (menu.isItemActivated(activatedItem))
  {
    handleMenuSelection(activatedItem);
  }

  // Update battery voltage periodically
  if (millis() - gLastBattUpdateMs > VBAT_UPDATE_INTERVAL_MS)
  {
    gLastBattUpdateMs = millis();
    int raw = analogRead(VBAT_ADC_PIN);
    float vpin = (float)raw / 4095.0f * 3.3f; // approx. ADC scale to volts
    float vbat = vpin * VBAT_DIVIDER_RATIO * VBAT_CALIBRATION;
    // Simple low-pass filter to smooth jitter
    if (gBattV <= 0.01f)
    {
      gBattV = vbat;
    }
    else
    {
      gBattV = (gBattV * 0.8f) + (vbat * 0.2f);
    }
  }

  // OLED refresh
  static uint32_t lastUiMs = 0;
  if (millis() - lastUiMs > 250)
  {
    lastUiMs = millis();

    // Check if menu is visible
    if (menu.getState() == MenuState::Visible)
    {
      // Render menu
      ui.setLine(0, "=== MENU ===");
      ui.setLine(1, "");
      MenuItem selected = menu.getSelectedItem();
      for (int i = 0; i < static_cast<int>(MenuItem::Count); i++)
      {
        MenuItem item = static_cast<MenuItem>(i);
        String line;
        if (item == selected)
        {
          line = "> " + String(menuItemToStr(item));
        }
        else
        {
          line = "  " + String(menuItemToStr(item));
        }
        ui.setLine(2 + i, line);
      }
      ui.setLine(5, "Long press to activate");
    }
    else
    {
      // Render normal status view
      ui.setLine(0, "Webasto Sender Bat:" + String(gBattV, 1) + "V");
      ui.setLine(1, String("Preset:") + String(gLastMinutes) + "min -> " + String(gLastStatus.minutesRemaining) + "min");

      if (gLastStatusRxMs == 0)
      {
        ui.setLine(2, "Status: (none)");
        ui.setLine(3, "");
        ui.setLine(4, "");
      }
      else
      {
        uint32_t age = (millis() - gLastStatusRxMs) / 1000;
        ui.setLine(2, String("Heater: ") + heaterStateToStr(gLastStatus.state) + " age:" + String(age) + "s");
        ui.setLine(3, formatMeasurements(gLastStatus));
        ui.setLine(4,
                   String("RSSI:" + String(gLastStatus.lastRssiDbm) +
                          " SNR:" + String(gLastStatus.lastSnrDb) + "dB"));
      }

      if (gAwaitingCmdSeq != 0)
      {
        ui.setLine(5, String("Waiting ACK ") + String(gAwaitingCmdSeq));
      }
      else
      {
        // ui.setLine(5, "Serial: start/stop/run");
        // show useful info instead
        ui.setLine(5, String("Last CmdSeq: ") + String(gLastStatus.lastCmdSeq));
      }
    }

    ui.render();
  }
}
