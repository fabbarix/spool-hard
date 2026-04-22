#pragma once
#include <Arduino.h>
#include "config.h"
#include <functional>

struct OtaConfig {
    String url        = OTA_DEFAULT_URL;
    bool   use_ssl    = true;
    bool   verify_ssl = true;

    void load();
    void save() const;
};

// Run a firmware update with the stored config.
// Calls progressCb(percent) during download. Returns true on success (device
// will have rebooted and this call will not return). Returns false on error.
bool otaRun(const OtaConfig& cfg, std::function<void(int)> progressCb = nullptr);
