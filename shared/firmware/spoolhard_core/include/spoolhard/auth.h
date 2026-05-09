#pragma once
#include <ESPAsyncWebServer.h>

// Authentication helpers shared by console and scale. Both products use
// the SAME NVS namespace/keys for the fixed-key gate and device name —
// this header unifies them so the auth surface is byte-identical.
//
// NVS schema:
//   namespace: "wifi_cfg"
//   key       "fixed_key"      → user-set Bearer key. Empty / "Change-Me!" = auth off.
//   key       "device_name"    → mDNS hostname + UI display name.
//   key       "ssid", "pass"   → STA credentials (owned by wifi_provisioning).
//
// The fixed-key gate is checked on:
//   - Every HTTP route through `requireAuth()`
//   - The WebSocket upgrade through `wsAuthHandshake()` (HTTP
//     `Authorization: Bearer` not available on WS upgrade in browsers,
//     so the WS path uses `?key=` query string instead).
namespace SpoolhardAuth {

// NVS namespace + keys. Single source of truth; both products use these.
constexpr const char* kNvsNsWifi        = "wifi_cfg";
constexpr const char* kNvsKeySsid       = "ssid";
constexpr const char* kNvsKeyPass       = "pass";
constexpr const char* kNvsKeyDeviceName = "device_name";
constexpr const char* kNvsKeyFixedKey   = "fixed_key";

// Default fixed-key shipped with fresh firmware. While this is the
// stored value, auth is treated as OFF so a freshly-flashed unit is
// reachable without a credential. The user must change it via
// /api/fixed-key-config to enable enforcement.
constexpr const char* kDefaultFixedKey  = "Change-Me!";

// Returns true if the request is allowed (no key set OR Bearer/?key=
// match). On false, sends 401 and the caller must `return;`.
bool requireAuth(AsyncWebServerRequest* req);

// WebSocket-handshake variant. AsyncWebSocket calls this BEFORE the
// upgrade is committed; returning false 401s the upgrade. Browser WS
// clients can't set Authorization headers, so this only checks the
// `?key=` query parameter.
bool wsAuthHandshake(AsyncWebServerRequest* req);

// /api/auth-status — never 401s. Reports whether a key is required and
// whether the request's Authorization header satisfies it. Frontend
// uses this both as the pre-login gate check and the verify endpoint
// on submit.
//
// `defaultDeviceName` is the fallback shown when no name has been
// stored yet (e.g. "SpoolHardConsole" / "SpoolHardScale").
// `productSlug` is "console" or "scale" — surfaced in the response so
// the frontend can route accordingly.
void handleAuthStatus(AsyncWebServerRequest* req,
                      const char* defaultDeviceName,
                      const char* productSlug);

// GET /api/device-name-config
void handleDeviceNameGet(AsyncWebServerRequest* req,
                         const char* defaultDeviceName);

// POST /api/device-name-config — body: {"device_name": "..."}
// Caller must already have run requireAuth.
void handleDeviceNamePost(AsyncWebServerRequest* req,
                          uint8_t* data, size_t len);

// GET /api/test-key — masked preview of the stored key for the
// settings UI. Auth-gated.
void handleTestKey(AsyncWebServerRequest* req);

// POST /api/fixed-key-config — body: {"key": "..."} stores the new
// fixed key. Caller must already have run requireAuth.
void handleFixedKeyConfigPost(AsyncWebServerRequest* req,
                              uint8_t* data, size_t len);

}  // namespace SpoolhardAuth
