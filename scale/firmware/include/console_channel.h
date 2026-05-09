#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <functional>

// Mounts the `/ws/console` WebSocket onto the scale's main web server
// (port 80, owned by ScaleWebServer). This is the channel the
// SpoolHard Console connects to after SSDP discovery.
//
// Was previously a separate AsyncWebServer on port 81 — collapsed into
// the single-port server so AsyncTCP's task pool, client lists, and
// keepalive timers all live under one roof.
//
// Inbound frames are delivered via onTextFrame(); callers (protocol.cpp)
// parse the serde-externally-tagged JSON.
class ConsoleChannel {
public:
    // Mount the WS handler onto the supplied server. The server is
    // expected to be `_server.begin()`'d by its owner — we just attach
    // a handler.
    void begin(AsyncWebServer& server);
    // Anchored on AsyncWebSocket's own count(), not our running tally.
    // count() filters by `_status == WS_CONNECTED`; the lib transitions
    // a client out of that status the moment its TCP socket dies, so
    // this returns the truth even if cleanupClients() hasn't yet swept
    // the zombie out of `_clients`. The LED logic upstream depends on
    // this returning false when the console is actually gone.
    bool isConnected() { return _ws.count() > 0; }

    // Send a raw text frame to all connected clients.
    void sendText(const String& payload);

    // Called once per received text frame.
    void onTextFrame(std::function<void(const String&)> cb) { _onText = std::move(cb); }

    // Rising edge: first client connects. `remoteIp` is the client's IP string.
    void onConnected(std::function<void(const String& remoteIp)> cb) { _onConnected = std::move(cb); }

    // Falling edge: last client disconnects.
    void onDisconnected(std::function<void()> cb) { _onDisconnected = std::move(cb); }

    const String& lastClientIp() const { return _lastIp; }
    uint32_t framesRx() const { return _framesRx; }
    uint32_t framesTx() const { return _framesTx; }

    // Drop AsyncWebSocketClient slots whose underlying TCP died
    // abruptly. Without periodic invocation, WS_EVT_DISCONNECT never
    // fires for dirty disconnects (console reboot, WiFi flap), the
    // zombie client lingers in `_clients`, `_clientCount` stays >0, and
    // the LED falsely shows green forever. Call once a second.
    void cleanupClients() { _ws.cleanupClients(); }

    // Live count of clients per AsyncWebSocket's `count()` (only the
    // entries whose status is WS_CONNECTED). Different from
    // `_clientCount` above which is our own running tally — exposed so
    // the LED logic can be re-grounded on the library's view of truth.
    size_t connectedCount() { return _ws.count(); }

    // DIAGNOSTIC: dump queue/state of every active client to dlog.
    // Call once per second from the main loop tick. Logs queueLen,
    // sndbuf space, status, and seconds-since-connect — the data we
    // need to discriminate between (a) producer-side queue fill,
    // (b) lib drain stall with full queue, (c) silent drop while queue
    // stays empty.
    void tickStats();

private:
    AsyncWebSocket  _ws{"/ws/console"};
    int             _clientCount = 0;
    String          _lastIp;
    uint32_t        _framesRx = 0;
    uint32_t        _framesTx = 0;
    // Per-active-client diagnostic state.
    uint32_t        _activeClientId = 0;
    uint32_t        _connectMs       = 0;
    uint32_t        _peakQueueLen    = 0;
    uint32_t        _txAttempts      = 0;  // sendText() calls while connected
    uint32_t        _txFullSeen      = 0;  // sendText() observed queueIsFull
    uint32_t        _statsSeq        = 0;  // tick counter
    std::function<void(const String&)> _onText;
    std::function<void(const String&)> _onConnected;
    std::function<void()>              _onDisconnected;

    void _handleEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len);
};

extern ConsoleChannel g_console;
