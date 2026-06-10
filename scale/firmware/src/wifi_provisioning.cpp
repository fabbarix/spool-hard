#include "wifi_provisioning.h"
#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include "spoolhard/psram_json_alloc.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include "ssdp_notify.h"
#include "spoolhard/serial_mirror.h"

#define CONNECT_TIMEOUT_MS  15000
#define AP_PREFIX           "SpoolHardScale-"

// ── Public ───────────────────────────────────────────────────

// Map a WIFI_REASON_* code to a short tag for the ring log. Diagnostic
// only — full numeric value is also printed for cross-referencing the
// ESP-IDF wifi_err_reason_t enum. Most-relevant for pinning:
//   201 NO_AP_FOUND        — BSSID not seen during scan
//   202 AUTH_FAIL          — password wrong (or AP rejected creds)
//   203 ASSOC_FAIL         — AP refused association (band-steering!)
//   204 HANDSHAKE_TIMEOUT  — WPA 4-way handshake timed out
//     2 AUTH_EXPIRE        — auth timed out
//     4 ASSOC_EXPIRE       — assoc timed out
//   200 BEACON_TIMEOUT     — was associated, lost beacons
//     8 ASSOC_LEAVE        — we initiated disconnect (e.g. WiFi.disconnect())
static const char* _wifiReasonTag(uint8_t r) {
    switch (r) {
        case 2:   return "AUTH_EXPIRE";
        case 4:   return "ASSOC_EXPIRE";
        case 8:   return "ASSOC_LEAVE";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL";
        case 203: return "ASSOC_FAIL";   // band-steering rejection lands here
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        default:  return "OTHER";
    }
}

void WifiProvisioning::begin(AsyncWebServer& server) {
    _server = &server;
    _loadDeviceName();
    _loadPinnedBssid();

    // Register WiFi event handler BEFORE any WiFi.begin so we capture
    // the connect+disconnect timeline. Helps diagnose pinned-BSSID
    // refusals: if the AP refuses our admission to a specific BSSID
    // (band-steering), the driver fires WIFI_EVENT_STA_DISCONNECTED
    // with reason=ASSOC_FAIL and we log the BSSID + reason. Without
    // this you only see "fallback active" in the UI with no clue why.
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_CONNECTED: {
                char bssid[18];
                snprintf(bssid, sizeof(bssid),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         info.wifi_sta_connected.bssid[0],
                         info.wifi_sta_connected.bssid[1],
                         info.wifi_sta_connected.bssid[2],
                         info.wifi_sta_connected.bssid[3],
                         info.wifi_sta_connected.bssid[4],
                         info.wifi_sta_connected.bssid[5]);
                Serial.printf("[WiFi-ev] STA_CONNECTED bssid=%s ch=%u\n",
                              bssid, (unsigned)info.wifi_sta_connected.channel);
                break;
            }
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
                char bssid[18];
                snprintf(bssid, sizeof(bssid),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         info.wifi_sta_disconnected.bssid[0],
                         info.wifi_sta_disconnected.bssid[1],
                         info.wifi_sta_disconnected.bssid[2],
                         info.wifi_sta_disconnected.bssid[3],
                         info.wifi_sta_disconnected.bssid[4],
                         info.wifi_sta_disconnected.bssid[5]);
                Serial.printf("[WiFi-ev] STA_DISCONNECTED bssid=%s reason=%u(%s)\n",
                              bssid,
                              (unsigned)info.wifi_sta_disconnected.reason,
                              _wifiReasonTag(info.wifi_sta_disconnected.reason));
                break;
            }
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                Serial.printf("[WiFi-ev] GOT_IP ip=%s\n",
                              IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
                break;
            default:
                break;
        }
    });

    _setupCaptiveRoutes();

    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    String ssid = prefs.getString(NVS_KEY_SSID, "");
    String pass = prefs.getString(NVS_KEY_PASS, "");
    prefs.end();

    if (ssid.isEmpty()) {
        Serial.println("[WiFi] No credentials — starting provisioning AP");
        _state = WifiState::Unconfigured;
        _startAP();
    } else {
        _startConnect(ssid, pass);
    }
}

