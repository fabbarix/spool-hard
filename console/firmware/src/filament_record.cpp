#include "filament_record.h"

void FilamentRecord::toJson(JsonDocument& doc) const {
    doc["setting_id"]        = setting_id;
    doc["stock"]             = stock;
    doc["name"]              = name;
    doc["base_id"]           = base_id;
    doc["parent_setting_id"] = parent_setting_id;
    doc["cloud_inherits"]    = cloud_inherits;
    doc["filament_type"]     = filament_type;
    doc["filament_subtype"]  = filament_subtype;
    doc["filament_vendor"]   = filament_vendor;
    doc["filament_id"]       = filament_id;
    doc["nozzle_temp_min"]   = nozzle_temp_min;
    doc["nozzle_temp_max"]   = nozzle_temp_max;
    doc["density"]           = density;
    // Inline variants as a real JSON array on the wire so the frontend
    // can parse without a second pass.
    if (variants_json.length()) {
        JsonDocument inner;
        if (!deserializeJson(inner, variants_json)) {
            doc["variants"] = inner;
        }
    } else {
        // Always emit an empty array so the React form can append safely.
        doc["variants"].to<JsonArray>();
    }
    doc["cloud_setting_id"]  = cloud_setting_id;
    if (cloud_variant_ids_json.length()) {
        JsonDocument inner;
        if (!deserializeJson(inner, cloud_variant_ids_json)) {
            doc["cloud_variant_ids"] = inner;
        }
    }
    doc["cloud_synced_at"]   = cloud_synced_at;
    doc["updated_at"]        = updated_at;
}

bool FilamentRecord::fromJson(const JsonDocument& doc) {
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
    str("setting_id",        setting_id);
    b  ("stock",             stock);
    str("name",              name);
    str("base_id",           base_id);
    str("parent_setting_id", parent_setting_id);
    str("cloud_inherits",    cloud_inherits);
    str("filament_type",     filament_type);
    str("filament_subtype",  filament_subtype);
    str("filament_vendor",   filament_vendor);
    str("filament_id",       filament_id);
    i32("nozzle_temp_min",   nozzle_temp_min);
    i32("nozzle_temp_max",   nozzle_temp_max);
    f32("density",           density);
    if (doc["variants"].is<JsonArrayConst>()) {
        if (doc["variants"].as<JsonArrayConst>().size() == 0) {
            variants_json = "";
        } else {
            String s;
            serializeJson(doc["variants"], s);
            variants_json = s;
        }
    }
    if (doc["cloud_variant_ids"].is<JsonArrayConst>()) {
        if (doc["cloud_variant_ids"].as<JsonArrayConst>().size() == 0) {
            cloud_variant_ids_json = "";
        } else {
            String s;
            serializeJson(doc["cloud_variant_ids"], s);
            cloud_variant_ids_json = s;
        }
    }
    str("cloud_setting_id",  cloud_setting_id);
    u32("cloud_synced_at",   cloud_synced_at);
    u32("updated_at",        updated_at);
    return setting_id.length() > 0;
}

