#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

// Adapted from scale/firmware/src/wifi_provisioning.{cpp,h}. Public contract
// is identical; only the AP prefix and mDNS service name differ.

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
    String getSecurityKey() const       { return _securityKey; }
    String getApSsid() const            { return _apSsid; }

private:
    AsyncWebServer* _server = nullptr;
    WifiState _state        = WifiState::Unconfigured;
    unsigned long _connectStarted = 0;
    String _deviceName;
    String _securityKey;
    String _apSsid;

    void _startAP();
    void _stopAP();
    void _startConnect(const String& ssid, const String& pass);
    void _startMdns();
    void _setupCaptiveRoutes();
    void _loadDeviceName();
    void _loadSecurityKey();
    void _saveCredentials(const String& ssid, const String& pass, const String& name);

    static String _buildScanJson();
    static String _buildPortalHtml();
};
