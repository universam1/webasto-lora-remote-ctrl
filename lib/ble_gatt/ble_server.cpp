#if defined(ENABLE_BLE) && ENABLE_BLE

#include "ble_server.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>

#include "../webasto_core/heater_types.h"

namespace {

BleServer* gSelf = nullptr;

constexpr uint16_t kMtu = 247;
// Advertising interval in 0.625 ms units: 800 = 500 ms.
constexpr uint16_t kAdvMin = 760;
constexpr uint16_t kAdvMax = 800;
// Connection interval in 1.25 ms units: 24 = 30 ms, 40 = 50 ms. Timeout in 10 ms units.
constexpr uint16_t kConnMin = 24;
constexpr uint16_t kConnMax = 40;
constexpr uint16_t kConnLatency = 0;
constexpr uint16_t kConnTimeout = 400;

class ServerCb : public NimBLEServerCallbacks {
 public:
  explicit ServerCb(BleServer* s) : s_(s) {}

  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    s_->onConnected(info.getConnHandle(), NimBLEDevice::getNumBonds());
    server->updateConnParams(info.getConnHandle(), kConnMin, kConnMax, kConnLatency, kConnTimeout);
    // NimBLE stops advertising on connect; keep it up for further bonded devices.
    if (server->getConnectedCount() < 3) NimBLEDevice::getAdvertising()->start();
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override {
    s_->onDisconnected(info.getConnHandle());
    NimBLEDevice::getAdvertising()->start();
  }

  uint32_t onPassKeyDisplay() override { return passkey_; }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    NimBLEServer* server = NimBLEDevice::getServer();
    if (!info.isEncrypted() || !info.isAuthenticated()) {
      server->disconnect(info.getConnHandle());
      return;
    }
    // A bond was added by this pairing when the bond count grew since the connect.
    const bool newBond = NimBLEDevice::getNumBonds() > s_->bondsAtConnect(info.getConnHandle());
    if (newBond && !s_->windowOpenNow()) {
      NimBLEDevice::deleteBond(info.getIdAddress());
      server->disconnect(info.getConnHandle());
      return;
    }
    s_->touch();
  }

  uint32_t passkey_ = BLE_DEFAULT_PASSKEY;

 private:
  BleServer* s_;
};

class CmdCb : public NimBLECharacteristicCallbacks {
 public:
  explicit CmdCb(BleServer* s) : s_(s) {}
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    const NimBLEAttValue& v = c->getValue();
    s_->onCommandWrite(v.data(), v.size());
  }

 private:
  BleServer* s_;
};

class StatusCb : public NimBLECharacteristicCallbacks {
 public:
  explicit StatusCb(BleServer* s) : s_(s) {}
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    s_->onSubscribed((subValue & 1) != 0);
  }

 private:
  BleServer* s_;
};

// Fields that trigger an immediate notification: flags (phase), err, climate status, target,
// last command id and result.
bool significantChange(const uint8_t* a, const uint8_t* b) {
  return a[ble::S_FLAGS] != b[ble::S_FLAGS] || a[ble::S_ERR] != b[ble::S_ERR] ||
         a[ble::S_CLIMATE] != b[ble::S_CLIMATE] || a[ble::S_TARGET_C] != b[ble::S_TARGET_C] ||
         a[ble::S_LAST_CMD_ID] != b[ble::S_LAST_CMD_ID] ||
         a[ble::S_LAST_CMD_RESULT] != b[ble::S_LAST_CMD_RESULT];
}

}  // namespace

