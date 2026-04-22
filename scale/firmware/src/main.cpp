#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "load_cell.h"
#include "nfc_reader.h"
#include "rgb_led.h"
#include "protocol.h"
#include "console_channel.h"
#include "web_server.h"
#include "wifi_provisioning.h"
#include "spoolhard/ota.h"
#include <SPIFFS.h>

// ── Globals ──────────────────────────────────────────────────
static LoadCell        g_scale;
static NfcReader       g_nfc;
static RgbLed          g_led;
static ScaleWebServer  g_web;
static WifiProvisioning g_wifi;

static WeightState   g_lastWeightState = WeightState::Uncalibrated;
static unsigned long g_lastRawSent      = 0;
static bool          g_pendingOta       = false;
// Deadline (millis) up to which an HTTP upload is considered "in flight".
// Refreshed on every chunk via web_server's onUploadProgress callback so the
// LED can stay on the amber pulse for the duration of the upload — without
// this, updateLed() would overwrite the pulse the moment the next loop tick
// runs after onUploadStarted fired its single one-shot.
static unsigned long g_uploadActiveUntil = 0;
static constexpr unsigned long UPLOAD_LIVENESS_MS = 3000;

// Feature-button state. GPIO 0 is active-LOW through an internal pull-up.
// We fire ButtonPressed on the FALLING edge (HIGH → LOW) so a tap registers
// within one loop tick — the old "wait 30 ms stable" path meant the user
// had to hold the button for however long it took the next loop iteration
// to come around, which on this firmware can be hundreds of ms on a busy
// tick. A single repeat-guard window covers both contact bounces and the
// user double-tapping within the same capture window.
static int           g_btnLastLevel     = HIGH;
static unsigned long g_btnLastSentMs    = 0;
// Deadline by which we expect a `ButtonResponse` after firing a
// `ButtonPressed`. 0 = no response pending. If we hit the deadline without
// the console replying we fire the ackCaptureFail burst ourselves so the
// user gets the same "didn't land" feedback they'd get from an explicit
// rejection.
static unsigned long g_btnResponseDeadline = 0;
static constexpr unsigned long BTN_REPEAT_GUARD_MS    = 400;
static constexpr unsigned long BTN_RESPONSE_TIMEOUT_MS = 2000;

// Forward-declared so GetCurrentWeight can include the state name in its reply
// without reordering the whole file.
static const char* weightStateName(WeightState s);

// ── OTA pending-state push ──────────────────────────────────
//
// The scale's OtaChecker (manifest fetch + version compare) lives in the
// shared spoolhard_core library; we drive it here. Every loop tick we
// peek at the cached pending state and, if it has changed since the last
// push, send an OtaPending frame to the console so its combined "updates
// available" banner stays in sync without polling.
//
// The hash is a cheap concatenation of the fields that matter — the only
// purpose is change detection, not security.
static String g_lastPendingHash;
static bool   g_pendingPushPending = false;  // force-push (e.g. on connect)

static String _pendingHash(const OtaPending& p, uint32_t lastTs, const String& lastSt) {
    String s;
    s.reserve(96);
    s += p.firmware_current; s += '|'; s += p.firmware_latest; s += '|';
    s += p.frontend_current; s += '|'; s += p.frontend_latest; s += '|';
    s += (p.firmware ? '1' : '0');
    s += (p.frontend ? '1' : '0');
    s += '|'; s += String(lastTs);
    s += '|'; s += lastSt;
    return s;
}

static void pushOtaPendingIfChanged(bool force) {
    if (!g_console.isConnected()) return;

    auto p       = g_ota_checker.pending();
    uint32_t ts  = g_ota_checker.lastCheckTs();
    const String& st = g_ota_checker.lastStatus();

    String h = _pendingHash(p, ts, st);
    if (!force && h == g_lastPendingHash) return;
    g_lastPendingHash = h;

    JsonDocument doc;
    doc["firmware_current"]  = p.firmware_current;
    doc["firmware_latest"]   = p.firmware_latest;
    doc["frontend_current"]  = p.frontend_current;
    doc["frontend_latest"]   = p.frontend_latest;
    doc["firmware_update"]   = p.firmware;
    doc["frontend_update"]   = p.frontend;
    doc["last_check_ts"]     = ts;
    doc["last_check_status"] = st;
    ScaleToConsole::send(ScaleToConsole::Type::OtaPending, doc);
}

