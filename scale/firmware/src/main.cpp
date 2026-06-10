#include <Arduino.h>
#include <ArduinoJson.h>
#include "spoolhard/psram_json_alloc.h"
// Force PlatformIO's library-dependency-finder to pull in HTTPClient.
// The shared spoolhard_core/src/ota.cpp uses it; LDF only walks
// #includes from the project's own src/ tree (chain+ default), not
// from libraries. Without this line the scale firmware fails to link
// with `HTTPClient.h: No such file or directory`. The console firmware
// resolves the same dep transitively because bambu_cloud.cpp includes
// HTTPClient — the scale has no equivalent caller.
#include <HTTPClient.h>

#include "config.h"
#include "load_cell.h"
#include "nfc_reader.h"
#include "sensor_task.h"
#include "nfc_task.h"
#include "console_tx.h"
#include "rgb_led.h"
#include "protocol.h"
#include "console_channel.h"
#include "web_server.h"
#include "wifi_provisioning.h"
#include "spoolhard/ota.h"
#include "spoolhard/ws_buffer_pool.h"
#include "spoolhard/panic_persist.h"
#include <SPIFFS.h>
#include <esp_task_wdt.h>
#include <atomic>

// Mirror everything printed to Serial into the in-RAM ring log so
// /api/logs surfaces the LAT_STEP / dlog output remotely. Must come
// after all framework + project headers since it macro-rewrites
// `Serial` for the rest of this translation unit.
#include "spoolhard/serial_mirror.h"
#include "spoolhard/ring_log.h"   // dlog()

// ── Globals ──────────────────────────────────────────────────
// g_scale and g_nfc have external linkage so the dedicated tasks
// (sensor_task, nfc_task) can declare them `extern` and own their
// update loops. The other singletons stay file-scoped — only the
// coordinator code in this TU touches them.
LoadCell                g_scale;
NfcReader               g_nfc;
static RgbLed           g_led;
static ScaleWebServer   g_web;
static WifiProvisioning g_wifi;

static WeightState   g_lastWeightState = WeightState::Uncalibrated;
static unsigned long g_lastRawSent      = 0;

// Atomics — these flags are set from AsyncTCP/HTTP handlers (a different
// task than `loop()`) and consumed by the main loop. Without atomics the
// compiler may reorder or cache the load/store and the loop sees stale
// values; we've observed this empirically as missed handshakes after
// reconnect on `-O2`. `memory_order_relaxed` is fine because each flag
// stands alone — there's no ordering dependency between them.
static std::atomic<bool>     g_pendingOta{false};
static std::atomic<uint32_t> g_uploadActiveUntil{0};
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
static std::atomic<bool> g_pendingPushPending{false};   // force-push (e.g. on connect)
// Set on console-connect (AsyncTCP task); consumed on the next loop
// tick (loopTask) which sends ScaleVersion + CalibrationStatus.
// Deferred (rather than sent inline from WS_EVT_CONNECT) because the
// AsyncTCP queue isn't fully drained of the HTTP-upgrade response yet
// at the moment WS_EVT_CONNECT fires; frames queued synchronously then
// can be lost. Atomic so the loopTask sees the AsyncTCP write
// immediately even on `-O2` reorders.
static std::atomic<bool> g_pendingPushHandshake{false};

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

    JsonDocument doc(&g_psramJsonAlloc);
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

