#include "protocol.h"
#include <deque>

// ── Scale → Console parse ────────────────────────────────────

namespace ScaleToConsole {

static std::function<void(const String&)> g_rxTap;
static std::deque<Message> g_queue;

void setRxTap(std::function<void(const String&)> cb) { g_rxTap = cb; }

static Type nameToType(const char* name) {
    if (strcmp(name, "Uncalibrated")        == 0) return Type::Uncalibrated;
    if (strcmp(name, "NewLoad")             == 0) return Type::NewLoad;
    if (strcmp(name, "LoadChangedStable")   == 0) return Type::LoadChangedStable;
    if (strcmp(name, "LoadChangedUnstable") == 0) return Type::LoadChangedUnstable;
    if (strcmp(name, "LoadRemoved")         == 0) return Type::LoadRemoved;
    if (strcmp(name, "ButtonPressed")       == 0) return Type::ButtonPressed;
    if (strcmp(name, "TagStatus")           == 0) return Type::TagStatus;
    if (strcmp(name, "PN532Status")         == 0) return Type::PN532Status;
    if (strcmp(name, "ScaleVersion")        == 0) return Type::ScaleVersion;
    if (strcmp(name, "OtaProgressUpdate")   == 0) return Type::OtaProgressUpdate;
    if (strcmp(name, "OtaPending")          == 0) return Type::OtaPending;
    if (strcmp(name, "Term")                == 0) return Type::Term;
    if (strcmp(name, "CurrentWeight")       == 0) return Type::CurrentWeight;
    return Type::Unknown;
}

const char* typeToString(Type t) {
    switch (t) {
        case Type::Uncalibrated:        return "Uncalibrated";
        case Type::NewLoad:             return "NewLoad";
        case Type::LoadChangedStable:   return "LoadChangedStable";
        case Type::LoadChangedUnstable: return "LoadChangedUnstable";
        case Type::LoadRemoved:         return "LoadRemoved";
        case Type::ButtonPressed:       return "ButtonPressed";
        case Type::TagStatus:           return "TagStatus";
        case Type::PN532Status:         return "PN532Status";
        case Type::ScaleVersion:        return "ScaleVersion";
        case Type::OtaProgressUpdate:   return "OtaProgressUpdate";
        case Type::OtaPending:          return "OtaPending";
        case Type::Term:                return "Term";
        case Type::CurrentWeight:       return "CurrentWeight";
        default:                        return "Unknown";
    }
}

void deliver(const String& frame) {
    if (g_rxTap) g_rxTap(frame);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, frame);
    if (err) {
        Serial.printf("[Protocol] JSON parse error: %s (frame=%s)\n", err.c_str(), frame.c_str());
        return;
    }

    Message out;
    out.type = Type::Unknown;

    if (doc.is<const char*>()) {
        out.type = nameToType(doc.as<const char*>());
    } else if (doc.is<JsonObject>()) {
        JsonObject o = doc.as<JsonObject>();
        if (o.size() != 1) {
            Serial.println("[Protocol] Expected exactly one top-level key");
            return;
        }
        auto kv = *o.begin();
        out.type = nameToType(kv.key().c_str());
        out.doc.set(kv.value());
    } else {
        Serial.println("[Protocol] Unexpected top-level JSON type");
        return;
    }

    if (out.type == Type::Unknown) {
        Serial.printf("[Protocol] Unknown message: %s\n", frame.c_str());
        return;
    }

    g_queue.push_back(std::move(out));
}

bool receive(Message& out) {
    if (g_queue.empty()) return false;
    out = std::move(g_queue.front());
    g_queue.pop_front();
    return true;
}

} // namespace ScaleToConsole

// ── Console → Scale build ────────────────────────────────────

namespace ConsoleToScale {

static std::function<void(const String&)> g_txTap;

void setTxTap(std::function<void(const String&)> cb) { g_txTap = cb; }
void emitTap(const String& frame) { if (g_txTap) g_txTap(frame); }

static const char* typeName(Type t) {
    switch (t) {
        case Type::Calibrate:            return "Calibrate";
        case Type::ButtonResponse:       return "ButtonResponse";
        case Type::RequestGcodeAnalysis: return "RequestGcodeAnalysis";
        case Type::GcodeAnalysisNotify:  return "GcodeAnalysisNotify";
        case Type::ReadTag:              return "ReadTag";
        case Type::WriteTag:             return "WriteTag";
        case Type::EraseTag:             return "EraseTag";
        case Type::EmulateTag:           return "EmulateTag";
        case Type::UpdateFirmware:       return "UpdateFirmware";
        case Type::TagsInStore:          return "TagsInStore";
        case Type::GetCurrentWeight:     return "GetCurrentWeight";
        case Type::RunOtaUpdate:         return "RunOtaUpdate";
        case Type::CheckOtaUpdates:      return "CheckOtaUpdates";
    }
    return "?";
}

String build(Type t) {
    // Unit variant: naked JSON string.
    String out;
    out.reserve(32);
    out += '"'; out += typeName(t); out += '"';
    emitTap(out);
    return out;
}

String build(Type t, const JsonDocument& payload) {
    JsonDocument env;
    env[typeName(t)] = payload;
    String out;
    serializeJson(env, out);
    emitTap(out);
    return out;
}

String buildTuple(Type t, int32_t value) {
    JsonDocument env;
    env[typeName(t)] = value;
    String out;
    serializeJson(env, out);
    emitTap(out);
    return out;
}

String buildTuple(Type t, bool value) {
    JsonDocument env;
    env[typeName(t)] = value;
    String out;
    serializeJson(env, out);
    emitTap(out);
    return out;
}

} // namespace ConsoleToScale
