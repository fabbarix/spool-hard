#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <atomic>
#include <functional>
#include "protocol.h"

// Console's link to the SpoolHard scale. Discovers the scale via SSDP, then
// opens a WebSocket client to scale:81/ws. Incoming frames are pushed into
// the ScaleToConsole parser. Outgoing messages are built via ConsoleToScale::
// and sent with sendText().
class ScaleLink {
public:
    using VoidCb   = std::function<void()>;
    using WeightCb = std::function<void(float grams, const char* state)>;
    using TagCb    = std::function<void(const char* uid, const char* url, bool bambu)>;

    void begin();

    // Spawn the dedicated FreeRTOS task that owns _ws. Must be called
    // AFTER begin() and AFTER WiFi is up. Once spawned, the link runs
    // entirely on its own thread — main loop no longer needs to call
    // update(). Idempotent.
    //
    // Why: the links2004 WebSocketsClient is synchronous — _ws.loop()
    // reads pending TCP bytes, runs the heartbeat ping/pong cycle, and
    // dispatches incoming frames to the user callback. When main loop
    // is blocked elsewhere (Bambu MQTT pushall parse, FTPS gcode
    // analyzer, big SPIFFS reads), _ws.loop() doesn't run. The
    // server-side AsyncWebSocketClient on the scale times out, closes
    // the socket, and the link reconnects ~3 s later. We saw this as
    // a clean ~17-19 s flap cycle even when both devices were on the
    // same mesh AP. Pinning ruled out radio causes; Wi-Fi event log
    // confirmed zero STA-level disconnects.
    //
    // After this task split, _ws.loop() runs at ~500 Hz regardless of
    // what main loop is doing, so PINGs are answered on time and
    // incoming TCP bytes are drained promptly.
    void beginTask();

    // Legacy entry point — now a no-op when the task is running.
    // Kept so existing main.cpp call sites compile without churn while
    // we transition; deletion candidate once we're confident.
    void update();

    // State of the end-to-end link with the paired scale. Used by the UI to
    // colour the status dot:
    //   Encrypted    — WS connected and HMAC handshake verified → green
    //   Unencrypted  — WS connected but no shared secret stored → amber
    //   Failed       — WS connected, secret set, but handshake rejected → amber
    //   Disconnected — WS is down                                → red
    // The handshake itself is future work (M2-adjacent); for now the
    // Encrypted/Unencrypted split reflects only "is a secret stored locally",
    // which is forward-compatible — the getter's contract doesn't change
    // when the real handshake lands.
    enum class Handshake { Disconnected, Unencrypted, Encrypted, Failed };

    bool isConnected() const { return _connected; }
    Handshake handshake() const { return _handshake; }

    // Called when the shared secret changes so the handshake state reflects
    // the new NVS value without waiting for a reconnect.
    void pokeHandshake() { _refreshHandshakeState(); }

    // Tear down the current WS connection so the auto-reconnect timer
    // re-runs `_connect()` with a freshly-loaded secret. Called from
    // POST /api/scale-secret after the new value lands in NVS — the
    // currently-open socket was authenticated with the old key (or
    // none) and the scale would reject the new one mid-flight if we
    // tried to upgrade in place.
    void forceReconnect();
    String scaleIp() const   { return _scaleIp.toString(); }
    String scaleName() const { return _scaleName; }

    // Last protocol event received from the scale, for the Config UI's
    // "last event" panel. `ms` is millis() at reception; 0 means none yet.
    // `scale` is the name of the scale the event came from (useful even
    // with one pairing so the UI can tell you which scale it was).
    uint32_t lastEventMs()           const { return _lastEventMs; }
    const String& lastEventKind()    const { return _lastEventKind; }
    const String& lastEventDetail()  const { return _lastEventDetail; }
    const String& lastEventScale()   const { return _lastEventScale; }

