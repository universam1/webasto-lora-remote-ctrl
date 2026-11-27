// uiTask (core 1, prio 1): the only task that touches the OLED (I2C), the button and the LED.
// The button is polled every 20 ms (debounce, long press); the display is refreshed every 100 ms
// (50 ms while the menu is open).
#include <Arduino.h>

#include "menu_handler.h"
#include "oled_pages.h"
#include "oled_ui.h"
#include "project_config.h"
#include "receiver_ctx.h"
#include "status_led.h"
#include "task_stacks.h"
#include "task_utils.h"

#ifdef ENABLE_MQTT_CONTROL
#include "net_task.h"
#endif

namespace rx {
namespace {

constexpr uint32_t kPollMs = 20;

OledUi g_ui;
OledPages g_pages;
MenuHandler g_menu;
StatusLed g_led;

const char* phaseText(wh::HeaterPhase p) {
  switch (p) {
    case wh::HeaterPhase::Off: return "Aus";
    case wh::HeaterPhase::Heating: return "Heizt";
    case wh::HeaterPhase::RunOn: return "Nachlauf";
    case wh::HeaterPhase::Ventilating: return "Lueftet";
    case wh::HeaterPhase::Error: return "FEHLER";
    default: return "unbekannt";
  }
}

const char* climateText(wh::ClimateStatus s) {
  switch (s) {
    case wh::ClimateStatus::Off: return "aus";
    case wh::ClimateStatus::Idle: return "bereit";
    case wh::ClimateStatus::Starting: return "startet";
    case wh::ClimateStatus::HeaterRunning: return "heizt";
    case wh::ClimateStatus::Pause: return "Pause";
    case wh::ClimateStatus::SensorFault: return "Fuehler?";
    case wh::ClimateStatus::LowVoltage: return "Spannung";
    case wh::ClimateStatus::Failed: return "Fehler";
  }
  return "?";
}

String temp(int16_t dC) {
  if (dC == wh::kUnknownDC) return "--";
  return String(dC / 10.0f, 1);
}

void pushMenuCommand(rt::CmdType type) {
  rt::Command c{};
  c.src = rt::Source::Menu;
  c.type = type;
  if (!ctx().cmdQueue.push(c)) Serial.println("[ui] command queue full");
}

void handleMenu(MenuItem item) {
  Serial.printf("[MENU] Activated: %s\n", menuItemToStr(item));
  switch (item) {
    case MenuItem::Start: pushMenuCommand(rt::CmdType::Heat); break;
    case MenuItem::Stop: pushMenuCommand(rt::CmdType::Stop); break;
    case MenuItem::ClimateOn: pushMenuCommand(rt::CmdType::ClimateSet); break;  // stored target and duration
    case MenuItem::ClimateOff: pushMenuCommand(rt::CmdType::ClimateOff); break;
    case MenuItem::QueryStatus: pushMenuCommand(rt::CmdType::Query); break;
    case MenuItem::BlePair: ctx().blePairRequest.store(true); break;
    case MenuItem::BleClear: ctx().bleClearRequest.store(true); break;
    default: break;
  }
}

void drawMenu() {
  const MenuItem selected = g_menu.getSelectedItem();
  const int selectedIdx = g_menu.getSelectedPos();
  const int totalItems = g_menu.getItemCount();
  const int visibleItems = 4;
  const int page = selectedIdx / visibleItems;
  const int totalPages = (totalItems + visibleItems - 1) / visibleItems;
  const int startIdx = page * visibleItems;

  g_ui.setLine(0, "=== MENU === (long press)");
  for (int i = 0; i < visibleItems; i++) {
    const int idx = startIdx + i;
    if (idx < totalItems) {
      const MenuItem item = g_menu.getItemAt(idx);
      g_ui.setLine(1 + i, String(item == selected ? "> " : "  ") + menuItemToStr(item));
    } else {
      g_ui.setLine(1 + i, "");
    }
  }
  const float progress = g_menu.getLongPressProgress();
  if (progress > 0.0f) {
    g_ui.setLine(5, "Hold...");
    g_ui.drawProgressBar(55, 60, progress);
  } else {
    g_ui.setLine(5, String("Page ") + String(page + 1) + "/" + String(totalPages));
    g_ui.drawProgressBar(0, 0, 0.0f);
  }
}

void drawNormal(uint32_t now, const wh::SystemSnapshot& s, bool haveSnap) {
  g_ui.drawProgressBar(0, 0, 0.0f);
  if (!haveSnap) {
    g_ui.setLine(0, "Webasto Receiver");
    g_ui.setLine(1, "warte auf Daten...");
    for (int i = 2; i < 6; i++) g_ui.setLine(i, "");
    return;
  }
  const wh::HeaterSnapshot& h = s.heater;
  String l0 = String("Webasto ") + phaseText(h.phase);
  if (h.phaseAssumed) l0 += "~";
  if (h.errorCode) l0 += String(" E") + String(h.errorCode);
  g_ui.setLine(0, l0);
  g_ui.setLine(1, String("In ") + temp(s.interiorValid ? s.interior_dC : wh::kUnknownDC) + " Au " +
                      temp(h.outside_dC.seen ? h.outside_dC.v : wh::kUnknownDC) + " Ko " + temp(h.coolant_dC));
  String l2 = h.voltage_mV.seen ? String(h.voltage_mV.v / 1000.0f, 1) + "V" : String("--V");
  l2 += String(" ") + String(wh::heatingPowerW(h.powerRaw)) + "W " + String(h.fuelUsedMl) + "ml";
  g_ui.setLine(2, l2);
  String l3 = String("Klima ") + climateText(s.climateStatus);
  if (s.climateActive) l3 += String(" ") + String(s.climateTargetC) + "C " + String(s.climateRemainingMin) + "m";
  g_ui.setLine(3, l3);
  String l4 = String("LoRa ") + (ctx().loraOk.load() ? (s.senderActive ? "aktiv" : "ruhig") : "FEHLER");
#if defined(ENABLE_BLE) && ENABLE_BLE
  l4 += ctx().bleConnected.load() ? " BLE:ja" : " BLE:-";
#endif
  g_ui.setLine(4, l4);
  String l5;
#if defined(ENABLE_BLE) && ENABLE_BLE
  if (ctx().bleDefaultPasskey.load() && ((now / 3000) & 1)) l5 = "BLE: Standard-Passkey!";
#endif
#ifdef ENABLE_MQTT_CONTROL
  if (l5.length() == 0) {
    const uint8_t st = netTaskLinkState();
    l5 = st == 2 ? "WiFi+MQTT OK" : (st == 1 ? "WiFi OK, MQTT..." : "WiFi: verbinde");
  }
#endif
#if defined(ENABLE_BLE) && ENABLE_BLE
  // Every third 3 s slot shows the BLE name (so it is visible even while WiFi/MQTT text occupies the line).
  if (ctx().bleOk.load() && (now / 3000) % 3 == 2 && !(ctx().bleDefaultPasskey.load() && ((now / 3000) & 1)))
    l5 = ctx().bleName;
#endif
  if (!h.healthy) l5 = h.healthDetail == 4 ? "W-BUS gestoert" : "CAN gestoert";
  g_ui.setLine(5, l5);
}

void uiTaskFn(void*) {
  Ctx& c = ctx();
  rt::LoopTimer timer(kPollMs);
  wh::SystemSnapshot snap;
  bool haveSnap = false;
  uint32_t lastDraw = 0;
  uint32_t lastRx = 0, lastTx = 0;
  bool lastOn = false;
  bool ledInit = false;

  for (;;) {
    const uint32_t now = millis();
    // BLE items only while the BLE stack runs (the build flag alone is not enough: begin may fail).
    g_menu.setVisibleMask(kMenuCommonMask | (c.bleOk.load() ? kMenuBleMask : 0));
    g_menu.update();
    MenuItem activated;
    if (g_menu.isItemActivated(activated)) handleMenu(activated);

    if (c.snapshot.read(snap)) haveSnap = true;

    // LED: solid while the heater runs, slow blink otherwise, short pulse on LoRa traffic.
    const bool on = haveSnap && wh::oledHeaterOn(snap.heater.phase);
    if (!ledInit || on != lastOn) {
      ledInit = true;
      lastOn = on;
      if (on) g_led.setOn(); else g_led.setBlink(1000);
    }
    const uint32_t rx = c.loraRxCount.load(), tx = c.loraTxCount.load();
    if (rx != lastRx || tx != lastTx) {
      lastRx = rx;
      lastTx = tx;
      g_led.toggle();
    }
    g_led.update();

    const bool menuOpen = g_menu.getState() == MenuState::Visible;
    const uint32_t interval = menuOpen ? 50 : 100;
    if ((uint32_t)(now - lastDraw) >= interval) {
      lastDraw = now;
      if (menuOpen) {
        drawMenu();
        g_ui.render();
      } else if (haveSnap && g_pages.update(now, snap)) {
        // full screen heater pages own the display (one page per second, large font)
      } else {
        drawNormal(now, snap, haveSnap);
        g_ui.render();
      }
    }
    timer.waitNext();
  }
}

}  // namespace

void startUiTask() {
  g_led.begin();
  g_ui.begin();
  g_ui.setLine(0, "Webasto Receiver");
  g_ui.setLine(1, "Start...");
  g_ui.render();
  g_pages.begin(g_ui.display());
  g_menu.begin(MENU_BUTTON_PIN);
  g_menu.setVisibleMask(kMenuCommonMask);  // BLE items: see uiTaskFn; no range test on the receiver
  rt::TaskSpec spec = {"uiTask", uiTaskFn, rt::kStackUi, 1, 1, nullptr};
  rt::startTask(spec);
}

}  // namespace rx
