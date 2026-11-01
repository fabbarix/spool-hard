#include "bambu_cloud_filaments.h"
#include "spoolhard/ring_log.h"
#include <ArduinoJson.h>
#include <map>
#include <math.h>
#include <utility>
#include "spoolhard/psram_json_alloc.h"

namespace BambuCloudFilaments {

// Slicer-version we send on the list query. The cloud uses this to
// decide what shape to send back. Hardcoded to a recent BambuStudio
// release; doesn't need to match what's running on a desktop.
static constexpr const char* kSlicerVersion = "02.04.00.70";

// Body cap for diagnostics — keeps NVS / response budget bounded.
static constexpr size_t kDiagBodyMax = 512;

static void _absorbDiag(Diag* diagOut, const char* stage,
                        const String& url,
                        const BambuCloudAuth::ApiResult& r) {
    dlog("CloudFil", "%s -> HTTP %d%s body=%u bytes",
         stage, r.httpStatus,
         r.cfBlocked ? " [CF block]" : "",
         (unsigned)r.body.length());
    if (r.httpStatus >= 400 || r.httpStatus <= 0) {
        String preview = r.body;
        if (preview.length() > 200) preview = preview.substring(0, 200) + "...";
        preview.replace("\n", " ");
        dlog("CloudFil", "   body: %s", preview.c_str());
    }
    if (!diagOut) return;
    diagOut->stage      = stage;
    diagOut->url        = url;
    diagOut->httpStatus = r.httpStatus;
    diagOut->cfBlocked  = r.cfBlocked;
    diagOut->body       = r.body.length() > kDiagBodyMax
                          ? r.body.substring(0, kDiagBodyMax) + "...[truncated]"
                          : r.body;
}

// "Bambu Lab H2S 0.4 nozzle"  → ("H2S",     0.4)
// "Bambu Lab X1 Carbon 0.6 nozzle" → ("X1C", 0.6)
// "Bambu Lab A1 mini 0.4 nozzle"   → ("A1mini", 0.4)
// "Bambu Lab P1P 0.4 nozzle" → ("P1P", 0.4)
// Returns false on unparseable input. The model code we emit is the
// short canonical form used everywhere downstream (matches what
// modelFromSerial() emits in bambu_printer.cpp).
static bool parseBambuCompatible(const String& s, String& model_out, float& nozzle_out) {
    // Locate " <N>.<NN> nozzle" suffix.
    int n_pos = s.indexOf(" nozzle");
    if (n_pos < 0) return false;
    int dia_start = s.lastIndexOf(' ', n_pos - 1);
    if (dia_start < 0) return false;
    String dia_s = s.substring(dia_start + 1, n_pos);
    float dia = dia_s.toFloat();
    if (dia <= 0.f) return false;
    // Strip "Bambu Lab " prefix to get the model name.
    int model_start = 0;
    if (s.startsWith("Bambu Lab ")) model_start = 10;
    String model_human = s.substring(model_start, dia_start);
    model_human.trim();
    // Canonicalize: "X1 Carbon" → "X1C", "A1 mini" → "A1mini", "X1" → "X1".
    String m = model_human;
    if      (m == "X1 Carbon") m = "X1C";
    else if (m == "A1 mini")   m = "A1mini";
    else                       m.replace(" ", ""); // "P1S" stays, "X1" stays
    model_out  = m;
    nozzle_out = dia;
    return true;
}

// Parse the array-or-comma-list shapes Bambu uses for numeric fields.
// Pull the value at `idx` (for fields where index encodes
// extruder-variant) and parse as int; default 0.
static int intAt(JsonVariantConst v, int idx) {
    if (v.is<JsonArrayConst>()) {
        JsonArrayConst arr = v.as<JsonArrayConst>();
        if (idx >= (int)arr.size()) idx = 0;
        if (idx >= (int)arr.size()) return 0;
        v = arr[idx];
    }
    if (v.is<const char*>()) {
        String t = v.as<const char*>();
        int comma = t.indexOf(',');
        return (comma >= 0 ? t.substring(0, comma) : t).toInt();
    }
    if (v.is<int>())   return v.as<int>();
    if (v.is<float>()) return (int)v.as<float>();
    return 0;
}
static float floatAt(JsonVariantConst v, int idx) {
    if (v.is<JsonArrayConst>()) {
        JsonArrayConst arr = v.as<JsonArrayConst>();
        if (idx >= (int)arr.size()) idx = 0;
        if (idx >= (int)arr.size()) return 0.f;
        v = arr[idx];
    }
    if (v.is<const char*>()) return atof(v.as<const char*>());
    if (v.is<float>())       return v.as<float>();
    if (v.is<int>())         return (float)v.as<int>();
    return 0.f;
}

// Identity + per-(printer,nozzle) variants extracted from one cloud preset.
// `cloud_preset_id` is the PFUS<hash> the cloud minted; we keep it so the
// grouper can record sibling ids for the eventual delete-and-recreate cycle.
struct ParsedPreset {
    String  cloud_preset_id;
    String  group_key;            // identity key — name prefix before " @"
    String  cloud_inherits;       // first variant's inherits chain
    String  filament_settings_id; // unquoted; lacks the " @<printer>" suffix
    // Identity fields (taken from the first preset of the group)
    String  filament_vendor;
    String  filament_type;
    String  filament_subtype;
    String  filament_id;
    String  base_id;
    int32_t nozzle_temp_min = -1;
    int32_t nozzle_temp_max = -1;
    float   density         = 0.f;
    // Variants emitted from this preset's compatible_printers cross-product
    std::vector<FilamentVariant> variants;
};

// Strip leading/trailing double quotes that BambuStudio cloud presets
// add around string values (`"\"PLA\""` → `PLA`).
static String unq(const String& v) {
    String s = v;
    while (s.startsWith("\"")) s.remove(0, 1);
    while (s.endsWith("\""))   s.remove(s.length() - 1, 1);
    return s;
}

// Locate the `setting` blob in either the top-level or `data`-wrapped shape.
static JsonObjectConst settingObj(const JsonDocument& doc) {
    JsonObjectConst s = doc["setting"].as<JsonObjectConst>();
    if (s.isNull()) s = doc["data"]["setting"].as<JsonObjectConst>();
    return s;
}

// Map a single cloud preset → ParsedPreset.
static bool parsePreset(const JsonDocument& doc,
                        const String& preset_id_hint,
                        ParsedPreset& out) {
    String id = doc["setting_id"]            | doc["id"]
              | doc["data"]["setting_id"]    | doc["data"]["id"]
              | (const char*)nullptr;
    if (!id.length()) id = preset_id_hint;
    out.cloud_preset_id = id;
    String name = doc["name"]    | doc["data"]["name"]    | "";
    out.base_id = doc["base_id"] | doc["data"]["base_id"] | "";
    out.filament_id = doc["filament_id"] | doc["data"]["filament_id"] | "";

    JsonObjectConst s = settingObj(doc);
    dlog("CloudFil", "parsePreset id=%s name=\"%s\" setting=%s",
         id.c_str(), name.c_str(), s.isNull() ? "NULL" : "ok");

    // Identity fields straight off the setting blob (when present).
    if (!s.isNull()) {
        if (s["filament_vendor"].is<const char*>())
            out.filament_vendor = unq(s["filament_vendor"].as<const char*>());
        if (s["filament_type"].is<const char*>())
            out.filament_type = unq(s["filament_type"].as<const char*>());
        if (s["filament_subtype"].is<const char*>())
            out.filament_subtype = unq(s["filament_subtype"].as<const char*>());
        if (s["inherits"].is<const char*>())
            out.cloud_inherits = s["inherits"].as<const char*>();
        if (s["filament_settings_id"].is<const char*>())
            out.filament_settings_id = unq(s["filament_settings_id"].as<const char*>());
        out.nozzle_temp_min = intAt(s["nozzle_temperature_range_low"],  0);
        out.nozzle_temp_max = intAt(s["nozzle_temperature_range_high"], 0);
        if (s["filament_density"].is<const char*>())
            out.density = atof(s["filament_density"].as<const char*>());
        // Range-fallback for older presets that only ship print-temp.
        if (out.nozzle_temp_max <= 0) {
            int nt = intAt(s["nozzle_temperature"], 0);
            if (nt > 0) {
                out.nozzle_temp_max = nt;
                if (out.nozzle_temp_min <= 0) out.nozzle_temp_min = nt - 10;
            }
        }
    }

    // Group key: the name without the " @<printer> <nozzle> nozzle" suffix.
    // Prefer the human `name` field; fall back to filament_settings_id.
    String key = name;
    int at_pos = key.indexOf(" @");
    if (at_pos >= 0) key = key.substring(0, at_pos);
    if (key.isEmpty() && out.filament_settings_id.length()) {
        key = out.filament_settings_id;
        int ap = key.indexOf(" @");
        if (ap >= 0) key = key.substring(0, ap);
    }
    if (key.isEmpty()) key = id;   // last-resort: every preset its own group
    out.group_key = key;

    if (s.isNull()) return true;

    // Build variants from compatible_printers. Each entry parses to a
    // (model, nozzle) pair. The numeric value arrays are keyed parallel
    // to `filament_extruder_variant` ("Direct Drive Standard;Direct
    // Drive High Flow") — index `i` in any of them refers to the same
    // extruder type. We mirror Bambu's shape internally so the user can
    // see + edit every entry instead of being collapsed onto slot 0.
    auto stripQuotes = [](String& tok) {
        tok.trim();
        while (tok.startsWith("\"")) tok.remove(0, 1);
        while (tok.endsWith("\""))   tok.remove(tok.length() - 1, 1);
    };
    std::vector<String> extr_variants;
    JsonVariantConst evRaw = s["filament_extruder_variant"];
    if (evRaw.is<JsonArrayConst>()) {
        // BambuStudio's on-disk shape: `["Direct Drive Standard",
        // "Direct Drive High Flow"]`.
        for (JsonVariantConst e : evRaw.as<JsonArrayConst>()) {
            String tok = e | "";
            stripQuotes(tok);
            if (tok.length()) extr_variants.push_back(tok);
        }
    } else if (evRaw.is<const char*>()) {
        // Cloud sometimes serializes as `;`-joined string with each
        // label quoted (`"Direct Drive Standard";"Direct Drive High
        // Flow"`).
        String es = evRaw.as<const char*>();
        int from = 0;
        while (from <= (int)es.length()) {
            int sc = es.indexOf(';', from);
            String tok = (sc < 0) ? es.substring(from) : es.substring(from, sc);
            stripQuotes(tok);
            if (tok.length()) extr_variants.push_back(tok);
            if (sc < 0) break;
            from = sc + 1;
        }
    }

    auto floatArr = [&](JsonVariantConst v) {
        std::vector<float> out;
        if (v.is<JsonArrayConst>()) {
            for (JsonVariantConst e : v.as<JsonArrayConst>()) {
                float f = 0.f;
                if (e.is<const char*>()) f = atof(e.as<const char*>());
                else if (e.is<float>())  f = e.as<float>();
                else if (e.is<int>())    f = (float)e.as<int>();
                out.push_back(f);
            }
        } else if (v.is<const char*>()) {
            // Cloud variant: comma-separated string (`"42,48"`).
            String t = v.as<const char*>();
            int from = 0;
            while (from <= (int)t.length()) {
                int c = t.indexOf(',', from);
                String tok = (c < 0) ? t.substring(from) : t.substring(from, c);
                tok.trim();
                if (tok.length()) out.push_back(tok.toFloat());
                if (c < 0) break;
                from = c + 1;
            }
        } else if (v.is<float>()) out.push_back(v.as<float>());
        else if (v.is<int>())    out.push_back((float)v.as<int>());
        return out;
    };
    int nt_print   = intAt(s["nozzle_temperature"], 0);
    int nt_initial = intAt(s["nozzle_temperature_initial_layer"], 0);
    auto mvs = floatArr(s["filament_max_volumetric_speed"]);
    auto pas = floatArr(s["pressure_advance"]);

    auto fillVariant = [&](FilamentVariant& fv) {
        fv.nozzle_temp_print         = nt_print   > 0 ? nt_print   : -1;
        fv.nozzle_temp_initial_layer = nt_initial > 0 ? nt_initial : -1;
        fv.extruder_variants         = extr_variants;
        fv.max_volumetric_speed      = mvs;
        fv.pressure_advance          = pas;
    };

    JsonVariantConst cp = s["compatible_printers"];
    dlog("CloudFil", "  cp.is_array=%d cp.is_str=%d extr_variants=%u mvs=%u pas=%u",
         (int)cp.is<JsonArrayConst>(), (int)cp.is<const char*>(),
         (unsigned)extr_variants.size(),
         (unsigned)mvs.size(),
         (unsigned)pas.size());
    auto pushVariant = [&](FilamentVariant&& fv) {
        dlog("CloudFil", "  variant model=%s nozzle=%.1f extr=%u speeds=%u pas=%u "
                         "Tprint=%d Tinit=%d",
             fv.printer_model.c_str(), fv.nozzle_diameter,
             (unsigned)fv.extruder_variants.size(),
             (unsigned)fv.max_volumetric_speed.size(),
             (unsigned)fv.pressure_advance.size(),
             (int)fv.nozzle_temp_print, (int)fv.nozzle_temp_initial_layer);
        out.variants.push_back(std::move(fv));
    };
    if (cp.is<JsonArrayConst>() && cp.as<JsonArrayConst>().size() > 0) {
        for (JsonVariantConst pv : cp.as<JsonArrayConst>()) {
            const char* p = pv | "";
            if (!*p) continue;
            FilamentVariant fv;
            if (!parseBambuCompatible(p, fv.printer_model, fv.nozzle_diameter)) {
                fv.printer_model   = p;
                fv.nozzle_diameter = 0.f;
            }
            fillVariant(fv);
            pushVariant(std::move(fv));
        }
    } else if (cp.is<const char*>()) {
        // Cloud serializes single-printer compatibility as a (quoted)
        // string — e.g. `"\"Bambu Lab H2S 0.4 nozzle\""`. Strip the
        // wrapping quotes and parse as one entry.
        String pe = cp.as<const char*>();
        while (pe.startsWith("\"")) pe.remove(0, 1);
        while (pe.endsWith("\""))   pe.remove(pe.length() - 1, 1);
        FilamentVariant fv;
        if (!parseBambuCompatible(pe, fv.printer_model, fv.nozzle_diameter)) {
            fv.printer_model   = pe;
            fv.nozzle_diameter = 0.f;
        }
        fillVariant(fv);
        pushVariant(std::move(fv));
    } else {
        FilamentVariant fv;
        if (at_pos >= 0) {
            String suffix = name.substring(at_pos + 2);
            parseBambuCompatible(suffix, fv.printer_model, fv.nozzle_diameter);
        }
        fillVariant(fv);
        pushVariant(std::move(fv));
    }
    return true;
}

// Fold a list of ParsedPreset into FilamentRecord groups keyed by
// group_key. Identity fields come from the first preset; later presets
// only contribute variants + cloud-id tracking.
static void foldGroups(std::vector<ParsedPreset>& parsed,
                       std::vector<FilamentRecord>& out) {
    std::map<String, size_t> idx;
    for (auto& p : parsed) {
        auto it = idx.find(p.group_key);
        FilamentRecord* rec;
        if (it == idx.end()) {
            FilamentRecord fr;
            fr.stock           = false;
            fr.name            = p.group_key;
            fr.base_id         = p.base_id;
            fr.cloud_inherits  = p.cloud_inherits;
            fr.filament_type   = p.filament_type;
            fr.filament_subtype = p.filament_subtype;
            fr.filament_vendor = p.filament_vendor;
            fr.filament_id     = p.filament_id;
            fr.nozzle_temp_min = p.nozzle_temp_min;
            fr.nozzle_temp_max = p.nozzle_temp_max;
            fr.density         = p.density;
            fr.cloud_setting_id = p.cloud_preset_id;  // first preset == anchor
            fr.cloud_synced_at  = (uint32_t)time(nullptr);
            // Track every cloud preset that landed in this group.
            JsonDocument idDoc;
            JsonArray arr = idDoc.to<JsonArray>();
            arr.add(p.cloud_preset_id);
            String idsJson;
            serializeJson(idDoc, idsJson);
            fr.cloud_variant_ids_json = idsJson;
            out.push_back(std::move(fr));
            idx[p.group_key] = out.size() - 1;
            rec = &out.back();
        } else {
            rec = &out[it->second];
            // Append cloud preset id to the sibling list.
            JsonDocument idDoc;
            if (rec->cloud_variant_ids_json.length())
                deserializeJson(idDoc, rec->cloud_variant_ids_json);
            else
                idDoc.to<JsonArray>();
            idDoc.as<JsonArray>().add(p.cloud_preset_id);
            String idsJson;
            serializeJson(idDoc, idsJson);
            rec->cloud_variant_ids_json = idsJson;
            // Promote identity fields if the first preset had them empty.
            if (rec->filament_vendor.isEmpty()) rec->filament_vendor = p.filament_vendor;
            if (rec->filament_type.isEmpty())   rec->filament_type   = p.filament_type;
            if (rec->filament_subtype.isEmpty())rec->filament_subtype = p.filament_subtype;
            if (rec->filament_id.isEmpty())     rec->filament_id     = p.filament_id;
            if (rec->base_id.isEmpty())         rec->base_id         = p.base_id;
            if (rec->nozzle_temp_min < 0 && p.nozzle_temp_min > 0)
                rec->nozzle_temp_min = p.nozzle_temp_min;
            if (rec->nozzle_temp_max < 0 && p.nozzle_temp_max > 0)
                rec->nozzle_temp_max = p.nozzle_temp_max;
            if (rec->density <= 0.f && p.density > 0.f)
                rec->density = p.density;
        }
        // Variants: append, deduping by (model, nozzle) so two presets
        // pointing at the same combo don't double-count.
        auto vs = rec->variants();
        for (auto& fv : p.variants) {
            bool dup = false;
            for (auto& ex : vs) {
                if (ex.printer_model == fv.printer_model
                 && fabsf(ex.nozzle_diameter - fv.nozzle_diameter) < 0.01f) {
                    dup = true; break;
                }
            }
            if (!dup) vs.push_back(fv);
        }
        rec->setVariants(vs);
    }
}

Result fetchAll(const String& token, BambuCloudAuth::Region region,
                std::vector<FilamentRecord>& out, Diag* diagOut) {
    out.clear();
    String path = String("/v1/iot-service/api/slicer/setting?version=")
                  + kSlicerVersion + "&public=false";
    String url  = String(BambuCloudAuth::regionBaseUrl(region)) + path;
    dlog("CloudFil", "fetchAll: GET %s", url.c_str());
    auto r = g_bambu_cloud.apiGet(region, path.c_str(), token);
    _absorbDiag(diagOut, "list", url, r);
    if (r.cfBlocked || r.httpStatus <= 0) return Result::Unreachable;
    if (r.httpStatus >= 400)              return Result::Rejected;

    JsonDocument listDoc;
    DeserializationError jerr = deserializeJson(listDoc, r.body);
    if (jerr) {
        dlog("CloudFil", "fetchAll: JSON parse failed (%s)", jerr.c_str());
        if (diagOut) diagOut->stage = "list (JSON parse failed)";
        return Result::Rejected;
    }

    int processed = 0, skipped = 0, failed = 0;
    std::vector<ParsedPreset> parsed;

    auto walkBucket = [&](const char* sub) -> Result {
        JsonArrayConst arr = listDoc["filament"][sub].as<JsonArrayConst>();
        if (arr.isNull()) return Result::Ok;
        dlog("CloudFil", "fetchAll: filament.%s has %u entries",
             sub, (unsigned)arr.size());
        for (JsonVariantConst v : arr) {
            const char* id = v["setting_id"] | "";
            if (!*id || strncmp(id, "PFUS", 4) != 0) { skipped++; continue; }
            String oneUrl = String(BambuCloudAuth::regionBaseUrl(region))
                          + "/v1/iot-service/api/slicer/setting/" + id;
            auto rr = g_bambu_cloud.apiGet(region,
                ("/v1/iot-service/api/slicer/setting/" + String(id)).c_str(),
                token);
            String stage = String("fetchOne ") + id;
            _absorbDiag(diagOut, stage.c_str(), oneUrl, rr);
            if (rr.cfBlocked || rr.httpStatus <= 0) return Result::Unreachable;
            if (rr.httpStatus >= 400) { failed++; continue; }
            JsonDocument doc;
            if (deserializeJson(doc, rr.body)) { failed++; continue; }
            ParsedPreset pp;
            if (parsePreset(doc, String(id), pp)) {
                parsed.push_back(std::move(pp));
                processed++;
            } else {
                failed++;
            }
        }
        return Result::Ok;
    };

    Result br = walkBucket("private");
    if (br == Result::Unreachable) return Result::Unreachable;
    br = walkBucket("public");
    if (br == Result::Unreachable) return Result::Unreachable;

    foldGroups(parsed, out);

    dlog("CloudFil", "fetchAll: %d ok, %d skipped, %d failed → %u groups",
         processed, skipped, failed, (unsigned)out.size());
    return Result::Ok;
}

Result fetchOne(const String& token, BambuCloudAuth::Region region,
                const String& preset_id, FilamentRecord& out,
                Diag* diagOut) {
    String path = "/v1/iot-service/api/slicer/setting/" + preset_id;
    String url  = String(BambuCloudAuth::regionBaseUrl(region)) + path;
    auto r = g_bambu_cloud.apiGet(region, path.c_str(), token);
    String stage = "fetchOne " + preset_id;
    _absorbDiag(diagOut, stage.c_str(), url, r);
    if (r.cfBlocked || r.httpStatus <= 0) return Result::Unreachable;
    if (r.httpStatus >= 400)              return Result::Rejected;

    JsonDocument doc;
    if (deserializeJson(doc, r.body)) {
        if (diagOut) diagOut->stage = stage + " (JSON parse failed)";
        return Result::Rejected;
    }
    ParsedPreset pp;
    if (!parsePreset(doc, preset_id, pp)) return Result::Rejected;
    std::vector<ParsedPreset> one;
    one.push_back(std::move(pp));
    std::vector<FilamentRecord> grouped;
    foldGroups(one, grouped);
    if (grouped.empty()) return Result::Rejected;
    out = grouped[0];
    return Result::Ok;
}

// ---- Cloud push: Studio-shaped standalone presets ----------------------
//
// Bambu Studio only resolves a user preset's filament_id (the AMS
// tray_info_idx) when the cloud copy looks like one Studio itself made:
// standalone (`inherits` ""), ~145 flattened keys, vendor/type unquoted,
// settings_id / compatible_printers quoted, per-extruder values
// comma-joined to the printer's `filament_extruder_variant` count. A thin
// preset that leans on `inherits` is shown as "unknown filament, generic
// preset used". The template below is a Studio-made "Jayo PETG Matte @Bambu
// Lab H2S" preset; record fields are overlaid on it per variant.

namespace {

enum : uint8_t {
    kPerExt     = 1,   // one value per extruder variant, comma-joined
    kPerExtSemi = 2,   // one quoted value per extruder variant, ';'-joined
    kPetgOnly   = 4,   // material-specific: only sent for the PETG family
};

struct TemplateKey { const char* key; const char* val; uint8_t flags; };

// Keys the overlay always computes (identity, temps, speeds, PA,
// compatibility, extruder variants) are not in this table.
const TemplateKey kTemplate[] = {
    {"activate_air_filtration",                 "0",   kPetgOnly},
    {"additional_cooling_fan_speed",            "0",   kPetgOnly},
    {"additional_fan_full_speed_layer",         "0",   0},
    {"chamber_temperatures",                    "0",   kPetgOnly},
    {"circle_compensation_speed",               "200", 0},
    {"close_additional_fan_first_x_layers",     "3",   0},
    {"close_fan_the_first_x_layers",            "3",   kPetgOnly},
    {"compatible_printers_condition",           "",    0},
    {"compatible_prints",                       "",    0},
    {"compatible_prints_condition",             "",    0},
    {"complete_print_exhaust_fan_speed",        "70",  kPetgOnly},
    {"cool_plate_temp",                         "0",   kPetgOnly},
    {"cool_plate_temp_initial_layer",           "0",   kPetgOnly},
    {"cooling_perimeter_transition_distance",   "10",  0},
    {"cooling_slowdown_logic",                  "uniform_cooling", 0},
    {"counter_coef_1",                          "0",      kPetgOnly},
    {"counter_coef_2",                          "0.0058", kPetgOnly},
    {"counter_coef_3",                          "0.0107", kPetgOnly},
    {"counter_limit_max",                       "0.15",   kPetgOnly},
    {"counter_limit_min",                       "0.01",   kPetgOnly},
    {"default_filament_colour",                 "\"\"", 0},
    {"diameter_limit",                          "50",  0},
    {"during_print_exhaust_fan_speed",          "70",  kPetgOnly},
    {"enable_overhang_bridge_fan",              "1",   0},
    {"eng_plate_temp",                          "70",  kPetgOnly},
    {"eng_plate_temp_initial_layer",            "70",  kPetgOnly},
    {"fan_cooling_layer_time",                  "20",  kPetgOnly},
    {"fan_max_speed",                           "50",  kPetgOnly},
    {"fan_min_speed",                           "20",  kPetgOnly},
    {"filament_adaptive_volumetric_speed",      "0",   kPerExt},
    {"filament_adhesiveness_category",          "300", kPetgOnly},
    {"filament_bridge_speed",                   "25",  kPerExt},
    {"filament_change_length",                  "4",   0},
    {"filament_change_length_nc",               "10",  0},
    {"filament_cooling_before_tower",           "10",  kPerExt},
    {"filament_cost",                           "0",   0},
    {"filament_deretraction_speed",             "nil", kPerExt},
    {"filament_dev_ams_drying_ams_limitations", "1;0", kPetgOnly},
    {"filament_dev_ams_drying_heat_distortion_temperature", "75", kPetgOnly},
    {"filament_dev_ams_drying_temperature",     "65,65,55,55", kPetgOnly},
    {"filament_dev_ams_drying_time",            "12,12,12,12", kPetgOnly},
    {"filament_dev_chamber_drying_bed_temperature", "80", kPetgOnly},
    {"filament_dev_chamber_drying_time",        "12",  kPetgOnly},
    {"filament_dev_drying_cooling_temperature", "55",  kPetgOnly},
    {"filament_dev_drying_softening_temperature", "60", kPetgOnly},
    {"filament_diameter",                       "1.75", 0},
    {"filament_enable_overhang_speed",          "1",   kPerExt},
    {"filament_end_gcode",                      "\"; filament end gcode \\n\"", 0},
    {"filament_extruder_compatibility",         "0",   0},
    {"filament_flow_ratio",                     "0.95", kPerExt},
    {"filament_flush_temp",                     "0",   kPerExt},
    {"filament_flush_temp_fast",                "0",   kPerExt},
    {"filament_flush_volumetric_speed",         "0",   kPerExt},
    {"filament_is_support",                     "0",   0},
    {"filament_long_retractions_when_cut",      "nil", kPerExt},
    {"filament_metal_stickiness",               "High", kPetgOnly},
    {"filament_minimal_purge_on_wipe_tower",    "15",  0},
    {"filament_notes",                          "",    0},
    {"filament_overhang_1_4_speed",             "0",   kPerExt},
    {"filament_overhang_2_4_speed",             "50",  kPerExt},
    {"filament_overhang_3_4_speed",             "30",  kPerExt},
    {"filament_overhang_4_4_speed",             "10",  kPerExt},
    {"filament_overhang_totally_speed",         "10",  kPerExt},
    {"filament_pre_cooling_temperature",        "0",   kPerExt},
    {"filament_pre_cooling_temperature_nc",     "0",   kPerExt},
    {"filament_preheat_temperature_delta",      "10",  kPerExt},
    {"filament_prime_volume",                   "30",  0},
    {"filament_prime_volume_nc",                "60",  0},
    {"filament_printable",                      "3",   0},
    {"filament_ramming_travel_time",            "0",   kPerExt},
    {"filament_ramming_travel_time_nc",         "0",   kPerExt},
    {"filament_ramming_volumetric_speed",       "-1",  kPerExt},
    {"filament_ramming_volumetric_speed_nc",    "-1",  kPerExt},
    {"filament_retract_before_wipe",            "nil", kPerExt},
    {"filament_retract_length_nc",              "14",  kPerExt},
    {"filament_retract_restart_extra",          "nil", kPerExt},
    {"filament_retract_when_changing_layer",    "nil", kPerExt},
    {"filament_retraction_distances_when_cut",  "nil", kPerExt},
    {"filament_retraction_length",              "nil", kPerExt},
    {"filament_retraction_minimum_travel",      "nil", kPerExt},
    {"filament_retraction_speed",               "nil", kPerExt},
    {"filament_scarf_gap",                      "0%",  0},
    {"filament_scarf_height",                   "10%", 0},
    {"filament_scarf_length",                   "10",  0},
    {"filament_scarf_seam_type",                "none", 0},
    {"filament_shrink",                         "100%", 0},
    {"filament_soluble",                        "0",   0},
    {"filament_start_gcode",                    "\"; filament start gcode\\n\"", 0},
    {"filament_tower_interface_pre_extrusion_dist",   "10", 0},
    {"filament_tower_interface_pre_extrusion_length", "0",  0},
    {"filament_tower_interface_print_temp",     "-1",  0},
    {"filament_tower_interface_purge_volume",   "20",  0},
    {"filament_tower_ironing_area",             "8",   0},
    {"filament_velocity_adaptation_factor",     "1",   0},
    {"filament_wipe",                           "nil", kPerExt},
    {"filament_wipe_distance",                  "nil", kPerExt},
    {"filament_z_hop",                          "nil", kPerExt},
    {"filament_z_hop_types",                    "nil", kPerExt},
    {"first_x_layer_fan_speed",                 "0",   0},
    {"first_x_layer_part_fan_speed",            "0",   0},
    {"full_fan_speed_layer",                    "0",   0},
    {"hole_coef_1",                             "0",       kPetgOnly},
    {"hole_coef_2",                             "-0.0042", kPetgOnly},
    {"hole_coef_3",                             "0.2006",  kPetgOnly},
    {"hole_limit_max",                          "0.2",     kPetgOnly},
    {"hole_limit_min",                          "0.09",    kPetgOnly},
    {"hot_plate_temp",                          "70",  kPetgOnly},
    {"hot_plate_temp_initial_layer",            "70",  kPetgOnly},
    {"impact_strength_z",                       "10.6", kPetgOnly},
    {"inherits",                                "",    0},
    {"ironing_fan_speed",                       "-1",  0},
    {"long_retractions_when_ec",                "0",   kPerExt},
    {"no_slow_down_for_cooling_on_outwalls",    "0",   0},
    {"overhang_fan_speed",                      "100", 0},
    {"overhang_fan_threshold",                  "10%", 0},
    {"overhang_threshold_participating_cooling", "95%", 0},
    {"override_process_overhang_speed",         "0",   kPerExt},
    {"pre_start_fan_time",                      "2",   0},
    {"reduce_fan_stop_start_freq",              "1",   0},
    {"required_nozzle_HRC",                     "3",   0},
    {"retraction_distances_when_ec",            "0",   kPerExt},
    {"slow_down_for_layer_cooling",             "1",   0},
    {"slow_down_layer_time",                    "10",  kPetgOnly},
    {"slow_down_min_speed",                     "20",  kPerExt},
    {"supertack_plate_temp",                    "60",  kPetgOnly},
    {"supertack_plate_temp_initial_layer",      "60",  kPetgOnly},
    {"temperature_vitrification",               "70",  kPetgOnly},
    {"textured_plate_temp",                     "70",  kPetgOnly},
    {"textured_plate_temp_initial_layer",       "70",  kPetgOnly},
    {"volumetric_speed_coefficients",           "0 0 0 0 0 0", kPerExtSemi},
};

// Non-PETG materials: the PETG-only keys above are left out (Studio fills
// a standalone preset's missing keys from its defaults) and these
// Bambu-generic plate temps / fan speeds go in instead. -1 = leave out.
struct MaterialDefaults {
    const char* prefix;     // matched against the upper-cased filament_type
    int cool, eng, hot, textured;
    int fanMin, fanMax;
    float maxVolSpeed;      // mm3/s at >= 0.4 mm nozzles
};
const MaterialDefaults kMaterials[] = {
    {"PETG", -1,  -1,  -1,  -1,  -1, -1, 12.f},  // PETG/PET/PCTG: template values
    {"PET",  -1,  -1,  -1,  -1,  -1, -1, 12.f},
    {"PCTG", -1,  -1,  -1,  -1,  -1, -1, 12.f},
    {"PLA",  35,  55,  55,  55, 100, 100, 21.f},
    {"ABS",   0,  90,  90,  90,  10, 80, 16.f},
    {"ASA",   0,  90,  90,  90,  10, 80, 16.f},
    {"TPU",  30,  30,  35,  35, 100, 100, 3.6f},
    {"PC",    0, 110, 110, 110,  -1, -1, 12.f},
    {"PA",    0, 100, 100, 100,  -1, -1, 8.f},
};

const MaterialDefaults* _materialFor(const String& type) {
    String t = type; t.toUpperCase();
    for (const auto& m : kMaterials)
        if (t.startsWith(m.prefix)) return &m;
    return nullptr;
}

bool _isPetgFamily(const MaterialDefaults* m) {
    return m && m->cool < 0;   // the template-backed rows
}

// `filament_extruder_variant` per printer/nozzle. Verified on Studio 2.8
// H2S presets: 0.2/0.8 list Standard + High Flow, 0.4/0.6 add E3D High
// Flow (older two-entry 0.4 presets still resolve). Other printers get Standard + High Flow (Studio 2.x's common
// filament base). A variant that carries its own list wins.
std::vector<String> _extruderVariantsFor(const String& model, float nozzle) {
    std::vector<String> v = {"Direct Drive Standard", "Direct Drive High Flow"};
    if (model == "H2S" && (fabsf(nozzle - 0.4f) < 0.01f || fabsf(nozzle - 0.6f) < 0.01f))
        v.push_back("Direct Drive E3D High Flow");
    return v;
}

} // namespace

// Parse "<base> @BBL <printer> [<nozzle> nozzle]" out of an `inherits`
// string so we can fill name suffix / compatible_printers when the
// variant didn't carry printer_model + nozzle_diameter (current cloud
// sync drops those). Returns false if no @BBL/@Bambu Lab marker.
static bool _parseInheritsPrinter(const String& inherits, String& model, float& nozzle) {
    int idx = inherits.indexOf(" @BBL ");
    int markerLen = 6;
    if (idx < 0) { idx = inherits.indexOf(" @Bambu Lab "); markerLen = 12; }
    if (idx < 0) return false;
    String rest = inherits.substring(idx + markerLen);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp < 0) {
        model  = rest;
        nozzle = 0.4f;   // default — most common
        return true;
    }
    model = rest.substring(0, sp);
    String tail = rest.substring(sp + 1);
    tail.trim();
    float n = tail.toFloat();
    nozzle = (n > 0.f) ? n : 0.4f;
    return true;
}

