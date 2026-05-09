#include "console_channel.h"
#include "spoolhard/auth.h"
#include "spoolhard/ring_log.h"
#include "spoolhard/serial_mirror.h"

ConsoleChannel g_console;

void ConsoleChannel::begin(AsyncWebServer& server) {
    // Auth-gate the upgrade with the same fixed key the dashboard
    // `/ws` uses. The paired console is expected to mirror this key
    // into its own NVS (`scale_cfg/secrets_json`, keyed by scale
    // name) and append `?key=<secret>` to the WS URL on connect —
    // see ScaleLink::_connect on the console side.
    //
    // Without this gate, any LAN client could open `/ws/console`,
    // receive every weight/NFC event the scale emits, AND send
    // commands like Calibrate / WriteTag / RunOtaUpdate (the OTA
    // trigger flashes from a user-configured URL — a rogue client
    // could brick the device by pointing it at a hostile manifest).
    // A no-key install (DEFAULT_FIXED_KEY) is treated as auth-off,
    // matching the dashboard's behaviour, so freshly-flashed scales
    // still pair without configuration.
    _ws.handleHandshake(SpoolhardAuth::wsAuthHandshake);
    _ws.onEvent([this](AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType t,
                       void* arg, uint8_t* data, size_t len) {
        _handleEvent(s, c, t, arg, data, len);
    });
    server.addHandler(&_ws);
    Serial.println("[Console] WS handler mounted at /ws/console "
                   "(port 80, auth=fixed_key, keepalive=5s/client)");
}

void ConsoleChannel::sendText(const String& payload) {
    // Ground the "anyone listening?" check on AsyncWebSocket's own
    // count() — only entries with _status == WS_CONNECTED. The library
    // updates that immediately when the TCP socket transitions to
    // disconnected, so we get the right answer even before the next
    // cleanupClients() sweep erases the slot. The local _clientCount
    // tally falls behind during dirty disconnects and was the source
    // of the LED-stuck-on-green bug.
    if (_ws.count() == 0) return;
    _txAttempts++;

    // DIAGNOSTIC: snapshot queue length + sndbuf BEFORE we hand the
    // frame to the library. If the queue is creeping up over time we
    // want to see it; if it's slamming to 32 between two consecutive
    // sendText() calls the second call's snapshot will be the smoking
    // gun.
    AsyncWebSocketClient* c = (_activeClientId != 0) ? _ws.client(_activeClientId) : nullptr;
    size_t qbefore = c ? c->queueLen() : 0;
    bool   full    = c ? c->queueIsFull() : false;
    if (full) _txFullSeen++;
    if (c && qbefore > _peakQueueLen) _peakQueueLen = qbefore;

    _ws.textAll(payload);
    _framesTx++;

    if (c && (qbefore >= 16 || full)) {
        // Only chatter when something looks unhealthy. 16 = half the
        // 32-slot lib limit, well below the close-on-full trigger.
        size_t qafter = c->queueLen();
        dlog("ws-tx", "qBefore=%u qAfter=%u full=%d txAttempts=%u peak=%u txFullSeen=%u sz=%u",
             (unsigned)qbefore, (unsigned)qafter, full ? 1 : 0,
             (unsigned)_txAttempts, (unsigned)_peakQueueLen,
             (unsigned)_txFullSeen, (unsigned)payload.length());
    }
}

void ConsoleChannel::tickStats() {
    if (_activeClientId == 0) return;
    AsyncWebSocketClient* c = _ws.client(_activeClientId);
    if (!c) {
        dlog("ws-stats", "id=%u GONE peak=%u txAttempts=%u txFullSeen=%u",
             (unsigned)_activeClientId, (unsigned)_peakQueueLen,
             (unsigned)_txAttempts, (unsigned)_txFullSeen);
        _activeClientId = 0;
        return;
    }
    AsyncClient* tcp = c->client();
    size_t qlen     = c->queueLen();
    bool   full     = c->queueIsFull();
    bool   canSend  = tcp ? tcp->canSend() : false;
    size_t sndbuf   = tcp ? tcp->space()  : 0;
    int    status   = c->status();
    uint32_t age_ms = millis() - _connectMs;
    if (qlen > _peakQueueLen) _peakQueueLen = qlen;
    dlog("ws-stats",
         "#%u age=%lus qlen=%u peak=%u full=%d cansend=%d sndbuf=%u status=%d txA=%u txF=%u rxF=%u",
         (unsigned)_statsSeq++,
         (unsigned long)(age_ms / 1000),
         (unsigned)qlen,
         (unsigned)_peakQueueLen,
         full ? 1 : 0,
         canSend ? 1 : 0,
         (unsigned)sndbuf,
         status,
         (unsigned)_txAttempts,
         (unsigned)_txFullSeen,
         (unsigned)_framesRx);
}

