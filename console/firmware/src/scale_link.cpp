#include "scale_link.h"
#include "config.h"
#include "ssdp_hub.h"
#include "scale_secrets.h"
#include "web_server.h"           // g_web.pushScaleLink()
#include <Preferences.h>
#include <WiFi.h>
#include "spoolhard/serial_mirror.h"

void ScaleLink::_refreshHandshakeState() {
    Handshake before = _handshake;
    if (!_connected) {
        _handshake = Handshake::Disconnected;
    } else {
        // Look up the per-scale secret from the NVS JSON map keyed by scale
        // name. The legacy single-secret NVS key is intentionally ignored:
        // it was parallel state that diverged from what the web handlers
        // wrote, which made secrets "disappear" on reboot even though they
        // were actually still in the map.
        String secret = ScaleSecrets::get(_scaleName);
        // Placeholder: the real HMAC handshake is future work. For now we
        // mark as Encrypted iff a secret is stored locally (forward-
        // compatible — callers reading handshake() will seamlessly see
        // Failed instead once the real negotiation lands).
        _handshake = secret.isEmpty() ? Handshake::Unencrypted : Handshake::Encrypted;
    }
    if (before != _handshake && _onHandshake) {
        _onHandshake(_handshake, _scaleName);
    }
    // Push to WS clients on every handshake refresh — covers connect,
    // disconnect, and key-pair changes. Also fires on no-op refreshes
    // but the rate gate in broadcastState (no entry for `scale_link` =
    // no gate, edge-only producer) means each call results in one push.
    g_web.pushScaleLink();
}

void ScaleLink::begin() {
    // Load last-known scale (if any).
    Preferences prefs;
    prefs.begin(NVS_NS_SCALE, true);
    String ipStr = prefs.getString(NVS_KEY_SCALE_IP, "");
    _scaleName   = prefs.getString(NVS_KEY_SCALE_NAME, "");
    prefs.end();
    if (ipStr.length() && _scaleIp.fromString(ipStr)) {
        _have_scale = true;
        Serial.printf("[ScaleLink] Loaded scale %s @ %s\n",
                      _scaleName.c_str(), _scaleIp.toString().c_str());
    }

    g_ssdp_1990.subscribe([this](const SsdpListener::Announce& a) {
        if (a.urn != SCALE_SSDP_URN) return;
        // Once paired, ignore announcements from any other scale on the LAN —
        // we're not opportunistically switching scales mid-session.
        if (!_scaleName.isEmpty() && a.usn != _scaleName) return;

        bool ipChanged   = !_have_scale || _scaleIp != a.ip;
        bool nameChanged = _scaleName != a.usn;
        if (ipChanged || nameChanged) {
            _scaleIp   = a.ip;
            _scaleName = a.usn;
            _have_scale = true;
            Preferences prefs;
            prefs.begin(NVS_NS_SCALE, false);
            prefs.putString(NVS_KEY_SCALE_IP, _scaleIp.toString());
            prefs.putString(NVS_KEY_SCALE_NAME, _scaleName);
            prefs.end();
            Serial.printf("[ScaleLink] SSDP: %s @ %s\n",
                          _scaleName.c_str(), _scaleIp.toString().c_str());
        }

        // Treat every NOTIFY from our paired scale as a liveness signal. If
        // the IP moved OR the WS thinks it's down, request a reconnect —
        // but only flag it: this callback runs on AsyncUDP's task and
        // mutating _ws from here races with _ws.loop() on the main loop
        // (which was tearing down in-flight connections every 5 s).
        if ((ipChanged || !_connected) && _wsStarted) {
            _ssdpKickReconnect = true;
        }
    });

    // Hook protocol parser's rx tap for debug visibility (optional).
    ScaleToConsole::setRxTap(nullptr);
}

// Public legacy entry point. The work moved into the dedicated
// _task once beginTask() is called; main.cpp's loop() may still
// invoke this for backwards compat, but it's a no-op when the task
// is running. If the task hasn't been started yet (boot ordering),
// fall through to the synchronous tick so the link still progresses.
void ScaleLink::update() {
    if (_task) return;          // task is running — nothing to do here
    _taskTick();
}

