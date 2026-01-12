#include "wifi_manager.h"
#include "project_config.h"

#ifdef ENABLE_MQTT_CONTROL

WiFiManager* WiFiManager::instance_ = nullptr;

WiFiManager::WiFiManager()
  : state_(DISCONNECTED),
    ssid_(nullptr),
    username_(nullptr),
    password_(nullptr),
    anonymousId_(nullptr),
    connectStartMs_(0),
    lastAttemptMs_(0),
    lastConnectedMs_(0),
    connectTimeoutMs_(MQTT_WIFI_TIMEOUT_MS),
    retryIntervalMs_(MQTT_WIFI_RETRY_INTERVAL_MS),
    bssidCached_(false),
    cachedChannel_(0) {
  instance_ = this;
}

void WiFiManager::begin(const char* ssid, const char* username, 
                        const char* password, const char* anonymousId) {
  ssid_ = ssid;
  username_ = username;
  password_ = password;
  anonymousId_ = anonymousId;

  // Register WiFi event handlers
  WiFi.onEvent(onWiFiEvent);

  Serial.println("[WiFi] Manager initialized");
  Serial.printf("[WiFi] SSID: %s, Username: %s\n", ssid_, username_);
}

void WiFiManager::update() {
  const uint32_t now = millis();

  switch (state_) {
    case DISCONNECTED:
      // Try to connect if retry interval elapsed
      if (now - lastAttemptMs_ >= retryIntervalMs_) {
        startConnection();
      }
      break;

    case CONNECTING:
      checkConnection();
      break;

    case CONNECTED:
      // Monitor connection health
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Connection lost");
        state_ = DISCONNECTED;
      }
      break;

    case FAILED:
      // Retry after interval
      if (now - lastAttemptMs_ >= retryIntervalMs_) {
        Serial.println("[WiFi] Retrying connection after failure");
        state_ = DISCONNECTED;
      }
      break;
  }
}

void WiFiManager::startConnection() {
  Serial.println("[WiFi] Starting connection...");
  
  state_ = CONNECTING;
  connectStartMs_ = millis();
  lastAttemptMs_ = connectStartMs_;

  // Set WiFi to station mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);  // Brief delay for clean disconnect

  // Configure WPA2 Enterprise
  if (anonymousId_ && strlen(anonymousId_) > 0) {
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)anonymousId_, strlen(anonymousId_));
    Serial.printf("[WiFi] Anonymous identity: %s\n", anonymousId_);
  }
  
  esp_wifi_sta_wpa2_ent_set_username((uint8_t*)username_, strlen(username_));
  esp_wifi_sta_wpa2_ent_set_password((uint8_t*)password_, strlen(password_));
  esp_wifi_sta_wpa2_ent_enable();

  // Start connection
  if (bssidCached_) {
    // Fast reconnect with cached BSSID and channel
    Serial.printf("[WiFi] Fast reconnect (channel %d)\n", cachedChannel_);
    WiFi.begin(ssid_, nullptr, cachedChannel_, cachedBssid_);
  } else {
    // Full scan
    Serial.println("[WiFi] Full network scan");
    WiFi.begin(ssid_);
  }
}

void WiFiManager::checkConnection() {
  const uint32_t now = millis();
  const wl_status_t status = WiFi.status();

  // Check for timeout
  if (now - connectStartMs_ > connectTimeoutMs_) {
    Serial.println("[WiFi] Connection timeout");
    WiFi.disconnect(true);
    state_ = FAILED;
    return;
  }

  // Check connection status
  if (status == WL_CONNECTED) {
    Serial.println("[WiFi] Connected!");
    Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
    
    state_ = CONNECTED;
    lastConnectedMs_ = now;
    
    // Cache BSSID and channel for fast reconnect
    cacheBssidInfo();
    
  } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    Serial.printf("[WiFi] Connection failed (status=%d)\n", status);
    WiFi.disconnect(true);
    state_ = FAILED;
    
    // Clear cached BSSID if connection failed
    bssidCached_ = false;
  }
  // Otherwise still connecting, keep waiting
}

void WiFiManager::handleDisconnected() {
  if (state_ == CONNECTED) {
    Serial.println("[WiFi] Disconnected event");
    state_ = DISCONNECTED;
  }
}

void WiFiManager::cacheBssidInfo() {
  uint8_t* bssid = WiFi.BSSID();
  if (bssid) {
    memcpy(cachedBssid_, bssid, 6);
    cachedChannel_ = WiFi.channel();
    bssidCached_ = true;
    
    Serial.printf("[WiFi] Cached BSSID: %02X:%02X:%02X:%02X:%02X:%02X, Channel: %d\n",
                  cachedBssid_[0], cachedBssid_[1], cachedBssid_[2],
                  cachedBssid_[3], cachedBssid_[4], cachedBssid_[5],
                  cachedChannel_);
  }
}

bool WiFiManager::isConnected() const {
  return state_ == CONNECTED && WiFi.status() == WL_CONNECTED;
}

IPAddress WiFiManager::getLocalIP() const {
  return WiFi.localIP();
}

const char* WiFiManager::getSSID() const {
  return WiFi.SSID().c_str();
}

void WiFiManager::disconnect() {
  WiFi.disconnect(true);
  state_ = DISCONNECTED;
  Serial.println("[WiFi] Manual disconnect");
}

uint32_t WiFiManager::getTimeSinceLastConnect() const {
  if (lastConnectedMs_ == 0) return 0xFFFFFFFF;
  return millis() - lastConnectedMs_;
}

void WiFiManager::onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (!instance_) return;

  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi] Event: Connected to AP");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] Event: Got IP: %s\n", 
                    WiFi.localIP().toString().c_str());
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WiFi] Event: Disconnected from AP");
      instance_->handleDisconnected();
      break;

    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
      Serial.println("[WiFi] Event: Auth mode changed");
      break;

    default:
      break;
  }
}

#endif // ENABLE_MQTT_CONTROL
