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

private:
    AsyncWebServer* _server = nullptr;
    WifiState _state        = WifiState::Unconfigured;
    unsigned long _connectStarted = 0;
    // True once we've successfully associated to the user's AP at least
    // once this boot. Used to distinguish "initial connect failed — typo'd
    // password, fall back to provisioning AP" from "we were up and lost
    // the link — just wait patiently for the driver to re-associate".
    bool _everConnected     = false;
    String _deviceName;
    SsdpNotify _ssdp;

    void _startAP();
    void _stopAP();
    void _startConnect(const String& ssid, const String& pass);
    void _startDiscovery();
    void _setupCaptiveRoutes();
    void _loadDeviceName();
    void _saveCredentials(const String& ssid, const String& pass, const String& name);

    static String _buildScanJson();
    static String _buildPortalHtml();
};
