#include "spoolhard/auth.h"
#include "spoolhard/psram_json_alloc.h"
#include "spoolhard/ring_log.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <mutex>

namespace SpoolhardAuth {

// -- Fixed-key RAM cache --------------------------------------------
// requireAuth() runs on every HTTP request; opening NVS each time is a
// flash-backed read on the AsyncTCP task. The key is loaded lazily and
// kept in RAM; every writer of wifi_cfg/fixed_key must call
// invalidateKeyCache() (handleFixedKeyConfigPost does; the backup-restore
// handlers do after RestoreSession::finish). `s_required` is false when
// the key is empty or still the ship-default placeholder.
static std::mutex s_keyMu;
static String     s_key;
static bool       s_required = false;
static bool       s_loaded   = false;

static void loadKeyLocked() {
    Preferences prefs;
    prefs.begin(kNvsNsWifi, true);
    s_key = prefs.getString(kNvsKeyFixedKey, "");
    prefs.end();
    s_required = !s_key.isEmpty() && s_key != kDefaultFixedKey;
    s_loaded = true;
}

void invalidateKeyCache() {
    std::lock_guard<std::mutex> g(s_keyMu);
    s_loaded = false;
    s_key = String();
    s_required = false;
}

static bool keyRequired() {
    std::lock_guard<std::mutex> g(s_keyMu);
    if (!s_loaded) loadKeyLocked();
    return s_required;
}

// Constant-time compare against the cached key: runtime depends only on the
// stored key's length, not on where the candidate first differs.
static bool keyMatches(const String& cand) {
    std::lock_guard<std::mutex> g(s_keyMu);
    if (!s_loaded) loadKeyLocked();
    if (!s_required) return false;
    const size_t m = s_key.length(), n = cand.length();
    uint8_t diff = (m != n) ? 1 : 0;
    for (size_t i = 0; i < m; i++) {
        diff |= (uint8_t)s_key[i] ^ (uint8_t)(i < n ? cand[i] : 0);
    }
    return diff == 0;
}

static bool bearerMatches(AsyncWebServerRequest* req) {
    String auth = req->header("Authorization");
    return auth.startsWith("Bearer ") && keyMatches(auth.substring(7));
}

bool requireAuth(AsyncWebServerRequest* req) {
    // No key set, or still the ship-default placeholder -> auth is off.
    if (!keyRequired()) return true;

    if (bearerMatches(req)) return true;

    // Allow ?key= fallback for multipart uploads and WebSocket handshake.
    if (req->hasParam("key") && keyMatches(req->getParam("key")->value())) return true;

    req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return false;
}

bool wsAuthHandshake(AsyncWebServerRequest* req) {
    if (!keyRequired()) return true;
    if (req->hasParam("key") && keyMatches(req->getParam("key")->value())) return true;
    return false;
}

void handleAuthStatus(AsyncWebServerRequest* req,
                      const char* defaultDeviceName,
                      const char* productSlug) {
    Preferences prefs;
    prefs.begin(kNvsNsWifi, true);
    String device = prefs.getString(kNvsKeyDeviceName, defaultDeviceName);
    prefs.end();

    bool required = keyRequired();
    bool authed   = !required || bearerMatches(req);
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
    invalidateKeyCache();
    dlog("Security", "Fixed key updated");
    req->send(200, "application/json", "{\"ok\":true}");
}

}  // namespace SpoolhardAuth