static String _humanizePrinterModel(const String& code) {
    if (code == "X1C")      return "X1 Carbon";
    if (code == "A1mini")   return "A1 mini";
    return code;   // X1, P1S, P1P, H2S, H2D, A1 — all match Bambu's "Bambu Lab <code>" naming verbatim
}

// Build the Studio-shaped preset for one (printer, nozzle) variant and
// POST it. `printerModel` / `nozzleDiam` are already resolved by createOne.
static Result createOneVariant(const String& token, BambuCloudAuth::Region region,
                               const FilamentRecord& in, const FilamentVariant& fv,
                               const String& printerModel, float nozzleDiam,
                               String& newCloudIdOut, Diag* diagOut) {
    char nz[16];
    snprintf(nz, sizeof(nz), "%.1f", nozzleDiam);
    const String compatPrinter = "Bambu Lab " + _humanizePrinterModel(printerModel)
                               + " " + nz + " nozzle";
    const String presetName = in.name + " @" + compatPrinter;
    auto quote = [](const String& s) { return "\"" + s + "\""; };

    const std::vector<String> extVariants = fv.extruder_variants.empty()
        ? _extruderVariantsFor(printerModel, nozzleDiam) : fv.extruder_variants;
    const size_t nExt = extVariants.size();
    auto fanOut = [&](const String& v, char sep, bool quoted) {
        String out;
        for (size_t i = 0; i < nExt; ++i) {
            if (i) out += sep;
            out += quoted ? quote(v) : v;
        }
        return out;
    };
    auto fanOutFloats = [&](const std::vector<float>& src, float fallback, int dp) {
        // Short stored arrays repeat their last value: Bambu rejects
        // mismatched widths. Unset (<= 0) entries take the fallback.
        String out;
        for (size_t i = 0; i < nExt; ++i) {
            float v = i < src.size() ? src[i] : (src.empty() ? fallback : src.back());
            if (v <= 0.f) v = fallback;
            if (i) out += ",";
            out += String(v, dp);
        }
        return out;
    };

    const MaterialDefaults* mat = _materialFor(in.filament_type);
    const bool petg = _isPetgFamily(mat);

    JsonDocument body(&g_psramJsonAlloc);
    body["type"]    = "filament";
    body["name"]    = presetName;
    body["version"] = "2.8.0.9";   // what Studio 2.x writes
    body["base_id"] = nullptr;     // standalone, like Studio's own presets
    if (in.filament_id.length()) body["filament_id"] = in.filament_id;
    JsonObject setting = body["setting"].to<JsonObject>();

    for (const auto& k : kTemplate) {
        if ((k.flags & kPetgOnly) && !petg) continue;
        if (k.flags & kPerExt)          setting[k.key] = fanOut(k.val, ',', false);
        else if (k.flags & kPerExtSemi) setting[k.key] = fanOut(k.val, ';', true);
        else                            setting[k.key] = k.val;
    }
    if (mat && !petg) {
        auto put = [&](const char* a, const char* b, int v) {
            if (v < 0) return;
            setting[a] = String(v);
            if (b) setting[b] = String(v);
        };
        put("cool_plate_temp",     "cool_plate_temp_initial_layer",     mat->cool);
        put("eng_plate_temp",      "eng_plate_temp_initial_layer",      mat->eng);
        put("hot_plate_temp",      "hot_plate_temp_initial_layer",      mat->hot);
        put("textured_plate_temp", "textured_plate_temp_initial_layer", mat->textured);
        put("fan_min_speed", nullptr, mat->fanMin);
        put("fan_max_speed", nullptr, mat->fanMax);
    }

    // Identity: settings_id / compatible_printers quoted, vendor / type /
    // filament_id plain (Studio's stored shapes).
    setting["filament_settings_id"] = quote(presetName);
    setting["compatible_printers"]  = quote(compatPrinter);
    if (in.filament_id.length())     setting["filament_id"]     = in.filament_id;
    if (in.filament_vendor.length()) setting["filament_vendor"] = in.filament_vendor;
    if (in.filament_type.length())   setting["filament_type"]   = in.filament_type;
    String ev;
    for (size_t i = 0; i < nExt; ++i) { if (i) ev += ";"; ev += quote(extVariants[i]); }
    setting["filament_extruder_variant"] = ev;

    // Temperatures: range is a scalar pair; the print temps are
    // per-extruder. Without a variant print temp use the range midpoint
    // (rounded to 5 C).
    int lo = in.nozzle_temp_min, hi = in.nozzle_temp_max;
    if (lo > 0 && hi > 0 && lo > hi) std::swap(lo, hi);
    if (lo > 0) setting["nozzle_temperature_range_low"]  = String(lo);
    if (hi > 0) setting["nozzle_temperature_range_high"] = String(hi);
    int nt = fv.nozzle_temp_print;
    if (nt <= 0 && lo > 0 && hi > 0) nt = ((lo + hi) / 2 + 2) / 5 * 5;
    if (nt <= 0) nt = hi > 0 ? hi : lo;
    if (nt > 0) {
        int ni = fv.nozzle_temp_initial_layer > 0 ? fv.nozzle_temp_initial_layer : nt;
        setting["nozzle_temperature"]               = fanOut(String(nt), ',', false);
        setting["nozzle_temperature_initial_layer"] = fanOut(String(ni), ',', false);
    }
    if (in.density > 0.f) setting["filament_density"] = String(in.density, 2);

    float mvs = mat ? mat->maxVolSpeed : 12.f;
    if (nozzleDiam < 0.3f && mvs > 2.f) mvs = 2.f;   // 0.2 mm nozzle
    setting["filament_max_volumetric_speed"] = fanOutFloats(fv.max_volumetric_speed, mvs, 1);

    // Studio stores PA as one scalar; only enable it for a real value.
    float pa = 0.f;
    for (float v : fv.pressure_advance) if (v > 0.f) { pa = v; break; }
    setting["pressure_advance"]        = pa > 0.f ? String(pa, 3) : String("0.02");
    setting["enable_pressure_advance"] = pa > 0.f ? "1" : "0";

    setting["updated_time"] = String((uint32_t)time(nullptr));

    String bodyStr;
    serializeJson(body, bodyStr);

    String url = String(BambuCloudAuth::regionBaseUrl(region))
                 + "/v1/iot-service/api/slicer/setting";
    auto r = g_bambu_cloud.apiPost(region, "/v1/iot-service/api/slicer/setting",
                                   token, bodyStr);
    dlog("CloudFil", "create '%s' (%u keys, %u B) -> HTTP %d",
         presetName.c_str(), (unsigned)setting.size(), (unsigned)bodyStr.length(),
         r.httpStatus);
    _absorbDiag(diagOut, "create", url, r);
    if (r.cfBlocked || r.httpStatus <= 0) return Result::Unreachable;
    if (r.httpStatus >= 400)              return Result::Rejected;

    JsonDocument resp;
    if (deserializeJson(resp, r.body)) {
        if (diagOut) diagOut->stage = "create (JSON parse failed)";
        return Result::Rejected;
    }
    newCloudIdOut = resp["setting_id"] | "";
    if (newCloudIdOut.isEmpty()) {
        if (diagOut) diagOut->stage = "create (no setting_id in response)";
        return Result::Rejected;
    }
    return Result::Ok;
}

