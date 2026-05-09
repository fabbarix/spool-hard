#include "bambu_cloud_filaments.h"
#include "spoolhard/ring_log.h"
#include <ArduinoJson.h>
#include <map>

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

// Build the cloud-side `setting` blob for one variant of a logical
// filament, then POST it. Used by createOne to emit one cloud preset
// per variant. Group identity (vendor, density, range_low/high) is
// repeated across every sibling preset so each is self-contained.
static Result createOneVariant(const String& token, BambuCloudAuth::Region region,
                               const FilamentRecord& in, const FilamentVariant& fv,
                               String& newCloudIdOut, Diag* diagOut) {
    JsonDocument body;
    body["type"]    = "filament";
    // Bambu's name convention: "<filament> @<printer> <nozzle> nozzle"
    String printerHuman = fv.printer_model;
    if (printerHuman == "X1C")    printerHuman = "X1 Carbon";
    else if (printerHuman == "A1mini") printerHuman = "A1 mini";
    String suffix;
    if (fv.printer_model.length() && fv.nozzle_diameter > 0.f) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", fv.nozzle_diameter);
        suffix = " @Bambu Lab " + printerHuman + " " + String(buf) + " nozzle";
    }
    body["name"]    = in.name + suffix;
    body["version"] = "2.0.0.0";
    body["base_id"] = in.base_id;
    JsonObject setting = body["setting"].to<JsonObject>();
    if (in.filament_vendor.length())
        setting["filament_vendor"] = "\"" + in.filament_vendor + "\"";
    if (in.filament_type.length())
        setting["filament_type"] = "\"" + in.filament_type + "\"";
    if (in.nozzle_temp_min > 0) {
        JsonArray a = setting["nozzle_temperature_range_low"].to<JsonArray>();
        a.add(String(in.nozzle_temp_min));
    }
    if (in.nozzle_temp_max > 0) {
        JsonArray a = setting["nozzle_temperature_range_high"].to<JsonArray>();
        a.add(String(in.nozzle_temp_max));
    }
    if (fv.nozzle_temp_print > 0 || in.nozzle_temp_max > 0) {
        int nt = fv.nozzle_temp_print > 0 ? fv.nozzle_temp_print : in.nozzle_temp_max;
        int ni = fv.nozzle_temp_initial_layer > 0 ? fv.nozzle_temp_initial_layer : nt;
        JsonArray a = setting["nozzle_temperature"].to<JsonArray>();
        a.add(String(nt));
        JsonArray b = setting["nozzle_temperature_initial_layer"].to<JsonArray>();
        b.add(String(ni));
    }
    if (in.density > 0.f)
        setting["filament_density"] = String(in.density, 3);
    if (!fv.max_volumetric_speed.empty()) {
        JsonArray a = setting["filament_max_volumetric_speed"].to<JsonArray>();
        for (float v : fv.max_volumetric_speed) a.add(String(v, 2));
    }
    if (!fv.pressure_advance.empty()) {
        JsonArray a = setting["pressure_advance"].to<JsonArray>();
        for (float v : fv.pressure_advance) a.add(String(v, 3));
        setting["enable_pressure_advance"] = "1";
    }
    if (!fv.extruder_variants.empty()) {
        // Cloud expects the `;`-joined "<v0>;<v1>" form.
        String joined;
        for (size_t i = 0; i < fv.extruder_variants.size(); ++i) {
            if (i) joined += ";";
            joined += fv.extruder_variants[i];
        }
        setting["filament_extruder_variant"] = joined;
    }
    if (fv.printer_model.length() && fv.nozzle_diameter > 0.f) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", fv.nozzle_diameter);
        JsonArray cp = setting["compatible_printers"].to<JsonArray>();
        cp.add(String("Bambu Lab ") + printerHuman + " " + String(buf) + " nozzle");
    }
    setting["updated_time"] = String((uint32_t)time(nullptr));

    String bodyStr;
    serializeJson(body, bodyStr);

    String url = String(BambuCloudAuth::regionBaseUrl(region))
                 + "/v1/iot-service/api/slicer/setting";
    auto r = g_bambu_cloud.apiPost(region, "/v1/iot-service/api/slicer/setting",
                                   token, bodyStr);
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
                 Diag* diagOut) {
    auto vs = in.variants();
    if (vs.empty()) {
        // Degenerate: no variants — fabricate a wildcard one so the
        // record still round-trips.
        FilamentVariant fv;
        fv.nozzle_temp_print         = in.nozzle_temp_max;
        fv.nozzle_temp_initial_layer = in.nozzle_temp_min;
        return createOneVariant(token, region, in, fv, newCloudIdOut, diagOut);
    }
    // Push every variant as a separate cloud preset. The first id wins
    // the `cloud_setting_id` slot; the caller persists `cloud_variant_ids`
    // to track all of them.
    JsonDocument idDoc;
    JsonArray arr = idDoc.to<JsonArray>();
    String firstId;
    for (size_t i = 0; i < vs.size(); ++i) {
        String thisId;
        Result rr = createOneVariant(token, region, in, vs[i], thisId, diagOut);
        if (rr != Result::Ok) {
            // Roll forward — record what we have so far, but surface failure.
            String idsJson; serializeJson(idDoc, idsJson);
            // Caller writes cloud_variant_ids based on newCloudIdOut state;
            // we emit the empty-on-failure form here and let the caller
            // surface the diagnostic message.
            return rr;
        }
        arr.add(thisId);
        if (firstId.isEmpty()) firstId = thisId;
    }
    newCloudIdOut = firstId;
    String idsJson; serializeJson(idDoc, idsJson);
    // Stash the sibling-id list inside diag.body when the caller asked
    // for diagnostics — that way it can persist them on the local record
    // without us needing to widen this signature.
    if (diagOut) {
        diagOut->stage = String("create (variants=") + (int)vs.size() + ")";
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