void WifiProvisioning::update() {
    if (_state == WifiState::Connecting) {
        if (WiFi.status() == WL_CONNECTED) {
            _stopAP();
            _state           = WifiState::Connected;
            _everConnected   = true;
            _linkDownSinceMs = 0;
            String actualBssid = WiFi.BSSIDstr();
            Serial.printf("[WiFi] Connected: %s (BSSID %s, ch %d, %d dBm)\n",
                          WiFi.localIP().toString().c_str(),
                          actualBssid.c_str(),
                          WiFi.channel(),
                          (int)WiFi.RSSI());
            // If we asked for a specific BSSID but the driver gave us
            // a different one, the AP refused our pinned-BSSID request
            // (band-steering, load balancing, etc.) and arduino-esp32
            // fell back to plain auto-select internally — well before
            // our 60 s app-side fallback would have fired. Clear the
            // RAM flag so we don't re-attempt the unreachable pin on
            // the next reconnect; the NVS pin is preserved so a reboot
            // re-tries (in case the AP changed policy). The wifi-status
            // mismatch the UI reads ("pin set to X (fallback active)")
            // remains accurate either way.
            if (_pinnedActive && !_pinnedBssid.isEmpty() &&
                !actualBssid.equalsIgnoreCase(_pinnedBssid)) {
                Serial.printf("[WiFi] Pin %s rejected by AP — driver "
                              "auto-selected %s instead. AP is "
                              "band-steering or refusing admission to "
                              "the pinned node.\n",
                              _pinnedBssid.c_str(), actualBssid.c_str());
                _pinnedActive = false;
            }
            _startDiscovery();
        } else if (_pinnedActive && millis() - _connectStarted > PINNED_FALLBACK_MS) {
            // Pinned BSSID didn't come up within 60 s — most likely the
            // mesh node is offline. Drop the pin in RAM (NVS preserves
            // user intent) and re-begin() against the SSID generically
            // so the driver picks the best available node. If the
            // pinned node returns later, the next reboot picks it up
            // again.
            Serial.printf("[WiFi] Pinned BSSID %s not reachable in %lus — "
                          "falling back to plain SSID for this session\n",
                          _pinnedBssid.c_str(),
                          PINNED_FALLBACK_MS / 1000UL);
            _pinnedActive = false;
            Preferences prefs;
            prefs.begin(NVS_NS_WIFI, true);
            String ssid = prefs.getString(NVS_KEY_SSID, "");
            String pass = prefs.getString(NVS_KEY_PASS, "");
            prefs.end();
            if (!ssid.isEmpty()) {
                WiFi.disconnect();
                delay(50);
                WiFi.begin(ssid.c_str(), pass.c_str());
                _connectStarted = millis();
            }
        } else if (!_everConnected && !_pinnedActive &&
                   millis() - _connectStarted > CONNECT_TIMEOUT_MS) {
            // Only fall back to provisioning on an INITIAL connect failure
            // *with no pin*. The pinned-fallback above gives the user-set
            // BSSID 60 s before reverting to plain SSID; the
            // CONNECT_TIMEOUT_MS path here is the original "typo'd
            // password" recovery. Dumping into provisioning mode while
            // the pinned-fallback is still ticking would short-circuit
            // it and corrupt the user's intent.
            Serial.println("[WiFi] Initial connect timed out — starting provisioning AP");
            _state = WifiState::Failed;
            _startAP();
        }
        return;
    }
    if (_state == WifiState::Connected) {
        // Detect a dropped STA link. Previously there was no transition out
        // of Connected, so if the AP glitched or we lost the lease, SSDP's
        // internal `WiFi.status() != WL_CONNECTED` guard silently stopped
        // broadcasting, mDNS stayed bound to a stale IP, and the scale was
        // "alive" (load cell + LED still running in main loop) but invisible
        // to the console. Push back to Connecting so the ESP32's driver-level
        // auto-reconnect has time to re-associate AND we re-run _startDiscovery
        // when the link comes back — mDNS doesn't survive an IP change on its
        // own.
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Link dropped — tearing down mDNS, awaiting auto-reconnect");
            MDNS.end();
            _state          = WifiState::Connecting;
            _connectStarted = millis();
            _lastReconnectKickMs = millis();   // grace period before first kick
            _linkDownSinceMs     = millis();
            return;
        }
        _ssdp.loop();
    }

    // Post-drop reconnect kicker. arduino-esp32's driver-level
    // setAutoReconnect(true) usually recovers on its own within a
    // minute, but gets stuck in some scenarios (after the radio briefly
    // entered AP_STA mode, or when the AP comes back on a different
    // channel). Without an explicit kick the scale would sit in
    // Connecting forever — visible only as a steady red LED. Every
    // ~30 s in this state we explicitly disconnect/reconnect; the
    // initial-failure timeout above (CONNECT_TIMEOUT_MS) is gated on
    // `!_everConnected` so it doesn't fire here.
    if (_state == WifiState::Connecting && _everConnected) {
        if (millis() - _lastReconnectKickMs >= 30000) {
            _lastReconnectKickMs = millis();
            Preferences prefs;
            prefs.begin(NVS_NS_WIFI, true);
            String ssid = prefs.getString(NVS_KEY_SSID, "");
            String pass = prefs.getString(NVS_KEY_PASS, "");
            prefs.end();
            if (!ssid.isEmpty()) {
                Serial.println("[WiFi] Driver auto-reconnect appears stuck — explicit re-begin()");
                WiFi.disconnect();
                delay(50);
                WiFi.begin(ssid.c_str(), pass.c_str());
            }
        }
        // Down-too-long failsafe: if neither the driver nor the kicker
        // recovered the link in 10 min, the WiFi stack is in a state we
        // can't fix from here — reboot. A clean SW reset (visible as
        // reset=sw in the next boot's log) reconnects in seconds;
        // staying down requires a human with a power plug.
        if (_linkDownSinceMs &&
            millis() - _linkDownSinceMs > 10UL * 60UL * 1000UL) {
            Serial.println("[WiFi] Link down >10 min despite reconnect kicks — restarting");
            delay(100);
            ESP.restart();
        }
    }
}

