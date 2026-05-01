#include "console_channel.h"

ConsoleChannel g_console;

void ConsoleChannel::begin() {
    _ws.onEvent([this](AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType t,
                       void* arg, uint8_t* data, size_t len) {
        _handleEvent(s, c, t, arg, data, len);
    });
    _server.addHandler(&_ws);
    _server.begin();
    Serial.println("[Console] WebSocket server listening on :81/ws");
}

void ConsoleChannel::sendText(const String& payload) {
    if (_clientCount == 0) return;
    _ws.textAll(payload);
    _framesTx++;
}

void ConsoleChannel::_handleEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                                  AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT: {
            // Drop frames instead of closing the client when the outbound
            // queue fills (default WS_MAX_QUEUED_MESSAGES=32). With
            // RawSamplesAvg pushed at 2 Hz, a console whose TCP ACKs are
            // delayed a few seconds fills the queue in ~16 s — the default
            // behaviour was to close the connection, which showed up as the
            // console's WS flapping every 10-20 s. RawSamplesAvg is debug
            // telemetry the console firmware discards, so losing a few
            // frames under back-pressure is harmless.
            client->setCloseClientOnQueueFull(false);
            _clientCount++;
            _lastIp = client->remoteIP().toString();
            Serial.printf("[Console] Client connected #%u from %s (total=%d)\n",
                          client->id(), _lastIp.c_str(), _clientCount);
            // Fire on EVERY connect, not just the 0→1 transition. The
            // 0→1 gate left a stale-count hole: when the console reboots
            // ungracefully (no clean WS close), the scale doesn't observe
            // a disconnect, _clientCount stays >0, and the new connection
            // arrives as 1→2 — never crossing the gate. The handshake
            // messages (ScaleVersion / CalibrationStatus / OtaPending)
            // never fire and the console's OTA panel sits on "scale
            // waiting" until the scale itself reboots. Each new client
            // wants its own handshake regardless; the messages are
            // broadcast (sent to all clients), so an existing console
            // re-receiving them is harmless — versions don't change.
            if (_onConnected) _onConnected(_lastIp);
            break;
        }
        case WS_EVT_DISCONNECT: {
            _clientCount = _clientCount > 0 ? _clientCount - 1 : 0;
            Serial.printf("[Console] Client disconnected (total=%d)\n", _clientCount);
            if (_clientCount == 0 && _onDisconnected) _onDisconnected();
            break;
        }
        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            if (info->opcode != WS_TEXT) {
                Serial.println("[Console] Non-text frame received, ignoring");
                return;
            }
            // The ESP32 AsyncWebSocket delivers text as contiguous for frames < ~1KB.
            // For larger messages we'd need to buffer by (info->num, info->index, info->len).
            if (info->final && info->index == 0 && info->len == len) {
                String s;
                s.reserve(len + 1);
                s.concat((const char*)data, len);
                _framesRx++;
                if (_onText) _onText(s);
            } else {
                // TODO: fragmented/continued frame assembly if we hit this in practice
                Serial.printf("[Console] Fragmented frame (num=%u idx=%llu len=%llu) — not yet handled\n",
                              info->num, info->index, info->len);
            }
            break;
        }
        default:
            break;
    }
}
