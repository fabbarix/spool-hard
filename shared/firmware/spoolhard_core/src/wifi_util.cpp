#include "spoolhard/wifi_util.h"
#include "spoolhard/auth.h"
#include <Preferences.h>
#include <WiFi.h>
#include "spoolhard/serial_mirror.h"   // keep last: mirrors Serial.* to the ring log

namespace SpoolhardWifi {

const char* reasonTag(uint8_t r) {
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

void installEventLogger() {
    // Captures every CONNECTED / DISCONNECTED with BSSID + reason code so
    // /api/logs surfaces what the driver and AP are negotiating. Critical
    // for diagnosing pinned-BSSID refusals via mesh band-steering
    // (reason=ASSOC_FAIL).
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
                              reasonTag(info.wifi_sta_disconnected.reason));
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
}

bool parseBssid(const String& s, uint8_t out[6]) {
    if (s.length() != 17) return false;   // "AA:BB:CC:DD:EE:FF"
    // Colons at positions 2, 5, 8, 11, 14 = `i*3 + 2` for byte i in 0..4.
    // (An earlier `i*3 + 1` was off by one and made every parse fail
    // silently, so the pin was stored but WiFi.begin took the "no pin" path.)
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

String startProvisioningAp(const char* prefix) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apName[32];
    snprintf(apName, sizeof(apName), "%s%02X%02X%02X",
             prefix, mac[3], mac[4], mac[5]);

    // Stop STA retries first - they hog the radio and block scanning
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName);
    Serial.printf("[WiFi] AP started: '%s'  IP: %s\n",
                  apName, WiFi.softAPIP().toString().c_str());

    // Kick off a scan so results are ready when the portal loads
    WiFi.scanNetworks(true);
    return String(apName);
}

void stopProvisioningAp() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
}

String loadPinnedBssid() {
    Preferences prefs;
    prefs.begin(SpoolhardAuth::kNvsNsWifi, true);
    String pinned = prefs.getString(kNvsKeyPinnedBssid, "");
    prefs.end();
    if (!pinned.isEmpty()) {
        Serial.printf("[WiFi] Pinned BSSID configured: %s\n", pinned.c_str());
    }
    return pinned;
}

void saveCredentials(const String& ssid, const String& pass, const String& name,
                     const String* pinnedBssid, String& deviceName, String& pinnedBssidOut) {
    Preferences prefs;
    prefs.begin(SpoolhardAuth::kNvsNsWifi, false);
    prefs.putString(SpoolhardAuth::kNvsKeySsid, ssid);
    prefs.putString(SpoolhardAuth::kNvsKeyPass, pass);
    if (!name.isEmpty()) {
        prefs.putString(SpoolhardAuth::kNvsKeyDeviceName, name);
        deviceName = name;
    }
    if (pinnedBssid) {
        prefs.putString(kNvsKeyPinnedBssid, *pinnedBssid);
        pinnedBssidOut = *pinnedBssid;
    }
    prefs.end();
}

}  // namespace SpoolhardWifi
