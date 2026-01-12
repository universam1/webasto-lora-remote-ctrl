#include "ota_updater.h"
#include "project_config.h"
#include <WiFi.h>

OTAUpdater* OTAUpdater::instance_ = nullptr;

OTAUpdater::OTAUpdater()
  : updateRequested_(false),
    progressCallback_(nullptr),
    completeCallback_(nullptr) {
  instance_ = this;
  lastError_[0] = '\0';
}

void OTAUpdater::requestUpdate(const char* url) {
  updateRequested_ = true;
  Serial.printf("[OTA] Update requested: %s\n", url);
}

bool OTAUpdater::canUpdate(bool heaterRunning) const {
  if (heaterRunning) {
    return false;  // CRITICAL: Never update while heater is running
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    return false;  // WiFi required for download
  }
  
  return true;
}

void OTAUpdater::setProgressCallback(OTAProgressCallback callback) {
  progressCallback_ = callback;
}

void OTAUpdater::setCompleteCallback(OTACompleteCallback callback) {
  completeCallback_ = callback;
}

OTAResult OTAUpdater::performUpdate(const char* url, const char* username, const char* password) {
  Serial.println("[OTA] Starting OTA update...");
  Serial.printf("[OTA] URL: %s\n", url);
  
  // Check WiFi
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(lastError_, sizeof(lastError_), "WiFi not connected");
    Serial.println("[OTA] ERROR: WiFi not connected");
    if (completeCallback_) {
      completeCallback_(OTAResult::WIFI_DISCONNECTED, lastError_);
    }
    return OTAResult::WIFI_DISCONNECTED;
  }
  
  HTTPClient http;
  http.setTimeout(30000);  // 30 second timeout
  
  // Set HTTP Basic Auth if provided
  if (username && strlen(username) > 0) {
    http.setAuthorization(username, password);
    Serial.println("[OTA] Using HTTP Basic Auth");
  }
  
  // Begin HTTP connection
  if (!http.begin(url)) {
    snprintf(lastError_, sizeof(lastError_), "Failed to begin HTTP connection");
    Serial.println("[OTA] ERROR: Failed to begin HTTP connection");
    if (completeCallback_) {
      completeCallback_(OTAResult::DOWNLOAD_FAILED, lastError_);
    }
    return OTAResult::DOWNLOAD_FAILED;
  }
  
  // Get firmware
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    snprintf(lastError_, sizeof(lastError_), "HTTP GET failed: %d", httpCode);
    Serial.printf("[OTA] ERROR: HTTP GET failed: %d\n", httpCode);
    http.end();
    if (completeCallback_) {
      completeCallback_(OTAResult::DOWNLOAD_FAILED, lastError_);
    }
    return OTAResult::DOWNLOAD_FAILED;
  }
  
  int contentLength = http.getSize();
  if (contentLength <= 0) {
    snprintf(lastError_, sizeof(lastError_), "Invalid content length: %d", contentLength);
    Serial.printf("[OTA] ERROR: Invalid content length: %d\n", contentLength);
    http.end();
    if (completeCallback_) {
      completeCallback_(OTAResult::DOWNLOAD_FAILED, lastError_);
    }
    return OTAResult::DOWNLOAD_FAILED;
  }
  
  Serial.printf("[OTA] Firmware size: %d bytes\n", contentLength);
  
  // Begin OTA update
  if (!Update.begin(contentLength)) {
    snprintf(lastError_, sizeof(lastError_), "Not enough space for OTA");
    Serial.printf("[OTA] ERROR: Not enough space for OTA (need %d bytes)\n", contentLength);
    http.end();
    if (completeCallback_) {
      completeCallback_(OTAResult::UPDATE_FAILED, lastError_);
    }
    return OTAResult::UPDATE_FAILED;
  }
  
  // Set progress callback
  if (progressCallback_) {
    Update.onProgress([](size_t current, size_t total) {
      if (OTAUpdater::instance_ && OTAUpdater::instance_->progressCallback_) {
        OTAUpdater::instance_->progressCallback_(current, total);
      }
    });
  }
  
  // Download and write firmware
  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  
  Serial.printf("[OTA] Written %d of %d bytes\n", written, contentLength);
  
  if (written != contentLength) {
    snprintf(lastError_, sizeof(lastError_), "Write incomplete: %d/%d", written, contentLength);
    Serial.printf("[OTA] ERROR: Write incomplete: %d/%d bytes\n", written, contentLength);
    Update.abort();
    http.end();
    if (completeCallback_) {
      completeCallback_(OTAResult::UPDATE_FAILED, lastError_);
    }
    return OTAResult::UPDATE_FAILED;
  }
  
  // Finalize update
  if (!Update.end()) {
    snprintf(lastError_, sizeof(lastError_), "Update.end() failed: %s", Update.errorString());
    Serial.printf("[OTA] ERROR: Update.end() failed: %s\n", Update.errorString());
    http.end();
    if (completeCallback_) {
      completeCallback_(OTAResult::UPDATE_FAILED, lastError_);
    }
    return OTAResult::UPDATE_FAILED;
  }
  
  http.end();
  
  // Verify update
  if (!Update.isFinished()) {
    snprintf(lastError_, sizeof(lastError_), "Update not finished");
    Serial.println("[OTA] ERROR: Update not finished");
    if (completeCallback_) {
      completeCallback_(OTAResult::UPDATE_FAILED, lastError_);
    }
    return OTAResult::UPDATE_FAILED;
  }
  
  Serial.println("[OTA] Update successful! Rebooting...");
  if (completeCallback_) {
    completeCallback_(OTAResult::SUCCESS, "Update successful");
  }
  
  delay(1000);  // Give time for message to be sent
  ESP.restart();  // Reboot to apply new firmware
  
  return OTAResult::SUCCESS;
}
