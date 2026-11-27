#pragma once

#include <WiFi.h>
#include <esp_wpa2.h>

#include "wifi_select.h"

class WiFiManager {
public:
  enum State {
    DISCONNECTED,
    SCANNING,
    CONNECTING,
    CONNECTED,
    FAILED
  };

  // One known network. auth Peap: WPA2-Enterprise PEAP + MSCHAPv2 (user + password),
  // auth Psk: WPA2-Personal (password only, user ignored). Empty ssid = slot unused.
  struct Network {
    const char* ssid = "";
    wsel::Auth auth = wsel::Auth::Psk;
    const char* user = "";
    const char* password = "";
    const char* anonymousId = "";
  };

  WiFiManager();

  // Up to wsel::kMaxNetworks networks. The manager scans and joins the strongest visible one.
  void begin(const Network* nets, size_t count);

  // Legacy single PEAP network.
  void begin(const char* ssid, const char* username, const char* password,
             const char* anonymousId = "");

  // Non-blocking state machine (no delay/busy wait). Call from the net task only.
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
  Network nets_[wsel::kMaxNetworks];
  size_t netCount_;
  int current_;          // index into nets_, -1 = none
  uint8_t failedMask_;   // networks that failed since the last successful scan round

  uint32_t connectStartMs_;
  uint32_t lastAttemptMs_;
  uint32_t lastConnectedMs_;
  uint32_t retryIntervalMs_;
  uint32_t scanStartMs_;
  bool modeSet_;

  // Fast reconnect optimization (only for the network that last connected)
  bool bssidCached_;
  int cachedNet_;
  uint8_t cachedBssid_[6];
  int32_t cachedChannel_;

  static constexpr uint32_t kScanTimeoutMs = 15000;
  static volatile uint8_t lastDisconnectReason_;

  void startScan();
  void handleScanResult();
  void connectTo(int idx);
  void checkConnection();
  void fail(const char* why);
  void handleDisconnected();
  void cacheBssidInfo();

  // WiFi event handlers
  static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
  static WiFiManager* instance_;
};
