#include "wifi_provisioning.h"
#include "config.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#define CONNECT_TIMEOUT_MS  45000
#define AP_PREFIX           "SpoolHardConsole-"

void WifiProvisioning::begin(AsyncWebServer& server) {
    _server = &server;
    _loadDeviceName();
    _loadSecurityKey();
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
            _state = WifiState::Connected;
            Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
            _startMdns();
        } else if (millis() - _connectStarted > CONNECT_TIMEOUT_MS) {
            Serial.println("[WiFi] Connection timed out — starting provisioning AP");
            _state = WifiState::Failed;
            _startAP();
        }
    }
}

void WifiProvisioning::_startAP() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apName[32];
    snprintf(apName, sizeof(apName), "%s%02X%02X%02X",
             AP_PREFIX, mac[3], mac[4], mac[5]);
    _apSsid = apName;

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName);
    Serial.printf("[WiFi] AP started: '%s'  IP: %s\n",
                  apName, WiFi.softAPIP().toString().c_str());

    WiFi.scanNetworks(true);
}

void WifiProvisioning::_stopAP() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
}

void WifiProvisioning::_startConnect(const String& ssid, const String& pass) {
    Serial.printf("[WiFi] Connecting to '%s'...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    // Disable modem sleep — default WIFI_PS_MIN_MODEM causes multi-second
    // TCP stalls on long-lived connections (e.g. the WS link to the scale),
    // which show up as the scale's traffic going silent for ~10 s at a
    // time until the heartbeat times the link out.
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    _state = WifiState::Connecting;
    _connectStarted = millis();
}

void WifiProvisioning::_startMdns() {
    String hostname = _deviceName;
    hostname.toLowerCase();
    hostname.replace(' ', '-');
    if (MDNS.begin(hostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[mDNS] Announced as %s.local\n", hostname.c_str());
    } else {
        Serial.println("[mDNS] Failed to start");
    }
}

void WifiProvisioning::_loadDeviceName() {
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    _deviceName = prefs.getString(NVS_KEY_DEVICE_NAME, "SpoolHardConsole");
    prefs.end();
}

void WifiProvisioning::_loadSecurityKey() {
    Preferences prefs;
    prefs.begin(NVS_NS_WIFI, true);
    _securityKey = prefs.getString(NVS_KEY_FIXED_KEY, DEFAULT_FIXED_KEY);
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
    // The console's primary onboarding UI is on the LCD. This HTML is a
    // fallback if someone joins the AP with a phone and wants to configure
    // via a browser rather than touch screen.
    return R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SpoolHard Console — WiFi Setup</title>
<style>body{font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 16px;background:#f5f5f5}
label{display:block;margin-top:14px;font-size:.9em;color:#555}
input,select{width:100%;box-sizing:border-box;padding:8px;margin-top:4px;border:1px solid #ccc;border-radius:4px}
button{margin-top:20px;width:100%;padding:10px;background:#f0b429;color:#111;border:none;border-radius:4px;font-weight:600}
#status{margin-top:14px;font-size:.9em;color:#388E3C}</style></head><body>
<h1>SpoolHard Console — WiFi Setup</h1>
<form id="form">
<label>Network<select id="ssid-select" onchange="document.getElementById('ssid').value=this.value"><option value="">Loading networks...</option></select></label>
<label>SSID<input type="text" id="ssid" name="ssid" required></label>
<label>Password<input type="password" id="pass" name="pass"></label>
<label>Device name<input type="text" id="name" name="name" placeholder="SpoolHardConsole"></label>
<button type="submit">Save &amp; Connect</button></form>
<p id="status"></p>
<script>
function loadNetworks(){fetch('/captive/api/wifi-scan').then(r=>r.json()).then(nets=>{
const sel=document.getElementById('ssid-select');
sel.innerHTML='<option value="">-- Select network --</option>';
nets.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.secure?' 🔒':'');sel.appendChild(o)})})}
document.getElementById('form').addEventListener('submit',e=>{e.preventDefault();
const body=JSON.stringify({ssid:document.getElementById('ssid').value,pass:document.getElementById('pass').value,name:document.getElementById('name').value});
document.getElementById('status').textContent='Saving...';
fetch('/captive/api/wifi-config',{method:'POST',headers:{'Content-Type':'application/json'},body}).then(r=>r.json()).then(j=>{document.getElementById('status').textContent=j.ok?'Saved! The device will connect.':'Error: '+j.error}).catch(()=>document.getElementById('status').textContent='Saved — device is connecting.')});
loadNetworks();
</script></body></html>
)html";
}

void WifiProvisioning::_setupCaptiveRoutes() {
    auto redirect = [](AsyncWebServerRequest* req) {
        req->redirect("http://" + WiFi.softAPIP().toString() + "/");
    };
    _server->on("/hotspot-detect.html", HTTP_GET, redirect);
    _server->on("/generate_204",        HTTP_GET, redirect);
    _server->on("/connecttest.txt",     HTTP_GET, redirect);
    _server->on("/ncsi.txt",            HTTP_GET, redirect);
    _server->on("/redirect",            HTTP_GET, redirect);
    _server->on("/canonical.html",      HTTP_GET, redirect);
    _server->on("/success.txt",         HTTP_GET, redirect);

    // IMPORTANT: /captive/api/* routes must be registered BEFORE any broader
    // /captive handler. AsyncWebServer's AsyncCallbackWebHandler matches with
    // a trailing-slash prefix rule ("/captive".startsWith + "/"), so a bare
    // /captive handler would swallow /captive/api/wifi-scan. We skip the bare
    // handler entirely — the React SPA is the onboarding UI.

    _server->on("/captive/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        int status = WiFi.scanComplete();
        Serial.printf("[WiFi] scan request, scanComplete=%d mode=%d\n", status, (int)WiFi.getMode());
        if (status > 0) {
            String json = _buildScanJson();
            WiFi.scanDelete();
            Serial.printf("[WiFi] returning %d networks\n", status);
            req->send(200, "application/json", json);
        } else {
            if (status != WIFI_SCAN_RUNNING) {
                int16_t n = WiFi.scanNetworks(true, true);  // async, show hidden
                Serial.printf("[WiFi] scanNetworks(async)=%d\n", n);
            }
            req->send(200, "application/json", "[]");
        }
    });

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
            delay(300);
            _startConnect(ssid, pass);
        }
    );
}
