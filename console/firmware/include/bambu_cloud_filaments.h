#pragma once
#include <Arduino.h>
#include <vector>
#include "filament_record.h"
#include "bambu_cloud.h"

// Bambu Cloud preset-sync API client. All calls go through
// BambuCloudAuth::api{Get,Post,Delete} so the WAF detection +
// browser-shaped headers are shared with the rest of the cloud
// integration.
//
// The cloud's API surface (per the bambuddy/maziggy preset-sync doc):
//   GET    /v1/iot-service/api/slicer/setting?version=...&public=false
//          → list of preset IDs the user has synced
//   GET    /v1/iot-service/api/slicer/setting/{id}
//          → full preset (filament/print/printer)
//   POST   /v1/iot-service/api/slicer/setting
//          → create; server returns new setting_id
//   DELETE /v1/iot-service/api/slicer/setting/{id}
//          → delete (response message:"success")
//   no PUT/PATCH — "update" is emulated as DELETE + POST (per the doc)
//
// Tri-state result mirrors VerifyResult so callers render
// "couldn't reach cloud" cleanly without a separate failure path per
// call.
namespace BambuCloudFilaments {

    enum class Result { Ok, Rejected, Unreachable };

    // Best-effort diagnostics filled by every call. The web layer
    // forwards this to the React UI on non-Ok outcomes so the user
    // can see WHY a sync failed instead of staring at a silent
    // banner. `body` is truncated to a sensible cap (kept compact
    // because we json-encode it back over the wire).
    struct Diag {
        String url;            // last URL hit
        int    httpStatus = 0; // 0 if transport failed
        String body;           // first ~512 bytes of response, if any
        bool   cfBlocked = false;
        // Free-form note about which step failed
        // ("list", "fetchOne <id>", "create", "delete <id>").
        String stage;
    };

    // Pull the user's full filament preset library from the cloud. Two-
    // round-trip: list IDs first, then GET each in turn. Slow on a big
    // library but no native bulk fetch exists. Skips non-PFUS rows
    // (only filament presets — print/machine ones not in scope).
    // `diagOut` is updated as the call progresses; on Ok it carries
    // the LAST step's HTTP status (typically the per-preset GET); on
    // failure it pinpoints which step + body.
    Result fetchAll(const String& token, BambuCloudAuth::Region region,
                    std::vector<FilamentRecord>& out, Diag* diagOut = nullptr);

    // Single-preset GET — used when reconciling a known cloud_setting_id.
    Result fetchOne(const String& token, BambuCloudAuth::Region region,
                    const String& preset_id, FilamentRecord& out,
                    Diag* diagOut = nullptr);

    // Push a new preset to the cloud. On Ok, `newCloudIdOut` is filled
    // with the server-issued PFUS<hash> id. The caller stores that on
    // the local record's cloud_setting_id field.
    Result createOne(const String& token, BambuCloudAuth::Region region,
                     const FilamentRecord& in, String& newCloudIdOut,
                     Diag* diagOut = nullptr);

    // Delete a cloud preset by id. Used by the "update" emulation
    // (delete-old-then-create-new) per the cloud's no-PUT contract.
    Result deleteOne(const String& token, BambuCloudAuth::Region region,
                     const String& preset_id, Diag* diagOut = nullptr);
}