// ── Console protocol ─────────────────────────────────────────
static void handleConsoleMessage(ConsoleToScale::Message& msg) {
    using T = ConsoleToScale::Type;

    switch (msg.type) {
        case T::Calibrate: {
            // {"Calibrate": i32} — 0 means tare, nonzero means calibrate with known weight
            int32_t weight = msg.doc.as<int32_t>();
            if (weight == 0) {
                Serial.println("[App] Tare (Calibrate 0)");
                g_scale.tare();
            } else {
                Serial.printf("[App] Calibrate with known weight=%ldg\n", (long)weight);
                g_scale.calibrate((float)weight);
            }
            break;
        }
        case T::ButtonResponse: {
            bool ok = msg.doc.as<bool>();
            Serial.printf("[App] ButtonResponse: %s\n", ok ? "ok" : "cancel");
            // Clear the pending-response deadline — the console answered in
            // time, we drive the ack from the answer itself.
            g_btnResponseDeadline = 0;
            if (ok) g_led.ackCaptureOk();
            else    g_led.ackCaptureFail();
            break;
        }
        case T::ReadTag: {
            // NfcReader reads continuously in update() and emits TagStatus
            // events on its own — nothing explicit to do here.
            Serial.println("[App] ReadTag (continuous reader — no-op)");
            break;
        }
        case T::WriteTag: {
            const char* text   = msg.doc["text"]   | "";
            const char* cookie = msg.doc["cookie"] | "";
            JsonArray uid_arr  = msg.doc["check_uid"].as<JsonArray>();
            uint8_t uid[7]; uint8_t uid_len = 0;
            for (JsonVariant v : uid_arr) uid[uid_len++] = v.as<uint8_t>();
            g_nfc.writeTag(uid, uid_len, text, cookie);
            break;
        }
        case T::EraseTag: {
            JsonArray uid_arr = msg.doc["check_uid"].as<JsonArray>();
            uint8_t uid[7]; uint8_t uid_len = 0;
            for (JsonVariant v : uid_arr) uid[uid_len++] = v.as<uint8_t>();
            g_nfc.eraseTag(uid, uid_len);
            break;
        }
        case T::EmulateTag: {
            const char* url = msg.doc["url"] | "";
            g_nfc.emulateTag(url);
            break;
        }
        case T::UpdateFirmware: {
            // Encrypted in real protocol — not yet decrypted here
            Serial.println("[App] UpdateFirmware received (encrypted payload, not yet handled)");
            g_pendingOta = true;
            break;
        }
        case T::RunOtaUpdate: {
            // Phase-5 trigger from the console. Same effect as the local
            // /api/ota-run on the scale's own web UI: queue an OTA run on
            // the next loop tick using the stored OtaConfig.
            Serial.println("[App] RunOtaUpdate received from console");
            g_pendingOta = true;
            break;
        }
        case T::CheckOtaUpdates: {
            // Console hit "Check now" — refresh our manifest immediately so
            // the next OtaPending push reflects the latest known versions.
            Serial.println("[App] CheckOtaUpdates received from console");
            g_ota_checker.kickNow();
            break;
        }
        case T::GetCurrentWeight: {
            // On-demand read — console asks for the current weight so its
            // UI can show a value without waiting for the next state
            // change. Send the full-precision float (not the rounded
            // display value) plus the scale's configured precision so the
            // console-side renderer can show `123.4 g` as the headline
            // and `123.456 g` in the muted full-precision fallback.
            JsonDocument out;
            out["weight_g"]  = g_scale.getWeightG();
            out["state"]     = weightStateName(g_scale.getState());
            out["precision"] = g_scale.params().precision;
            ScaleToConsole::send(ScaleToConsole::Type::CurrentWeight, out);
            break;
        }
        case T::TagsInStore: {
            String tags = msg.doc["tags"] | "";
            Serial.printf("[App] TagsInStore received (%d chars)\n", tags.length());

            // Persist to SPIFFS so it survives page reloads (not reboots — it's a cache)
            File f = SPIFFS.open("/tags_in_store.txt", FILE_WRITE);
            if (f) { f.print(tags); f.close(); }

            // Push to dashboard
            JsonDocument dbg;
            dbg["tags"] = tags;
            g_web.broadcastDebug("tags_in_store", dbg);
            break;
        }
        case T::RequestGcodeAnalysis: {
            // Only the console has the printer FTP credentials and the gcode
            // analyzer — the scale has no reason to be asked to analyse. Log
            // and drop.
            Serial.println("[App] RequestGcodeAnalysis ignored (analysis runs on console)");
            break;
        }
        case T::GcodeAnalysisNotify: {
            // Console completed a gcode analysis for one of its configured
            // printers. The scale doesn't hold a spool store so there's no
            // direct bookkeeping to update here — instead we forward the
            // payload onto the /ws debug channel so any live dashboard can
            // surface the per-tool forecast next to the current weight.
            String printer = msg.doc["printer"] | "";
            float  total   = msg.doc["total_grams"] | 0.f;
            JsonArrayConst tools = msg.doc["tools"].as<JsonArrayConst>();
            Serial.printf("[App] GcodeAnalysisNotify from %s: %.1fg across %d tools\n",
                          printer.c_str(), total, (int)tools.size());
            for (JsonVariantConst t : tools) {
                int    idx = t["tool_idx"] | -1;
                float  g   = t["grams"]    | 0.f;
                String sp  = t["spool_id"] | "";
                String mat = t["material"] | "";
                Serial.printf("  T%d: %.1fg spool=%s material=%s\n",
                              idx, g, sp.c_str(), mat.c_str());
            }
            g_web.broadcastDebug("gcode_analysis", msg.doc);
            break;
        }
        default:
            Serial.println("[App] Unknown ConsoleToScale message");
            break;
    }
}

