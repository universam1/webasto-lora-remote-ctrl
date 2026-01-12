#pragma once

#include <WiFi.h>
#include <esp_wpa2.h>

class WiFiManager {
public:
  enum State {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    FAILED
  };

  WiFiManager();

  // Initialize WiFi with WPA2 Enterprise credentials
  void begin(const char* ssid, const char* username, const char* password, 
             const char* anonymousId = "");

  // Non-blocking update - call in loop()
  void update();

  // Check if WiFi is connected
  bool isConnected() const;

  // Get current state
  State getState() const { return state_; }

  // Get connection info
  IPAddress getLocalIP() const;
  const char* getSSID() const;

  // Force disconnect
  void disconnect();

  // Get time since last successful connection
  uint32_t getTimeSinceLastConnect() const;

private:
  State state_;
  const char* ssid_;
  const char* username_;
  const char* password_;
  const char* anonymousId_;
  
  uint32_t connectStartMs_;
  uint32_t lastAttemptMs_;
  uint32_t lastConnectedMs_;
  uint32_t connectTimeoutMs_;
  uint32_t retryIntervalMs_;

  // Fast reconnect optimization
  bool bssidCached_;
  uint8_t cachedBssid_[6];
  int32_t cachedChannel_;

  void startConnection();
  void checkConnection();
  void handleDisconnected();
  void cacheBssidInfo();

  // WiFi event handlers
  static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
  static WiFiManager* instance_;
};
