#include "protocol.h"
#include "console_channel.h"
#include "console_tx.h"
#include "spoolhard/psram_json_alloc.h"
#include <deque>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "spoolhard/serial_mirror.h"

// ── helpers ─────────────────────────────────────────────────

static const char* s2cName(ScaleToConsole::Type t) {
    using T = ScaleToConsole::Type;
    switch (t) {
        case T::Uncalibrated:        return "Uncalibrated";
        case T::NewLoad:             return "NewLoad";
        case T::LoadChangedStable:   return "LoadChangedStable";
        case T::LoadChangedUnstable: return "LoadChangedUnstable";
        case T::LoadRemoved:         return "LoadRemoved";
        case T::ButtonPressed:       return "ButtonPressed";
        case T::TagStatus:           return "TagStatus";
        case T::PN532Status:         return "PN532Status";
        case T::ScaleVersion:        return "ScaleVersion";
        case T::OtaProgressUpdate:   return "OtaProgressUpdate";
        case T::OtaPending:          return "OtaPending";
        case T::Term:                return "Term";
        case T::CurrentWeight:       return "CurrentWeight";
        case T::CalibrationStatus:   return "CalibrationStatus";
        case T::Heartbeat:           return "Heartbeat";
    }
    return "?";
}

const char* typeToString(ScaleToConsole::Type t) { return s2cName(t); }

// ── Scale → Console ─────────────────────────────────────────

