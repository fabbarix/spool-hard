#include "spoolhard/auth.h"
#include "spoolhard/psram_json_alloc.h"
#include "spoolhard/ring_log.h"
#include <Preferences.h>
#include <ArduinoJson.h>

namespace SpoolhardAuth {

bool requireAuth(AsyncWebServerRequest* req) {
    Preferences prefs;
    prefs.begin(kNvsNsWifi, true);
    String stored = prefs.getString(kNvsKeyFixedKey, "");
    prefs.end();

    // No key set, or still the ship-default placeholder → auth is off.
    if (stored.isEmpty() || stored == kDefaultFixedKey) return true;

    String auth = req->header("Authorization");
    if (auth.startsWith("Bearer ") && auth.substring(7) == stored) return true;

    // Allow ?key= fallback for multipart uploads and WebSocket handshake.
    if (req->hasParam("key") && req->getParam("key")->value() == stored) return true;

    req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return false;
}

bool wsAuthHandshake(AsyncWebServerRequest* req) {
    Preferences prefs;
    prefs.begin(kNvsNsWifi, true);
    String stored = prefs.getString(kNvsKeyFixedKey, "");
    prefs.end();
    if (stored.isEmpty() || stored == kDefaultFixedKey) return true;
    if (req->hasParam("key") && req->getParam("key")->value() == stored) return true;
    return false;
}

void handleAuthStatus(AsyncWebServerRequest* req,
                      const char* defaultDeviceName,
                      const char* productSlug) {
    Preferences prefs;
    prefs.begin(kNvsNsWifi, true);
    String stored = prefs.getString(kNvsKeyFixedKey, "");
    String device = prefs.getString(kNvsKeyDeviceName, defaultDeviceName);
    prefs.end();

    bool required = !stored.isEmpty() && stored != kDefaultFixedKey;
    bool authed   = !required;
    if (required) {
        String auth = req->header("Authorization");
        if (auth.startsWith("Bearer ") && auth.substring(7) == stored) authed = true;
    }
    JsonDocument doc(&g_psramJsonAlloc);
    doc["auth_required"] = required;
    doc["authenticated"] = authed;
    doc["device_name"]   = device;
    doc["product"]       = productSlug;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void handleDeviceNameGet(AsyncWebServerRequest* req,
                         const char* defaultDeviceName) {
    if (!requireAuth(req)) return;
    Preferences prefs;
    prefs.begin(kNvsNsWifi, true);
    String name = prefs.getString(kNvsKeyDeviceName, defaultDeviceName);
    prefs.end();

    JsonDocument doc(&g_psramJsonAlloc);
    doc["device_name"] = name;
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
}

void handleDeviceNamePost(AsyncWebServerRequest* req,
                          uint8_t* data, size_t len) {
    JsonDocument doc(&g_psramJsonAlloc);
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return;
    }
    String name = doc["device_name"] | "";
    if (name.isEmpty()) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"name required\"}");
        return;
    }
    Preferences prefs;
    prefs.begin(kNvsNsWifi, false);
    prefs.putString(kNvsKeyDeviceName, name);
    prefs.end();
    dlog("Config", "Device name changed to '%s'", name.c_str());
    req->send(200, "application/json", "{\"ok\":true}");
}

void handleTestKey(AsyncWebServerRequest* req) {
    if (!requireAuth(req)) return;
    Preferences prefs;
    prefs.begin(kNvsNsWifi, true);
    bool   configured = prefs.isKey(kNvsKeyFixedKey);
    String key        = prefs.getString(kNvsKeyFixedKey, kDefaultFixedKey);
    prefs.end();

    String masked = key;
    if (key.length() > 4)
        masked = key.substring(0, 2) + "***" + key.substring(key.length() - 2);

    JsonDocument doc(&g_psramJsonAlloc);
    doc["configured"]  = configured;
    doc["key_preview"] = masked;
    String resp;
    serializeJson(doc, resp);
    req->send(200, "application/json", resp);
}

void handleFixedKeyConfigPost(AsyncWebServerRequest* req,
                              uint8_t* data, size_t len) {
    JsonDocument doc(&g_psramJsonAlloc);
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return;
    }
    String key = doc["key"] | "";
    if (key.isEmpty()) {
        req->send(400, "application/json", "{\"ok\":false,\"error\":\"key required\"}");
        return;
    }
    Preferences prefs;
    prefs.begin(kNvsNsWifi, false);
    prefs.putString(kNvsKeyFixedKey, key);
    prefs.end();
    dlog("Security", "Fixed key updated");
    req->send(200, "application/json", "{\"ok\":true}");
}

}  // namespace SpoolhardAuth
