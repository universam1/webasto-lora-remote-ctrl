/**
 * Simple LoRa test - based on RandomNerdTutorials TTGO LoRa32 example
 * https://randomnerdtutorials.com/ttgo-lora32-sx1276-arduino-ide/
 * 
 * Usage:
 *   Type 't' to transmit a test packet
 *   Type 'r' to put into receive mode and wait for packets
 *   Type 'f' to cycle through frequencies
 *   Type 'p' to toggle TX power (low/high) - use low when boards are close!
 */

#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>

// TTGO LoRa32-OLED V1.0 pins (verified from tutorial)
#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_CS 18
#define LORA_RST 14
#define LORA_DIO0 26

// Multiple frequencies to try - our boards are 433 MHz variants!
const long frequencies[] = {
  (long)433E6,   // 433 MHz ISM band (our boards!)
  (long)868E6,   // Standard EU868
  (long)866E6,   // Tutorial example
  (long)915E6,   // US915
  869500000L     // Specific EU frequency
};
const int NUM_FREQUENCIES = sizeof(frequencies) / sizeof(frequencies[0]);
int currentFreqIndex = 0;  // Start at 433 MHz

#define SYNC_WORD 0x12
#define BANDWIDTH 125E3
#define SPREADING_FACTOR 7
#define CODING_RATE 5

// TX power levels
#define TX_POWER_LOW 2     // Minimum power for close-range testing
#define TX_POWER_HIGH 17   // Default/normal power

uint32_t txCount = 0;
uint32_t rxCount = 0;
bool receiveMode = false;
bool callbackMode = false;
int txPower = TX_POWER_LOW;  // Start with low power for close-range testing

// ISR call counter for diagnostics
volatile uint32_t isrCallCount = 0;

// Callback for interrupt-based receive
void onReceiveCallback(int packetSize) {
  isrCallCount++;  // Track ISR invocations
  Serial.printf("*** CALLBACK #%lu: Received packet! size=%d ***\n", isrCallCount, packetSize);
  Serial.print("  Data: ");
  while (LoRa.available()) {
    char c = (char)LoRa.read();
    Serial.print(c);
  }
  Serial.println();
  Serial.printf("  RSSI=%d SNR=%.1f\n", LoRa.packetRssi(), LoRa.packetSnr());
  rxCount++;
}

void initLoRa() {
  long frequency = frequencies[currentFreqIndex];
  
  // Initialize SPI with explicit pins (critical for TTGO board)
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  
  // Set LoRa pins
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(frequency)) {
    Serial.println("LoRa init FAILED!");
    while (1) {
      delay(1000);
      Serial.println("LoRa init FAILED - halted");
    }
  }
  
  // Configure LoRa parameters
  LoRa.setTxPower(txPower);
  LoRa.setSyncWord(SYNC_WORD);
  LoRa.setSignalBandwidth(BANDWIDTH);
  LoRa.setSpreadingFactor(SPREADING_FACTOR);
  LoRa.setCodingRate4(CODING_RATE);
  LoRa.enableCrc();
  
  // Put in standby (idle) mode first, then explicitly go to receive
  LoRa.idle();
  
  Serial.printf("LoRa init OK! Freq=%lu Hz (index %d)\n", frequency, currentFreqIndex);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=== LORA TEST (Multi-frequency) ===");
  Serial.printf("Pins: SCK=%d MISO=%d MOSI=%d CS=%d RST=%d DIO0=%d\n",
                LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS, LORA_RST, LORA_DIO0);
  Serial.printf("Config: bw=%lu sf=%d cr=%d sync=0x%02X\n",
                (unsigned long)BANDWIDTH, SPREADING_FACTOR, CODING_RATE, SYNC_WORD);
  Serial.printf("TX Power: %d dBm (LOW for close-range testing)\n", txPower);
  Serial.printf("Available frequencies:\n");
  for (int i = 0; i < NUM_FREQUENCIES; i++) {
    Serial.printf("  [%d] %lu Hz%s\n", i, frequencies[i], i == currentFreqIndex ? " <-- current" : "");
  }
  
  initLoRa();
  
  Serial.println("\nCommands:");
  Serial.println("  't' = transmit test packet");
  Serial.println("  'r' = enter receive mode (polling)");
  Serial.println("  'c' = enter receive mode (callback/interrupt)");
  Serial.println("  's' = stop receive mode");
  Serial.println("  'f' = cycle to next frequency");
  Serial.println("  'p' = toggle TX power (2dBm / 17dBm)");
  Serial.println("  'd' = diagnostics");
  Serial.println("  'i' = re-init LoRa");
  Serial.println("  'x' = register dump");
  Serial.println();
}

void transmitTest() {
  Serial.printf("Transmitting packet #%lu...", txCount);
  
  LoRa.beginPacket();
  LoRa.print("Hello ");
  LoRa.print(txCount);
  int result = LoRa.endPacket();
  
  if (result == 1) {
    Serial.println(" OK!");
    txCount++;
  } else {
    Serial.printf(" FAILED (result=%d)\n", result);
  }
}

void checkReceive() {
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    Serial.printf("Received packet! size=%d rssi=%d snr=%.1f\n",
                  packetSize, LoRa.packetRssi(), LoRa.packetSnr());
    Serial.print("  Data: ");
    while (LoRa.available()) {
      char c = (char)LoRa.read();
      Serial.print(c);
    }
    Serial.println();
    rxCount++;
    Serial.printf("Total received: %lu\n", rxCount);
  }
}

