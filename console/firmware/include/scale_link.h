#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <WebSocketsClient.h>
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
    void update();    // called from loop on Core 0

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

    // Commands.
    void tare();                                 // Calibrate(0)
    void calibrate(int32_t known_weight);        // Calibrate(w)
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

    float    _lastWeightG     = 0.f;
    String   _lastWeightState;
    uint32_t _lastWeightMs    = 0;
    // Display precision announced by the scale (0..4 decimals). 1 is a
    // sensible pre-handshake default — matches the scale's own default.
    int      _scalePrecision  = 1;

    VoidCb     _onConnect;
    VoidCb     _onDisconnect;
    WeightCb   _onWeight;
    TagCb      _onTag;
    VoidCb     _onButton;
    HandshakeCb _onHandshake;

    // Cached scale-side OTA state — populated by _dispatch on every
    // OtaPending frame.
    ScaleOtaPending _scaleOta;

    // Cached scale-side OTA-in-flight state — updated on each
    // OtaProgressUpdate frame the scale pushes during a self-flash.
    ScaleOtaInFlight _scaleOtaInFlight;

    void _connect();
    void _onWsEvent(WStype_t type, uint8_t* payload, size_t length);
    void _dispatch(const ScaleToConsole::Message& msg);
    void _send(String frame);  // by value — WebSocketsClient::sendTXT wants String& (non-const)
    void _recordEvent(const char* kind, const String& detail);
    // Recompute the handshake state from _connected and the stored secret.
    // Called on every WS connect/disconnect and whenever the secret changes.
    void _refreshHandshakeState();
    // Idempotent "we lost the link" cleanup. Called both by the library's
    // (often slow) WStype_DISCONNECTED event and by the staleness check in
    // update() when the WebSocketsClient hasn't noticed yet.
    void _markDisconnected(const char* reason);

    // If we haven't heard a WS event from the scale in this long while
    // _connected is supposedly true, treat the link as down. Sized to span
    // 2 missed heartbeat cycles (ping every 5 s) plus some slack — the
    // library's own heartbeat timeout can stall for ~60 s when the peer
    // dies without a clean RST because clientDisconnect() blocks in TCP
    // flush()/stop() while lwIP exhausts its retransmit schedule.
    static constexpr uint32_t kStaleMs = 15000;

    // Minimum time after kicking off _connect() before we'll honour an
    // SSDP-driven reconnect. The Bambu MQTT connect can hold the main loop
    // for ~30 s; during that window the WS handshake response sits in TCP
    // buffers waiting for the next _ws.loop(). If we tore the socket down
    // on the first SSDP NOTIFY back, we'd kill the in-flight connect just
    // before it would have completed. Give it enough breathing room to
    // outlast a worst-case bambu stall.
    static constexpr uint32_t kReconnectGuardMs = 35000;
    uint32_t _lastConnectMs = 0;
};