// ── Private ──────────────────────────────────────────────────

void WifiProvisioning::_startAP() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apName[32];
    snprintf(apName, sizeof(apName), "%s%02X%02X%02X",
             AP_PREFIX, mac[3], mac[4], mac[5]);

    // Stop STA retries first — they hog the radio and block scanning
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName);
    Serial.printf("[WiFi] AP started: '%s'  IP: %s\n",
                  apName, WiFi.softAPIP().toString().c_str());

    // Kick off a scan so results are ready when the portal loads
    WiFi.scanNetworks(true);
}

void WifiProvisioning::_stopAP() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
}

void WifiProvisioning::_startConnect(const String& ssid, const String& pass) {
    WiFi.mode(WIFI_STA);
    // Disable modem sleep — default WIFI_PS_MIN_MODEM lets the radio
    // doze between DTIM beacons, adding 100-300 ms RX latency to every
    // packet. See earlier diagnosis in MEMORY for the full rationale.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

    // Branch on the user's BSSID pin. WiFi.begin's 5-arg form takes
    // (ssid, pass, channel, bssid, connect). Channel=0 lets the driver
    // probe; specifying a BSSID + channel=0 means "find this AP on
    // whatever channel it advertises". If we can parse the stored
    // string into 6 bytes we lock to that node; otherwise (typo,
    // empty) we fall through to the plain begin() form.
    uint8_t bssid_bytes[6];
    if (!_pinnedBssid.isEmpty() && _parseBssid(_pinnedBssid, bssid_bytes)) {
        Serial.printf("[WiFi] Connecting to '%s' pinned to BSSID %s\n",
                      ssid.c_str(), _pinnedBssid.c_str());
        WiFi.begin(ssid.c_str(), pass.c_str(), 0, bssid_bytes, true);
        _pinnedActive = true;
    } else {
        Serial.printf("[WiFi] Connecting to '%s' (no pin)\n", ssid.c_str());
        WiFi.begin(ssid.c_str(), pass.c_str());
        _pinnedActive = false;
    }
    _state          = WifiState::Connecting;
    _connectStarted = millis();
}