void loop() {
  // Check for serial commands
  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case 't':
      case 'T':
        transmitTest();
        break;
      case 'r':
      case 'R':
        Serial.println("Entering receive mode (polling)...");
        LoRa.receive();
        receiveMode = true;
        Serial.println("Now listening for packets...");
        break;
      case 'c':
      case 'C':
      {
        // Callback-based receive (uses DIO0 interrupt)
        Serial.println("Entering receive mode (callback/interrupt)...");
        Serial.printf("DIO0 pin %d -> interrupt %d\n", LORA_DIO0, digitalPinToInterrupt(LORA_DIO0));
        pinMode(LORA_DIO0, INPUT);
        int dio0State = digitalRead(LORA_DIO0);
        Serial.printf("DIO0 initial state: %s\n", dio0State ? "HIGH" : "LOW");
        isrCallCount = 0;  // Reset counter
        LoRa.onReceive(onReceiveCallback);
        LoRa.receive();
        receiveMode = false;  // Don't poll, use callback
        callbackMode = true;
        Serial.println("Now listening via interrupt callback...");
        Serial.println("Watch for DIO0 state changes and ISR call count...");
        break;
      }
      case 's':
      case 'S':
        Serial.println("Stopping receive mode");
        LoRa.onReceive(NULL);  // Disable callback if set
        LoRa.idle();
        receiveMode = false;
        callbackMode = false;
        break;
      case 'f':
      case 'F':
        // Cycle to next frequency
        LoRa.onReceive(NULL);  // Disable callback
        receiveMode = false;
        currentFreqIndex = (currentFreqIndex + 1) % NUM_FREQUENCIES;
        Serial.printf("\n*** Switching to frequency %lu Hz ***\n", frequencies[currentFreqIndex]);
        initLoRa();
        break;
      case 'p':
      case 'P':
        // Toggle TX power between low (2dBm) and high (17dBm)
        if (txPower == TX_POWER_LOW) {
          txPower = TX_POWER_HIGH;
          Serial.printf("TX Power set to HIGH (%d dBm)\n", txPower);
        } else {
          txPower = TX_POWER_LOW;
          Serial.printf("TX Power set to LOW (%d dBm) for close-range testing\n", txPower);
        }
        LoRa.setTxPower(txPower);
        break;
      case 'd':
      case 'D':
        // Diagnostic dump
        Serial.println("=== LoRa Diagnostics ===");
        Serial.printf("  Current RSSI: %d\n", LoRa.rssi());  // Current RSSI (not packet)
        Serial.printf("  Last packet RSSI: %d\n", LoRa.packetRssi());
        Serial.printf("  Last packet SNR: %.1f\n", LoRa.packetSnr());
        Serial.printf("  Frequency error: %ld Hz\n", LoRa.packetFrequencyError());
        Serial.printf("  DIO0 pin state: %s\n", digitalRead(LORA_DIO0) ? "HIGH" : "LOW");
        Serial.printf("  ISR call count: %lu\n", isrCallCount);
        Serial.printf("  Callback mode: %s\n", callbackMode ? "ACTIVE" : "inactive");
        Serial.println("  NOTE: RSSI around -127 to -157 means no signal detected");
        Serial.println("  Check that antennas are connected on BOTH boards!");
        break;
      case 'i':
      case 'I':
        // Re-init LoRa (workaround from tutorial comments)
        Serial.println("Re-initializing LoRa...");
        initLoRa();
        if (receiveMode) {
          LoRa.receive();
          Serial.println("Re-entered receive mode");
        }
        break;
      case 'x':
      case 'X':
        // Dump registers to verify SPI communication
        Serial.println("=== LoRa Register Dump ===");
        LoRa.dumpRegisters(Serial);
        break;
    }
  }
  
  // If in receive mode, continuously check for packets
  if (receiveMode) {
    checkReceive();
    
    // Print status every 5 seconds
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 5000) {
      Serial.printf("[Status] Listening... rxCount=%lu rssi_floor=%d\n", rxCount, LoRa.packetRssi());
      lastStatus = millis();
    }
  }
  
  // If in callback mode, monitor DIO0 and ISR status
  if (callbackMode) {
    static uint32_t lastDio0Check = 0;
    static uint32_t lastIsrCount = 0;
    static uint8_t lastDio0State = 0;
    
    if (millis() - lastDio0Check > 3000) {
      int dio0 = digitalRead(LORA_DIO0);
      if (isrCallCount != lastIsrCount) {
        Serial.printf("[Callback] ISR active! Count=%lu (delta=%lu), DIO0=%d\n", 
                      isrCallCount, isrCallCount - lastIsrCount, dio0);
        lastIsrCount = isrCallCount;
      } else {
        Serial.printf("[Callback] No ISR calls (count still %lu), DIO0=%d\n", isrCallCount, dio0);
      }
      
      if (dio0 != lastDio0State) {
        Serial.printf("[Callback] DIO0 changed: %d -> %d\n", lastDio0State, dio0);
      }
      
      lastDio0State = dio0;
      lastDio0Check = millis();
    }
  }
  
  delay(1);
}
