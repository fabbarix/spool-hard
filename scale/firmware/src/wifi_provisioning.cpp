#include "wifi_provisioning.h"
#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "ssdp_notify.h"

#define CONNECT_TIMEOUT_MS  15000
#define AP_PREFIX           "SpoolHardScale-"

// ── Public ───────────────────────────────────────────────────

void WifiProvisioning::begin(AsyncWebServer& server) {
    _server = &server;
    _loadDeviceName();
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
            _state          = WifiState::Connected;
            _everConnected  = true;
            Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
            _startDiscovery();
        } else if (!_everConnected && millis() - _connectStarted > CONNECT_TIMEOUT_MS) {
            // Only fall back to provisioning on an INITIAL connect failure —
            // a typo'd password needs a path for the user to fix it. A drop
            // after a successful association is a different beast: the AP
            // might just be rebooting and the driver-level auto-reconnect
            // will usually recover within a minute. Dumping the scale into
            // provisioning mode every time the AP hiccups is worse than
            // waiting.
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
            return;
        }
        _ssdp.loop();
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
    Serial.printf("[WiFi] Connecting to '%s'...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    // Make the driver-level reconnect explicit. Arduino-ESP32's default has
    // drifted across versions; setting it here guarantees the radio will try
    // to re-associate on its own after a link drop, which is what our
    // application-layer detection in update() relies on.
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    _state = WifiState::Connecting;
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

void WifiProvisioning::_saveCredentials(const String& ssid, const String& pass, const String& name) {
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, false);
    prefs.putString(NVS_KEY_SSID, ssid);
    prefs.putString(NVS_KEY_PASS, pass);
    if (!name.isEmpty()) {
        prefs.putString(NVS_KEY_DEVICE_NAME, name);
        _deviceName = name;
    }
    prefs.end();
}

String WifiProvisioning::_buildScanJson() {
    int n = WiFi.scanComplete();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            JsonObject net = arr.add<JsonObject>();
            net["ssid"]   = WiFi.SSID(i);
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
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
                return;
            }
            String ssid = doc["ssid"] | "";
            String pass = doc["pass"] | "";
            String name = doc["name"] | "";
            if (ssid.isEmpty()) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"ssid required\"}");
                return;
            }
            _saveCredentials(ssid, pass, name);
            req->send(200, "application/json", "{\"ok\":true}");
            // Connect after a short delay so the HTTP response can be sent first
            delay(300);
            _startConnect(ssid, pass);
        }
    );

    // Device name config (used by existing /api/device-name-config too)
    _server->on("/captive/api/device-name-config", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["device_name"] = _deviceName;
        String resp;
        serializeJson(doc, resp);
        req->send(200, "application/json", resp);
    });
}
