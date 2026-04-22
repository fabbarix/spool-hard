#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>

// Wire format matches yanshay/SpoolEase shared/src/scale.rs — serde-default
// externally-tagged enum representation over a WebSocket text frame.
//
// This is the CONSOLE side of the protocol. Compared to the scale's
// protocol.{cpp,h} the two namespaces are swapped: we SEND ConsoleToScale
// messages and RECEIVE ScaleToConsole messages.

// Scale → Console (we parse these)
namespace ScaleToConsole {
    enum class Type {
        Unknown,
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
        CurrentWeight,
    };

    struct Message {
        Type type;
        JsonDocument doc;  // inner payload (unwrapped from the tag)
    };

    // Feed a raw WS text frame into the parser. Pushes onto an internal queue.
    void deliver(const String& frame);

    // Pull next pending message. Returns false if queue is empty.
    bool receive(Message& out);

    void setRxTap(std::function<void(const String&)> cb);

    const char* typeToString(Type t);
}

// Console → Scale (we produce these)
namespace ConsoleToScale {
    enum class Type {
        Calibrate,             // {"Calibrate": i32}          0=tare, nonzero=cal weight
        ButtonResponse,        // {"ButtonResponse": bool}
        RequestGcodeAnalysis,  // {"RequestGcodeAnalysis": {...}}   (M3)
        GcodeAnalysisNotify,   // {"GcodeAnalysisNotify": {...}}    (M3)
        ReadTag,               // "ReadTag"
        WriteTag,              // {"WriteTag": {"text","check_uid","cookie"}}
        EraseTag,              // {"EraseTag": {"check_uid","cookie"}}
        EmulateTag,            // {"EmulateTag": {"url"}}
        UpdateFirmware,        // {"UpdateFirmware": {...}}
        TagsInStore,           // {"TagsInStore": {"tags"}}
        GetCurrentWeight,      // "GetCurrentWeight"  — scale replies with CurrentWeight
    };

    // Build a wire frame for the given message. Callers feed the resulting
    // String into the WebSocket `sendTXT`. For struct variants, `payload`
    // should carry the inner fields; for unit variants it's ignored.
    String build(Type t);
    String build(Type t, const JsonDocument& payload);
    String buildTuple(Type t, int32_t value);
    String buildTuple(Type t, bool value);

    // Optional tap: called with the JSON text after it is built.
    void setTxTap(std::function<void(const String&)> cb);

    // Trigger the tap for a freshly-built frame (called by build*()).
    void emitTap(const String& frame);
}
