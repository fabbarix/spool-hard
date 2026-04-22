#include "ota.h"
#include "config.h"
#include <Preferences.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>

void OtaConfig::load() {
    Preferences prefs;
    prefs.begin(NVS_NS_OTA, true);
    url        = prefs.getString(NVS_KEY_OTA_URL,    OTA_DEFAULT_URL);
    use_ssl    = prefs.getBool(NVS_KEY_OTA_USE_SSL,  true);
    verify_ssl = prefs.getBool(NVS_KEY_OTA_VERIFY,   true);
    prefs.end();
}

void OtaConfig::save() const {
    Preferences prefs;
    prefs.begin(NVS_NS_OTA, false);
    prefs.putString(NVS_KEY_OTA_URL,   url);
    prefs.putBool(NVS_KEY_OTA_USE_SSL, use_ssl);
    prefs.putBool(NVS_KEY_OTA_VERIFY,  verify_ssl);
    prefs.end();
}

bool otaRun(const OtaConfig& cfg, std::function<void(int)> progressCb) {
    Serial.printf("[OTA] Starting update from: %s\n", cfg.url.c_str());
    httpUpdate.rebootOnUpdate(false);

    if (progressCb) {
        httpUpdate.onProgress([progressCb](int cur, int total) {
            if (total > 0) progressCb((cur * 100) / total);
        });
    }

    t_httpUpdate_return result;
    if (cfg.use_ssl) {
        WiFiClientSecure client;
        client.setInsecure();  // verify_ssl would load a CA bundle — TODO follow-up
        result = httpUpdate.update(client, cfg.url);
    } else {
        WiFiClient client;
        result = httpUpdate.update(client, cfg.url);
    }

    switch (result) {
        case HTTP_UPDATE_OK:
            Serial.println("[OTA] Update successful — rebooting");
            delay(500);
            ESP.restart();
            return true;
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("[OTA] No update available");
            return false;
        case HTTP_UPDATE_FAILED:
            Serial.printf("[OTA] Update failed: (%d) %s\n",
                          httpUpdate.getLastError(),
                          httpUpdate.getLastErrorString().c_str());
            return false;
    }
    return false;
}
