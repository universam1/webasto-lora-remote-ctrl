#pragma once
// Unique, role-aware BLE device name: "Webasto-RX-AB12" (receiver) or "Webasto-TX-AB12" (sender).
// AB12 = last two bytes of the base MAC, uppercase hex. Pure function, no ESP includes: the caller
// reads the MAC (e.g. esp_read_mac(mac, ESP_MAC_BT)). The PWA filters on the "Webasto-RX" / "Webasto-TX" prefix.
#include <stddef.h>
#include <stdint.h>

#include "ble_profile.h"

namespace ble {

constexpr size_t kNameLen = 16;  // "Webasto-RX-AB12" + NUL; fits the 31 byte scan response (2 + 15)

// Returns the name length without NUL, or 0 when the buffer is too small (out then holds ""). 
inline size_t makeBleName(char* out, size_t n, uint8_t role, const uint8_t mac[6]) {
  if (!out || n == 0) return 0;
  out[0] = 0;
  if (n < kNameLen) return 0;
  static const char hex[] = "0123456789ABCDEF";
  const char* prefix = role == RoleSender ? "Webasto-TX-" : "Webasto-RX-";
  size_t i = 0;
  while (prefix[i]) { out[i] = prefix[i]; i++; }
  out[i++] = hex[mac[4] >> 4];
  out[i++] = hex[mac[4] & 0x0F];
  out[i++] = hex[mac[5] >> 4];
  out[i++] = hex[mac[5] & 0x0F];
  out[i] = 0;
  return i;
}

}  // namespace ble