std::vector<FilamentVariant> FilamentRecord::variants() const {
    std::vector<FilamentVariant> out;
    if (variants_json.isEmpty()) return out;
    JsonDocument doc;
    if (deserializeJson(doc, variants_json)) return out;
    for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
        FilamentVariant fv;
        fv.printer_model             = v["printer_model"]             | "";
        fv.nozzle_diameter           = v["nozzle_diameter"]           | 0.f;
        fv.nozzle_temp_print         = v["nozzle_temp_print"]         | -1;
        fv.nozzle_temp_initial_layer = v["nozzle_temp_initial_layer"] | -1;
        // extruder_variants / max_volumetric_speed / pressure_advance
        // come in as JSON arrays. We also accept the older singular
        // scalars (`extruder_variant`, `max_volumetric_speed` as a
        // float) so records written before this change keep loading.
        if (v["extruder_variants"].is<JsonArrayConst>()) {
            for (JsonVariantConst e : v["extruder_variants"].as<JsonArrayConst>())
                fv.extruder_variants.push_back(String(e | ""));
        } else if (v["extruder_variant"].is<const char*>()) {
            String s = v["extruder_variant"].as<const char*>();
            // semicolon-separated legacy form.
            int from = 0;
            while (from <= (int)s.length()) {
                int sc = s.indexOf(';', from);
                String tok = (sc < 0) ? s.substring(from) : s.substring(from, sc);
                if (tok.length()) fv.extruder_variants.push_back(tok);
                if (sc < 0) break;
                from = sc + 1;
            }
        }
        auto loadFloatArr = [&](const char* key, const char* legacyScalar,
                                std::vector<float>& dst) {
            if (v[key].is<JsonArrayConst>()) {
                for (JsonVariantConst e : v[key].as<JsonArrayConst>()) {
                    float f = 0.f;
                    if (e.is<float>()) f = e.as<float>();
                    else if (e.is<int>()) f = (float)e.as<int>();
                    else if (e.is<const char*>()) f = atof(e.as<const char*>());
                    dst.push_back(f);
                }
            } else if (legacyScalar && v[legacyScalar].is<float>()) {
                dst.push_back(v[legacyScalar].as<float>());
            } else if (legacyScalar && v[legacyScalar].is<int>()) {
                dst.push_back((float)v[legacyScalar].as<int>());
            }
        };
        loadFloatArr("max_volumetric_speed", "max_volumetric_speed", fv.max_volumetric_speed);
        loadFloatArr("pressure_advance",     "pressure_advance",     fv.pressure_advance);
        out.push_back(std::move(fv));
    }
    return out;
}

void FilamentRecord::setVariants(const std::vector<FilamentVariant>& vs) {
    if (vs.empty()) { variants_json = ""; return; }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& fv : vs) {
        JsonObject o = arr.add<JsonObject>();
        if (fv.printer_model.length()) o["printer_model"]   = fv.printer_model;
        if (fv.nozzle_diameter > 0.f)  o["nozzle_diameter"] = fv.nozzle_diameter;
        if (fv.nozzle_temp_print > 0)         o["nozzle_temp_print"]         = fv.nozzle_temp_print;
        if (fv.nozzle_temp_initial_layer > 0) o["nozzle_temp_initial_layer"] = fv.nozzle_temp_initial_layer;
        if (!fv.extruder_variants.empty()) {
            JsonArray ea = o["extruder_variants"].to<JsonArray>();
            for (const auto& s : fv.extruder_variants) ea.add(s);
        }
        if (!fv.max_volumetric_speed.empty()) {
            JsonArray ma = o["max_volumetric_speed"].to<JsonArray>();
            for (float f : fv.max_volumetric_speed) ma.add(f);
        }
        if (!fv.pressure_advance.empty()) {
            JsonArray pa = o["pressure_advance"].to<JsonArray>();
            for (float f : fv.pressure_advance) pa.add(f);
        }
    }
    String s;
    serializeJson(doc, s);
    variants_json = s;
}

bool FilamentRecord::resolveVariant(const String& printer_model, float nozzle_diameter,
                                    FilamentVariant& out) const {
    auto vs = variants();
    if (vs.empty()) return false;
    auto match = [&](bool need_model, bool need_nozzle) -> int {
        for (size_t i = 0; i < vs.size(); ++i) {
            const auto& v = vs[i];
            bool model_ok = need_model
                ? (v.printer_model == printer_model)
                : (v.printer_model.isEmpty());
            bool nozzle_ok = need_nozzle
                ? (fabsf(v.nozzle_diameter - nozzle_diameter) < 0.01f)
                : (v.nozzle_diameter <= 0.f);
            if (model_ok && nozzle_ok) return (int)i;
        }
        return -1;
    };
    // Priority order: tightest match first.
    int idx = match(true,  true);
    if (idx < 0) idx = match(true,  false);
    if (idx < 0) idx = match(false, true);
    if (idx < 0) idx = match(false, false);
    if (idx < 0) return false;
    out = vs[idx];
    return true;
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