    // Latest weight received from the paired scale. `_lastWeightG` is the
    // value; `_lastWeightState` is "new" | "stable" | "unstable" | "removed"
    // | "uncalibrated"; `_lastWeightMs` is millis() at reception. The web UI
    // reads these to drive the "capture current weight" button.
    //
    // Note on "new" vs "stable": the scale's wire-level `NewLoad` event means
    // "the reading just settled after a load was placed" — it IS a stable
    // reading, just the *first* one. `LoadChangedStable` means "stable again
    // after a subsequent change". Both are settled; `hasStableWeight()` has
    // to accept either, otherwise capture bails out whenever the user hasn't
    // wiggled the spool after placing it.
    float         lastWeightG()      const { return _lastWeightG; }
    const String& lastWeightState()  const { return _lastWeightState; }
    uint32_t      lastWeightMs()     const { return _lastWeightMs; }
    bool          hasStableWeight()  const {
        return _lastWeightMs > 0 &&
               (_lastWeightState == "stable" || _lastWeightState == "new");
    }
    // Display precision the scale would use on its own screen (0..4 decimals).
    // Refreshed from the ScaleVersion handshake on connect and from every
    // CurrentWeight response. Default 1 until the scale announces itself so
    // there's always a sensible value to render with.
    int           scalePrecision()   const { return _scalePrecision; }
    // Scale's reported firmware version (from the ScaleVersion handshake
    // message it pushes on every connect). Empty until the first message
    // arrives; persisted across WS flaps the same way scaleOtaPending() is
    // — a flap rarely changes versions and the cached value is strictly
    // more useful than blanking the OTA panel.
    const String& scaleFirmwareVersion() const { return _scaleFirmwareVersion; }

    // Scale-side telemetry fed by the periodic Heartbeat message
    // (ScaleToConsole::Heartbeat at ~5 s cadence). 0 until the first
    // heartbeat lands; we persist the cached values across WS flaps so
    // the dashboard doesn't blank out for the brief reconnect window
    // (the heartbeat will overwrite within 5 s of the link returning).
    // `_scaleHeartbeatRxMs` is the millis() at which we last received
    // one — handy for the UI to fade the panel if heartbeats stop.
    uint32_t scaleUptimeS()      const { return _scaleUptimeS; }
    uint32_t scaleFreeHeap()     const { return _scaleFreeHeap; }
    uint32_t scaleMinFreeHeap()  const { return _scaleMinFreeHeap; }
    uint32_t scaleHeartbeatRxMs() const { return _scaleHeartbeatRxMs; }

    // Callbacks — called from main loop after update() drains the WS queue.
    void onConnect(VoidCb cb)      { _onConnect = std::move(cb); }
    void onDisconnect(VoidCb cb)   { _onDisconnect = std::move(cb); }
    void onWeight(WeightCb cb)     { _onWeight = std::move(cb); }
    void onTag(TagCb cb)           { _onTag = std::move(cb); }
    // Fires on a `ButtonPressed` frame — the scale's physical feature
    // button, used by main.cpp as a "confirm capture current weight on the
    // scanned spool" trigger. No payload.
    void onButton(VoidCb cb)       { _onButton = std::move(cb); }
    // Fires whenever the (connected, handshake) pair transitions — WS up/down
    // or the stored secret changes. The scale-name is passed so the LCD can
    // show "<name>: online" without re-reading NVS.
    using HandshakeCb = std::function<void(Handshake, const String& scaleName)>;
    void onHandshakeChanged(HandshakeCb cb) { _onHandshake = std::move(cb); }
    // Fires on every CalibrationStatus frame the scale pushes —
    // emitted right after every tare / addCalPoint / clearCalPoints
    // and once on console-connect as a baseline. `num_points` is the
    // size of the multi-point curve; `tare_raw` is the persisted zero
    // reading (nonzero ⇒ scale has been tared at least once).
    using CalibrationStatusCb = std::function<void(int num_points, int32_t tare_raw)>;
    void onCalibrationStatus(CalibrationStatusCb cb) { _onCalStatus = std::move(cb); }
    // Most recent CalibrationStatus snapshot — useful for callers that
    // want the current state without waiting for the next push (e.g.
    // the scale-settings screen reading the value when it opens).
    int     calNumPoints() const { return _calNumPoints; }
    int32_t calTareRaw()   const { return _calTareRaw; }

    // Commands.
    void tare();                                 // Calibrate(0)
    void calibrate(int32_t known_weight);        // Calibrate(w)  legacy single-point
    // Multi-point calibration controls used by the LCD wizard.
    // addCalPoint asks the scale to sample its current raw reading and
    // append a (weight, raw-tare_raw) entry to its piecewise-linear
    // curve (max 8 points). clearCalPoints wipes the curve back to
    // empty. Both fire-and-forget; the scale pushes a CalibrationStatus
    // frame back which the UI hooks via onCalibrationStatus() below.
    void addCalPoint(int32_t known_weight);      // {"AddCalPoint": w}
    void clearCalPoints();                       // "ClearCalPoints"
    void readTag();                              // "ReadTag"
    void writeTag(const String& text, const String& uidHex);
    void emulateTag(const String& url);
    // Ask the scale for its current weight + state. The scale replies with a
    // CurrentWeight frame that flows through the same onWeight callback as
    // state-change events, so callers just trigger this and wait for the
    // callback to fire. Safe to call while disconnected — silently drops.
    void requestCurrentWeight();

