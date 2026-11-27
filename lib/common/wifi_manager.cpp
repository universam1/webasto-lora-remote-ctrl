#include "wifi_manager.h"
#include "project_config.h"

#ifdef ENABLE_MQTT_CONTROL

WiFiManager* WiFiManager::instance_ = nullptr;
volatile uint8_t WiFiManager::lastDisconnectReason_ = 0;

static const char* authName(wsel::Auth a) { return a == wsel::Auth::Peap ? "PEAP" : "PSK"; }

WiFiManager::WiFiManager()
  : state_(DISCONNECTED),
    netCount_(0),
    current_(-1),
    failedMask_(0),
    connectStartMs_(0),
    lastAttemptMs_(0),
    lastConnectedMs_(0),
    retryIntervalMs_(MQTT_WIFI_RETRY_INTERVAL_MS),
    scanStartMs_(0),
    modeSet_(false),
    bssidCached_(false),
    cachedNet_(-1),
    cachedChannel_(0) {
  instance_ = this;
}

void WiFiManager::begin(const Network* nets, size_t count) {
  netCount_ = 0;
  for (size_t i = 0; i < count && netCount_ < wsel::kMaxNetworks; ++i) {
    if (!nets[i].ssid || !nets[i].ssid[0]) continue;
    nets_[netCount_++] = nets[i];
  }

  // Register WiFi event handlers
  WiFi.onEvent(onWiFiEvent);

  Serial.println("[WiFi] Manager initialized");
  for (size_t i = 0; i < netCount_; ++i) {
    Serial.printf("[WiFi] Net %u: %s (%s%s%s)\n", (unsigned)i, nets_[i].ssid, authName(nets_[i].auth),
                  nets_[i].auth == wsel::Auth::Peap ? ", user " : "",
                  nets_[i].auth == wsel::Auth::Peap ? nets_[i].user : "");
  }
  if (netCount_ == 0) Serial.println("[WiFi] No network configured");
}

void WiFiManager::begin(const char* ssid, const char* username, const char* password,
                        const char* anonymousId) {
  Network n;
  n.ssid = ssid;
  n.auth = wsel::Auth::Peap;
  n.user = username;
  n.password = password;
  n.anonymousId = anonymousId;
  begin(&n, 1);
}

void WiFiManager::update() {
  const uint32_t now = millis();

  switch (state_) {
    case DISCONNECTED:
      if (netCount_ > 0 && (lastAttemptMs_ == 0 || now - lastAttemptMs_ >= retryIntervalMs_)) {
        startScan();
      }
      break;

    case SCANNING:
      handleScanResult();
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
        state_ = DISCONNECTED;
      }
      break;
  }
}

void WiFiManager::startScan() {
  lastAttemptMs_ = millis();
  if (!modeSet_) {
    // Station mode once; do not cycle the radio off/on on every attempt (that produced
    // "wifi:timeout when WiFi un-init" and never let an association finish).
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);  // the state machine reconnects itself
    modeSet_ = true;
  }
  // Fast path: try the network that worked last time directly with its cached BSSID/channel.
  if (bssidCached_ && cachedNet_ >= 0 && !(failedMask_ & (1u << cachedNet_))) {
    Serial.printf("[WiFi] Fast reconnect to %s (channel %d)\n", nets_[cachedNet_].ssid, (int)cachedChannel_);
    connectTo(cachedNet_);
    return;
  }
  WiFi.scanDelete();
  const int16_t r = WiFi.scanNetworks(true /*async*/, false /*hidden*/);
  if (r == WIFI_SCAN_FAILED) {
    fail("scan could not be started");
    return;
  }
  scanStartMs_ = millis();
  state_ = SCANNING;
}

void WiFiManager::handleScanResult() {
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (millis() - scanStartMs_ > kScanTimeoutMs) {
      WiFi.scanDelete();
      fail("scan timeout");
    }
    return;
  }
  if (n < 0) {
    fail("scan failed");
    return;
  }

  static constexpr int kMaxSeen = 32;
  wsel::Seen seen[kMaxSeen];
  String names[kMaxSeen];
  const int count = n < kMaxSeen ? n : kMaxSeen;
  for (int i = 0; i < count; ++i) {
    names[i] = WiFi.SSID(i);
    seen[i].ssid = names[i].c_str();
    seen[i].rssi = WiFi.RSSI(i);
  }
  wsel::Known known[wsel::kMaxNetworks];
  for (size_t i = 0; i < netCount_; ++i) {
    known[i].ssid = nets_[i].ssid;
    known[i].auth = nets_[i].auth;
  }

  int pick = wsel::pickNetwork(known, netCount_, seen, count, failedMask_);
  if (pick < 0 && failedMask_) {
    // Every visible known network failed once: start a new round with all of them.
    failedMask_ = 0;
    pick = wsel::pickNetwork(known, netCount_, seen, count, 0);
  }

  Serial.printf("[WiFi] Scan: %d networks", n);
  for (size_t k = 0; k < netCount_; ++k) {
    int32_t best = -128;
    for (int i = 0; i < count; ++i)
      if (names[i] == nets_[k].ssid && seen[i].rssi > best) best = seen[i].rssi;
    if (best > -128) Serial.printf(", %s %d dBm", nets_[k].ssid, (int)best);
    else Serial.printf(", %s not seen", nets_[k].ssid);
  }
  Serial.println();
  WiFi.scanDelete();

  if (pick < 0) {
    fail("no known network in range");
    return;
  }
  connectTo(pick);
}

