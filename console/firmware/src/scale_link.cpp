#include "scale_link.h"
#include "config.h"
#include "ssdp_hub.h"
#include "scale_secrets.h"
#include <Preferences.h>
#include <WiFi.h>

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

void ScaleLink::update() {
    if (WiFi.status() != WL_CONNECTED || !_have_scale) return;

    if (!_wsStarted) {
        _wsStarted = true;
        _connect();
    }

    // Drive the WS state machine FIRST so any pending events (most importantly
    // CONNECTED) get processed before we decide whether the link is down.
    // Order matters: when the bambu MQTT connect blocks the main loop for ~30 s,
    // a WS handshake response can sit in TCP buffers across that whole window;
    // running ws.loop() up here drains it on the very next tick instead of
    // letting the staleness/SSDP-kick checks below fire on a stale snapshot.
    _ws.loop();

    // Staleness short-circuit: the library's heartbeat-based disconnect can
    // stall for ~60 s when the scale dies without a clean RST (clientDisconnect
    // blocks in flush()/stop() while lwIP retransmits give up). _lastWsEventMs
    // ticks on every event coming from the scale (CONNECTED, TEXT, PING, PONG),
    // so if it hasn't moved in kStaleMs the link is effectively dead — flip our
    // own state now and let the library catch up in the background.
    if (_connected && _lastWsEventMs &&
        (millis() - _lastWsEventMs) > kStaleMs) {
        _markDisconnected("stale");
    }

    // Drain the SSDP-driven reconnect flag set on AsyncUDP's task. Doing
    // the actual _ws mutation here keeps all WebSocketsClient access on
    // the main loop, which the library is not thread-safe against. The
    // guard window prevents us from killing an in-flight connect before
    // the WS upgrade has had time to complete (a bambu stall pushes that
    // out by tens of seconds).
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

void ScaleLink::_connect() {
    Serial.printf("[ScaleLink] Connecting ws://%s:%u%s\n",
                  _scaleIp.toString().c_str(), SCALE_WS_PORT, SCALE_WS_PATH);
    _lastConnectMs = millis();
    _ws.begin(_scaleIp.toString(), SCALE_WS_PORT, SCALE_WS_PATH);
    _ws.onEvent([this](WStype_t t, uint8_t* p, size_t l) { _onWsEvent(t, p, l); });
    _ws.setReconnectInterval(3000);
    // Ping every 5 s; drop connection if no pong within 3 s (after 2 failures).
    // This detects a scale that has crashed or rebooted without a clean TCP close.
    _ws.enableHeartbeat(5000, 3000, 2);
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

    switch (type) {
        case WStype_CONNECTED:
            _connected = true;
            Serial.println("[ScaleLink] Connected");
            if (_onConnect) _onConnect();
            // Re-derive handshake now that we're connected — this is where
            // the LCD + web status flip from red "offline" to amber/green.
            _refreshHandshakeState();
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
        default:
            break;  // PN532Status, ButtonPressed, Term
    }
}

void ScaleLink::_send(String frame) {
    if (!_connected) {
        Serial.println("[ScaleLink] _send: not connected, dropping");
        return;
    }
    _ws.sendTXT(frame);
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
