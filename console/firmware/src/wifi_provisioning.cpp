#include "wifi_provisioning.h"
#include "config.h"
#include "crash_logger.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "spoolhard/serial_mirror.h"

#define CONNECT_TIMEOUT_MS  45000
#define AP_PREFIX           "SpoolHardConsole-"

// See scale/firmware/src/wifi_provisioning.cpp for the table of
// WIFI_REASON_* codes and what they mean for pinning diagnostics.
static const char* _wifiReasonTag(uint8_t r) {
    switch (r) {
        case 2:   return "AUTH_EXPIRE";
        case 4:   return "ASSOC_EXPIRE";
        case 8:   return "ASSOC_LEAVE";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        default:  return "OTHER";
    }
}

void WifiProvisioning::begin(AsyncWebServer& server) {
    _server = &server;
    _loadDeviceName();
    _loadSecurityKey();
    _loadPinnedBssid();

    // WiFi event logger — captures every CONNECTED / DISCONNECTED with
    // BSSID + reason code so /api/logs surfaces what the driver and AP
    // are actually negotiating. Critical for diagnosing pinned-BSSID
    // refusals via mesh band-steering (reason=ASSOC_FAIL).
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
            // Driver-side fallback detection — see scale's mirror.
            if (_pinnedActive && !_pinnedBssid.isEmpty() &&
                !actualBssid.equalsIgnoreCase(_pinnedBssid)) {
                Serial.printf("[WiFi] Pin %s rejected by AP — driver "
                              "auto-selected %s instead. AP is "
                              "band-steering or refusing admission to "
                              "the pinned node.\n",
                              _pinnedBssid.c_str(), actualBssid.c_str());
                _pinnedActive = false;
            }
            _startMdns();
        } else if (_pinnedActive && millis() - _connectStarted > PINNED_FALLBACK_MS) {
            // Pinned BSSID didn't come up in 60 s — most likely the
            // mesh node is offline. Drop pin in RAM (NVS preserves
            // user intent) and re-begin() against plain SSID.
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
            // Plain-SSID INITIAL connect timed out — back to provisioning.
            // Gated on !_everConnected (like the scale): after a mid-session
            // drop we must keep retrying the known-good credentials, not
            // dump a working install into provisioning mode.
            Serial.println("[WiFi] Initial connect timed out — starting provisioning AP");
            _state = WifiState::Failed;
            _startAP();
        }
        // fall through to the post-drop kicker below
    }

    if (_state == WifiState::Connected) {
        // Detect a dropped STA link — ported from the scale. The console
        // previously had NO transition out of Connected: if the AP glitched
        // and the driver's auto-reconnect wedged, the console sat invisible
        // on the network for days (observed Jun 2026) until a power cycle.
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Link dropped — tearing down mDNS, awaiting auto-reconnect");
            MDNS.end();
            _state               = WifiState::Connecting;
            _connectStarted      = millis();
            _lastReconnectKickMs = millis();   // grace period before first kick
            _linkDownSinceMs     = millis();
            return;
        }
    }

    // Post-drop reconnect kicker — ported from the scale. The driver's
    // setAutoReconnect(true) usually recovers alone, but gets stuck in
    // some scenarios (radio briefly in AP_STA mode, AP back on another
    // channel). Every ~30 s in Connecting we explicitly re-begin().
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
        // can't fix from here — reboot. A clean SW reset reconnects in
        // ~10 s; staying down requires a human with a power plug.
        if (_linkDownSinceMs &&
            millis() - _linkDownSinceMs > 10UL * 60UL * 1000UL) {
            Serial.println("[WiFi] Link down >10 min despite reconnect kicks — restarting");
            CrashLogger::flush();   // make sure the reason reaches the SD log
            ESP.restart();
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
    WiFi.mode(WIFI_STA);
    // Disable modem sleep — default WIFI_PS_MIN_MODEM causes multi-second
    // TCP stalls on long-lived connections (e.g. the WS link to the scale).
    WiFi.setSleep(false);

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
    if (pinnedBssid) {
        prefs.putString(NVS_KEY_PINNED_BSSID, *pinnedBssid);
        _pinnedBssid = *pinnedBssid;
    }
    prefs.end();
}

bool WifiProvisioning::_parseBssid(const String& s, uint8_t out[6]) {
    if (s.length() != 17) return false;
    // Colons at positions 2, 5, 8, 11, 14 = `i*3 + 2` for byte i in 0..4.
    // Earlier `i*3 + 1` was off by one (matched the second hex digit
    // instead), causing every parse to fail silently — pin was loaded
    // from NVS and stored, but WiFi.begin always took the "no pin" path.
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
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            JsonObject net = arr.add<JsonObject>();
            net["ssid"]    = WiFi.SSID(i);
            net["bssid"]   = WiFi.BSSIDstr(i);
            net["channel"] = WiFi.channel(i);
            net["rssi"]    = WiFi.RSSI(i);
            net["secure"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
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
            String name = doc["name"] | "";
            if (ssid.isEmpty()) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"ssid required\"}");
                return;
            }
            // Password handling: omitted `pass` key → preserve NVS.
            // Empty string → explicit clear (open network). Same
            // three-state contract as pinned_bssid. Prevents
            // accidental credential clobbering from a partial POST.
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
            // pinned_bssid handling — see scale's mirror for the contract.
            const String* pinnedPtr = nullptr;
            String pinnedVal;
            if (doc["pinned_bssid"].is<const char*>()) {
                pinnedVal = doc["pinned_bssid"].as<const char*>();
                pinnedPtr = &pinnedVal;
            }
            if (passSpecified) {
                _saveCredentials(ssid, pass, name, pinnedPtr);
            } else {
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
            delay(300);
            _startConnect(ssid, pass);
        }
    );
}
