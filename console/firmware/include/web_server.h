#pragma once
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <functional>
#include "spoolhard/product_signature.h"
#include "spoolhard/version_marker.h"

class SpoolStore;
class ScaleLink;

class ConsoleWebServer {
public:
    void begin();        // register routes
    void start();        // server.begin()

    AsyncWebServer& server() { return _server; }

    void broadcastDebug(const String& type, const JsonDocument& payload);

    void setStore(SpoolStore* s)      { _store = s; }
    void setScaleLink(ScaleLink* s)   { _scale = s; }

    void onUploadStarted(std::function<void(const char*)> cb) { _onUploadStarted = std::move(cb); }
    void onUploadProgress(std::function<void(int percent, const char* type, const char* label)> cb)
                                                              { _onUploadProgress = std::move(cb); }
    void onOtaRequested(std::function<void()> cb)             { _onOtaRequested = std::move(cb); }

private:
    AsyncWebServer _server{80};
    AsyncWebSocket _ws{"/ws"};

    SpoolStore* _store = nullptr;
    ScaleLink*  _scale = nullptr;

    std::function<void(const char*)> _onUploadStarted;
    std::function<void(int, const char*, const char*)> _onUploadProgress;
    std::function<void()>            _onOtaRequested;

    // Per-upload signature check state. Arduino's Update lib is a singleton,
    // so one matcher is enough. Invariant: `_uploadAccepted` defaults to false
    // and is only flipped true on a clean final-chunk where the signature was
    // seen AND Update.end() committed. Any other path — no upload at all,
    // connection dropped mid-stream, wrong product, failed commit — leaves it
    // false so the response handler serves 400.
    ProductSignatureMatcher _uploadMatcher;
    bool _uploadAccepted = false;
    const char* _uploadRejectReason = "no upload";  // pointer to literal, never freed

    VersionMarkerParser _uploadVersion;

    // Progress reporting throttle — only fire the callback when the percent
    // has advanced at least PROGRESS_STEP points since the last report.
    int _uploadLastReportedPct = -1;
    uint32_t _uploadContentLength = 0;
    String   _uploadLabel;   // refreshed once the version is known

    void _setupRoutes();

    /// Check Authorization: Bearer header against the stored fixed key.
    /// Returns true and lets the caller continue if auth is not required
    /// (no key set, or key == DEFAULT_FIXED_KEY). Otherwise sends 401 and
    /// returns false — the caller must simply `return;`.
    bool _requireAuth(AsyncWebServerRequest* req);

    /// /api/auth-status — always 200, never 401. Reports whether the device
    /// has a non-default key set and whether the request's credentials pass.
    void _handleAuthStatus(AsyncWebServerRequest* req);

    // Config endpoints (ported from scale)
    void _handleDeviceName(AsyncWebServerRequest* req);
    void _handleOtaConfigGet(AsyncWebServerRequest* req);
    void _handleOtaConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleOtaStatus(AsyncWebServerRequest* req);
    void _handleReset(AsyncWebServerRequest* req);
    void _handleTestKey(AsyncWebServerRequest* req);
    void _handleFixedKeyConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleWifiStatus(AsyncWebServerRequest* req);
    void _handleFirmwareInfo(AsyncWebServerRequest* req);

