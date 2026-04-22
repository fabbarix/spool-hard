#include "spool_record.h"

void SpoolRecord::toJson(JsonDocument& doc) const {
    doc["id"]                = id;
    doc["tag_id"]            = tag_id;
    doc["material_type"]     = material_type;
    doc["material_subtype"]  = material_subtype;
    doc["color_name"]        = color_name;
    doc["color_code"]        = color_code;
    doc["brand"]             = brand;
    doc["weight_advertised"] = weight_advertised;
    doc["weight_core"]       = weight_core;
    doc["weight_new"]        = weight_new;
    doc["weight_current"]    = weight_current;
    doc["consumed_since_add"]    = consumed_since_add;
    doc["consumed_since_weight"] = consumed_since_weight;
    doc["nozzle_temp_min"]   = nozzle_temp_min;
    doc["nozzle_temp_max"]   = nozzle_temp_max;
    doc["density"]           = density;
    doc["slicer_filament"]   = slicer_filament;
    doc["note"]              = note;
    doc["data_origin"]       = data_origin;
    doc["tag_type"]          = tag_type;
    if (ext_json.length()) {
        JsonDocument inner;
        if (!deserializeJson(inner, ext_json)) {
            doc["ext"] = inner;
        }
    }
}

bool SpoolRecord::fromJson(const JsonDocument& doc) {
    // Overlay semantics: only fields actually present in `doc` are touched.
    // Missing keys leave the corresponding struct field alone. Callers that
    // want full-replace semantics (e.g. fromLine loading a fresh record from
    // disk) just call this on a default-constructed SpoolRecord.
    //
    // This matters for the web POST handler, which has to tolerate partial
    // payloads — "Capture from scale" sends only {id, weight_current,
    // consumed_since_weight}, and a previous implementation wiped every
    // other field (including nozzle_temp_min/max, slicer_filament, note…)
    // back to defaults on every such call.
    auto str  = [&](const char* k, String& dst)  {
        if (doc[k].is<const char*>()) dst = doc[k].as<const char*>();
    };
    auto i32  = [&](const char* k, int32_t& dst) {
        if (doc[k].is<int>()) dst = doc[k].as<int32_t>();
    };
    auto f32  = [&](const char* k, float& dst) {
        if (doc[k].is<float>() || doc[k].is<int>()) dst = doc[k].as<float>();
    };
    str("id",                id);
    str("tag_id",            tag_id);
    str("material_type",     material_type);
    str("material_subtype",  material_subtype);
    str("color_name",        color_name);
    str("color_code",        color_code);
    str("brand",             brand);
    i32("weight_advertised", weight_advertised);
    i32("weight_core",       weight_core);
    i32("weight_new",        weight_new);
    i32("weight_current",    weight_current);
    f32("consumed_since_add",    consumed_since_add);
    f32("consumed_since_weight", consumed_since_weight);
    i32("nozzle_temp_min",   nozzle_temp_min);
    i32("nozzle_temp_max",   nozzle_temp_max);
    f32("density",           density);
    str("slicer_filament",   slicer_filament);
    str("note",              note);
    str("data_origin",       data_origin);
    str("tag_type",          tag_type);
    if (doc["ext"].is<JsonObjectConst>()) {
        serializeJson(doc["ext"], ext_json);
    }
    // Missing ext => leave ext_json alone (overlay semantics).
    return id.length() > 0;
}

String SpoolRecord::toLine() const {
    JsonDocument doc;
    toJson(doc);
    String out;
    serializeJson(doc, out);
    return out;
}

bool SpoolRecord::fromLine(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) return false;
    return fromJson(doc);
}

// Nozzle diameters round to 0.01 mm — Bambu reports 0.2/0.4/0.6/0.8/1.0. A
// tighter epsilon avoids collisions between the 0.40 and 0.42 we'd otherwise
// get from float noise.
static bool _nozzleMatch(float a, float b) { return fabsf(a - b) < 0.01f; }

bool SpoolRecord::upsertKValue(const String& printer_serial, float nozzle_diameter,
                               int extruder_idx, float k, int cali_idx) {
    if (printer_serial.isEmpty() || nozzle_diameter <= 0.f) return false;

    JsonDocument ext;
    if (ext_json.length() && deserializeJson(ext, ext_json)) {
        // Corrupt ext — start fresh rather than silently losing the update.
        ext.clear();
    }
    if (!ext.is<JsonObject>()) ext.to<JsonObject>();

    JsonArray arr = ext["k_values"].is<JsonArray>()
        ? ext["k_values"].as<JsonArray>()
        : ext["k_values"].to<JsonArray>();

    // Find existing entry.
    for (JsonObject e : arr) {
        if (e["printer"]  == printer_serial.c_str() &&
            _nozzleMatch(e["nozzle"] | 0.f, nozzle_diameter) &&
            (e["extruder"] | 0) == extruder_idx) {
            float  old_k        = e["k"]        | 0.f;
            int    old_cali_idx = e["cali_idx"] | -1;
            if (fabsf(old_k - k) < 0.0005f && old_cali_idx == cali_idx) return false;
            e["k"]        = k;
            e["cali_idx"] = cali_idx;
            serializeJson(ext, ext_json);
            return true;
        }
    }

    // Append new.
    JsonObject add = arr.add<JsonObject>();
    add["printer"]  = printer_serial;
    add["nozzle"]   = nozzle_diameter;
    add["extruder"] = extruder_idx;
    add["k"]        = k;
    add["cali_idx"] = cali_idx;
    serializeJson(ext, ext_json);
    return true;
}
