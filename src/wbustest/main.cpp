/**
 * W-BUS UART Test - Simple echo/loopback test for receiver/simulator connection
 * 
 * For RECEIVER (TTGO LoRa32):
 *   TX = GPIO17, RX = GPIO25 (GPIO34 is INPUT-ONLY, GPIO16 conflicts with OLED_RST!)
 * 
 * For SIMULATOR (ESP32 DevKit):
 *   TX = GPIO17, RX = GPIO16
 * 
 * Wiring:
 *   Simulator GPIO17 (TX) <-> Receiver GPIO25 (RX)
 *   Receiver GPIO17 (TX)  <-> Simulator GPIO16 (RX)
 *   GND <-> GND
 */

#include <Arduino.h>
#include <HardwareSerial.h>

// Use Serial2 for W-BUS communication
HardwareSerial wbusSerial(2);

#ifndef WBUS_TX_PIN
#define WBUS_TX_PIN 17
#endif
#ifndef WBUS_RX_PIN
#define WBUS_RX_PIN 25  // Default for receiver (GPIO25), simulator uses GPIO16
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=== W-BUS UART TEST ===");
  Serial.printf("TX Pin: GPIO%d\n", WBUS_TX_PIN);
  Serial.printf("RX Pin: GPIO%d\n", WBUS_RX_PIN);
  
  // Initialize UART with 2400 baud, 8E1 (8 bits, even parity, 1 stop bit)
  wbusSerial.begin(2400, SERIAL_8E1, WBUS_RX_PIN, WBUS_TX_PIN);
  
  Serial.println("UART initialized (2400 baud, 8E1)");
  Serial.println();
  
#ifdef DEVICE_ROLE_SIMULATOR
  Serial.println("Mode: SIMULATOR");
  Serial.println("  Type any character to send test message");
  Serial.println("  Will echo back any received bytes");
#else
  Serial.println("Mode: RECEIVER");
  Serial.println("  Type any character to send test message");
  Serial.println("  Will echo back any received bytes");
#endif
  
  Serial.println();
}

void loop() {
  // Check for data from USB serial (commands from user)
  if (Serial.available()) {
    char cmd = Serial.read();
    
    // Send a test message on W-BUS UART
    Serial.println("[TX] Sending test message...");
    wbusSerial.write("HELLO");
    wbusSerial.flush();
    Serial.println("[TX] Sent: HELLO");
  }
  
  // Check for data on W-BUS UART
  if (wbusSerial.available()) {
    Serial.print("[RX] Received: ");
    while (wbusSerial.available()) {
      char c = wbusSerial.read();
      if (c >= 32 && c < 127) {
        Serial.write(c);
      } else {
        Serial.printf("[0x%02X]", (uint8_t)c);
      }
    }
    Serial.println();
  }
  
  delay(10);
}