    // Console-specific
    void _handleSpoolsList(AsyncWebServerRequest* req);
    void _handleSpoolGet(AsyncWebServerRequest* req);
    void _handleSpoolPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleSpoolDelete(AsyncWebServerRequest* req);
    // User filament CRUD (locally-managed presets stored on SD).
    void _handleUserFilamentsList(AsyncWebServerRequest* req);
    void _handleUserFilamentGet(AsyncWebServerRequest* req);
    void _handleUserFilamentPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleUserFilamentDelete(AsyncWebServerRequest* req);
    // Bambu Cloud sync (Phase C). The full-library pull runs on a
    // FreeRTOS task to keep the AsyncTCP event loop unblocked — the
    // POST handler returns 202 immediately and the frontend polls
    // `/cloud-sync/status` for the eventual result. Both routes
    // inherit the soft-fail pattern from the token paste flow: a CF
    // block lands as `status:"unreachable"`, not a hard HTTP error.
    void _handleUserFilamentsCloudSync(AsyncWebServerRequest* req);
    void _handleUserFilamentsCloudSyncStatus(AsyncWebServerRequest* req);
    void _handleUserFilamentCloudPush(AsyncWebServerRequest* req);
    // Per-preset "show me everything Bambu Cloud knows about this one"
    // proxy. Issues `GET /v1/iot-service/api/slicer/setting/{cloud_id}`
    // with the user's stored token and returns the raw response body
    // verbatim (plus our usual diagnostics envelope on failure).
    // Inline call — single GET, ~1s typical, blocks AsyncTCP for that
    // window. Usage is user-initiated ("Show cloud details" button) so
    // the brief stall is acceptable; if it becomes a hot path move it
    // to the cloud-sync FreeRTOS task pattern.
    void _handleUserFilamentCloudDetail(AsyncWebServerRequest* req);
    void _handleScaleLinkStatus(AsyncWebServerRequest* req);
    void _handleScaleSecretGet(AsyncWebServerRequest* req);
    void _handleScaleSecretPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handlePrintersList(AsyncWebServerRequest* req);
    void _handlePrinterGet(AsyncWebServerRequest* req);
    void _handlePrinterPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handlePrinterDelete(AsyncWebServerRequest* req);
    void _handleDiscoveryPrinters(AsyncWebServerRequest* req);
    void _handleDiscoveryScales(AsyncWebServerRequest* req);
    void _handlePrinterAnalyzeStart(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handlePrinterAnalysisGet(AsyncWebServerRequest* req);
    void _handleDisplayConfigGet(AsyncWebServerRequest* req);
    void _handleDisplayConfigPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handlePrinterAmsMappingPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handlePrinterFtpDebug(AsyncWebServerRequest* req, uint8_t* data, size_t len);

    // Bambu Lab cloud authentication. The login flow is multi-step
    // (password → optional email-code or TFA → token), so each step
    // is its own POST. The frontend tracks the in-progress session
    // and supplies whatever the previous step returned (account /
    // tfaKey) on the next call.
    void _handleBambuCloudGet(AsyncWebServerRequest* req);
    void _handleBambuCloudLogin(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleBambuCloudLoginCode(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleBambuCloudLoginTfa(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleBambuCloudSetToken(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleBambuCloudVerify(AsyncWebServerRequest* req);
    void _handleBambuCloudClear(AsyncWebServerRequest* req);

    // Core-weights + quick-weights (used by the new-spool wizard).
    void _handleCoreWeightsGet(AsyncWebServerRequest* req);
    void _handleCoreWeightsPut(AsyncWebServerRequest* req, uint8_t* data, size_t len);
    void _handleCoreWeightsDelete(AsyncWebServerRequest* req);
    void _handleQuickWeightsGet(AsyncWebServerRequest* req);
    void _handleQuickWeightsPost(AsyncWebServerRequest* req, uint8_t* data, size_t len);

    // Filaments library (SD-resident SQLite DB sourced from bambu-filaments).
    // Upload writes straight to /sd/filaments.db — GET streams it back to the
    // browser so sql.js can query it client-side. The File handle below is
    // held across streaming-upload chunks; reset on each new upload.
    File        _filamentsUploadFile;
    size_t      _filamentsUploadBytes = 0;
    bool        _filamentsUploadOk    = false;
    const char* _filamentsUploadErr   = "no upload";

    // Backup / restore. The download endpoint serializes a single JSON
    // document with every NVS namespace + filesystem file the console
    // owns; restore parses an upload of the same shape and writes
    // everything back. See spoolhard_core/backup.h for the wire format.
    void _handleBackupGet(AsyncWebServerRequest* req);
    void _handleRestorePost(AsyncWebServerRequest* req,
                            uint8_t* data, size_t len, size_t index, size_t total);
    // Restore is delivered in chunks (the file can be ~hundreds of KB);
    // accumulate into PSRAM until the final chunk lands, then parse +
    // apply in the response handler. Reset on every new upload.
    String _restoreBuffer;
    bool   _restoreReady = false;
    String _restoreError;

public:
    // Debug toggles (session-only; default off on every boot). Exposed as
    // public so hot-path code in bambu_printer etc. can check them without
    // a function call. When enabled, the producer forwards the raw JSON to
    // the /ws debug channel — the Config → Debug UI subscribes and renders.
    bool _logAmsRaw = false;
};

extern ConsoleWebServer g_web;   // defined in main.cpp
