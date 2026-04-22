#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>

// Wire format matches yanshay/SpoolEase shared/src/scale.rs — serde-default
// externally-tagged enum representation over a WebSocket text frame:
//
//   unit variant    : "Uncalibrated"
//   tuple variant   : {"NewLoad": 1234}
//   struct variant  : {"ScaleVersion": {"version": "0.1.0"}}
//
// WebSocket ping/pong is handled at the frame layer by ESPAsyncWebServer —
// do NOT send app-level Ping/Pong messages, they are not part of the spec.

// Messages sent from Scale → Console
namespace ScaleToConsole {
    enum class Type {
        Uncalibrated,
        NewLoad,
        LoadChangedStable,
        LoadChangedUnstable,
        LoadRemoved,
        ButtonPressed,
        TagStatus,
        PN532Status,
        ScaleVersion,
        OtaProgressUpdate,
        Term,
        // Reply to ConsoleToScale::GetCurrentWeight. Struct variant:
        //   {"CurrentWeight": {"weight_g": 1234, "state": "StableLoad"}}
        // state is the WeightState name (Uncalibrated / Idle / NewLoad /
        // StableLoad / LoadChangedStable / LoadChangedUnstable / LoadRemoved).
        CurrentWeight,
    };

    // Generic typed send. `doc` carries fields that are mapped into the
    // correct externally-tagged envelope based on `type`. See protocol.cpp.
    void send(Type type, JsonDocument& doc);
    void sendSimple(Type type);

    // Optional tap: called with the JSON text after it is emitted
    void setTxTap(std::function<void(const String&)> cb);
}

// Messages received by Scale ← Console
namespace ConsoleToScale {
    enum class Type {
        Unknown,
        Calibrate,             // {"Calibrate": i32}
        ButtonResponse,        // {"ButtonResponse": bool}
        RequestGcodeAnalysis,  // {"RequestGcodeAnalysis": {...}}  (encrypted)
        GcodeAnalysisNotify,   // {"GcodeAnalysisNotify": {...}}
        ReadTag,               // "ReadTag"
        WriteTag,              // {"WriteTag": {"text","check_uid","cookie"}}
        EraseTag,              // {"EraseTag": {"check_uid","cookie"}}
        EmulateTag,            // {"EmulateTag": {"url"}}
        UpdateFirmware,        // {"UpdateFirmware": {...}}        (encrypted)
        TagsInStore,           // {"TagsInStore": {"tags"}}
        GetCurrentWeight,      // "GetCurrentWeight"  — on-demand read, scale replies with CurrentWeight
    };

    struct Message {
        Type type;
        JsonDocument doc;   // the inner payload (unwrapped from the tag)
    };

    // Feed a raw WS text frame into the parser. Pushes the message to an
    // internal queue consumed by receive().
    void deliver(const String& frame);

    bool receive(Message& out);
    void setRxTap(std::function<void(const String&)> cb);
}

const char* typeToString(ScaleToConsole::Type t);
