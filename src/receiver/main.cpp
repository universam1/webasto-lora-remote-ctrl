// Webasto LoRa receiver, Phase D task architecture.
//
// setup() only initialises hardware and starts the tasks; loop() is empty. Deep sleep is NOT
// supported any more (DISABLE_SLEEP semantics: the receiver never sleeps), because the LoRa,
// CAN and BLE tasks have to run continuously.
//
// Task        Core Prio Stack  Owns
// canTask     1    5    (lib)  TWAI (HeaterCan)      [CAN build, lib/heater]
// wbusTask    1    4    (lib)  Serial2 (HeaterWBus)  [W-BUS build, lib/heater]
// loraTask    1    4    6144   SX1276 / SPI
// ctrlTask    1    3    6144   OneWire (DS18B20), NVS, climate controller, command router
// bleTask     0    2    4096   NimBLE server API (BleServer::service/notifyResult)
// netTask     0    2    8192   WiFi, PubSubClient (lib/common/net_task)
// uiTask      1    1    6144   OLED (I2C), button, LED
//
// Flow: sources (menu, LoRa, MQTT, BLE) -> cmdQueue -> ctrlTask -> heater::submit().
//       heater events -> ctrlTask -> per source event queue (loraEvents, bleEvents, netEvents).
//       ctrlTask -> SnapshotBuffer<SystemSnapshot> -> all other tasks (wait-free read).
#include <Arduino.h>

#include "heater_iface.h"
#include "project_config.h"
#include "receiver_ctx.h"

#ifdef ENABLE_MQTT_CONTROL
#include "net_task.h"
#endif

namespace rx {
Ctx& ctx() {
  static Ctx c;
  return c;
}
}  // namespace rx

void setup() {
  // First: install the heater transport so the TWAI TX pin is recessive as early as possible.
  heater::instance().begin();

  Serial.begin(115200);
  Serial.println("\n==================================");
  Serial.println("  WEBASTO LORA RECEIVER (Phase D)");
  Serial.printf("  heater transport: %s\n", heater::instance().transport());
  Serial.println("==================================");

  rx::Ctx& c = rx::ctx();
  if (!c.cmdQueue.valid() || !c.loraEvents.valid() || !c.bleEvents.valid() || !c.netEvents.valid()) {
    Serial.println("[setup] queue creation failed");
  }

  rx::startUiTask();       // OLED, LED, button
  rx::startLoraTask();     // radio + task (continues without the task if the radio is missing)
  rx::startControlTask();  // snapshot producer: start before the readers matter
#if defined(ENABLE_BLE) && ENABLE_BLE
  rx::startBleTask();
#endif
#ifdef ENABLE_MQTT_CONTROL
  netTaskStart(c.cmdQueue, c.netEvents, c.snapshot);
#endif
  Serial.println("[setup] tasks started");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