Result createOne(const String& token, BambuCloudAuth::Region region,
                 const FilamentRecord& in, String& newCloudIdOut,
                 Diag* diagOut, const String& defaultModel, float defaultNozzle) {
    // One cloud preset per (printer, nozzle). A variant without its own
    // printer/nozzle takes it from `cloud_inherits`, else from the
    // default (the current printer and its installed nozzle). No
    // variants at all = one preset for the default.
    struct Target { FilamentVariant fv; String model; float nozzle; };
    std::vector<Target> targets;
    auto addTarget = [&](const FilamentVariant& fv) {
        String model = fv.printer_model;
        float  nozzle = fv.nozzle_diameter;
        if (model.isEmpty() || nozzle <= 0.f) {
            String pm; float pn = 0.f;
            if (_parseInheritsPrinter(in.cloud_inherits, pm, pn)) {
                if (model.isEmpty()) model  = pm;
                if (nozzle <= 0.f)   nozzle = pn;
            }
        }
        if (model.isEmpty())  model  = defaultModel;
        if (nozzle <= 0.f)    nozzle = defaultNozzle;
        if (model.isEmpty() || nozzle <= 0.f) return;
        // Same name twice = HTTP 409 "setting name conflict".
        for (const auto& t : targets)
            if (t.model == model && fabsf(t.nozzle - nozzle) < 0.01f) return;
        targets.push_back({fv, model, nozzle});
    };
    auto vs = in.variants();
    for (const auto& fv : vs) addTarget(fv);
    if (targets.empty()) addTarget(FilamentVariant{});
    if (targets.empty()) {
        if (diagOut) diagOut->stage = "create (no printer: connect a printer or add a printer/nozzle variant)";
        return Result::Rejected;
    }

    // The first id wins the `cloud_setting_id` slot; the caller persists
    // `cloud_variant_ids` to track all of them.
    JsonDocument idDoc;
    JsonArray arr = idDoc.to<JsonArray>();
    String firstId;
    for (const auto& t : targets) {
        String thisId;
        Result rr = createOneVariant(token, region, in, t.fv, t.model, t.nozzle,
                                     thisId, diagOut);
        if (rr != Result::Ok) return rr;
        arr.add(thisId);
        if (firstId.isEmpty()) firstId = thisId;
    }
    newCloudIdOut = firstId;
    String idsJson; serializeJson(idDoc, idsJson);
    // Stash the sibling-id list inside diag.body when the caller asked
    // for diagnostics — that way it can persist them on the local record
    // without us needing to widen this signature.
    if (diagOut) {
        diagOut->stage = String("create (presets=") + (int)targets.size() + ")";
        diagOut->body  = idsJson;
        diagOut->httpStatus = 200;
    }
    return Result::Ok;
}

Result deleteOne(const String& token, BambuCloudAuth::Region region,
                 const String& preset_id, Diag* diagOut) {
    String path = "/v1/iot-service/api/slicer/setting/" + preset_id;
    String url  = String(BambuCloudAuth::regionBaseUrl(region)) + path;
    auto r = g_bambu_cloud.apiDelete(region, path.c_str(), token);
    String stage = "delete " + preset_id;
    _absorbDiag(diagOut, stage.c_str(), url, r);
    if (r.cfBlocked || r.httpStatus <= 0) return Result::Unreachable;
    if (r.httpStatus >= 400)              return Result::Rejected;
    return Result::Ok;
}

} // namespace BambuCloudFilaments
