#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <functional>

// Owns the AsyncWebServer on port 81 hosting /ws, which is the channel the
// SpoolHard Console connects to after SSDP discovery. Decoupled from the
// port-80 config UI.
//
// Inbound frames are delivered via onTextFrame(); callers (protocol.cpp)
// parse the serde-externally-tagged JSON.
class ConsoleChannel {
public:
    void begin();
    bool isConnected() const { return _clientCount > 0; }

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

private:
    AsyncWebServer  _server{81};
    AsyncWebSocket  _ws{"/ws"};
    int             _clientCount = 0;
    String          _lastIp;
    uint32_t        _framesRx = 0;
    uint32_t        _framesTx = 0;
    std::function<void(const String&)> _onText;
    std::function<void(const String&)> _onConnected;
    std::function<void()>              _onDisconnected;

    void _handleEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len);
};

extern ConsoleChannel g_console;
