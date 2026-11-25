#pragma once
// BLE GATT server (NimBLE-Arduino 2.x) shared by sender and receiver.
// The body is compiled only with -D ENABLE_BLE=1; otherwise this header declares nothing.
//
// THREADING (read this):
//  * NimBLE callbacks (onConnect, onWrite, ...) run in the NimBLE host task. They never block,
//    never call notify(), and only touch atomics, the command queue (push, timeout 0) and a small
//    lock-free single-producer/single-consumer ring of pending result events.
//  * service(now) and notifyResult() are the ONLY places that call notify() / setValue().
//    The coordinator must call them from one single task (service() about every 100 ms).
//    They never block. Do not call them from any other context.
//  * openPairingWindow(), clearBonds(), pairingWindowOpen(), the getters are safe from any task.
#if defined(ENABLE_BLE) && ENABLE_BLE

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "../webasto_core/ble_profile.h"
#include "../rt/command_queue.h"
#include "../rt/commands.h"
#include "ble_cmd.h"

#ifndef BLE_DEFAULT_PASSKEY
#define BLE_DEFAULT_PASSKEY 123456u
#endif
#ifndef BLE_PAIRING_WINDOW_MS
#define BLE_PAIRING_WINDOW_MS 120000u
#endif
#ifndef BLE_STATUS_MIN_INTERVAL_MS
#define BLE_STATUS_MIN_INTERVAL_MS 1000u  // plain value changes
#endif
#ifndef BLE_STATUS_HEARTBEAT_MS
#define BLE_STATUS_HEARTBEAT_MS 5000u  // while subscribed, even when unchanged
#endif

class NimBLEServer;
class NimBLECharacteristic;

class BleServer {
 public:
  using CommandQueue = rt::CommandQueue<rt::Command, 8>;
  using StatusProvider = bool (*)(uint8_t out[ble::kStatusLen]);

  // role: ble::Role, transport: ble::Transport, fw: ASCII (max 16 bytes used),
  // deviceName: "Webasto-RX" / "Webasto-TX", passkey: 0..999999.
  // `cmdQueue` must outlive the server. Call once from setup() or the BLE owner task.
  bool begin(CommandQueue& cmdQueue, uint8_t role, uint8_t transport, const char* fw,
             const char* deviceName, uint32_t passkey);

  void setStatusProvider(StatusProvider p) { provider_ = p; }

  // Owner task, about every 100 ms. Non blocking. now = millis().
  void service(uint32_t now);
  // Owner task (same one as service()). Encodes and notifies on the Result characteristic.
  void notifyResult(const rt::CmdEvent& ev);

  // Pairing control (any task).
  void openPairingWindow(uint32_t now);
  bool pairingWindowOpen(uint32_t now) const;
  void clearBonds();
  bool defaultPasskeyInUse() const { return passkey_ == BLE_DEFAULT_PASSKEY; }

  bool connected() const { return connCount_.load() > 0; }
  uint8_t connectionCount() const { return connCount_.load(); }
  // millis() of the last connect, subscribe, or Command write. 0 if never.
  uint32_t lastActivityMs() const { return lastActivity_.load(); }

  // Internal, used by the NimBLE callbacks (public only because the callback classes are
  // private to the .cpp).
  void onConnected(uint16_t handle, int bondsAtConnect);
  void onDisconnected(uint16_t handle);
  void onCommandWrite(const uint8_t* data, size_t len);
  void onSubscribed(bool on);
  void touch();
  bool windowOpenNow() const;
  int bondsAtConnect(uint16_t handle) const;

 private:
  static constexpr uint8_t kMaxConn = 3;
  static constexpr uint8_t kRing = 8;

  void pushPending(const rt::CmdEvent& ev);
  bool popPending(rt::CmdEvent& ev);

  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* chrStatus_ = nullptr;
  NimBLECharacteristic* chrResult_ = nullptr;
  CommandQueue* queue_ = nullptr;
  StatusProvider provider_ = nullptr;
  uint32_t passkey_ = BLE_DEFAULT_PASSKEY;

  std::atomic<uint8_t> connCount_{0};
  std::atomic<uint32_t> lastActivity_{0};
  std::atomic<bool> subscribed_{false};
  std::atomic<bool> windowOpen_{false};
  std::atomic<uint32_t> windowStart_{0};
  struct ConnSlot {
    std::atomic<int32_t> handle{-1};
    std::atomic<int32_t> bonds{0};
  };
  ConnSlot slots_[kMaxConn];

  // SPSC ring: producer = NimBLE host task, consumer = service().
  rt::CmdEvent ring_[kRing];
  std::atomic<uint8_t> ringHead_{0};  // written by consumer
  std::atomic<uint8_t> ringTail_{0};  // written by producer

  // Owner-task state only.
  uint8_t last_[ble::kStatusLen] = {0};
  bool haveLast_ = false;
  uint32_t lastNotify_ = 0;
};

#endif  // ENABLE_BLE
