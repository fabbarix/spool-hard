#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// One filament preset as the firmware sees it. Same flat shape regardless
// of source — stock (BambuStudio export, read-only) or user (locally
// editable, optionally synced to Bambu Cloud).
//
// Wire matches the JSON the React UI exchanges so the same struct can be
// loaded from disk, marshalled over the web API, and pushed to Bambu Cloud
// (after light field renaming in the cloud module). Keep field names in
// sync with `useUserFilaments` / `FilamentsPage` on the frontend.
//
// `setting_id` conventions:
//   "PFUS<hash>" — synced to Bambu Cloud (server-issued, mirrors cloud)
//   "PFUL<hash>" — local-only user filament (we generate)
//   ""           — stock preset (build-pipeline-emitted JSONL row); the
//                  stock JSONL also stamps the row with `stock=true`.
//
// `cloud_setting_id` is the cloud's ID for this preset when we've synced
// it. May differ from `setting_id` when a local preset (PFUL) gets pushed
// to the cloud — the local id stays stable for spool linkage, the cloud
// id is what we DELETE+POST against on update.
struct FilamentRecord {
    String  setting_id;            // PK
    bool    stock          = false;
    String  name;                  // human-readable, e.g. "Bambu PLA Basic"
    String  base_id;               // Bambu's parent preset, e.g. "GFSA00"
    // Local-stock linkage: when a user creates a custom by picking a
    // stock entry as the base, this holds that stock entry's setting_id
    // (e.g. "Bambu PETG Basic @base"). The frontend uses it to resolve
    // unset numeric/PA fields from the parent at display time, and the
    // form shows the parent's values as placeholders for any field the
    // user hasn't overridden. Empty for cloud-synced customs (their
    // `base_id` is Bambu's GFXX00 namespace, which doesn't map to any
    // local stock entry one-to-one) and for stock rows themselves.
    String  parent_setting_id;
    // Cloud-side parent reference. The detail response carries
    // `setting.inherits` like "Bambu PETG Basic @BBL X1C 0.8 nozzle"
    // — the human name of the next preset up the BambuStudio
    // inheritance chain. Captured during cloud sync so the edit form
    // can fetch the parent's full settings via the public-catalog
    // cache and pre-fill numeric fields the user hasn't customised.
    String  cloud_inherits;
    String  filament_type;         // PLA, PETG, TPU, …
    String  filament_subtype;      // basic, matte, translucent, … (optional)
    String  filament_vendor;
    String  filament_id;           // Bambu's tray_info_idx, e.g. "GFL99"

    int32_t nozzle_temp_min = -1;  // °C; -1 = unset (user-only fields)
    int32_t nozzle_temp_max = -1;  // °C
    float   density         = 0.f; // g/cm³; 0 = unset

    // Pressure-advance (K) for this filament. PA depends on BOTH the
    // filament chemistry and the installed nozzle — a 0.6 mm nozzle wants
    // a different K than a 0.4 mm one. Two storage shapes:
    //   `pressure_advance`           — legacy scalar; default for any
    //                                   nozzle without a per-nozzle row,
    //                                   and what Bambu Cloud round-trips
    //   `pa_by_nozzle_json` (array)  — preferred per-nozzle entries
    //                                   [{"nozzle":0.4,"k":0.040},
    //                                    {"nozzle":0.6,"k":0.055}]
    // `paForNozzle()` walks the array first, then falls back to the
    // scalar. Pre-loaded from a stock library entry's PA on create.
    float   pressure_advance = 0.f;
    String  pa_by_nozzle_json;     // serialized JSON array; "" = none

    // Cloud-sync metadata (only relevant for user filaments).
    String   cloud_setting_id;     // PFUS<hash>; "" = not synced
    uint32_t cloud_synced_at = 0;  // epoch s; last successful push/pull
    uint32_t updated_at      = 0;  // epoch s; mtime, drives "out of sync"

    void toJson(JsonDocument& doc) const;
    bool fromJson(const JsonDocument& doc);

    // Resolve PA for the given nozzle diameter (mm). Returns 0.f if no
    // entry is set for this nozzle AND the scalar fallback is also unset.
    float paForNozzle(float nozzle_diameter_mm) const;

    // JSONL line round-trip (mirrors SpoolRecord's pattern).
    String toLine() const;
    bool   fromLine(const String& line);
};