namespace ScaleToConsole {

static std::function<void(const String&)> g_txTap;
static SemaphoreHandle_t g_tx_tap_mtx = nullptr;

static void _ensureTxTapMutex() {
    if (!g_tx_tap_mtx) g_tx_tap_mtx = xSemaphoreCreateMutex();
}

void setTxTap(std::function<void(const String&)> cb) {
    _ensureTxTapMutex();
    if (xSemaphoreTake(g_tx_tap_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_txTap = std::move(cb);
        xSemaphoreGive(g_tx_tap_mtx);
    }
}

// Snapshot the tx tap under brief lock — emit helpers call this once
// per send so a concurrent setTxTap() can't replace the std::function
// mid-deref. The snapshot is a copy of the std::function (cheap; uses
// the small-buffer-optimization typical std::function carries).
static std::function<void(const String&)> _txTapSnap() {
    _ensureTxTapMutex();
    std::function<void(const String&)> snap;
    if (xSemaphoreTake(g_tx_tap_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        snap = g_txTap;
        xSemaphoreGive(g_tx_tap_mtx);
    }
    return snap;
}

// Emit a unit variant: naked JSON string, e.g. "Uncalibrated".
static void emitUnit(const char* name) {
    String out;
    out.reserve(strlen(name) + 2);
    out += '"'; out += name; out += '"';
    ConsoleTx::send(out);
    { auto __t = _txTapSnap(); if (__t) __t(out); }
}

// Emit a single-i32 tuple variant: {"NewLoad": 1234}.
static void emitTupleI32(const char* name, int32_t value) {
    JsonDocument env(&g_psramJsonAlloc);
    env[name] = value;
    String out;
    serializeJson(env, out);
    ConsoleTx::send(out);
    { auto __t = _txTapSnap(); if (__t) __t(out); }
}

// Emit a single-float tuple variant: {"NewLoad": 1234.5}.
// JSON numbers are numbers — wire-level the difference is just whether a
// decimal point is serialized — so clients that still ask for `.as<int32>()`
// degrade to truncated integers. The console firmware reads this as float.
static void emitTupleFloat(const char* name, float value) {
    JsonDocument env(&g_psramJsonAlloc);
    env[name] = value;
    String out;
    serializeJson(env, out);
    ConsoleTx::send(out);
    { auto __t = _txTapSnap(); if (__t) __t(out); }
}

// Emit a struct variant, copying fields from `inner` under the tag.
static void emitStruct(const char* name, const JsonDocument& inner) {
    JsonDocument env(&g_psramJsonAlloc);
    env[name] = inner;
    String out;
    serializeJson(env, out);
    ConsoleTx::send(out);
    { auto __t = _txTapSnap(); if (__t) __t(out); }
}

void sendSimple(Type type) {
    // Only unit variants make sense here
    switch (type) {
        case Type::Uncalibrated:  emitUnit("Uncalibrated");  break;
        case Type::LoadRemoved:   emitUnit("LoadRemoved");   break;
        case Type::ButtonPressed: emitUnit("ButtonPressed"); break;
        default:
            Serial.printf("[Protocol] sendSimple called on non-unit variant %s\n", s2cName(type));
            break;
    }
}

void send(Type type, JsonDocument& doc) {
    switch (type) {
        // unit variants ignore `doc`
        case Type::Uncalibrated:
        case Type::LoadRemoved:
        case Type::ButtonPressed:
            sendSimple(type);
            break;

        // Weight events carry a float so the console can display fractional
        // grams matching the user's configured precision. Previously these
        // were truncated to int32 on the wire, so the "decimal places"
        // setting silently had no effect on the console-side display.
        case Type::NewLoad:
        case Type::LoadChangedStable:
        case Type::LoadChangedUnstable: {
            emitTupleFloat(s2cName(type), doc["weight_g"].as<float>());
            break;
        }

        // tuple variant with bool
        case Type::PN532Status: {
            JsonDocument env(&g_psramJsonAlloc);
            env["PN532Status"] = doc["status"].as<bool>();
            String out; serializeJson(env, out);
            ConsoleTx::send(out);
            { auto __t = _txTapSnap(); if (__t) __t(out); }
            break;
        }

        // struct variants — pass the whole doc as the inner object
        case Type::ScaleVersion: {
            JsonDocument inner(&g_psramJsonAlloc);
            inner["version"] = doc["version"].as<const char*>();
            // Display precision (0–4 decimals) travels alongside the
            // version so the console knows how many decimal places to
            // render from the float weight it receives. Re-emitted
            // whenever the user saves a new value on the scale's config.
            if (doc["precision"].is<int>()) {
                inner["precision"] = doc["precision"].as<int>();
            }
            emitStruct("ScaleVersion", inner);
            break;
        }
        case Type::TagStatus: {
            // Forward whatever fields main.cpp built as the inner object.
            emitStruct("TagStatus", doc);
            break;
        }
        case Type::OtaPending: {
            // Forward whatever fields main.cpp built as the inner object.
            // Schema: firmware_current/latest, frontend_current/latest,
            // firmware_update (bool), frontend_update (bool),
            // last_check_ts (epoch), last_check_status.
            emitStruct("OtaPending", doc);
            break;
        }
        case Type::OtaProgressUpdate: {
            // Map progress into Status{text, percent, kind}. The `text`
            // field is what older console builds key off (it shows the
            // updating screen on the LCD); newer builds also read
            // `percent` and `kind` to drive the web UI's per-product
            // progress bars.
            JsonDocument inner(&g_psramJsonAlloc);
            JsonDocument status(&g_psramJsonAlloc);
            const char* kind = doc["kind"]    | "";   // "" | "firmware" | "frontend"
            int         pct  = doc["percent"] | -1;
            if (doc["text"].is<const char*>()) {
                status["text"] = doc["text"].as<const char*>();
            } else if (pct >= 0) {
                const char* label =
                    !strcmp(kind, "frontend") ? "Updating Frontend"
                  : !strcmp(kind, "firmware") ? "Updating Firmware"
                  :                              "Updating";
                String t = String(label) + ": " + pct + "%";
                status["text"] = t;
            } else {
                // Nothing useful — emit Start as a fallback
                emitStruct("OtaProgressUpdate", JsonDocument(&g_psramJsonAlloc));
                return;
            }
            if (pct >= 0)        status["percent"] = pct;
            if (kind && *kind)   status["kind"]    = kind;
            inner["Status"] = status;
            emitStruct("OtaProgressUpdate", inner);
            break;
        }
        case Type::Term: {
            JsonDocument env(&g_psramJsonAlloc);
            env["Term"] = doc["text"].as<const char*>();
            String out; serializeJson(env, out);
            ConsoleTx::send(out);
            { auto __t = _txTapSnap(); if (__t) __t(out); }
            break;
        }
        case Type::CurrentWeight: {
            // Reply contract:
            //   {"CurrentWeight": {"weight_g": float, "state": "StableLoad",
            //                      "precision": 0..4 }}
            // weight_g is a float so the console can display with the same
            // precision the scale's own screen would use; precision is the
            // scale's stored decimal-places setting so the console knows
            // how many digits to render.
            JsonDocument inner(&g_psramJsonAlloc);
            inner["weight_g"]  = doc["weight_g"].as<float>();
            inner["state"]     = doc["state"].as<const char*>();
            if (doc["precision"].is<int>()) inner["precision"] = doc["precision"].as<int>();
            emitStruct("CurrentWeight", inner);
            break;
        }
        case Type::CalibrationStatus: {
            // Pushed after every tare / addCalPoint / clear. The console
            // LCD's scale-settings screen renders "Calibration: N points"
            // straight from this — it never has to poll the calibration
            // state otherwise.
            JsonDocument inner(&g_psramJsonAlloc);
            inner["num_points"] = doc["num_points"].as<int>();
            inner["tare_raw"]   = doc["tare_raw"].as<int32_t>();
            emitStruct("CalibrationStatus", inner);
            break;
        }
        case Type::Heartbeat: {
            // Periodic liveness + lightweight telemetry. The console
            // caches uptime_s and renders "scale up for 3d 4h" beside
            // the connection-state badge. free_heap / min_free_heap let
            // the user see the scale's memory health from the console
            // without having to open the scale's own web UI.
            JsonDocument inner(&g_psramJsonAlloc);
            inner["uptime_s"]      = doc["uptime_s"].as<uint32_t>();
            inner["free_heap"]     = doc["free_heap"].as<uint32_t>();
            inner["min_free_heap"] = doc["min_free_heap"].as<uint32_t>();
            emitStruct("Heartbeat", inner);
            break;
        }
    }
}

} // namespace ScaleToConsole

// ── Console → Scale ─────────────────────────────────────────

namespace ConsoleToScale {

static std::function<void(const String&)> g_rxTap;
static std::deque<Message> g_queue;
// Bound the inbound message queue so a misbehaving / runaway console
// (or a hostile peer that knows the auth key) can't grow this deque
// without limit. 64 entries × ~256 B per Message is ~16 KB worst case.
// On overflow we drop the OLDEST — newer messages are usually the more
// actionable ones (e.g. ButtonResponse needs to land before its
// timeout sweeps it).
static constexpr size_t kMaxQueueDepth = 64;

// Concurrency guard for g_queue.
//
// `deliver()` runs on the AsyncTCP task (port-81 WS_EVT_DATA fires on
// that task's stack). `receive()` runs on the main `loopTask`. Without
// a lock, simultaneous push_back / pop_front would corrupt the deque's
// internal node list and crash the firmware — historically observed
// as a panic during reconnect storms when CheckOtaUpdates spam
// briefly raised inbound-frame rate.
//
// The mutex hold time per call is microseconds (one alloc + one
// `std::move` of a Message). Both ends drop on a 50 ms timeout: the
// AsyncTCP path discards the inbound frame (better than blocking the
// network task), the main path returns "no message" (next loop tick
// will retry).
static SemaphoreHandle_t g_queue_mtx = nullptr;
static SemaphoreHandle_t g_tap_mtx   = nullptr;

static void _ensureMutex() {
    if (!g_queue_mtx) g_queue_mtx = xSemaphoreCreateMutex();
    if (!g_tap_mtx)   g_tap_mtx   = xSemaphoreCreateMutex();
}

void setRxTap(std::function<void(const String&)> cb) {
    _ensureMutex();
    if (xSemaphoreTake(g_tap_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_rxTap = std::move(cb);
        xSemaphoreGive(g_tap_mtx);
    }
}

static Type nameToType(const char* name) {
    if (strcmp(name, "Calibrate")            == 0) return Type::Calibrate;
    if (strcmp(name, "ButtonResponse")       == 0) return Type::ButtonResponse;
    if (strcmp(name, "RequestGcodeAnalysis") == 0) return Type::RequestGcodeAnalysis;
    if (strcmp(name, "GcodeAnalysisNotify")  == 0) return Type::GcodeAnalysisNotify;
    if (strcmp(name, "ReadTag")              == 0) return Type::ReadTag;
    if (strcmp(name, "WriteTag")             == 0) return Type::WriteTag;
    if (strcmp(name, "EraseTag")             == 0) return Type::EraseTag;
    if (strcmp(name, "EmulateTag")           == 0) return Type::EmulateTag;
    if (strcmp(name, "UpdateFirmware")       == 0) return Type::UpdateFirmware;
    if (strcmp(name, "TagsInStore")          == 0) return Type::TagsInStore;
    if (strcmp(name, "GetCurrentWeight")     == 0) return Type::GetCurrentWeight;
    if (strcmp(name, "RunOtaUpdate")         == 0) return Type::RunOtaUpdate;
    if (strcmp(name, "CheckOtaUpdates")      == 0) return Type::CheckOtaUpdates;
    if (strcmp(name, "AddCalPoint")          == 0) return Type::AddCalPoint;
    if (strcmp(name, "ClearCalPoints")       == 0) return Type::ClearCalPoints;
    return Type::Unknown;
}

void deliver(const String& frame) {
    _ensureMutex();

    // Snapshot the tap pointer under its own brief mutex so another task
    // calling setRxTap() can't race the deref below.
    std::function<void(const String&)> tap;
    if (xSemaphoreTake(g_tap_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        tap = g_rxTap;
        xSemaphoreGive(g_tap_mtx);
    }
    if (tap) tap(frame);

    JsonDocument doc(&g_psramJsonAlloc);
    DeserializationError err = deserializeJson(doc, frame);
    if (err) {
        Serial.printf("[Protocol] JSON parse error: %s (frame=%s)\n",
                      err.c_str(), frame.c_str());
        return;
    }

    Message out;
    out.type = Type::Unknown;

    if (doc.is<const char*>()) {
        // Unit variant, e.g. "ReadTag"
        out.type = nameToType(doc.as<const char*>());
    } else if (doc.is<JsonObject>()) {
        JsonObject o = doc.as<JsonObject>();
        if (o.size() != 1) {
            Serial.println("[Protocol] Expected exactly one top-level key");
            return;
        }
        auto kv = *o.begin();
        out.type = nameToType(kv.key().c_str());
        // Copy the inner value into out.doc so handlers can read fields
        out.doc.set(kv.value());
    } else {
        Serial.println("[Protocol] Unexpected top-level JSON type");
        return;
    }

    if (out.type == Type::Unknown) {
        Serial.printf("[Protocol] Unknown message: %s\n", frame.c_str());
        return;
    }

    if (xSemaphoreTake(g_queue_mtx, pdMS_TO_TICKS(50)) != pdTRUE) {
        Serial.println("[Protocol] queue mutex timeout — dropping inbound frame");
        return;
    }
    g_queue.push_back(std::move(out));
    while (g_queue.size() > kMaxQueueDepth) {
        Serial.println("[Protocol] inbound queue overflow — dropping oldest");
        g_queue.pop_front();
    }
    xSemaphoreGive(g_queue_mtx);
}

bool receive(Message& out) {
    _ensureMutex();
    if (xSemaphoreTake(g_queue_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool got = false;
    if (!g_queue.empty()) {
        out = std::move(g_queue.front());
        g_queue.pop_front();
        got = true;
    }
    xSemaphoreGive(g_queue_mtx);
    return got;
}

} // namespace ConsoleToScale
