#include "bambu_cloud_filaments.h"
#include "ring_log.h"
#include <ArduinoJson.h>

namespace BambuCloudFilaments {

// Slicer-version we send on the list query. The cloud uses this to
// decide what shape to send back. Hardcoded to a recent BambuStudio
// release; doesn't need to match what's running on a desktop.
// Bambu's API differentiates the response shape on this string. Sniffed
// from bambuddy + verified against api.bambulab.com: the dotted-zero-padded
// "02.XX.XX.XX" format unlocks the public catalog (~1600 filament
// entries) on /slicer/setting; the freeform "2.4.0.5" silently returns
// public:[] regardless of `?public=true`. Keep this padded.
static constexpr const char* kSlicerVersion = "02.04.00.70";

// Body cap for diagnostics — keeps NVS / response budget bounded.
// 512B is enough to pin down most cloud-side error messages.
static constexpr size_t kDiagBodyMax = 512;

// Map BambuCloudAuth::ApiResult into our diagnostics struct + log to
// serial. Centralised so every cloud call surfaces the same shape.
static void _absorbDiag(Diag* diagOut, const char* stage,
                        const String& url,
                        const BambuCloudAuth::ApiResult& r) {
    dlog("CloudFil", "%s -> HTTP %d%s body=%u bytes",
         stage, r.httpStatus,
         r.cfBlocked ? " [CF block]" : "",
         (unsigned)r.body.length());
    if (r.httpStatus >= 400 || r.httpStatus <= 0) {
        // Print a short body preview when something went wrong so the
        // user can grep for it in the log.
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

// Map a single preset JSON document (as the cloud returns it) into a
// FilamentRecord. The cloud's `setting` object carries the *delta* from
// the parent base preset, so resolved nozzle temps / density may not be
// present in every preset. Fields we can't extract default to unset
// (-1 / 0.f) — the user can fill them in via the UI later.
//
// `preset_id_hint` is the PFUS<hash> we already learned from the list
// endpoint; the per-preset GET response embeds the real preset under
// `data.setting_id` (and a few other nested shapes), so the top-level
// lookup we used originally was returning empty for every preset. We
// fall back to the hint to guarantee a non-empty id even if the body
// shape changes.
static void parseCloudPreset(const JsonDocument& doc,
                             const String& preset_id_hint,
                             FilamentRecord& out) {
    // Walk the most likely places the cloud returns the id, then fall
    // back to the URL hint. Observed shapes in practice:
    //   { setting_id: "PFUS...", name: "...", ... }
    //   { data: { setting_id: "PFUS...", ... } }
    //   { message:"success", setting:{ ... } }   (no id at all)
    out.setting_id = doc["setting_id"]            | doc["id"]
                   | doc["data"]["setting_id"]    | doc["data"]["id"]
                   | (const char*)nullptr;
    if (!out.setting_id.length()) out.setting_id = preset_id_hint;
    out.cloud_setting_id = out.setting_id;
    out.stock      = false;
    // Same fallback chain for the human-facing fields.
    out.name       = doc["name"]    | doc["data"]["name"]    | "";
    out.base_id    = doc["base_id"] | doc["data"]["base_id"] | "";
    out.filament_id = doc["filament_id"] | doc["data"]["filament_id"] | "";
    out.cloud_synced_at = (uint32_t)time(nullptr);

    // Pull what we can out of the delta `setting` blob. The blob lives
    // under either `setting` (top-level shape) or `data.setting` (the
    // wrapped shape — same fallback chain as the id walk above).
    JsonObjectConst s = doc["setting"].as<JsonObjectConst>();
    if (s.isNull()) s = doc["data"]["setting"].as<JsonObjectConst>();
    if (!s.isNull()) {
        const char* vendor = s["filament_vendor"] | "";
        if (*vendor) {
            // Cloud quotes string values: "\"Devil Design\"" — strip.
            String v = vendor;
            v.replace("\"", "");
            out.filament_vendor = v;
        }
        // nozzle_temperature is "<max>,<initial-layer>" comma-list — we
        // pull the first as max, default min to max-10 if not given.
        const char* nt = s["nozzle_temperature"] | "";
        if (*nt) {
            String s_nt = nt;
            int comma = s_nt.indexOf(',');
            int hi = (comma >= 0 ? s_nt.substring(0, comma) : s_nt).toInt();
            if (hi > 0) {
                out.nozzle_temp_max = hi;
                if (out.nozzle_temp_min < 0) out.nozzle_temp_min = hi - 10;
            }
        }
        if (s["filament_type"].is<const char*>()) {
            String t = s["filament_type"].as<const char*>();
            t.replace("\"", "");
            out.filament_type = t;
        }
        if (s["pressure_advance"].is<const char*>()) {
            out.pressure_advance = atof(s["pressure_advance"].as<const char*>());
        }
        if (s["filament_density"].is<const char*>()) {
            out.density = atof(s["filament_density"].as<const char*>());
        }
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

    // List response shape (observed against api.bambulab.com — the
    // public bambuddy doc was incomplete here):
    //   { "message":"success", "code":null, "error":null,
    //     "print":    { "public":[...], "private":[...] },
    //     "filament": { "public":[...], "private":[...] },
    //     "printer":  { "public":[...], "private":[...] } }
    // Each leaf array carries preset summary objects with setting_id +
    // metadata. We only care about the filament category — entries
    // there have PFUS<hash> setting_ids.
    JsonDocument listDoc;
    DeserializationError jerr = deserializeJson(listDoc, r.body);
    if (jerr) {
        dlog("CloudFil", "fetchAll: JSON parse failed (%s)", jerr.c_str());
        if (diagOut) diagOut->stage = "list (JSON parse failed)";
        return Result::Rejected;
    }

    int processed = 0, skipped = 0, failed = 0;

    auto walkBucket = [&](const char* sub) -> Result {
        JsonArrayConst arr = listDoc["filament"][sub].as<JsonArrayConst>();
        if (arr.isNull()) return Result::Ok;  // missing bucket = nothing to do
        dlog("CloudFil", "fetchAll: filament.%s has %u entries",
             sub, (unsigned)arr.size());
        for (JsonVariantConst v : arr) {
            const char* id = v["setting_id"] | "";
            // Defensive: only PFUS<hash>-prefixed ids are filament
            // presets. Skip anything else even if it lands here.
            if (!*id || strncmp(id, "PFUS", 4) != 0) { skipped++; continue; }
            FilamentRecord rec;
            Diag perDiag;
            Result fr = fetchOne(token, region, String(id), rec, &perDiag);
            if (fr == Result::Unreachable) {
                if (diagOut) *diagOut = perDiag;
                return Result::Unreachable;
            }
            if (fr == Result::Ok) { out.push_back(std::move(rec)); processed++; }
            else                  { failed++; }
        }
        return Result::Ok;
    };

    Result br = walkBucket("private");
    if (br == Result::Unreachable) return Result::Unreachable;
    br = walkBucket("public");
    if (br == Result::Unreachable) return Result::Unreachable;

    dlog("CloudFil", "fetchAll: %d ok, %d skipped, %d failed",
         processed, skipped, failed);
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
    parseCloudPreset(doc, preset_id, out);
    return Result::Ok;
}

Result createOne(const String& token, BambuCloudAuth::Region region,
                 const FilamentRecord& in, String& newCloudIdOut,
                 Diag* diagOut) {
    // Build the create-preset body per the doc's example.
    JsonDocument body;
    body["type"]    = "filament";
    body["name"]    = in.name;
    body["version"] = "2.0.0.0";   // any string; cloud doesn't validate strictly
    body["base_id"] = in.base_id;
    JsonObject setting = body["setting"].to<JsonObject>();
    if (in.filament_vendor.length())
        setting["filament_vendor"] = "\"" + in.filament_vendor + "\"";
    if (in.nozzle_temp_max > 0)
        setting["nozzle_temperature"] =
            String(in.nozzle_temp_max) + "," + String(in.nozzle_temp_max);
    if (in.density > 0.f)
        setting["filament_density"] = String(in.density, 3);
    if (in.pressure_advance > 0.f) {
        setting["pressure_advance"] = String(in.pressure_advance, 3);
        setting["enable_pressure_advance"] = "1";
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