void WiFiManager::connectTo(int idx) {
  current_ = idx;
  const Network& n = nets_[idx];
  state_ = CONNECTING;
  connectStartMs_ = millis();
  lastAttemptMs_ = connectStartMs_;
  lastDisconnectReason_ = 0;

  const bool fast = bssidCached_ && cachedNet_ == idx;
  Serial.printf("[WiFi] Connecting to %s (%s)\n", n.ssid, authName(n.auth));
  if (n.auth == wsel::Auth::Peap) {
    const char* ident = (n.anonymousId && n.anonymousId[0]) ? n.anonymousId : n.user;
    if (fast) WiFi.begin(n.ssid, WPA2_AUTH_PEAP, ident, n.user, n.password, nullptr, nullptr, nullptr,
                         cachedChannel_, cachedBssid_);
    else WiFi.begin(n.ssid, WPA2_AUTH_PEAP, ident, n.user, n.password);
  } else {
    if (fast) WiFi.begin(n.ssid, n.password, cachedChannel_, cachedBssid_);
    else WiFi.begin(n.ssid, n.password);
  }
}

void WiFiManager::checkConnection() {
  const uint32_t now = millis();
  const wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected to %s, IP %s, RSSI %d dBm\n", nets_[current_].ssid,
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    state_ = CONNECTED;
    lastConnectedMs_ = now;
    failedMask_ = 0;
    cacheBssidInfo();
    return;
  }

  if (now - connectStartMs_ > wsel::connectTimeoutMs(nets_[current_].auth)) {
    fail("connection timeout");
    return;
  }
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    fail(status == WL_NO_SSID_AVAIL ? "SSID not available" : "connect failed");
  }
  // Otherwise still connecting, keep waiting
}

void WiFiManager::fail(const char* why) {
  if (current_ >= 0 && state_ == CONNECTING) {
    Serial.printf("[WiFi] %s: %s (status %d, disconnect reason %u)\n", nets_[current_].ssid, why,
                  (int)WiFi.status(), (unsigned)lastDisconnectReason_);
    failedMask_ |= (uint8_t)(1u << current_);
    if (cachedNet_ == current_) bssidCached_ = false;  // retry with a full scan
    WiFi.disconnect(false);                         // keep the radio on
  } else {
    Serial.printf("[WiFi] %s\n", why);
  }
  current_ = -1;
  state_ = FAILED;
  lastAttemptMs_ = millis();
}

void WiFiManager::handleDisconnected() {
  if (state_ == CONNECTED) {
    Serial.printf("[WiFi] Disconnected (reason %u)\n", (unsigned)lastDisconnectReason_);
    state_ = DISCONNECTED;
  }
}

void WiFiManager::cacheBssidInfo() {
  uint8_t* bssid = WiFi.BSSID();
  if (bssid) {
    memcpy(cachedBssid_, bssid, 6);
    cachedChannel_ = WiFi.channel();
    cachedNet_ = current_;
    bssidCached_ = true;
  }
}

bool WiFiManager::isConnected() const {
  return state_ == CONNECTED && WiFi.status() == WL_CONNECTED;
}

IPAddress WiFiManager::getLocalIP() const {
  return WiFi.localIP();
}

const char* WiFiManager::getSSID() const {
  return (state_ == CONNECTED && current_ >= 0) ? nets_[current_].ssid : "";
}

void WiFiManager::disconnect() {
  WiFi.disconnect(false);
  state_ = DISCONNECTED;
  current_ = -1;
  Serial.println("[WiFi] Manual disconnect");
}

uint32_t WiFiManager::getTimeSinceLastConnect() const {
  if (lastConnectedMs_ == 0) return 0xFFFFFFFF;
  return millis() - lastConnectedMs_;
}

void WiFiManager::onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (!instance_) return;

  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] Event: Got IP: %s\n", WiFi.localIP().toString().c_str());
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      lastDisconnectReason_ = info.wifi_sta_disconnected.reason;
      instance_->handleDisconnected();
      break;

    default:
      break;
  }
}

#endif // ENABLE_MQTT_CONTROL