void WifiProvisioning::_startDiscovery() {
    // Build mDNS-safe hostname: lowercase, spaces→hyphens
    String hostname = _deviceName;
    hostname.toLowerCase();
    hostname.replace(' ', '-');

    if (MDNS.begin(hostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[mDNS] Announced as %s.local\n", hostname.c_str());
    } else {
        Serial.println("[mDNS] Failed to start");
    }

    // SpoolHard "SSDP" — not real UPnP. The console listens passively on UDP
    // 239.255.255.250:1990 for NOTIFY packets and parses three fields:
    //   NT:  must contain "urn:spoolhard-io:device:spoolscale"
    //   Location: a bare IPv4 address (no URL, no port, no path)
    //   USN: must equal the user-configured scale name in the console
    // See yanshay/SpoolEase:core/src/{ssdp,spool_scale}.rs.
    _ssdp.begin(_deviceName, /*intervalMs*/ 5000);
    Serial.println("[SSDP] NOTIFY broadcaster started");
}

void WifiProvisioning::_loadDeviceName() {
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    _deviceName = prefs.getString(NVS_KEY_DEVICE_NAME, "SpoolHardScale");
    prefs.end();
}

void WifiProvisioning::_loadPinnedBssid() {
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    _pinnedBssid = prefs.getString(NVS_KEY_PINNED_BSSID, "");
    prefs.end();
    if (!_pinnedBssid.isEmpty()) {
        Serial.printf("[WiFi] Pinned BSSID configured: %s\n", _pinnedBssid.c_str());
    }
}

void WifiProvisioning::_saveCredentials(const String& ssid, const String& pass,
                                        const String& name,
                                        const String* pinnedBssid) {
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, false);
    prefs.putString(NVS_KEY_SSID, ssid);
    prefs.putString(NVS_KEY_PASS, pass);
    if (!name.isEmpty()) {
        prefs.putString(NVS_KEY_DEVICE_NAME, name);
        _deviceName = name;
    }
    // pinned_bssid update is optional — null pointer leaves the existing
    // NVS value alone. Empty string clears the pin. Any other value is
    // stored verbatim and parsed at next connect.
    if (pinnedBssid) {
        prefs.putString(NVS_KEY_PINNED_BSSID, *pinnedBssid);
        _pinnedBssid = *pinnedBssid;
    }
    prefs.end();
}

bool WifiProvisioning::_parseBssid(const String& s, uint8_t out[6]) {
    if (s.length() != 17) return false;   // "AA:BB:CC:DD:EE:FF"
    // Colons at positions 2, 5, 8, 11, 14 — that's `i*3 + 2` for byte i
    // in 0..4. The previous formula `i*3 + 1` was off by one and matched
    // the second hex digit instead, so every parse failed silently and
    // _pinnedActive never became true. Confirmed via WiFi event log:
    // "Connecting to 'Porcini' (no pin)" despite a stored pinned_bssid.
    for (int i = 0; i < 6; ++i) {
        if (i < 5 && s.charAt(i*3 + 2) != ':') return false;
        char hi = s.charAt(i*3);
        char lo = s.charAt(i*3 + 1);
        auto val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return -1;
        };
        int h = val(hi), l = val(lo);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

String WifiProvisioning::_buildScanJson() {
    int n = WiFi.scanComplete();
    JsonDocument doc(&g_psramJsonAlloc);
    JsonArray arr = doc.to<JsonArray>();
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            JsonObject net = arr.add<JsonObject>();
            net["ssid"]   = WiFi.SSID(i);
            net["bssid"]  = WiFi.BSSIDstr(i);
            net["channel"] = WiFi.channel(i);
            net["rssi"]   = WiFi.RSSI(i);
            net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
    }
    String out;
    serializeJson(doc, out);
    return out;
}

