#pragma once
#include <Arduino.h>
#include "config.h"

struct OtaConfig {
    String url        = OTA_DEFAULT_URL;
    bool   use_ssl    = true;
    bool   verify_ssl = true;

    void load();
    void save() const;
};

// Run a firmware update with the stored config.
// Calls progressCb(percent) during download. Returns true on success.
bool otaRun(const OtaConfig& cfg, std::function<void(int)> progressCb = nullptr);
