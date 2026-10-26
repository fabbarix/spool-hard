#pragma once
// ArduinoJson deserialization filter for Bambu MQTT reports.
//
// A full report is 4-17 KB (H2S ~15.8 KB, ~100 keys under `print`) but
// BambuPrinter::_parseReport() / _recordCommandAck() / _parseAms() /
// _parseExternalTray() read only ~25 of them. Deserializing through this
// filter drops everything else at parse time, so the JsonDocument node tree
// is a fraction of the size.
//
// MAINTENANCE: if you read a new key from the report doc in bambu_printer.cpp,
// add it here. The host test (console/firmware/test/host/test_report_filter.cpp)
// scans bambu_printer.cpp for `["key"]` literals in the report-reading
// functions and fails when one is missing from this filter.
//
// ArduinoJson v7 filter semantics used below:
//   - `true` keeps the whole subtree (used for ams / vt_tray / vir_slot so the
//     "Log AMS messages" raw debug dump stays complete and every per-tray
//     subfield any model sends is preserved);
//   - an array filter is `[0] = <element filter>` and applies to ALL elements;
//   - keys absent from the filter are skipped (top-level `info`/`system`
//     reports are dropped entirely: nothing consumes them).
#include <ArduinoJson.h>

// Populate `f` (an empty JsonDocument) with the report filter.
inline void bambuBuildReportFilter(JsonDocument& f) {
    JsonObject p = f["print"].to<JsonObject>();
    // Command acks (_recordCommandAck)
    p["command"]     = true;
    p["result"]      = true;
    p["reason"]      = true;
    p["err_code"]    = true;
    p["sequence_id"] = true;
    // Job / telemetry scalars (_parseReport)
    p["fun"]             = true;   // Developer-Mode bit hook (currently disabled)
    p["gcode_state"]     = true;
    p["subtask_name"]    = true;
    p["gcode_file"]      = true;
    p["task_id"]         = true;
    p["mc_percent"]      = true;
    p["layer_num"]       = true;
    p["total_layer_num"] = true;
    p["bed_temper"]      = true;
    p["nozzle_temper"]   = true;
    // Nozzle inventory + H2-series active-slot (device.extruder.info[].snow)
    JsonObject dev = p["device"].to<JsonObject>();
    dev["nozzle"]["info"][0]["diameter"] = true;
    JsonObject ext = dev["extruder"].to<JsonObject>();
    ext["state"] = true;
    ext["info"][0]["id"]   = true;
    ext["info"][0]["snow"] = true;
    // AMS (tray_now, ams[].{id,humidity,temp,dry_setting,tray[]...}) and
    // external tray: whole subtrees (small; also feeds the ams_raw debug dump).
    p["ams"]      = true;
    p["vt_tray"]  = true;
    p["vir_slot"] = true;
    // Signing-certificate registration replies (_onSecurityReport).
    JsonObject sec = f["security"].to<JsonObject>();
    sec["command"]     = true;
    sec["result"]      = true;
    sec["reason"]      = true;
    sec["sequence_id"] = true;
    sec["cert_ids"]    = true;
}

// Process-wide filter document, built once on first use. Pass the allocator
// the report document uses (PSRAM on the firmware).
inline JsonDocument& bambuReportFilter(Allocator* alloc = nullptr) {
    static JsonDocument* f = [alloc]() {
        JsonDocument* d = alloc ? new JsonDocument(alloc) : new JsonDocument();
        bambuBuildReportFilter(*d);
        return d;
    }();
    return *f;
}