bool BleServer::begin(CommandQueue& cmdQueue, uint8_t role, uint8_t transport, const char* fw,
                      const char* deviceName, uint32_t passkey) {
  queue_ = &cmdQueue;
  passkey_ = passkey % 1000000u;
  gSelf = this;

  if (!NimBLEDevice::init(deviceName)) return false;
  NimBLEDevice::setMTU(kMtu);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityInitKey(BLE_HS_KEY_DIST_ENC_KEY | BLE_HS_KEY_DIST_ID_KEY);
  NimBLEDevice::setSecurityRespKey(BLE_HS_KEY_DIST_ENC_KEY | BLE_HS_KEY_DIST_ID_KEY);
  NimBLEDevice::setSecurityPasskey(passkey_);
  // At most 3 bonds: CONFIG_BT_NIMBLE_MAX_BONDS defaults to 3 in NimBLE-Arduino's nimconfig.h.

  server_ = NimBLEDevice::createServer();
  if (!server_) return false;
  ServerCb* scb = new ServerCb(this);
  scb->passkey_ = passkey_;
  server_->setCallbacks(scb);
  server_->advertiseOnDisconnect(false);  // ServerCb restarts advertising itself

  NimBLEService* svc = server_->createService(BLE_SERVICE_UUID);
  if (!svc) return false;

  NimBLECharacteristic* info =
      svc->createCharacteristic(BLE_CHR_INFO_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
  chrStatus_ = svc->createCharacteristic(
      BLE_CHR_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY |
                               NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
  NimBLECharacteristic* cmd = svc->createCharacteristic(
      BLE_CHR_COMMAND_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
                                NIMBLE_PROPERTY::WRITE_AUTHEN);
  chrResult_ = svc->createCharacteristic(
      BLE_CHR_RESULT_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  if (!info || !chrStatus_ || !cmd || !chrResult_) return false;

  uint8_t infoVal[3 + 16];
  infoVal[0] = ble::kInfoVersion;
  infoVal[1] = role;
  infoVal[2] = transport;
  size_t n = fw ? strnlen(fw, 16) : 0;
  if (n) memcpy(infoVal + 3, fw, n);
  info->setValue(infoVal, 3 + n);

  uint8_t zero[ble::kStatusLen] = {0};
  zero[ble::S_VER] = ble::kStatusVersion;
  chrStatus_->setValue(zero, sizeof(zero));

  cmd->setCallbacks(new CmdCb(this));
  chrStatus_->setCallbacks(new StatusCb(this));

  // server start() also starts the services (NimBLE 2.x)
  if (!server_->start()) return false;

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData ad;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  ad.setCompleteServices(NimBLEUUID(BLE_SERVICE_UUID));
  NimBLEAdvertisementData sr;
  sr.setName(deviceName);
  adv->setAdvertisementData(ad);
  adv->setScanResponseData(sr);
  adv->setMinInterval(kAdvMin);
  adv->setMaxInterval(kAdvMax);
  return adv->start();
}

void BleServer::openPairingWindow(uint32_t now) {
  windowStart_.store(now);
  windowOpen_.store(true);
}

bool BleServer::pairingWindowOpen(uint32_t now) const {
  return windowOpen_.load() && (uint32_t)(now - windowStart_.load()) < BLE_PAIRING_WINDOW_MS;
}

bool BleServer::windowOpenNow() const { return pairingWindowOpen(millis()); }

void BleServer::clearBonds() {
  windowOpen_.store(false);
  NimBLEDevice::deleteAllBonds();
  if (server_) {
    for (uint16_t h : server_->getPeerDevices()) server_->disconnect(h);
  }
}

void BleServer::touch() { lastActivity_.store(millis()); }

void BleServer::onConnected(uint16_t handle, int bonds) {
  for (auto& s : slots_) {
    int32_t expect = -1;
    if (s.handle.compare_exchange_strong(expect, (int32_t)handle)) {
      s.bonds.store(bonds);
      break;
    }
  }
  connCount_.fetch_add(1);
  touch();
}

void BleServer::onDisconnected(uint16_t handle) {
  for (auto& s : slots_) {
    int32_t expect = (int32_t)handle;
    if (s.handle.compare_exchange_strong(expect, -1)) break;
  }
  uint8_t c = connCount_.load();
  if (c > 0) connCount_.store(c - 1);
  if (connCount_.load() == 0) subscribed_.store(false);
}

int BleServer::bondsAtConnect(uint16_t handle) const {
  for (const auto& s : slots_) {
    if (s.handle.load() == (int32_t)handle) return s.bonds.load();
  }
  return 1000;  // unknown connection: never counts as a new bond
}

void BleServer::onSubscribed(bool on) {
  if (on) subscribed_.store(true);
  touch();
}

void BleServer::pushPending(const rt::CmdEvent& ev) {
  uint8_t t = ringTail_.load(std::memory_order_relaxed);
  uint8_t next = (uint8_t)((t + 1) % kRing);
  if (next == ringHead_.load(std::memory_order_acquire)) return;  // full: drop
  ring_[t] = ev;
  ringTail_.store(next, std::memory_order_release);
}

bool BleServer::popPending(rt::CmdEvent& ev) {
  uint8_t h = ringHead_.load(std::memory_order_relaxed);
  if (h == ringTail_.load(std::memory_order_acquire)) return false;
  ev = ring_[h];
  ringHead_.store((uint8_t)((h + 1) % kRing), std::memory_order_release);
  return true;
}

// NimBLE host task. No notify() here: failures are queued for service().
void BleServer::onCommandWrite(const uint8_t* data, size_t len) {
  touch();
  rt::Command cmd;
  rt::CmdEvent ev{};
  ev.src = rt::Source::Ble;
  ev.stage = ble::STAGE_FAILED;
  if (decodeBleCommand(data, len, cmd)) {
    if (queue_ && queue_->push(cmd)) return;
    ev.tag = cmd.tag;
    ev.result = (uint8_t)wh::CmdResult::Busy;
    pushPending(ev);
    return;
  }
  if (len >= 2 && data[1] != 0) {  // cmdId known: tell the phone the command was rejected
    ev.tag = data[1];
    ev.result = (uint8_t)wh::CmdResult::Unsupported;
    pushPending(ev);
  }
}

void BleServer::notifyResult(const rt::CmdEvent& ev) {
  if (!chrResult_ || !connected()) return;
  uint8_t b[4];
  size_t n = encodeBleResult(ev, b);
  chrResult_->setValue(b, n);
  chrResult_->notify(b, n);
}

void BleServer::service(uint32_t now) {
  if (!chrStatus_) return;

  rt::CmdEvent ev;
  while (popPending(ev)) notifyResult(ev);

  if (!provider_) return;
  uint8_t cur[ble::kStatusLen];
  if (!provider_(cur)) return;

  const bool changed = !haveLast_ || memcmp(cur, last_, sizeof(cur)) != 0;
  const bool significant = !haveLast_ || significantChange(cur, last_);
  const uint32_t age = now - lastNotify_;

  if (changed) chrStatus_->setValue(cur, sizeof(cur));  // keeps reads current

  if (!subscribed_.load() || !connected()) {
    if (changed) {
      memcpy(last_, cur, sizeof(cur));
      haveLast_ = true;
    }
    return;
  }

  bool send = false;
  if (significant) send = true;
  else if (changed && age >= BLE_STATUS_MIN_INTERVAL_MS) send = true;
  else if (age >= BLE_STATUS_HEARTBEAT_MS) send = true;
  if (!send) return;

  chrStatus_->notify(cur, sizeof(cur));
  memcpy(last_, cur, sizeof(cur));
  haveLast_ = true;
  lastNotify_ = now;
}

#endif  // ENABLE_BLE