// Push the current calibration state to the console so its LCD scale-
// settings screen renders "Calibration: N points" without polling.
// Called after every action that mutates calibration (tare, addCalPoint,
// clear, legacy single-point calibrate). Reads via SensorTask snapshot
// so the cal struct comes from a brief mutex (consistent w.r.t. the
// task's own concurrent saveCalibration writes).
static void pushCalibrationStatus() {
    auto cal = SensorTask::snapshotCal();
    JsonDocument doc(&g_psramJsonAlloc);
    doc["num_points"] = (int)cal.numPoints;
    doc["tare_raw"]   = (int32_t)cal.tare_raw;
    ScaleToConsole::send(ScaleToConsole::Type::CalibrationStatus, doc);
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
                SensorTask::requestTare();
            } else {
                Serial.printf("[App] Calibrate with known weight=%ldg\n", (long)weight);
                SensorTask::requestCalibrate((float)weight);
            }
            // Coordinator loop polls SensorTask::consumeCalDirty() and
            // pushes CalibrationStatus once the request lands. Don't
            // push synchronously from here — the cal data is mid-write.
            break;
        }
        case T::AddCalPoint: {
            // {"AddCalPoint": i32} — capture raw + add to multi-point curve.
            int32_t weight = msg.doc.as<int32_t>();
            if (weight <= 0) {
                Serial.printf("[App] AddCalPoint ignored — bad weight %ld\n", (long)weight);
                break;
            }
            Serial.printf("[App] AddCalPoint %ldg → sensor task\n", (long)weight);
            SensorTask::requestAddCalPoint((float)weight);
            break;
        }
        case T::ClearCalPoints: {
            Serial.println("[App] ClearCalPoints → sensor task");
            SensorTask::requestClearCalPoints();
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
            NfcTask::WriteRequest r{};
            r.ndef_message = msg.doc["text"]   | "";
            r.cookie       = msg.doc["cookie"] | "";
            JsonArray uid_arr = msg.doc["check_uid"].as<JsonArray>();
            for (JsonVariant v : uid_arr) {
                if (r.uid_len < 7) r.uid[r.uid_len++] = v.as<uint8_t>();
            }
            NfcTask::requestWrite(r);
            break;
        }
        case T::EraseTag: {
            NfcTask::EraseRequest r{};
            JsonArray uid_arr = msg.doc["check_uid"].as<JsonArray>();
            for (JsonVariant v : uid_arr) {
                if (r.uid_len < 7) r.uid[r.uid_len++] = v.as<uint8_t>();
            }
            NfcTask::requestErase(r);
            break;
        }
        case T::EmulateTag: {
            String url = msg.doc["url"] | "";
            NfcTask::requestEmulate(url);
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
            // Snapshot read — sensor_task publishes via seqlock, no
            // blocking, no torn reads.
            auto snap = SensorTask::snapshot();
            JsonDocument out(&g_psramJsonAlloc);
            out["weight_g"]  = snap.weight_g;
            out["state"]     = weightStateName(snap.state);
            out["precision"] = snap.precision;
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
            JsonDocument dbg(&g_psramJsonAlloc);
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
    auto snap = SensorTask::snapshot();
    JsonDocument doc(&g_psramJsonAlloc);
    // Send the raw float so the console can render at whatever precision
    // the user configured (see ScaleVersion/CurrentWeight `precision`
    // field). getDisplayWeight() would pre-round and throw away decimals.
    doc["weight_g"] = snap.weight_g;

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

    JsonDocument dbg(&g_psramJsonAlloc);
    dbg["state"]    = weightStateName(state);
    dbg["weight_g"] = SensorTask::getWeightG();
    g_web.broadcastDebug("weight_state", dbg);
}

// Most recent successful tag read, kept across the lifetime of the
// firmware for replay-on-reconnect. The console-scale WS link can
// momentarily wedge (PONGs flow but TEXT doesn't) — if a tag scan
// fires during that window the TagStatus frame gets queued, the
// queue eventually overflows or the lib tears the connection down,
// and the message is lost. Replaying the cached tag in the deferred
// on-connect handshake ensures the console catches up after the
// reconnect even though the original frame never landed.
//
// Only ReadSuccess frames are cached; Idle / Failure don't carry a
// useful UID to replay.
// Tag-replay cache. Today these are only touched from the main loop,
// so there's no race. Phase C will move NFC into its own task, at
// which point the writer becomes `nfc_task` and the reader is the
// app coordinator on the same loop. The bool + uint32_t are atomic
// already; the SpoolTag struct will need a mutex once it crosses the
// task boundary. Leaving plain for now — guard added together with
// the task split so the type changes land in one commit.
static SpoolTag           g_lastSuccessTag = {};
static TagStatus          g_lastSuccessTagStatus = TagStatus::Idle;
static std::atomic<uint32_t> g_lastSuccessTagMs{0};
static std::atomic<bool>     g_haveLastSuccessTag{false};

static void _writeNfcDoc(JsonDocument& doc, TagStatus status, const SpoolTag& tag) {
    doc["status"] = (int)status;
    if (tag.uid_len > 0) {
        JsonArray uid = doc["uid"].to<JsonArray>();
        for (int i = 0; i < tag.uid_len; i++) uid.add(tag.uid[i]);
        doc["url"] = tag.ndef_url;
        doc["is_bambulab"] = tag.is_bambulab;
    }
}

