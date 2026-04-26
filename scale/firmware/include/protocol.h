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
        // Scale's view of what (if anything) needs updating, pushed up to
        // the console so its combined banner can show a single update list.
        // Struct variant: {"OtaPending": {firmware_current, firmware_latest,
        // frontend_current, frontend_latest, firmware_update (bool),
        // frontend_update (bool), last_check_ts (epoch), last_check_status}}.
        // Pushed once on console-connect and again whenever the pending
        // state changes (so the console doesn't have to poll us).
        OtaPending,
        Term,
        // Reply to ConsoleToScale::GetCurrentWeight. Struct variant:
        //   {"CurrentWeight": {"weight_g": 1234, "state": "StableLoad"}}
        // state is the WeightState name (Uncalibrated / Idle / NewLoad /
        // StableLoad / LoadChangedStable / LoadChangedUnstable / LoadRemoved).
        CurrentWeight,
        // Pushed after every tare / addCalPoint / clear so the console
        // LCD's scale-settings screen can render "Calibration: N
        // points" without polling. Struct variant:
        //   {"CalibrationStatus": {"num_points": 3, "tare_raw": 12345}}
        // tare_raw is the persisted zero reading; nonzero means the
        // scale has been tared at least once.
        CalibrationStatus,
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
        // Phase-5 OTA orchestration. Both unit variants — the scale uses
        // its own stored OtaConfig (manifest URL, ssl flags) so the
        // console doesn't have to know per-device settings.
        RunOtaUpdate,          // "RunOtaUpdate"   — flash now using stored config
        CheckOtaUpdates,       // "CheckOtaUpdates" — kick the manifest checker now
        // Multi-point calibration controls used by the LCD wizard. The
        // legacy `Calibrate` tuple stays for compatibility.
        //   {"AddCalPoint": <int32>}   — sample raw + addCalPoint(weight,raw)
        //   "ClearCalPoints"           — wipe all stored points
        AddCalPoint,
        ClearCalPoints,
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