    // ACK for a ButtonPressed event — triggers the scale's RGB LED to flash
    // the "capture ok / fail" pattern so the user gets immediate feedback
    // that the console honoured (or rejected) the press.
    void sendButtonResponse(bool ok);

    // Push a GcodeAnalysisNotify frame to the scale. `tools` is a JsonArray
    // of {tool_idx, grams, spool_id, material} objects the scale can use to
    // update its per-tray consumption bookkeeping. Safe to call when the link
    // is down; drops the frame with a Serial log.
    void pushGcodeAnalysis(const String& printer_serial, float total_grams,
                           const JsonDocument& tools);

    // ── Scale-side OTA state (Phase 5) ──────────────────────────
    //
    // Cached snapshot of the most recent OtaPending frame the scale pushed
    // (on console-connect and whenever its pending-state changes). The
    // console folds this into /api/ota-status so the React UI shows
    // console + scale in one combined banner without polling the scale.
    struct ScaleOtaPending {
        bool     valid = false;        // false until the scale has pushed
        String   firmware_current;
        String   firmware_latest;
        String   frontend_current;
        String   frontend_latest;
        bool     firmware_update = false;
        bool     frontend_update = false;
        uint32_t last_check_ts   = 0;
        String   last_check_status;
        // millis() at which we received the cached snapshot — UI can use
        // this to fade the panel if the link has been down for too long.
        uint32_t received_ms     = 0;
    };
    const ScaleOtaPending& scaleOtaPending() const { return _scaleOta; }

    // Mirror of the most recent OtaProgressUpdate frame the scale pushed
    // while flashing itself. The web layer reads it via scaleOtaInFlight()
    // to render a progress bar on the console's combined OTA banner.
    // `valid == false` once the link drops or after a long idle window.
    struct ScaleOtaInFlight {
        bool        valid       = false;
        String      kind;          // "" | "firmware" | "frontend"
        int         percent      = 0;
        uint32_t    last_update_ms = 0;   // millis() at last frame
    };
    const ScaleOtaInFlight& scaleOtaInFlight() const { return _scaleOtaInFlight; }

    // Tell the scale to flash itself NOW using its stored OtaConfig. Maps
    // to ConsoleToScale::RunOtaUpdate. Drops silently if the link is down.
    void requestScaleOtaUpdate();
    // Tell the scale to refresh its manifest immediately. Maps to
    // ConsoleToScale::CheckOtaUpdates. Drops silently if down.
    void requestScaleOtaCheck();

private:
    // SSDP discovery comes from the shared g_ssdp_1990 hub — we don't own
    // our own AsyncUDP socket because AsyncUDP can't multiplex two listeners
    // on the same (multicast, port) pair.
    WebSocketsClient   _ws;
    IPAddress          _scaleIp;
    String             _scaleName;
    bool               _have_scale  = false;
    bool               _connected   = false;
    bool               _wsStarted   = false;
    Handshake          _handshake   = Handshake::Disconnected;
    // Set by the SSDP callback (runs on AsyncUDP's task) and consumed by
    // update() (runs on the main loop). Touching the underlying _ws from the
    // SSDP task races with _ws.loop() and was tearing down in-flight
    // connections — defer all WS mutation to the main loop instead.
    volatile bool      _ssdpKickReconnect = false;

    uint32_t _lastEventMs     = 0;
    String   _lastEventKind;
    String   _lastEventDetail;
    String   _lastEventScale;

    uint32_t _lastWsEventMs   = 0;  // debug: last _onWsEvent timestamp
    // Last TEXT/BIN frame received from the scale. Tracked separately
    // from _lastWsEventMs because the WebSocketsClient lib has been
    // observed entering a "PONG-only" state where TCP keepalive
    // pings/pongs flow normally but real frames stop arriving — usually
    // after a long uptime or some specific disconnect+reconnect race.
    // The kStaleMs check has to gate on text freshness, not any-event,
    // or PONGs would mask the dead-data state forever.
    uint32_t _lastWsTextMs    = 0;

