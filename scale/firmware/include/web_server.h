#pragma once
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoJson.h>
#include <functional>
#include "product_signature.h"

class ScaleWebServer {
public:
    void begin();        // register routes only (call before start())
    void start();        // call server.begin() — call after all routes registered
    AsyncWebServer& server() { return _server; }

    void broadcastDebug(const String& type, const JsonDocument& payload);
    void broadcastConsoleFrame(const char* dir, const String& frame);

    // Called when a direct upload begins. Arg: "firmware" or "spiffs".
    void onUploadStarted(std::function<void(const char* type)> cb) { _onUploadStarted = std::move(cb); }
    // Called for every chunk of an in-flight upload. main.cpp uses it as a
    // liveness ping so the LED can stay on "updating" for the whole upload —
    // onUploadStarted only fires once at index 0, after which updateLed()
    // would otherwise overwrite the amber pulse on the next loop tick.
    void onUploadProgress(std::function<void()> cb) { _onUploadProgress = std::move(cb); }
    void onTare(std::function<void()> cb) { _onTare = std::move(cb); }
    void onCalibrate(std::function<void(float)> cb) { _onCalibrate = std::move(cb); }
    void onAddCalPoint(std::function<void(float)> cb) { _onAddCalPoint = std::move(cb); }
    void onClearCal(std::function<void()> cb) { _onClearCal = std::move(cb); }
    void onCaptureRaw(std::function<long()> cb) { _onCaptureRaw = std::move(cb); }
    // Fires after POST /api/scale-config has persisted the new params to
    // NVS. main.cpp wires this to LoadCell::loadParams() so precision,
    // rounding, sampling etc. all take effect live instead of requiring
    // a device restart.
    void onConfigChanged(std::function<void()> cb) { _onConfigChanged = std::move(cb); }

private:
    AsyncWebServer  _server{80};
    AsyncWebSocket  _ws{"/ws"};

    void _setupRoutes();

    /// Check Authorization: Bearer header against the stored fixed key.
    /// Returns true (and lets the caller continue) if auth isn't required
    /// (no key set, or key == DEFAULT_FIXED_KEY). Otherwise sends 401 and
    /// returns false — caller must simply `return;`.
    bool _requireAuth(AsyncWebServerRequest* req);

    /// /api/auth-status — always 200, never 401. Frontend probes this to
    /// decide whether to show the Login page; a subsequent request with the
    /// Authorization header doubles as the login-verify endpoint.
    void _handleAuthStatus(AsyncWebServerRequest* req);

    void _handleNfcConfig(AsyncWebServerRequest* req);
    void _handleDeviceName(AsyncWebServerRequest* req);
    void _handleOtaConfigGet(AsyncWebServerRequest* req);
    void _handleOtaConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleReset(AsyncWebServerRequest* req);
    void _handleTestKey(AsyncWebServerRequest* req);
    void _handleFixedKeyConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleWifiStatus(AsyncWebServerRequest* req);
    void _handleFirmwareInfo(AsyncWebServerRequest* req);
    void _handleScaleConfigGet(AsyncWebServerRequest* req);
    void _handleScaleConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);

    // Per-upload state. Only one upload runs at a time (Arduino's Update lib
    // is a singleton), so a single matcher + rejection flag is sufficient.
    ProductSignatureMatcher _uploadMatcher;
    bool _uploadRejectedProduct = false;  // set on signature mismatch
    bool _uploadIsSpiffs        = false;  // remembered for the response path

    std::function<void(const char*)> _onUploadStarted;
    std::function<void()>            _onUploadProgress;
    std::function<void()>            _onTare;
    std::function<void(float)>       _onCalibrate;
    std::function<void(float)>       _onAddCalPoint;
    std::function<void()>            _onClearCal;
    std::function<long()>            _onCaptureRaw;
    std::function<void()>            _onConfigChanged;
};
