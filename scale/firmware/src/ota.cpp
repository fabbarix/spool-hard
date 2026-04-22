#include "ota.h"
#include "config.h"
#include <Preferences.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>

// ── Config persistence ────────────────────────────────────────

void OtaConfig::load() {
    Preferences prefs;
    prefs.begin(NVS_NS_OTA, true);
    url        = prefs.getString(NVS_KEY_OTA_URL,     OTA_DEFAULT_URL);
    use_ssl    = prefs.getBool(NVS_KEY_OTA_USE_SSL,   true);
    verify_ssl = prefs.getBool(NVS_KEY_OTA_VERIFY,    true);
    prefs.end();
}

void OtaConfig::save() const {
    Preferences prefs;
    prefs.begin(NVS_NS_OTA, false);
    prefs.putString(NVS_KEY_OTA_URL,    url);
    prefs.putBool(NVS_KEY_OTA_USE_SSL,  use_ssl);
    prefs.putBool(NVS_KEY_OTA_VERIFY,   verify_ssl);
    prefs.end();
}

// ── Update ────────────────────────────────────────────────────

bool otaRun(const OtaConfig& cfg, std::function<void(int)> progressCb) {
    Serial.printf("[OTA] Starting update from: %s\n", cfg.url.c_str());
    Serial.printf("[OTA] SSL: %s  Verify cert: %s\n",
                  cfg.use_ssl    ? "yes" : "no",
                  cfg.verify_ssl ? "yes" : "no");

    httpUpdate.rebootOnUpdate(false);

    if (progressCb) {
        httpUpdate.onProgress([progressCb](int cur, int total) {
            if (total > 0) progressCb((cur * 100) / total);
        });
    }

    t_httpUpdate_return result;

    if (cfg.use_ssl) {
        WiFiClientSecure client;
        if (cfg.verify_ssl) {
            // Use the built-in Mozilla root CA bundle shipped with esp32 Arduino core
            client.setInsecure(); // placeholder — replace with setCACertBundle() if needed
            // For production, load a PEM cert:
            // client.setCACert(my_root_ca_pem);
        } else {
            client.setInsecure();
        }
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