    float    _lastWeightG     = 0.f;
    String   _lastWeightState;
    uint32_t _lastWeightMs    = 0;
    // Display precision announced by the scale (0..4 decimals). 1 is a
    // sensible pre-handshake default — matches the scale's own default.
    int      _scalePrecision  = 1;
    String   _scaleFirmwareVersion;   // populated from ScaleVersion msg
    // Heartbeat-fed telemetry. Persists across WS flaps; the next
    // heartbeat (~5 s after reconnect) overwrites with fresh values.
    uint32_t _scaleUptimeS       = 0;
    uint32_t _scaleFreeHeap      = 0;
    uint32_t _scaleMinFreeHeap   = 0;
    uint32_t _scaleHeartbeatRxMs = 0;   // millis() at last heartbeat

    VoidCb     _onConnect;
    VoidCb     _onDisconnect;
    WeightCb   _onWeight;
    TagCb      _onTag;
    VoidCb     _onButton;
    HandshakeCb _onHandshake;
    CalibrationStatusCb _onCalStatus;
    // Most-recent CalibrationStatus payload, cached so callers can
    // read the latest snapshot synchronously without waiting for the
    // next push event. Updated on every CalibrationStatus frame and
    // reset on disconnect.
    int        _calNumPoints  = 0;
    int32_t    _calTareRaw    = 0;

    // Cached scale-side OTA state — populated by _dispatch on every
    // OtaPending frame.
    ScaleOtaPending _scaleOta;

    // Cached scale-side OTA-in-flight state — updated on each
    // OtaProgressUpdate frame the scale pushes during a self-flash.
    ScaleOtaInFlight _scaleOtaInFlight;

    void _connect();
    void _onWsEvent(WStype_t type, uint8_t* payload, size_t length);
    void _dispatch(const ScaleToConsole::Message& msg);
    void _send(String frame);  // queue path; runs only on _task
    void _recordEvent(const char* kind, const String& detail);

    // FreeRTOS task that owns _ws. Created by beginTask(); body lives in
    // _taskBody (static) which casts arg to `this` and calls _taskTick().
    TaskHandle_t  _task        = nullptr;
    QueueHandle_t _sendQueue   = nullptr;       // holds heap-alloc String*
    static constexpr size_t kSendQueueDepth = 16;
    // forceReconnect() sets this; the task drains it next iteration so
    // _ws.disconnect() runs only on the owning thread. The lib is not
    // safe against concurrent calls from multiple tasks.
    std::atomic<bool> _forceReconnectPending{false};

    static void _taskBody(void* arg);
    void _taskTick();
    void _drainSendQueue();
    // Recompute the handshake state from _connected and the stored secret.
    // Called on every WS connect/disconnect and whenever the secret changes.
    void _refreshHandshakeState();
    // Idempotent "we lost the link" cleanup. Called both by the library's
    // (often slow) WStype_DISCONNECTED event and by the staleness check in
    // update() when the WebSocketsClient hasn't noticed yet.
    void _markDisconnected(const char* reason);

    // Pure backstop for the case where the WebSocketsClient lib's own
    // heartbeat (5 s ping × 3 missed × 5 s timeout ≈ 25 s detection)
    // gets wedged on a half-dead TCP. The lib is the primary
    // authority for "this peer is gone" — it tears down + reconnects
    // when its heartbeat fires. kStaleMs only fires if the lib STILL
    // hasn't noticed after 90 s of silence, which is rare.
    //
    // Was 30 s but each fire spawned a `_markDisconnected` →
    // SSDP-driven reconnect cycle (lwIP socket teardown + new TCP +
    // WS upgrade — full alloc churn). On a Wi-Fi link that
    // occasionally drops 1-2 s of packets, 30 s was tearing down
    // healthy connections every couple of minutes. 90 s gives the
    // lib three full heartbeat cycles to act first.
    static constexpr uint32_t kStaleMs = 90000;

    // Minimum time after kicking off _connect() before we'll honour an
    // SSDP-driven reconnect. The Bambu MQTT connect can hold the main loop
    // for ~30 s; during that window the WS handshake response sits in TCP
    // buffers waiting for the next _ws.loop(). If we tore the socket down
    // on the first SSDP NOTIFY back, we'd kill the in-flight connect just
    // before it would have completed. Give it enough breathing room to
    // outlast a worst-case bambu stall.
    static constexpr uint32_t kReconnectGuardMs = 35000;
    uint32_t _lastConnectMs = 0;

    // Throttle for CheckOtaUpdates kicks on reconnect. Without this,
    // a flapping link triggered an HTTPS manifest fetch on the scale
    // every 17-30 s — which itself caused the next flap (mbedtls
    // pressure on AsyncTCP). 5 min minimum spacing breaks the cycle
    // and is harmless for OTA UX (the scale runs its own scheduled
    // check at the configured interval, default 24 h).
    uint32_t _lastOtaKickMs = 0;
};