static const char* weightStateName(WeightState s) {
    switch (s) {
        case WeightState::Uncalibrated:        return "Uncalibrated";
        case WeightState::Idle:                return "Idle";
        case WeightState::NewLoad:             return "NewLoad";
        case WeightState::StableLoad:          return "StableLoad";
        case WeightState::LoadChangedUnstable: return "LoadChangedUnstable";
        case WeightState::LoadChangedStable:   return "LoadChangedStable";
        case WeightState::LoadRemoved:         return "LoadRemoved";
        default:                               return "Unknown";
    }
}

static void sendWeightEvent(WeightState state) {
    using T = ScaleToConsole::Type;
    JsonDocument doc;
    // Send the raw float so the console can render at whatever precision
    // the user configured (see ScaleVersion/CurrentWeight `precision`
    // field). getDisplayWeight() would pre-round and throw away decimals.
    doc["weight_g"] = g_scale.getWeightG();

    switch (state) {
        case WeightState::Uncalibrated:
            ScaleToConsole::sendSimple(T::Uncalibrated);       break;
        case WeightState::NewLoad:
            // Transient: load just detected, not yet stable — send as unstable
            ScaleToConsole::send(T::LoadChangedUnstable, doc); break;
        case WeightState::StableLoad:
            // First stable reading after load placed — this is the real NewLoad
            ScaleToConsole::send(T::NewLoad, doc);             break;
        case WeightState::LoadChangedStable:
            ScaleToConsole::send(T::LoadChangedStable, doc);   break;
        case WeightState::LoadChangedUnstable:
            ScaleToConsole::send(T::LoadChangedUnstable, doc); break;
        case WeightState::LoadRemoved:
            ScaleToConsole::sendSimple(T::LoadRemoved);        break;
        default: break;
    }

    JsonDocument dbg;
    dbg["state"]    = weightStateName(state);
    dbg["weight_g"] = g_scale.getWeightG();
    g_web.broadcastDebug("weight_state", dbg);
}