// Legacy entry point kept around in case the LCD refresh path needs it
// later. The loop's NfcTask::pollEvent drain inlines the same work and
// uses the event-snapshot tag (which can't race the next nfc_task
// sample). Marked [[maybe_unused]] so removing the last caller doesn't
// trip -Wunused-function under -Wall.
[[maybe_unused]]
static void sendNfcEvent_unused() {
    auto snap = NfcTask::snapshot();
    JsonDocument doc(&g_psramJsonAlloc);
    _writeNfcDoc(doc, snap.status, snap.tag);

    ScaleToConsole::send(ScaleToConsole::Type::TagStatus, doc);
    if (snap.status != TagStatus::Idle)
        g_web.broadcastDebug("nfc", doc);

    // Cache for replay on the next console connect — only ReadSuccess
    // is worth replaying, the transient FoundTagNowReading status
    // doesn't carry payload the console acts on.
    if (snap.status == TagStatus::ReadSuccess && snap.tag.uid_len > 0) {
        g_lastSuccessTag       = snap.tag;
        g_lastSuccessTagStatus = snap.status;
        g_lastSuccessTagMs.store(millis());
        g_haveLastSuccessTag.store(true);
    }
}

// LED test override. Set by the /api/led-test handler; updateLed() honours
// it for up to LED_TEST_MS, pinning the requested pattern over every steady
// state except OTA (which is a real in-flight operation the user can't
// preempt for a UI demo). Loads on AsyncTCP, drains on loopTask.
enum class LedTestState : uint8_t {
    None = 0,
    Offline, ApMode, WifiOnly, ConsoleConnected,
    WeightUnstable, WeightStable, NfcActivity,
    Updating, Uncalibrated,
    AckTagRead, AckCaptureOk, AckCaptureFail,
};
static std::atomic<uint8_t>  g_ledTestState{(uint8_t)LedTestState::None};
static std::atomic<uint32_t> g_ledTestUntil{0};

// Run a `show*` once per tick for the held duration of the test. Returns
// true if a test pattern is currently being driven (caller should skip
// the normal arbitration).
static bool _drivLedTest() {
    uint32_t until = g_ledTestUntil.load(std::memory_order_acquire);
    if (until == 0) return false;
    if ((int32_t)(millis() - until) >= 0) {
        // Window expired — clear and let the next tick fall through to
        // normal arbitration so the LED snaps back without a frame of
        // off in between.
        g_ledTestUntil.store(0, std::memory_order_release);
        g_ledTestState.store((uint8_t)LedTestState::None,
                             std::memory_order_release);
        return false;
    }
    auto st = (LedTestState)g_ledTestState.load(std::memory_order_acquire);
    switch (st) {
        case LedTestState::Offline:          g_led.showOffline();          break;
        case LedTestState::ApMode:           g_led.showApMode();           break;
        case LedTestState::WifiOnly:         g_led.showWifiOnly();         break;
        case LedTestState::ConsoleConnected: g_led.showConsoleConnected(); break;
        case LedTestState::WeightUnstable:   g_led.showWeightUnstable();   break;
        case LedTestState::WeightStable:     g_led.showWeightStable();     break;
        case LedTestState::NfcActivity:      g_led.showNfcActivity();      break;
        case LedTestState::Updating:         g_led.showUpdating();         break;
        case LedTestState::Uncalibrated:     g_led.showUncalibrated();     break;
        // Bursts self-clear after ~330 ms; re-fire whenever the LED has
        // gone idle so the pattern loops visibly for the whole test
        // window instead of flashing once and going dark.
        case LedTestState::AckTagRead:
            if (!g_led.isBusy()) g_led.ackTagRead();
            break;
        case LedTestState::AckCaptureOk:
            if (!g_led.isBusy()) g_led.ackCaptureOk();
            break;
        case LedTestState::AckCaptureFail:
            if (!g_led.isBusy()) g_led.ackCaptureFail();
            break;
        case LedTestState::None:
        default:
            return false;
    }
    return true;
}

