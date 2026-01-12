#pragma once

#include <WiFiClient.h>
#include <HTTPClient.h>
#include <Update.h>

// OTA update result
enum class OTAResult {
  SUCCESS,
  NO_UPDATE,           // No update available or not triggered
  DOWNLOAD_FAILED,     // Failed to download firmware
  UPDATE_FAILED,       // Failed to apply update
  HEATER_RUNNING,      // Cannot update while heater is running
  WIFI_DISCONNECTED    // WiFi not available
};

// OTA update callback for progress reporting
typedef void (*OTAProgressCallback)(size_t current, size_t total);
typedef void (*OTACompleteCallback)(OTAResult result, const char* message);

class OTAUpdater {
public:
  OTAUpdater();

  // Check if an OTA update is requested
  bool isUpdateRequested() const { return updateRequested_; }
  
  // Trigger an OTA update (called from MQTT message)
  void requestUpdate(const char* url);
  
  // Check if it's safe to update (heater must be OFF)
  bool canUpdate(bool heaterRunning) const;
  
  // Perform the OTA update (blocking operation - only call when heater is OFF!)
  // Returns true if update successful, false otherwise
  OTAResult performUpdate(const char* url, const char* username = "", const char* password = "");
  
  // Set progress callback
  void setProgressCallback(OTAProgressCallback callback);
  
  // Set complete callback
  void setCompleteCallback(OTACompleteCallback callback);
  
  // Reset update request flag
  void clearUpdateRequest() { updateRequested_ = false; }
  
  // Get last error message
  const char* getLastError() const { return lastError_; }

private:
  bool updateRequested_;
  char lastError_[128];
  
  OTAProgressCallback progressCallback_;
  OTACompleteCallback completeCallback_;
  
  // HTTP Basic Auth helper
  bool downloadAndUpdate(HTTPClient& http);
  
  // Update progress callback (static for Arduino Update library)
  static void updateProgress(size_t current, size_t total);
  static OTAUpdater* instance_;
};
