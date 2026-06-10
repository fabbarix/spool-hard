#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include "ssdp_notify.h"

enum class WifiState {
    Unconfigured,   // no credentials stored
    Connecting,     // credentials stored, attempting connection
    Connected,      // connected to AP
    Failed,         // connection failed, fell back to provisioning AP
};

class WifiProvisioning {
public:
    void begin(AsyncWebServer& server);
    void update();                      // call in loop

    WifiState getState() const          { return _state; }
    bool isConnected() const            { return _state == WifiState::Connected; }
    String getDeviceName() const        { return _deviceName; }
    SsdpNotify& ssdp()                  { return _ssdp; }

    // Currently-stored BSSID pin, "AA:BB:CC:DD:EE:FF" or "" if unpinned.
    // Surfaced in /api/wifi-status so the UI can render the dropdown
    // selection accurately.
    String getPinnedBssid() const       { return _pinnedBssid; }

private:
    AsyncWebServer* _server = nullptr;
    WifiState _state        = WifiState::Unconfigured;
    unsigned long _connectStarted = 0;
    // True once we've successfully associated to the user's AP at least
    // once this boot. Used to distinguish "initial connect failed — typo'd
    // password, fall back to provisioning AP" from "we were up and lost
    // the link — just wait patiently for the driver to re-associate".
    bool _everConnected     = false;
    // millis() of the last explicit WiFi.disconnect()/begin() kick the
    // post-drop reconnect path issued. arduino-esp32's auto-reconnect
    // does the heavy lifting; this is just a fallback that fires every
    // ~30 s when the driver gets stuck.
    unsigned long _lastReconnectKickMs = 0;
    // millis() when the Connected→Connecting drop was detected; 0 while
    // up. Feeds the down-too-long reboot failsafe.
    unsigned long _linkDownSinceMs     = 0;
    String _deviceName;

    // Mesh-pinning state. `_pinnedBssid` is the user-saved value from NVS,
    // colon-separated MAC string. `_pinnedActive` is true while we're
    // attempting a BSSID-pinned connect; cleared by the 60 s fallback so
    // a single re-begin() with the plain SSID can be issued. The fallback
    // does NOT touch NVS — the user's intent is preserved across reboots.
    String _pinnedBssid;
    bool   _pinnedActive    = false;
    SsdpNotify _ssdp;

    // 60 s fallback: if a BSSID-pinned connect doesn't reach
    // WL_CONNECTED within this window, drop the pin in RAM and
    // retry plain begin(). Long enough for a marginal AP to recover;
    // short enough that a dead mesh node doesn't make the device
    // permanently unreachable.
    static constexpr unsigned long PINNED_FALLBACK_MS = 60000;

    void _startAP();
    void _stopAP();
    void _startConnect(const String& ssid, const String& pass);
    void _startDiscovery();
    void _setupCaptiveRoutes();
    void _loadDeviceName();
    void _loadPinnedBssid();
    void _saveCredentials(const String& ssid, const String& pass,
                          const String& name, const String* pinnedBssid /*nullable*/);

    // Parse "AA:BB:CC:DD:EE:FF" into 6 raw bytes. Returns true on success.
    static bool _parseBssid(const String& s, uint8_t out[6]);

    static String _buildScanJson();
    static String _buildPortalHtml();
};