// LED priority (highest first), see rgb_led.h for the full palette:
//   updating > test-override > burst > NFC activity > uncalibrated >
//   weight > console connected > WiFi only > AP mode > offline
static void updateLed() {
    // OTA flow pins the amber slow pulse; let it run undisturbed.
    if (g_pendingOta.load() || otaTaskInFlight()) return;
    // HTTP upload in flight — keep the amber pulse pinned. Idempotent in
    // showUpdating() so re-calling each tick doesn't reset the animation.
    if (g_uploadActiveUntil.load() && millis() < g_uploadActiveUntil.load()) {
        g_led.showUpdating();
        return;
    }
    // Test override — placed here so the legend's "Test for 5 s" button
    // wins over every steady state below but never over an in-flight
    // OTA / firmware upload.
    if (_drivLedTest()) return;

    // Transient bursts (tag-read ack, capture ok/fail) self-clear — leave
    // them be so the pattern plays through to completion.
    if (g_led.isBusy()) return;

    // NFC read/write in flight — takes priority over weighing so the user
    // knows "hold the tag steady" rather than "the weight is settled".
    TagStatus ts = NfcTask::getStatus();
    if (ts == TagStatus::FoundTagNowReading ||
        ts == TagStatus::FoundTagNowWriting) {
        g_led.showNfcActivity();
        return;
    }

    // Uncalibrated takes precedence over the network ladder: a paired,
    // online scale that can't weigh is useless for its primary job, so
    // surface it loudly. Sits below NFC/weight (which can only fire when
    // calibrated anyway) and below transient acks so calibration captures
    // still flash their burst through.
    if (!g_scale.isCalibrated()) {
        g_led.showUncalibrated();
        return;
    }

    // Weight overlay. Anything load-bearing on the scale switches to the
    // dark-teal family: flashing while the reading is settling, solid once
    // it's stable. Same hue means "weight in progress → weight done" reads
    // as a single state with two phases rather than two unrelated ones.
    WeightState ws = SensorTask::getState();
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

    // Crash log persistence — promotes the previous boot's pending
    // ring-tail to a stable /crash_<seq>.txt if the previous reset
    // reason was crashy. Surfaces via the /api/crashes route below.
    PanicPersist::begin();

    // Pre-allocate the WS broadcast pool BEFORE any code path that might
    // emit a state.* push (web server start, console connect callback).
    // 8 slots × 8 KB each — covers every envelope shape the scale sends
    // with margin. Slots live in DRAM (vector<uint8_t>) but are reused
    // forever after; no per-broadcast allocation under steady state.
    g_wsBufPool.begin(/*count*/ 8, /*initial_capacity*/ 8192);

    g_led.begin();
    g_led.showOffline();   // red solid until WiFi + console catch up

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    g_scale.begin();
    g_nfc.begin();

    // Spawn the dedicated FreeRTOS tasks that own the load cell + PN532.
    // Order matters: g_scale.begin() / g_nfc.begin() must finish first
    // so the tasks see a fully-initialized chip on their first poll.
    SensorTask::begin();
    NfcTask::begin();
    // Single-writer WS sender for the port-81 console link. Must be
    // up before any ScaleToConsole::send() call — protocol.cpp's emit
    // helpers route through ConsoleTx::send() now.
    ConsoleTx::begin();

    // All HTTP-handler callbacks now post commands to the sensor task
    // instead of mutating g_scale directly. This isolates blocking
    // HX711 reads (up to ~5 s for tare's 20-sample average) from the
    // AsyncTCP task and lets HTTP responses complete instantly.
    g_web.onTare([]()            { SensorTask::requestTare();        });
    g_web.onCalibrate([](float w){ SensorTask::requestCalibrate(w); });
    g_web.onAddCalPoint([](float w){ SensorTask::requestAddCalPoint(w); });
    g_web.onClearCal([]()        { SensorTask::requestClearCalPoints(); });
    // /api/scale-raw returns the LATEST cached raw sample — the HTTP
    // handler doesn't block waiting for a fresh average. Frontend uses
    // this as a live-display value, not a cal-quality reading.
    g_web.onCaptureRaw([]() -> long { return SensorTask::getLastRaw(); });
    g_web.onConfigChanged([]() {
        SensorTask::requestReloadParams();
        if (g_console.isConnected()) {
            JsonDocument doc(&g_psramJsonAlloc);
            doc["version"]   = FW_VERSION;
            doc["precision"] = SensorTask::getPrecision();
            ScaleToConsole::send(ScaleToConsole::Type::ScaleVersion, doc);
        }
    });
    g_web.onLedTest([](const String& id, uint32_t ms) {
        // Map the catalog id (see /api/led-legend in web_server.cpp) to
        // the test override enum. Unknown ids are dropped; updateLed()
        // then keeps the normal arbitration. All `show*` and `ack*`
        // calls happen from loopTask inside `_drivLedTest` — this
        // callback only flips two atomics, so g_led itself stays in a
        // single-writer regime even though the callback runs on
        // AsyncTCP.
        LedTestState st = LedTestState::None;
        if (id == "offline")                st = LedTestState::Offline;
        else if (id == "ap_mode")           st = LedTestState::ApMode;
        else if (id == "wifi_only")         st = LedTestState::WifiOnly;
        else if (id == "console_connected") st = LedTestState::ConsoleConnected;
        else if (id == "weight_unstable")   st = LedTestState::WeightUnstable;
        else if (id == "weight_stable")     st = LedTestState::WeightStable;
        else if (id == "nfc_activity")      st = LedTestState::NfcActivity;
        else if (id == "updating")          st = LedTestState::Updating;
        else if (id == "uncalibrated")      st = LedTestState::Uncalibrated;
        else if (id == "ack_tag_read")      st = LedTestState::AckTagRead;
        else if (id == "ack_capture_ok")    st = LedTestState::AckCaptureOk;
        else if (id == "ack_capture_fail")  st = LedTestState::AckCaptureFail;
        if (st == LedTestState::None) return;
        g_ledTestState.store((uint8_t)st, std::memory_order_release);
        g_ledTestUntil.store(millis() + ms, std::memory_order_release);
    });

    g_web.onUploadStarted([](const char* /*type*/) {
        g_led.showUpdating();
        g_uploadActiveUntil.store(millis() + UPLOAD_LIVENESS_MS);
    });
    g_web.onUploadProgress([]() {
        g_uploadActiveUntil.store(millis() + UPLOAD_LIVENESS_MS);
    });
    g_web.onOtaRequested([]() {
        g_pendingOta.store(true);
    });
    g_web.begin();                   // register API routes (port 80)
    g_wifi.begin(g_web.server());    // register captive portal routes

    // Mount the console-link WS at /ws/console on the SAME port-80
    // server. Single AsyncWebServer instance now hosts: SPA static,
    // /api/* HTTP, /ws (browser dashboard), /ws/console (paired
    // console). Must be done BEFORE start() — addHandler is only safe
    // on a not-yet-listening server.
    g_console.begin(g_web.server());

    g_web.start();                   // start the unified server
    g_console.onConnected([](const String& ip) {
        Serial.printf("[Console] SpoolHard Console connected from %s\n", ip.c_str());
        // Push a connection event to the dashboard
        JsonDocument dbg(&g_psramJsonAlloc);
        dbg["connected"] = true;
        dbg["ip"]        = ip;
        g_web.broadcastDebug("console_conn", dbg);

        // Defer the actual handshake frames (ScaleVersion +
        // CalibrationStatus) AND the OtaPending push to the next loop
        // tick. AsyncTCP's queue is in a transient state at the moment
        // WS_EVT_CONNECT fires — the WS upgrade response hasn't been
        // fully ACK'd yet — and frames sent synchronously from this
        // handler are intermittently dropped (the version field on the
        // dashboard kept showing empty after a reconnect because of
        // exactly this).
        g_pendingPushHandshake.store(true);
        g_pendingPushPending.store(true);
    });
    g_console.onDisconnected([]() {
        Serial.println("[Console] SpoolHard Console disconnected");
        JsonDocument dbg(&g_psramJsonAlloc);
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

    // Watch the coordinator loop with the task WDT. The scale has been
    // observed fully wedged (off-network, needs a power cycle) with no
    // panic evidence — if loopTask ever hangs, the WiFi reconnect kicker
    // in g_wifi.update() stops running and the device is unreachable
    // until someone pulls the plug. A WDT panic instead resets us AND
    // panic_persist promotes the ring-log tail, so we get a post-mortem.
    // 60 s is far above any legitimate stall (worst observed LoopLat is
    // <1 s; SPIFFS GC worst-case is seconds) but converts a forever-hang
    // into a recovery. Note this re-init also stretches the timeout for
    // AsyncTCP's task, which adds itself to the same WDT.
    esp_task_wdt_init(60, /*panic=*/true);
    esp_task_wdt_add(nullptr);   // nullptr = calling task (loopTask)

    Serial.println("[Main] Init complete");
}

// ── Loop ──────────────────────────────────────────────────────
//
// Per-step latency instrumentation. If any step takes longer than 50 ms
// we log the offender so we can diagnose flapping on the scale↔console
// link — long stalls in the main loop starve AsyncTCP and the WS
// heartbeat (5 s cadence) starts arriving late, which the console reads
// as a soft-disconnect.
#define LAT_STEP(name, expr) do {                                \
    uint32_t __t0 = millis();                                    \
    expr;                                                        \
    uint32_t __dt = millis() - __t0;                             \
    if (__dt > 50) Serial.printf("[LoopLat] %s=%lums\n", name,   \
                                 (unsigned long)__dt);           \
} while (0)

void loop() {
    uint32_t __loop_t0 = millis();
    esp_task_wdt_reset();
    // WiFi provisioning state machine
    LAT_STEP("wifi", g_wifi.update());

    // Weight — owned by sensor_task. Drain its event queue here and
    // also re-check the snapshot in case we missed a transient (queue
    // full → drop policy means the latest state is still authoritative).
    SensorTask::WeightEvent we;
    while (SensorTask::pollEvent(we)) {
        Serial.printf("[Weight] → %s (%.1fg)\n",
                      weightStateName(we.new_state), we.weight_g);
        sendWeightEvent(we.new_state);
        g_lastWeightState = we.new_state;
    }
    // Calibration mutation that landed on the sensor task → push status.
    if (SensorTask::consumeCalDirty()) {
        pushCalibrationStatus();
    }

    // Raw samples every 500ms — local debug UI only (port 80 WS).
    if (millis() - g_lastRawSent > 500) {
        g_lastRawSent = millis();
        if (g_web.wsClientCount() > 0) {
            auto snap = SensorTask::snapshot();
            JsonDocument doc(&g_psramJsonAlloc);
            doc["weight_g"] = snap.weight_g;
            doc["raw"]      = snap.last_raw;
            g_web.broadcastDebug("raw_sample", doc);
        }
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

    // NFC — owned by nfc_task. Drain transitions; snapshot carries
    // the full tag, so we don't need to re-poll g_nfc here.
    NfcTask::StatusEvent ne;
    while (NfcTask::pollEvent(ne)) {
        // Build the protocol+debug payload from the event's snapshot
        // rather than calling g_nfc.getLastTag() — that would race the
        // nfc_task's next sample.
        JsonDocument doc(&g_psramJsonAlloc);
        _writeNfcDoc(doc, ne.new_status, ne.tag);
        ScaleToConsole::send(ScaleToConsole::Type::TagStatus, doc);
        if (ne.new_status != TagStatus::Idle) {
            g_web.broadcastDebug("nfc", doc);
        }
        if (ne.new_status == TagStatus::ReadSuccess && ne.tag.uid_len > 0) {
            g_lastSuccessTag       = ne.tag;
            g_lastSuccessTagStatus = ne.new_status;
            g_lastSuccessTagMs.store(millis());
            g_haveLastSuccessTag.store(true);
            g_led.ackTagRead();
        }
    }

    // Console messages
    ConsoleToScale::Message msg;
    LAT_STEP("console_msg", while (ConsoleToScale::receive(msg)) {
        handleConsoleMessage(msg);
    });

    // WebSocket-layer Ping/Pong is handled automatically by ESPAsyncWebServer.
    // No app-level Ping is part of the SpoolHard protocol.

    // LED (state + flash tick)
    updateLed();
    g_led.update();

    // OTA periodic check (no-op until WiFi is up + interval elapsed).
    LAT_STEP("ota_check", g_ota_checker.update());

    // Periodic flush of the ring-log tail to SPIFFS, so a panic on the
    // next loop iteration leaves us a forensics trail. 30 s rate-gate
    // inside.
    PanicPersist::tick();

    // Push the latest pending state to the console if it changed (or if a
    // forced push was queued, e.g. on console reconnect). Cheap — peeks
    // cached fields, no network.
    // exchange(false) returns the previous value AND clears the flag in
    // a single atomic step — no TOCTOU window where AsyncTCP could set
    // the flag between our read and our clear. Same idiom on the
    // handshake flag below.
    pushOtaPendingIfChanged(g_pendingPushPending.exchange(false));

    // Deferred on-connect handshake (set by g_console.onConnected).
    // Sending these from the loop instead of inline from WS_EVT_CONNECT
    // sidesteps the AsyncTCP-still-mid-upgrade race that was eating the
    // ScaleVersion frame.
    if (g_pendingPushHandshake.load() && g_console.isConnected()) {
        g_pendingPushHandshake.store(false);
        JsonDocument verDoc(&g_psramJsonAlloc);
        verDoc["version"]   = FW_VERSION;
        verDoc["precision"] = SensorTask::getPrecision();
        ScaleToConsole::send(ScaleToConsole::Type::ScaleVersion, verDoc);

        auto cal = SensorTask::snapshotCal();
        JsonDocument calDoc(&g_psramJsonAlloc);
        calDoc["num_points"] = (int)cal.numPoints;
        calDoc["tare_raw"]   = (int32_t)cal.tare_raw;
        ScaleToConsole::send(ScaleToConsole::Type::CalibrationStatus, calDoc);

        // Replay the most recent successful tag scan if it was within
        // the last 60 s — the user just held the spool up and saw the
        // LED ack, but the original TagStatus frame likely got lost in
        // the WS wedge that triggered this reconnect. The console's
        // PendingAms latch and wizard-start logic is idempotent on tag
        // UID, so a duplicate replay after a real frame already landed
        // is a no-op.
        if (g_haveLastSuccessTag.load() &&
            (millis() - g_lastSuccessTagMs.load()) < 60000UL) {
            JsonDocument tagDoc(&g_psramJsonAlloc);
            _writeNfcDoc(tagDoc, g_lastSuccessTagStatus, g_lastSuccessTag);
            ScaleToConsole::send(ScaleToConsole::Type::TagStatus, tagDoc);
            Serial.println("[Console] Replayed recent tag scan to console");
        }
    }

    // Browser-WS push for the scale's own dashboard. Each helper is
    // rate-gated inside broadcastState (5–30 s) and short-circuits when
    // no clients are connected, so a per-loop call is cheap.
    LAT_STEP("ota_push",  g_web.pushOtaStatus());
    LAT_STEP("wifi_push", g_web.pushWifiStatus());

    // Once-per-second tick: cleanupClients() on both WS servers so dirty
    // disconnects don't leave zombies pinning queued buffers / stuck
    // counts (the LED-stuck-on-green bug). Also a 5 s Heartbeat to the
    // console with uptime + heap so the dashboard can show "scale up
    // for 3d 4h" beside the link badge.
    {
        static uint32_t s_lastCleanupMs = 0;
        static uint32_t s_lastHeartbeatMs = 0;
        uint32_t now = millis();
        if (now - s_lastCleanupMs >= 1000) {
            s_lastCleanupMs = now;
            g_console.cleanupClients();
            g_web.cleanupClients();
            // DIAGNOSTIC: dump WS client queue/state every second so we
            // can correlate the cycle's progression with queue-fill or
            // drain-stall behaviour.
            g_console.tickStats();
            // Surface the FreeRTOS-level ConsoleTx queue state too.
            // If this hits 0 free, we know the producer is over-running
            // even before the lib's per-client queue gets a look-in.
            static uint32_t s_lastTxStat = 0;
            uint32_t tx  = ConsoleTx::framesTx();
            uint32_t drp = ConsoleTx::framesDropped();
            if (tx != s_lastTxStat || drp != 0) {
                dlog("ConsoleTx", "free=%u/%u tx=%u dropped=%u",
                     (unsigned)ConsoleTx::free(),
                     (unsigned)ConsoleTx::total(),
                     (unsigned)tx, (unsigned)drp);
                s_lastTxStat = tx;
            }
        }
        // Heartbeat cadence: 1.5 s. Originally 5 s, but in real-world
        // operation the dashboard's "scale connection" telemetry was
        // showing 15-25 s gaps between heartbeats — likely a mix of
        // TCP retransmits absorbed by AsyncTCP queues + the
        // occasional dropped frame on a marginal Wi-Fi link. At 5 s
        // a single missed frame already exceeds the console's
        // staleness window. At 1.5 s we have ~3-4 chances per 5 s
        // window so even with occasional packet loss the dashboard
        // sees fresh data.
        if (g_console.isConnected() && (now - s_lastHeartbeatMs >= 1500)) {
            s_lastHeartbeatMs = now;
            JsonDocument hb(&g_psramJsonAlloc);
            hb["uptime_s"]      = (uint32_t)(now / 1000);
            hb["free_heap"]     = (uint32_t)ESP.getFreeHeap();
            hb["min_free_heap"] = (uint32_t)ESP.getMinFreeHeap();
            ScaleToConsole::send(ScaleToConsole::Type::Heartbeat, hb);
            // Single-line trace so we can grep /api/logs and confirm
            // the scale is actually queuing heartbeats. If the dlog
            // prints but the console doesn't see them, the loss is
            // wire-level (TCP / radio); if the dlog stops, it's a
            // local issue (count()==0, scheduler stall, etc.).
            static uint32_t s_hbSeq = 0;
            dlog("HB", "#%u @%us heap=%u",
                 ++s_hbSeq,
                 (unsigned)(now/1000),
                 (unsigned)ESP.getFreeHeap());
        }
    }

    // Whole-tick latency. Anything over 100 ms is a candidate cause of
    // pong delays — the WS lib's ping reply path runs on AsyncTCP's
    // task, but `_ws.textAll()` queues from the main loop so a stalled
    // loop does block app-level Heartbeat queuing.
    uint32_t __loop_dt = millis() - __loop_t0;
    if (__loop_dt > 100) Serial.printf("[LoopLat] total=%lums\n",
                                       (unsigned long)__loop_dt);

    // OTA (triggered by console command or UpdateFirmware message).
    // Spawned on its own FreeRTOS task so the main loop continues to
    // sample weight, poll NFC, animate LED, and service the WS link
    // throughout the multi-minute fetch+flash cycle. Was previously
    // inline here, which starved AsyncTCP for the entire OTA window
    // and caused the link to flap during every update.
    if (g_pendingOta.exchange(false)) {
        if (otaTaskInFlight()) {
            Serial.println("[OTA] already in progress, ignoring spawn");
        } else {
            g_led.showUpdating();
            OtaConfig cfg;
            cfg.load();
            g_ota_in_flight = { true, "firmware", 0, millis() };

            JsonDocument prog_doc(&g_psramJsonAlloc);
            prog_doc["kind"]    = "firmware";
            prog_doc["percent"] = 0;
            ScaleToConsole::send(ScaleToConsole::Type::OtaProgressUpdate, prog_doc);

            otaTaskSpawn(cfg, [](OtaProgress p) {
                const char* kind =
                    p.kind == OtaProgress::Kind::Frontend ? "frontend" :
                    p.kind == OtaProgress::Kind::Firmware ? "firmware" :
                                                            "";
                g_ota_in_flight = { true, kind, p.percent, g_ota_in_flight.started_ms };
                // The progress callback runs on the OTA task's
                // context. ScaleToConsole::send goes through the
                // protocol-level mutexed sender (tap snapshot +
                // g_console.sendText, which is itself thread-safe via
                // AsyncWebSocket's internal locking), so it's safe to
                // emit from this task. After Phase D this will go
                // through the dedicated console_tx_task queue
                // instead.
                JsonDocument doc(&g_psramJsonAlloc);
                if (*kind) doc["kind"] = kind;
                doc["percent"] = p.percent;
                ScaleToConsole::send(ScaleToConsole::Type::OtaProgressUpdate, doc);
            });
        }
    }

    // Surface OTA failure (task exited without rebooting) so the LED
    // returns to a sensible state. We poll otaTaskInFlight() once per
    // loop tick after the task was spawned: if it's now false but
    // g_ota_in_flight.valid is still true, the run failed.
    if (g_ota_in_flight.valid && !otaTaskInFlight() &&
        g_ota_in_flight.percent < 100) {
        g_ota_in_flight = {};
        g_led.showOffline();
    }

    delay(10);
}