void ConsoleChannel::_handleEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                                  AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT: {
            // Port 81 is single-client by design — only the SpoolHard
            // Console connects here. Any pre-existing slot at the
            // moment a NEW connection arrives is by definition a
            // zombie from a console reconnect that didn't tear down
            // cleanly (TCP keepalive timeout hadn't fired yet, lwIP
            // didn't observe RST, etc.). textAll() iterates all
            // WS_CONNECTED slots — broadcasting to the zombie pads
            // its outbound queue with frames that never get sent,
            // and (the empirically-observed bug) somehow squelches
            // the new client's flow too. Force-close zombies BEFORE
            // we finish setting up the new client so subsequent
            // textAll calls only target this connection.
            //
            // Walk the lib's actual client list via getClients() so we
            // catch zombies regardless of their id (lib counter
            // increments on each connection — over a long uptime the
            // numbers go far past the 8-slot default range).
            //
            // const_cast is safe: the lib's getClients() returns const
            // for read-only public traversal, but `close()` is a
            // non-const member. We're closing zombies on behalf of the
            // server, semantically identical to the lib's own
            // closeAll() with a filter.
            auto& clients =
                const_cast<std::list<AsyncWebSocketClient>&>(server->getClients());
            for (auto& other : clients) {
                if (other.id() == client->id()) continue;
                if (other.status() != WS_CONNECTED) continue;
                Serial.printf("[Console] Closing stale slot #%u before "
                              "honouring new connect #%u\n",
                              other.id(), client->id());
                other.close(1001, "supplanted");
            }

            // Close the client when its 32-slot per-client queue fills.
            // Was `false` (drop frames, keep client alive). Once the
            // console-side WS pool exposed buffers via shared_ptr, a
            // slow / wedged client whose queue stayed at 32 was
            // pinning broadcast buffers indefinitely AND the buffer
            // pinning seemed to correlate with the console-side
            // "TEXT-frames-stop-arriving" wedge. Closing on full
            // gives the system a clean restart path: lib closes the
            // client, cleanupClients() reaps it, console reconnects.
            client->setCloseClientOnQueueFull(true);
            // INVESTIGATION (0.10.x): bumped keepAlive 5→60s and
            // ackTimeout 30→120s to neutralise both lib-level closes
            // (mathieucarbou's _onTimeout unconditionally close()s
            // when AsyncTCP fires its ack timeout cb). If 17-20s
            // disconnect cycle persists with these settings, the
            // close is coming from lwIP TCP retransmit exhaustion
            // or the AP itself, not from any WS-server-side timer.
            // Liveness backstop on the console side is the
            // links2004 heartbeat (5/5/6 = 30s) + the app-level
            // text-stale check (90s).
            client->keepAlivePeriod(60);
            if (client->client()) {
                client->client()->setAckTimeout(120000);
            }
            _clientCount++;
            _lastIp = client->remoteIP().toString();
            // DIAGNOSTIC: track this id so tickStats can poll its queue.
            // If a previous active id is still set, we lost track of its
            // disconnect — log a hint so we know.
            if (_activeClientId && _activeClientId != client->id()) {
                dlog("ws-stats", "active-id roll #%u -> #%u (no DISCONNECT seen)",
                     (unsigned)_activeClientId, (unsigned)client->id());
            }
            _activeClientId = client->id();
            _connectMs      = millis();
            _peakQueueLen   = 0;
            _txAttempts     = 0;
            _txFullSeen     = 0;
            _statsSeq       = 0;
            Serial.printf("[Console] Client connected #%u from %s (total=%d)\n",
                          client->id(), _lastIp.c_str(), _clientCount);
            // Fire on EVERY connect, not just the 0→1 transition. The
            // 0→1 gate left a stale-count hole: when the console reboots
            // ungracefully, _clientCount stayed >0 and the new connection
            // arrived as 1→2 — never crossing the gate. The handshake
            // (ScaleVersion / CalibrationStatus / OtaPending) needs to
            // re-fire for the new client regardless.
            if (_onConnected) _onConnected(_lastIp);
            break;
        }
        case WS_EVT_DISCONNECT: {
            _clientCount = _clientCount > 0 ? _clientCount - 1 : 0;
            uint32_t age_ms = (_connectMs && client->id() == _activeClientId)
                                  ? millis() - _connectMs : 0;
            dlog("ws-stats", "DISCONN #%u age=%lums peak=%u txA=%u txF=%u rxF=%u",
                 (unsigned)client->id(),
                 (unsigned long)age_ms,
                 (unsigned)_peakQueueLen,
                 (unsigned)_txAttempts,
                 (unsigned)_txFullSeen,
                 (unsigned)_framesRx);
            if (client->id() == _activeClientId) _activeClientId = 0;
            Serial.printf("[Console] Client disconnected (total=%d)\n", _clientCount);
            if (_clientCount == 0 && _onDisconnected) _onDisconnected();
            break;
        }
        case WS_EVT_ERROR: {
            // The lib emits this when a WS-level error occurs (close
            // code > 1001 received from the peer, decoding failure, …).
            // Should be rare; if it correlates with the disconnect
            // cycle, we have our smoking gun.
            uint16_t code = arg ? *(uint16_t*)arg : 0;
            dlog("ws-evt", "ERROR #%u code=%u len=%u",
                 (unsigned)client->id(), (unsigned)code, (unsigned)len);
            break;
        }
        case WS_EVT_PONG: {
            // Server-side PONGs from the client (reply to our keepAlive
            // ping). Console is the WS *client* in our setup so we don't
            // expect many — but track them in case the lib starts emitting
            // server keepalive pings.
            dlog("ws-evt", "PONG #%u len=%u", (unsigned)client->id(), (unsigned)len);
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
