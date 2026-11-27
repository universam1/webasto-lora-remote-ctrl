#pragma once
// Pure selection logic for the WiFi manager (host-testable, no Arduino includes).
//
// The firmware knows up to kMaxNetworks networks (index 0 is the WPA2-Enterprise/PEAP network,
// the others are WPA2-PSK). After a scan, pickNetwork() returns the index of the known network
// with the strongest signal that is visible, skipping networks that failed recently.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wsel {

constexpr size_t kMaxNetworks = 3;

enum class Auth : uint8_t { Peap, Psk };

struct Known {
  const char* ssid = nullptr;  // nullptr or "" = slot unused
  Auth auth = Auth::Psk;
};

struct Seen {
  const char* ssid = nullptr;
  int32_t rssi = -127;
};

inline bool used(const Known& k) { return k.ssid && k.ssid[0]; }

// failedMask: bit i set = network i failed in the current round and is skipped.
// Returns the index of the chosen known network, or -1 if none is visible.
inline int pickNetwork(const Known* known, size_t nKnown, const Seen* seen, size_t nSeen,
                       uint8_t failedMask = 0) {
  int best = -1;
  int32_t bestRssi = -128;
  for (size_t i = 0; i < nKnown && i < kMaxNetworks; ++i) {
    if (!used(known[i]) || (failedMask & (1u << i))) continue;
    for (size_t j = 0; j < nSeen; ++j) {
      if (!seen[j].ssid || strcmp(seen[j].ssid, known[i].ssid) != 0) continue;
      if (seen[j].rssi > bestRssi) {
        bestRssi = seen[j].rssi;
        best = (int)i;
      }
    }
  }
  return best;
}

// Connect timeout per auth type: PEAP needs the EAP handshake on top of association.
inline uint32_t connectTimeoutMs(Auth a) { return a == Auth::Peap ? 20000u : 12000u; }

}  // namespace wsel
