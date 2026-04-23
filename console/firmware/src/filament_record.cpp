#include "filament_record.h"

void FilamentRecord::toJson(JsonDocument& doc) const {
    doc["setting_id"]       = setting_id;
    doc["stock"]            = stock;
    doc["name"]             = name;
    doc["base_id"]          = base_id;
    doc["filament_type"]    = filament_type;
    doc["filament_subtype"] = filament_subtype;
    doc["filament_vendor"]  = filament_vendor;
    doc["filament_id"]      = filament_id;
    doc["nozzle_temp_min"]  = nozzle_temp_min;
    doc["nozzle_temp_max"]  = nozzle_temp_max;
    doc["density"]          = density;
    doc["pressure_advance"] = pressure_advance;
    // Inline the per-nozzle PA list as a real JSON array on the wire so
    // the frontend can parse it directly. Stored as a serialized string
    // internally (mirrors SpoolRecord::ext_json) to keep the struct POD-ish.
    if (pa_by_nozzle_json.length()) {
        JsonDocument inner;
        if (!deserializeJson(inner, pa_by_nozzle_json)) {
            doc["pa_by_nozzle"] = inner;
        }
    }
    doc["cloud_setting_id"] = cloud_setting_id;
    doc["cloud_synced_at"]  = cloud_synced_at;
    doc["updated_at"]       = updated_at;
}

bool FilamentRecord::fromJson(const JsonDocument& doc) {
    // Overlay semantics — same pattern as SpoolRecord::fromJson. Lets a
    // PUT on /api/user-filaments/{id} send only the changed fields
    // without wiping anything else.
    auto str = [&](const char* k, String& dst) {
        if (doc[k].is<const char*>()) dst = doc[k].as<const char*>();
    };
    auto i32 = [&](const char* k, int32_t& dst) {
        if (doc[k].is<int>()) dst = doc[k].as<int32_t>();
    };
    auto u32 = [&](const char* k, uint32_t& dst) {
        if (doc[k].is<uint32_t>() || doc[k].is<int>()) dst = doc[k].as<uint32_t>();
    };
    auto f32 = [&](const char* k, float& dst) {
        if (doc[k].is<float>() || doc[k].is<int>()) dst = doc[k].as<float>();
    };
    auto b   = [&](const char* k, bool& dst) {
        if (doc[k].is<bool>()) dst = doc[k].as<bool>();
    };
    str("setting_id",       setting_id);
    b  ("stock",            stock);
    str("name",             name);
    str("base_id",          base_id);
    str("filament_type",    filament_type);
    str("filament_subtype", filament_subtype);
    str("filament_vendor",  filament_vendor);
    str("filament_id",      filament_id);
    i32("nozzle_temp_min",  nozzle_temp_min);
    i32("nozzle_temp_max",  nozzle_temp_max);
    f32("density",          density);
    f32("pressure_advance", pressure_advance);
    if (doc["pa_by_nozzle"].is<JsonArrayConst>()) {
        // Re-serialize so we keep the canonical compact form on disk.
        // Empty array clears the field (so the form can wipe it).
        if (doc["pa_by_nozzle"].as<JsonArrayConst>().size() == 0) {
            pa_by_nozzle_json = "";
        } else {
            String s;
            serializeJson(doc["pa_by_nozzle"], s);
            pa_by_nozzle_json = s;
        }
    }
    str("cloud_setting_id", cloud_setting_id);
    u32("cloud_synced_at",  cloud_synced_at);
    u32("updated_at",       updated_at);
    return setting_id.length() > 0;
}

float FilamentRecord::paForNozzle(float nozzle_diameter_mm) const {
    if (pa_by_nozzle_json.length()) {
        JsonDocument arr;
        if (!deserializeJson(arr, pa_by_nozzle_json)) {
            for (JsonVariantConst e : arr.as<JsonArrayConst>()) {
                float n = e["nozzle"] | 0.f;
                // 0.01 mm tolerance — matches SpoolRecord::_nozzleMatch.
                if (fabsf(n - nozzle_diameter_mm) < 0.01f) {
                    return e["k"] | 0.f;
                }
            }
        }
    }
    return pressure_advance;
}

String FilamentRecord::toLine() const {
    JsonDocument doc;
    toJson(doc);
    String out;
    serializeJson(doc, out);
    return out;
}

bool FilamentRecord::fromLine(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) return false;
    return fromJson(doc);
}