String WifiProvisioning::_buildPortalHtml() {
    return R"html(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SpoolHardScale — WiFi Setup</title>
<style>
  body { font-family: sans-serif; max-width: 420px; margin: 40px auto; padding: 0 16px; background: #f5f5f5; }
  h1 { font-size: 1.3em; color: #333; }
  label { display: block; margin-top: 14px; font-size: .9em; color: #555; }
  input, select { width: 100%; box-sizing: border-box; padding: 8px; margin-top: 4px;
                  border: 1px solid #ccc; border-radius: 4px; font-size: 1em; }
  button { margin-top: 20px; width: 100%; padding: 10px; background: #2196F3;
           color: #fff; border: none; border-radius: 4px; font-size: 1em; cursor: pointer; }
  button:hover { background: #1976D2; }
  #status { margin-top: 14px; font-size: .9em; color: #388E3C; }
  #scan-btn { background: #757575; margin-top: 8px; }
</style>
</head>
<body>
<h1>SpoolHardScale WiFi Setup</h1>
<form id="form">
  <label>Network
    <select id="ssid-select" onchange="document.getElementById('ssid').value=this.value">
      <option value="">Loading networks...</option>
    </select>
  </label>
  <label>SSID (or type manually)
    <input type="text" id="ssid" name="ssid" required placeholder="Network name">
  </label>
  <label>Password
    <input type="password" id="pass" name="pass" placeholder="Leave blank for open networks">
  </label>
  <label>Device name
    <input type="text" id="name" name="name" placeholder="SpoolHardScale">
  </label>
  <button type="submit">Save &amp; Connect</button>
  <button type="button" id="scan-btn" onclick="loadNetworks()">Scan again</button>
</form>
<p id="status"></p>
<script>
function loadNetworks() {
  fetch('/captive/api/wifi-scan')
    .then(r => r.json())
    .then(nets => {
      const sel = document.getElementById('ssid-select');
      sel.innerHTML = '<option value="">-- Select network --</option>';
      nets.sort((a,b) => b.rssi - a.rssi)
          .forEach(n => {
            const opt = document.createElement('option');
            opt.value = n.ssid;
            opt.textContent = n.ssid + '  (' + n.rssi + ' dBm)' + (n.secure ? ' 🔒' : '');
            sel.appendChild(opt);
          });
    });
}
document.getElementById('form').addEventListener('submit', e => {
  e.preventDefault();
  const body = JSON.stringify({
    ssid: document.getElementById('ssid').value,
    pass: document.getElementById('pass').value,
    name: document.getElementById('name').value,
  });
  document.getElementById('status').textContent = 'Saving...';
  fetch('/captive/api/wifi-config', { method:'POST', headers:{'Content-Type':'application/json'}, body })
    .then(r => r.json())
    .then(j => {
      document.getElementById('status').textContent =
        j.ok ? 'Saved! The device will now connect. You can close this page.' : ('Error: ' + j.error);
    })
    .catch(() => {
      document.getElementById('status').textContent = 'Saved — device is connecting (connection to this AP will drop).';
    });
});
loadNetworks();
</script>
</body>
</html>
)html";
}

void WifiProvisioning::_setupCaptiveRoutes() {
    // Captive portal detection redirects (iOS, Android, Windows) → main page
    auto redirect = [](AsyncWebServerRequest* req) {
        req->redirect("http://" + WiFi.softAPIP().toString() + "/");
    };
    _server->on("/hotspot-detect.html",     HTTP_GET, redirect);
    _server->on("/generate_204",            HTTP_GET, redirect);
    _server->on("/connecttest.txt",         HTTP_GET, redirect);
    _server->on("/ncsi.txt",               HTTP_GET, redirect);
    _server->on("/redirect",               HTTP_GET, redirect);
    _server->on("/canonical.html",         HTTP_GET, redirect);
    _server->on("/success.txt",            HTTP_GET, redirect);

    // WiFi scan — returns cached results if available, otherwise triggers a scan.
    _server->on("/captive/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        int status = WiFi.scanComplete();
        Serial.printf("[WiFi] Scan requested, scanComplete=%d\n", status);
        if (status > 0) {
            // Results ready — return them, then delete so next call triggers a fresh scan
            String json = _buildScanJson();
            WiFi.scanDelete();
            Serial.printf("[WiFi] Returning %d networks\n", status);
            req->send(200, "application/json", json);
        } else {
            // No results: either never scanned (-1), scan running (-2), or 0 found
            if (status != WIFI_SCAN_RUNNING) {
                WiFi.scanNetworks(true);
                Serial.println("[WiFi] Async scan started");
            }
            req->send(200, "application/json", "[]");
        }
    });

    // Save credentials
    _server->on("/captive/api/wifi-config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&g_psramJsonAlloc);
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
                return;
            }
            String ssid = doc["ssid"] | "";
            String name = doc["name"] | "";
            if (ssid.isEmpty()) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"ssid required\"}");
                return;
            }
            // Password handling: if the JSON has no `pass` key, preserve
            // the stored value. If it has the key with an empty string,
            // treat as "user wants to set an empty password" (open
            // network) — explicit clear. Three-state same as pinned_bssid.
            // This avoids the "test POST without pass clobbers credentials"
            // footgun.
            String pass;
            bool passSpecified = doc["pass"].is<const char*>();
            if (passSpecified) {
                pass = doc["pass"].as<const char*>();
            } else {
                Preferences prefs;
                prefs.begin(NVS_NS_WIFI, true);
                pass = prefs.getString(NVS_KEY_PASS, "");
                prefs.end();
            }
            // pinned_bssid is optional. Distinct cases:
            //   (a) field absent in payload   → leave NVS unchanged (pass nullptr).
            //   (b) field present, empty      → clear the pin (pass empty String*).
            //   (c) field present, non-empty  → store as-is (must be 17-char MAC).
            const String* pinnedPtr = nullptr;
            String pinnedVal;
            if (doc["pinned_bssid"].is<const char*>()) {
                pinnedVal = doc["pinned_bssid"].as<const char*>();
                pinnedPtr = &pinnedVal;
            }
            // Only write the password to NVS if the caller actually
            // sent a `pass` field. Same protection as pinned_bssid.
            if (passSpecified) {
                _saveCredentials(ssid, pass, name, pinnedPtr);
            } else {
                // ssid+name+pin path: don't churn the password.
                Preferences prefs;
                prefs.begin(NVS_NS_WIFI, false);
                prefs.putString(NVS_KEY_SSID, ssid);
                if (!name.isEmpty()) {
                    prefs.putString(NVS_KEY_DEVICE_NAME, name);
                    _deviceName = name;
                }
                if (pinnedPtr) {
                    prefs.putString(NVS_KEY_PINNED_BSSID, *pinnedPtr);
                    _pinnedBssid = *pinnedPtr;
                }
                prefs.end();
            }
            req->send(200, "application/json", "{\"ok\":true}");
            // Connect after a short delay so the HTTP response can be sent first
            delay(300);
            _startConnect(ssid, pass);
        }
    );

    // Device name config (used by existing /api/device-name-config too)
    _server->on("/captive/api/device-name-config", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc(&g_psramJsonAlloc);
        doc["device_name"] = _deviceName;
        String resp;
        serializeJson(doc, resp);
        req->send(200, "application/json", resp);
    });
}