static void sendNfcEvent() {
    JsonDocument doc;
    doc["status"] = (int)g_nfc.getStatus();

    const SpoolTag& tag = g_nfc.getLastTag();
    if (tag.uid_len > 0) {
        JsonArray uid = doc["uid"].to<JsonArray>();
        for (int i = 0; i < tag.uid_len; i++) uid.add(tag.uid[i]);
        doc["url"] = tag.ndef_url;
        doc["is_bambulab"] = tag.is_bambulab;
    }

    ScaleToConsole::send(ScaleToConsole::Type::TagStatus, doc);
    if (g_nfc.getStatus() != TagStatus::Idle)
        g_web.broadcastDebug("nfc", doc);
}

// LED priority (highest first), see rgb_led.h for the full palette:
//   updating > burst > NFC activity > stable weight >
//   console connected > WiFi only > AP mode > offline
static void updateLed() {
    // OTA flow pins the amber slow pulse; let it run undisturbed.
    if (g_pendingOta) return;
    // HTTP upload in flight — keep the amber pulse pinned. Idempotent in
    // showUpdating() so re-calling each tick doesn't reset the animation.
    if (g_uploadActiveUntil && millis() < g_uploadActiveUntil) {
        g_led.showUpdating();
        return;
    }
    // Transient bursts (tag-read ack, capture ok/fail) self-clear — leave
    // them be so the pattern plays through to completion.
    if (g_led.isBusy()) return;

    // NFC read/write in flight — takes priority over weighing so the user
    // knows "hold the tag steady" rather than "the weight is settled".
    if (g_nfc.getStatus() == TagStatus::FoundTagNowReading ||
        g_nfc.getStatus() == TagStatus::FoundTagNowWriting) {
        g_led.showNfcActivity();
        return;
    }

    // Weight overlay. Anything load-bearing on the scale switches to the
    // dark-teal family: flashing while the reading is settling, solid once
    // it's stable. Same hue means "weight in progress → weight done" reads
    // as a single state with two phases rather than two unrelated ones.
    WeightState ws = g_scale.getState();
    bool stable      = ws == WeightState::StableLoad ||
                       ws == WeightState::LoadChangedStable;
    bool loadPresent = stable ||
                       ws == WeightState::NewLoad ||
                       ws == WeightState::LoadChangedUnstable;
    if (loadPresent) {
        if (stable) g_led.showWeightStable();
        else        g_led.showWeightUnstable();
        return;
    }

    // Health ladder.
    if (g_console.isConnected()) {
        g_led.showConsoleConnected();
    } else if (g_wifi.isConnected()) {
        g_led.showWifiOnly();
    } else if (g_wifi.getState() == WifiState::Unconfigured ||
               g_wifi.getState() == WifiState::Failed) {
        g_led.showApMode();
    } else {
        // Covers boot, initial Connecting, and post-drop reconnect.
        g_led.showOffline();
    }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    CONSOLE_SERIAL.begin(CONSOLE_BAUD);

    Serial.printf("\n[Main] SpoolHardScale fw %s starting\n", FW_VERSION);

    if (!SPIFFS.begin(true)) {
        Serial.println("[Main] SPIFFS mount failed");
    }

    g_led.begin();
    g_led.showOffline();   // red solid until WiFi + console catch up

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    g_scale.begin();
    g_nfc.begin();

    g_web.onTare([]() { g_scale.tare(); });
    g_web.onCalibrate([](float w) { g_scale.calibrate(w); });
    g_web.onAddCalPoint([](float w) {
        long raw = g_scale.captureRaw();
        g_scale.addCalPoint(w, raw);
    });
    g_web.onClearCal([]() { g_scale.clearCalPoints(); });
    g_web.onCaptureRaw([]() -> long { return g_scale.captureRaw(); });
    // Apply precision / rounding / sampling changes live — no reboot
    // required. The web handler just wrote NVS, so re-read it AND push
    // the updated precision to the console so its dashboard/LCD re-
    // format the live weight immediately. Re-using ScaleVersion here
    // (vs. adding a new message type) costs a few extra bytes on the
    // wire for the version string; worth it to keep the protocol
    // surface small.
    g_web.onConfigChanged([]() {
        g_scale.loadParams();
        if (g_console.isConnected()) {
            JsonDocument doc;
            doc["version"]   = FW_VERSION;
            doc["precision"] = g_scale.params().precision;
            ScaleToConsole::send(ScaleToConsole::Type::ScaleVersion, doc);
        }
    });
    g_web.onUploadStarted([](const char* /*type*/) {
        // Slow amber pulse for the duration of the upload — same signal as
        // the OTA-triggered path below; from the user's perspective both
        // are "software being updated, please wait".
        g_led.showUpdating();
        g_uploadActiveUntil = millis() + UPLOAD_LIVENESS_MS;
    });
    g_web.onUploadProgress([]() {
        // Each chunk pushes the deadline out so updateLed() keeps the
        // amber pulse pinned. If the client drops mid-upload the deadline
        // expires UPLOAD_LIVENESS_MS later and the LED returns to normal.
        g_uploadActiveUntil = millis() + UPLOAD_LIVENESS_MS;
    });
    g_web.onOtaRequested([]() {
        // POST /api/ota-run on the scale's own web UI. Defer to the main
        // loop's OTA path so the HTTP response can finish before we start
        // pulling firmware over HTTPS.
        g_pendingOta = true;
    });
    g_web.begin();                   // register API routes (port 80)
    g_wifi.begin(g_web.server());    // register captive portal routes
    g_web.start();                   // start the config server

    // Port-81 WebSocket server — this is the channel the SpoolHard Console
    // connects to after SSDP discovery finds us.
    g_console.begin();
    g_console.onConnected([](const String& ip) {
        Serial.printf("[Console] SpoolHard Console connected from %s\n", ip.c_str());
        // Push a connection event to the dashboard
        JsonDocument dbg;
        dbg["connected"] = true;
        dbg["ip"]        = ip;
        g_web.broadcastDebug("console_conn", dbg);

        // Version + current display precision on the same on-connect
        // handshake. Console caches precision so it knows how many
        // decimal places to render from the weight floats it receives.
        JsonDocument doc;
        doc["version"]   = FW_VERSION;
        doc["precision"] = g_scale.params().precision;
        ScaleToConsole::send(ScaleToConsole::Type::ScaleVersion, doc);

        // Force a fresh OtaPending push on every reconnect — the console
        // doesn't persist the cached state across its own reboots, so a
        // reconnect (e.g. after a console OTA) needs to re-seed it.
        g_pendingPushPending = true;
    });
    g_console.onDisconnected([]() {
        Serial.println("[Console] SpoolHard Console disconnected");
        JsonDocument dbg;
        dbg["connected"] = false;
        g_web.broadcastDebug("console_conn", dbg);
    });
    g_console.onTextFrame([](const String& frame) {
        ConsoleToScale::deliver(frame);
    });

    // Mirror protocol traffic to the debug WS on the config UI (port 80)
    ScaleToConsole::setTxTap([](const String& frame) {
        g_web.broadcastConsoleFrame("out", frame);
    });
    ConsoleToScale::setRxTap([](const String& frame) {
        g_web.broadcastConsoleFrame("in", frame);
    });

    // SNTP for the OTA checker's last_check_ts (and any future wall-clock
    // needs). Fires once an interface is up; harmless to call before WiFi
    // associates.
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");

    // Manifest-driven OTA checker (shared spoolhard_core lib). begin()
    // only records boot millis; the first network fetch happens once
    // WiFi is up and the configured interval elapses.
    g_ota_checker.begin();

    Serial.println("[Main] Init complete");
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
    // WiFi provisioning state machine
    g_wifi.update();

    // Weight
    g_scale.update();
    WeightState ws = g_scale.getState();
    if (ws != g_lastWeightState) {
        Serial.printf("[Weight] %s → %s (%.1fg)\n",
                      weightStateName(g_lastWeightState), weightStateName(ws),
                      g_scale.getDisplayWeight());
        sendWeightEvent(ws);
        g_lastWeightState = ws;
    }

    // Raw samples every 500ms — local debug UI only (port 80 WS). Not sent
    // to the console WS channel: at 2 Hz it filled the per-client outbound
    // queue whenever TCP ACKs were delayed, which caused AsyncWebSocket to
    // close the console client (every 10-20 s link flap). The console
    // firmware ignored these frames anyway.
    if (millis() - g_lastRawSent > 500) {
        JsonDocument doc;
        doc["weight_g"] = g_scale.getWeightG();
        doc["raw"]      = g_scale.getRaw();
        g_web.broadcastDebug("raw_sample", doc);
        g_lastRawSent = millis();
    }

    // Feature button — GPIO 0, active-LOW with pull-up. Fire on the first
    // sampled LOW after a stable HIGH (falling edge) so a tap registers
    // instantly — no need to hold the button until the next loop tick.
    // Contact bounce and accidental double-press are both handled by a
    // REPEAT_GUARD_MS lockout after each emit.
    {
        unsigned long now = millis();
        int level = digitalRead(BUTTON_PIN);
        if (level == LOW && g_btnLastLevel == HIGH &&
            (now - g_btnLastSentMs) >= BTN_REPEAT_GUARD_MS) {
            g_btnLastSentMs = now;
            if (g_console.isConnected()) {
                ScaleToConsole::sendSimple(ScaleToConsole::Type::ButtonPressed);
                // Arm the response deadline so we can self-fire ackCaptureFail
                // if the console drops the frame or never replies in time.
                g_btnResponseDeadline = now + BTN_RESPONSE_TIMEOUT_MS;
                Serial.println("[Button] pressed → console");
            } else {
                // No console, no round-trip: fail immediately so the user
                // gets the same "didn't land" cue they'd get from an
                // explicit rejection.
                Serial.println("[Button] pressed (console not connected)");
                g_led.ackCaptureFail();
            }
        }
        g_btnLastLevel = level;

        // Deadline sweep for an in-flight ButtonPressed.
        if (g_btnResponseDeadline && now >= g_btnResponseDeadline) {
            Serial.println("[Button] no ButtonResponse within timeout — failing");
            g_btnResponseDeadline = 0;
            g_led.ackCaptureFail();
        }
    }

    // NFC
    TagStatus prevNfc = g_nfc.getStatus();
    g_nfc.update();
    TagStatus newNfc = g_nfc.getStatus();
    if (newNfc != prevNfc) {
        sendNfcEvent();
        // Transient green twin-flash on a successful read — confirms to the
        // user that the tag was detected AND parsed, separate from the
        // steady blue NFC-activity indicator that was on during the read.
        if (newNfc == TagStatus::ReadSuccess) g_led.ackTagRead();
    }

    // Console messages
    ConsoleToScale::Message msg;
    while (ConsoleToScale::receive(msg)) {
        handleConsoleMessage(msg);
    }

    // WebSocket-layer Ping/Pong is handled automatically by ESPAsyncWebServer.
    // No app-level Ping is part of the SpoolHard protocol.

    // LED (state + flash tick)
    updateLed();
    g_led.update();

    // OTA periodic check (no-op until WiFi is up + interval elapsed).
    g_ota_checker.update();

    // Push the latest pending state to the console if it changed (or if a
    // forced push was queued, e.g. on console reconnect). Cheap — peeks
    // cached fields, no network.
    pushOtaPendingIfChanged(g_pendingPushPending);
    g_pendingPushPending = false;

    // OTA (triggered by console command or UpdateFirmware message)
    if (g_pendingOta) {
        g_pendingOta = false;
        g_led.showUpdating();    // amber slow pulse (same as web-upload)

        OtaConfig cfg;
        cfg.load();

        JsonDocument prog_doc;
        prog_doc["percent"] = 0;
        ScaleToConsole::send(ScaleToConsole::Type::OtaProgressUpdate, prog_doc);

        otaRun(cfg, [](int pct) {
            JsonDocument doc;
            doc["percent"] = pct;
            ScaleToConsole::send(ScaleToConsole::Type::OtaProgressUpdate, doc);
        });
        // otaRun() reboots on success; if we get here it failed — show the
        // "offline / broken" red so the user sees something went wrong
        // rather than us quietly falling back to the normal state ladder.
        g_led.showOffline();
    }

    delay(10);
}