void ScaleLink::beginTask() {
    if (_task) return;
    _sendQueue = xQueueCreate(kSendQueueDepth, sizeof(String*));
    if (!_sendQueue) {
        Serial.println("[ScaleLink] xQueueCreate failed");
        return;
    }
    BaseType_t r = xTaskCreatePinnedToCore(
        _taskBody, "scale_link", /*stack*/ 6 * 1024,
        this, /*priority*/ 4, &_task, /*core*/ 1);
    if (r != pdPASS) {
        Serial.println("[ScaleLink] xTaskCreate failed");
        _task = nullptr;
        return;
    }
    Serial.println("[ScaleLink] task started — _ws now owned by scale_link task");
}

void ScaleLink::_taskBody(void* arg) {
    auto* self = static_cast<ScaleLink*>(arg);
    Serial.println("[ScaleLink] task running");
    // Periodic alive-tick log — proves the task is scheduled even when
    // _ws.loop() has nothing to emit. If we go silent on /api/logs for
    // longer than the tick interval, the task itself is being starved.
    uint32_t s_iter = 0;
    uint32_t s_lastAliveMs = millis();
    for (;;) {
        self->_taskTick();
        s_iter++;
        uint32_t now = millis();
        if (now - s_lastAliveMs >= 5000) {
            Serial.printf("[ScaleLink-task] alive iter=%u "
                          "iter_per_s=%u heap=%u\n",
                          (unsigned)s_iter,
                          (unsigned)(s_iter / 5),
                          (unsigned)ESP.getFreeHeap());
            s_iter = 0;
            s_lastAliveMs = now;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void ScaleLink::_taskTick() {
    if (WiFi.status() != WL_CONNECTED || !_have_scale) return;

    if (!_wsStarted) {
        _wsStarted = true;
        _connect();
    }

    // Outbound first — we want sends initiated this tick to actually
    // hit the wire before the next _ws.loop() iteration. _send() now
    // pushes onto _sendQueue; the actual sendTXT() runs here only.
    _drainSendQueue();

    // Force-reconnect from forceReconnect() — owner-thread teardown.
    if (_forceReconnectPending.exchange(false)) {
        Serial.println("[ScaleLink] forceReconnect — tearing down WS so the next "
                       "auto-reconnect picks up the fresh secret");
        _ws.disconnect();
        _markDisconnected("secret-changed");
    }

    // Drive the WS state machine. Was running on main loop pre-task
    // split; now running at ~500 Hz so PINGs always get answered on
    // time regardless of bambu/FTPS/MQTT activity on other tasks.
    _ws.loop();

    // Staleness short-circuit. Gates on TEXT-frame freshness, not
    // any-event freshness — see kStaleMs comment in scale_link.h.
    if (_connected) {
        uint32_t baseline = _lastWsTextMs ? _lastWsTextMs : _lastConnectMs;
        if (baseline && (millis() - baseline) > kStaleMs) {
            _markDisconnected("text-stale");
        }
    }

    // SSDP-driven reconnect. Flag is set on AsyncUDP's task; consumed
    // here so all _ws mutation stays on the owning thread.
    if (_ssdpKickReconnect) {
        _ssdpKickReconnect = false;
        if (_wsStarted && !_connected &&
            (millis() - _lastConnectMs) > kReconnectGuardMs) {
            Serial.println("[ScaleLink] SSDP-driven reconnect");
            _ws.disconnect();
            _wsStarted = false;
        }
    }

    ScaleToConsole::Message msg;
    while (ScaleToConsole::receive(msg)) {
        _dispatch(msg);
    }
}

void ScaleLink::_drainSendQueue() {
    if (!_sendQueue) return;
    String* frame = nullptr;
    while (xQueueReceive(_sendQueue, &frame, 0) == pdTRUE && frame) {
        if (_connected) {
            _ws.sendTXT(*frame);
        } else {
            // Drop on the floor — caller's contract is "best-effort
            // when up". The retry-on-reconnect path is tag-replay etc.,
            // already handled at the application layer.
        }
        delete frame;
    }
}

void ScaleLink::forceReconnect() {
    // Owner-thread-only call — the actual _ws.disconnect() runs on the
    // scale_link task next iteration. Setting an atomic flag is safe
    // from any thread (HTTP handlers, main loop). Saves the user
    // surprise of "I called forceReconnect from AsyncTCP and the lib
    // crashed mid-loop" — links2004 isn't safe against concurrent
    // calls from multiple tasks.
    _forceReconnectPending.store(true, std::memory_order_release);
}

void ScaleLink::_connect() {
    // Append `?key=<secret>` to the WS path so the scale's
    // wsAuthHandshake gate accepts the upgrade. The secret is the
    // same value as the scale's NVS `wifi_cfg/fixed_key`; the user
    // mirrors it into the console via the per-scale Security UI
    // (POST /api/scale-secret, stored in `scale_cfg/secrets_json`).
    //
    // If no secret is stored, we connect with the bare path. The
    // scale will accept this iff its fixed_key is unset / still the
    // ship default — matching the dashboard's pre-pairing behaviour.
    // URL-encode the secret because the user might pick characters
    // (`+`, `&`, `=`, space) that would otherwise break parsing.
    String url = SCALE_WS_PATH;
    String secret = ScaleSecrets::get(_scaleName);
    if (!secret.isEmpty()) {
        url += "?key=";
        // Tiny URL-encode: only escape characters that affect query
        // parsing. Most fixed-key values are alphanumeric so this
        // typically reserves nothing.
        for (size_t i = 0; i < secret.length(); ++i) {
            char c = secret[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                url += c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
                url += buf;
            }
        }
    }
    Serial.printf("[ScaleLink] Connecting ws://%s:%u%s%s\n",
                  _scaleIp.toString().c_str(), SCALE_WS_PORT, SCALE_WS_PATH,
                  secret.isEmpty() ? " (no key)" : " (auth)");
    _lastConnectMs = millis();
    _ws.begin(_scaleIp.toString(), SCALE_WS_PORT, url);
    _ws.onEvent([this](WStype_t t, uint8_t* p, size_t l) { _onWsEvent(t, p, l); });
    _ws.setReconnectInterval(3000);
    // Forgiving ping — every 5 s, 5 s pong window, 6 misses before
    // disconnect (~30 s detection horizon). The scale's main loop
    // occasionally stalls 3-4 s on AsyncTCP / mbedtls work that's
    // pinned to the same core as the WS server — at the previous
    // 5/5/3 setting (~15 s) those stalls were enough to lose 3 pongs
    // in a row and tear the link down. With 6 misses we tolerate up
    // to 30 s of pong silence; the application-side _lastWsTextMs /
    // kStaleMs (90 s) check is the backstop for genuinely dead links
    // where the lib still thinks pongs are flowing. The scale's
    // 1.5 s Heartbeat over TEXT is what ticks _lastWsTextMs in
    // practice, so a truly wedged link still trips within 90 s.
    _ws.enableHeartbeat(5000, 5000, 6);
}

void ScaleLink::_onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    static const char* kNames[] = {
        "ERROR","DISCONNECTED","CONNECTED","TEXT","BIN",
        "FRAG_TEXT_START","FRAG_BIN_START","FRAGMENT","FRAGMENT_FIN",
        "PING","PONG"
    };
    const char* tname = ((int)type >= 0 && (int)type < (int)(sizeof(kNames)/sizeof(*kNames)))
                        ? kNames[(int)type] : "?";
    uint32_t now = millis();
    uint32_t dt = _lastWsEventMs ? (now - _lastWsEventMs) : 0;
    Serial.printf("[ScaleLink][ws] t=%s len=%u dt=%lums heap=%u\n",
                  tname, (unsigned)length, (unsigned long)dt,
                  (unsigned)ESP.getFreeHeap());
    _lastWsEventMs = now;
    // Only TEXT/BIN frames count as "real" traffic for the stale
    // detector — PING/PONG keep the TCP connection alive but don't
    // tell us the scale-side app is actually pushing data.
    if (type == WStype_TEXT || type == WStype_BIN) {
        _lastWsTextMs = now;
    }

    switch (type) {
        case WStype_CONNECTED:
            _connected = true;
            Serial.println("[ScaleLink] Connected");
            if (_onConnect) _onConnect();
            // Re-derive handshake now that we're connected — this is where
            // the LCD + web status flip from red "offline" to amber/green.
            _refreshHandshakeState();
            // Kick the scale's OTA checker — but throttle. Sending
            // CheckOtaUpdates on EVERY reconnect was a death loop:
            // each kick triggers an HTTPS manifest fetch on the scale
            // (~60 KB temp heap, ~3-4 s of mbedtls work). On core 0
            // alongside AsyncTCP, that scheduling pressure delayed
            // Pongs for the 5/5/3 heartbeat window, the console then
            // detected the link dead and reconnected — which fired
            // another CheckOtaUpdates — etc. We saw 17-30 s connect
            // cycles indefinitely. Now: send only if it's been at
            // least 5 minutes since the last kick. The scale also has
            // its own scheduled checker on a 24 h interval, so missing
            // a kick on a flapping reconnect is harmless.
            {
                uint32_t now = millis();
                if (_lastOtaKickMs == 0 ||
                    (now - _lastOtaKickMs) > 5UL * 60UL * 1000UL) {
                    _lastOtaKickMs = now;
                    _send(ConsoleToScale::build(ConsoleToScale::Type::CheckOtaUpdates));
                } else {
                    Serial.printf("[ScaleLink] Skip CheckOtaUpdates kick "
                                  "(last %lus ago)\n",
                                  (unsigned long)((now - _lastOtaKickMs) / 1000));
                }
            }
            break;
        case WStype_DISCONNECTED:
            _markDisconnected("ws-event");
            break;
        case WStype_TEXT: {
            String s;
            s.reserve(length + 1);
            for (size_t i = 0; i < length; i++) s += (char)payload[i];
            ScaleToConsole::deliver(s);
            break;
        }
        case WStype_ERROR:
            if (length && payload) {
                Serial.printf("[ScaleLink][ws] ERROR payload=%.*s\n",
                              (int)length, (const char*)payload);
            }
            break;
        default:
            break;
    }
}

void ScaleLink::_markDisconnected(const char* reason) {
    if (!_connected) return;
    Serial.printf("[ScaleLink] Disconnected (%s)\n", reason);
    _connected = false;
    // Reset the text-freshness tracker so the next session starts
    // measuring from its own _lastConnectMs baseline rather than
    // inheriting a stale timestamp from the previous one.
    _lastWsTextMs = 0;
    // Keep the cached OtaPending across disconnects. Earlier code wiped
    // the cache on every disconnect, which meant a link flap (the WS
    // bounces every few minutes under load) reset the frontend to
    // "scale waiting" until the scale's next push landed — sometimes
    // many seconds. Versions almost never change on a flap, so showing
    // the last-known snapshot with the offline pill is strictly more
    // useful than blanking it. The scale force-pushes a fresh
    // OtaPending on every reconnect (g_pendingPushPending), so any
    // genuine version change updates promptly.
    //
    // The in-flight tracker DOES clear — a half-completed scale OTA
    // can't be resumed across a link drop, so showing "60% installing"
    // when the link is down would be a lie.
    _scaleOtaInFlight = ScaleOtaInFlight{};
    if (_onDisconnect) _onDisconnect();
    _refreshHandshakeState();
}

void ScaleLink::_recordEvent(const char* kind, const String& detail) {
    _lastEventMs = millis();
    _lastEventKind = kind;
    _lastEventDetail = detail;
    // Every dispatched event (weight, tag, ota, calibration, version)
    // funnels through here and updates last_event_*. Push the new
    // scale_link snapshot so the dashboard's StatsRow + scale-link
    // panel update without polling. Edge-only — no rate gate needed.
    g_web.pushScaleLink();
}

void ScaleLink::_dispatch(const ScaleToConsole::Message& msg) {
    using T = ScaleToConsole::Type;
    switch (msg.type) {
        case T::NewLoad:
        case T::LoadChangedStable:
        case T::LoadChangedUnstable: {
            // Post-refactor wire format: a float tuple so the scale's
            // configured decimal precision survives the trip. Legacy
            // firmware used int32 here — `as<float>()` handles both
            // transparently.
            float grams = msg.doc.as<float>();
            const char* state =
                (msg.type == T::NewLoad)             ? "new" :
                (msg.type == T::LoadChangedStable)   ? "stable" :
                                                        "unstable";
            _lastWeightG     = grams;
            _lastWeightState = state;
            _lastWeightMs    = millis();
            if (_onWeight) _onWeight(grams, state);
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f g (%s)", grams, state);
            _recordEvent("weight", buf);
            break;
        }
        case T::LoadRemoved:
            _lastWeightG     = 0.f;
            _lastWeightState = "removed";
            _lastWeightMs    = millis();
            if (_onWeight) _onWeight(0.f, "removed");
            _recordEvent("weight", "removed");
            break;
        case T::Uncalibrated:
            _lastWeightG     = 0.f;
            _lastWeightState = "uncalibrated";
            _lastWeightMs    = millis();
            if (_onWeight) _onWeight(0.f, "uncalibrated");
            _recordEvent("uncalibrated", "scale needs calibration");
            break;
        case T::CurrentWeight: {
            // Reply to requestCurrentWeight(): the scale sends its raw
            // WeightState name so we re-map it here into the short tokens the
            // rest of the console expects ("stable"/"unstable"/...). Anything
            // that means "no load" collapses to "removed" with 0 g so the
            // "Scale: -- (empty)" label renders cleanly.
            float grams           = msg.doc["weight_g"] | 0.f;
            const char* rawState  = msg.doc["state"]    | "";
            const char* state     = "unknown";
            if      (!strcmp(rawState, "StableLoad") ||
                     !strcmp(rawState, "LoadChangedStable"))   state = "stable";
            else if (!strcmp(rawState, "NewLoad"))             state = "new";
            else if (!strcmp(rawState, "LoadChangedUnstable")) state = "unstable";
            else if (!strcmp(rawState, "LoadRemoved") ||
                     !strcmp(rawState, "Idle"))                { state = "removed";      grams = 0.f; }
            else if (!strcmp(rawState, "Uncalibrated"))        { state = "uncalibrated"; grams = 0.f; }
            if (msg.doc["precision"].is<int>()) {
                int p = msg.doc["precision"].as<int>();
                if (p >= 0 && p <= 4) _scalePrecision = p;
            }
            _lastWeightG     = grams;
            _lastWeightState = state;
            _lastWeightMs    = millis();
            if (_onWeight) _onWeight(grams, state);
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f g (%s)", grams, state);
            _recordEvent("weight", buf);
            break;
        }
        case T::TagStatus: {
            // {"TagStatus": {status, uid[], url, is_bambulab}}
            String uid;
            if (msg.doc["uid"].is<JsonArrayConst>()) {
                for (JsonVariantConst v : msg.doc["uid"].as<JsonArrayConst>()) {
                    char buf[4];
                    snprintf(buf, sizeof(buf), "%02X", v.as<uint8_t>());
                    uid += buf;
                }
            }
            const char* url = msg.doc["url"] | "";
            bool bambu = msg.doc["is_bambulab"] | false;
            if (_onTag) _onTag(uid.c_str(), url, bambu);
            String detail = uid.length() ? ("tag " + uid) : "no tag";
            if (bambu) detail += " (bambu)";
            _recordEvent("tag", detail);
            break;
        }
        case T::ButtonPressed:
            _recordEvent("button", "pressed");
            if (_onButton) _onButton();
            break;
        case T::OtaPending: {
            // Phase-5 push from the scale: cache the snapshot so the
            // console's /api/ota-status can serve it as the "scale" block.
            _scaleOta.valid             = true;
            _scaleOta.firmware_current  = msg.doc["firmware_current"]  | "";
            _scaleOta.firmware_latest   = msg.doc["firmware_latest"]   | "";
            _scaleOta.frontend_current  = msg.doc["frontend_current"]  | "";
            _scaleOta.frontend_latest   = msg.doc["frontend_latest"]   | "";
            _scaleOta.firmware_update   = msg.doc["firmware_update"]   | false;
            _scaleOta.frontend_update   = msg.doc["frontend_update"]   | false;
            _scaleOta.last_check_ts     = msg.doc["last_check_ts"]     | 0u;
            _scaleOta.last_check_status = msg.doc["last_check_status"] | "";
            _scaleOta.received_ms       = millis();
            Serial.printf("[ScaleLink] OtaPending: fw %s→%s (%s) fe %s→%s (%s)\n",
                _scaleOta.firmware_current.c_str(),
                _scaleOta.firmware_latest.c_str(),
                _scaleOta.firmware_update ? "update" : "ok",
                _scaleOta.frontend_current.c_str(),
                _scaleOta.frontend_latest.c_str(),
                _scaleOta.frontend_update ? "update" : "ok");
            _recordEvent("ota", _scaleOta.firmware_update || _scaleOta.frontend_update
                                   ? "update available"
                                   : "up to date");
            break;
        }
        case T::ScaleVersion: {
            const char* v = msg.doc["version"] | "";
            if (v && *v) _scaleFirmwareVersion = v;
            // The scale also includes its current display-precision here so
            // the console can render the live weight with the same number
            // of decimals the scale's own screen would show. Re-emitted by
            // the scale whenever the user saves a new value on its config
            // page, so this stays in sync without polling.
            bool precisionChanged = false;
            if (msg.doc["precision"].is<int>()) {
                int p = msg.doc["precision"].as<int>();
                if (p < 0) p = 0;
                if (p > 4) p = 4;
                if (p != _scalePrecision) {
                    _scalePrecision = p;
                    precisionChanged = true;
                }
            }
            Serial.printf("[ScaleLink] Scale version: %s  precision: %d\n",
                          v, _scalePrecision);
            _recordEvent("version", String("scale fw ") + v);
            // When the user changed the decimal setting mid-run, the scale
            // doesn't generate a new weight event (the load hasn't moved)
            // so the home-screen weight card stays stuck on the old format
            // until something else disturbs the scale. Replay the cached
            // reading through _onWeight so the UI re-renders with the new
            // precision immediately.
            if (precisionChanged && _onWeight && _lastWeightMs > 0) {
                _onWeight(_lastWeightG, _lastWeightState.c_str());
            }
            break;
        }
        case T::CalibrationStatus: {
            // {"CalibrationStatus": {"num_points": N, "tare_raw": R}}
            // Pushed once on console-connect and after every tare /
            // addCalPoint / clearCalPoints on the scale. The LCD's
            // scale-settings screen renders "Calibration: N points"
            // straight from the cached values; main.cpp also forwards
            // the event into the scale-settings screen via
            // onCalibrationStatus().
            int     n = msg.doc["num_points"] | 0;
            int32_t r = msg.doc["tare_raw"]   | 0;
            _calNumPoints = n;
            _calTareRaw   = r;
            if (_onCalStatus) _onCalStatus(n, r);
            char buf[48];
            snprintf(buf, sizeof(buf), "%d point%s, tare_raw=%ld",
                     n, n == 1 ? "" : "s", (long)r);
            _recordEvent("calibration", buf);
            break;
        }
        case T::OtaProgressUpdate: {
            // Wire shape: {"OtaProgressUpdate": {"Status": {text, percent, kind}}}
            // We read the structured fields when present; the human-
            // readable `text` is also surfaced in the recorded event.
            JsonVariantConst st = msg.doc["Status"];
            if (!st.is<JsonObject>()) break;
            int pct = st["percent"] | -1;
            const char* kind = st["kind"] | "";
            if (pct >= 0) {
                _scaleOtaInFlight.valid          = true;
                _scaleOtaInFlight.kind           = kind;
                _scaleOtaInFlight.percent        = pct;
                _scaleOtaInFlight.last_update_ms = millis();
            }
            const char* text = st["text"] | "";
            if (text && *text) _recordEvent("ota", text);
            break;
        }
        case T::Heartbeat: {
            // Periodic 5 s tick. Cache uptime + heap so the dashboard's
            // /api/scale-link payload can render "scale up for 3d 4h"
            // beside the connection badge. NOT logged via _recordEvent
            // — that fires the WS push to browser clients, and at 5 s
            // cadence it would just be noise. The fields show up in the
            // periodic scale_link heartbeat the console-side already
            // emits (see g_web.pushScaleLink in main.cpp's slow tick).
            _scaleUptimeS       = msg.doc["uptime_s"]      | 0u;
            _scaleFreeHeap      = msg.doc["free_heap"]     | 0u;
            _scaleMinFreeHeap   = msg.doc["min_free_heap"] | 0u;
            _scaleHeartbeatRxMs = millis();
            break;
        }
        default:
            break;  // PN532Status, ButtonPressed, Term
    }
}

void ScaleLink::_send(String frame) {
    // Producer-side: any thread (main loop, HTTP, AsyncTCP) can call.
    // Heap-allocate the String so we can hand a pointer through the
    // FreeRTOS queue without invoking String's copy semantics from
    // the lib's task. The scale_link task drains and calls
    // _ws.sendTXT() — single owner of the WS object.
    //
    // We accept frames even while disconnected and drop them on
    // dequeue. The queue is small (16) and `_drainSendQueue` runs
    // hot, so the worst case is a tiny burst of orphaned frames
    // immediately after disconnect — they're freed promptly.
    if (!_sendQueue) {
        // Pre-task fallback — happens once at boot before beginTask().
        if (_connected) _ws.sendTXT(frame);
        return;
    }
    auto* p = new String(std::move(frame));
    if (xQueueSend(_sendQueue, &p, pdMS_TO_TICKS(10)) != pdTRUE) {
        Serial.println("[ScaleLink] send queue full — frame dropped");
        delete p;
    }
}

void ScaleLink::tare() {
    _send(ConsoleToScale::buildTuple(ConsoleToScale::Type::Calibrate, (int32_t)0));
}

void ScaleLink::calibrate(int32_t known_weight) {
    _send(ConsoleToScale::buildTuple(ConsoleToScale::Type::Calibrate, known_weight));
}

void ScaleLink::addCalPoint(int32_t known_weight) {
    _send(ConsoleToScale::buildTuple(ConsoleToScale::Type::AddCalPoint, known_weight));
}

void ScaleLink::clearCalPoints() {
    _send(ConsoleToScale::build(ConsoleToScale::Type::ClearCalPoints));
}

void ScaleLink::readTag() {
    _send(ConsoleToScale::build(ConsoleToScale::Type::ReadTag));
}

void ScaleLink::writeTag(const String& text, const String& uidHex) {
    JsonDocument inner;
    inner["text"] = text;
    JsonArray uid = inner["check_uid"].to<JsonArray>();
    for (size_t i = 0; i + 1 < uidHex.length(); i += 2) {
        uid.add((uint8_t)strtoul(uidHex.substring(i, i + 2).c_str(), nullptr, 16));
    }
    inner["cookie"] = "";
    _send(ConsoleToScale::build(ConsoleToScale::Type::WriteTag, inner));
}

void ScaleLink::emulateTag(const String& url) {
    JsonDocument inner;
    inner["url"] = url;
    _send(ConsoleToScale::build(ConsoleToScale::Type::EmulateTag, inner));
}

void ScaleLink::requestCurrentWeight() {
    _send(ConsoleToScale::build(ConsoleToScale::Type::GetCurrentWeight));
}

void ScaleLink::sendButtonResponse(bool ok) {
    _send(ConsoleToScale::buildTuple(ConsoleToScale::Type::ButtonResponse, ok));
}

void ScaleLink::requestScaleOtaUpdate() {
    if (!_connected) {
        Serial.println("[ScaleLink] requestScaleOtaUpdate: not connected, dropping");
        return;
    }
    _send(ConsoleToScale::build(ConsoleToScale::Type::RunOtaUpdate));
}

void ScaleLink::requestScaleOtaCheck() {
    if (!_connected) {
        Serial.println("[ScaleLink] requestScaleOtaCheck: not connected, dropping");
        return;
    }
    _send(ConsoleToScale::build(ConsoleToScale::Type::CheckOtaUpdates));
}

void ScaleLink::pushGcodeAnalysis(const String& printer_serial, float total_grams,
                                  const JsonDocument& tools) {
    if (!_connected) {
        Serial.println("[ScaleLink] pushGcodeAnalysis: not connected, dropping");
        return;
    }
    JsonDocument inner;
    inner["printer"]     = printer_serial;
    inner["total_grams"] = total_grams;
    // Copy the tools array verbatim.
    inner["tools"].set(tools.as<JsonArrayConst>());
    _send(ConsoleToScale::build(ConsoleToScale::Type::GcodeAnalysisNotify, inner));
}
