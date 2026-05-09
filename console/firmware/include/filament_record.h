#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// One filament preset as the firmware sees it. Same shape regardless of
// source — stock (BambuStudio export, read-only) or user (locally
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
// **Variants model.** Bambu's cloud catalog encodes per-(printer, nozzle)
// settings as separate presets named `<filament> @<printer> <nozzle>
// nozzle` — e.g. "SUNLU PLA 2+ @Bambu Lab H2S 0.4 nozzle" and
// "SUNLU PLA 2+ @Bambu Lab H2S 0.6 nozzle" are siblings of the same
// logical filament. We collapse those siblings into one FilamentRecord
// with a `variants` array. Identity (vendor/material/density/operational
// range) lives at the top level; print setpoints, max volumetric speed
// and PA — which depend on the printer extruder + nozzle bore — live on
// each variant. At spool-load time the firmware reads the active
// printer's model code + nozzle diameter from MQTT pushall and picks the
// matching variant; if none, it falls back to the top-level scalars.
//
// `cloud_setting_id` is the cloud's ID for this preset when we've synced
// it (only the FIRST variant pushed; siblings are tracked via
// `cloud_variant_ids` so DELETE+POST cycles can clean up all of them).
// One per-(printer, nozzle) variant of a logical filament. Mirrors the
// shape of a single Bambu cloud preset's `setting` blob: the per-extruder
// arrays (`filament_extruder_variant`, `filament_max_volumetric_speed`,
// `pressure_advance`) are kept parallel — index `i` in any of them refers
// to the same extruder type. Bambu serializes
// `filament_extruder_variant` as a `;`-separated string ("Direct Drive
// Standard;Direct Drive High Flow") but ships the value arrays as JSON
// arrays; we normalise everything to JSON arrays internally.
struct FilamentVariant {
    // Printer model — short canonical code, NOT Bambu's human name.
    // E.g. "X1C", "P1S", "P1P", "A1", "A1mini", "H2D", "H2S".
    // Empty string = "applies to any printer" (unscoped fallback).
    String  printer_model;
    // Nozzle diameter in mm. 0 = wildcard (any nozzle).
    float   nozzle_diameter   = 0.f;
    // Slicer setpoints (NOT the operational range — that's at the top).
    int32_t nozzle_temp_print          = -1;  // °C; main-layer temp
    int32_t nozzle_temp_initial_layer  = -1;  // °C; first-layer temp
    // Parallel per-extruder arrays. `extruder_variants[i]` labels the
    // value at `max_volumetric_speed[i]` and `pressure_advance[i]`.
    // Empty arrays == this variant doesn't characterise the per-extruder
    // values yet.
    std::vector<String> extruder_variants;     // e.g. ["Direct Drive Standard", "Direct Drive High Flow"]
    std::vector<float>  max_volumetric_speed;  // mm³/s
    std::vector<float>  pressure_advance;      // K
};

#include <vector>

struct FilamentRecord {
    String  setting_id;            // PK
    bool    stock          = false;
    String  name;                  // human-readable, e.g. "Bambu PLA Basic"
    String  base_id;               // Bambu's parent preset, e.g. "GFSA00"
    // Local-stock linkage: when a user creates a custom by picking a
    // stock entry as the base, this holds that stock entry's setting_id
    // (e.g. "Bambu PETG Basic @base"). The frontend uses it to resolve
    // unset numeric/PA fields from the parent at display time. Empty for
    // cloud-synced customs (their `base_id` is Bambu's GFXX00 namespace,
    // which doesn't map to any local stock entry one-to-one) and for
    // stock rows themselves.
    String  parent_setting_id;
    // Cloud-side parent reference. Captured during cloud sync from
    // `setting.inherits` so the edit form can resolve the parent's full
    // settings via the public-catalog cache.
    String  cloud_inherits;
    String  filament_type;         // PLA, PETG, TPU, …
    String  filament_subtype;      // basic, matte, translucent, …
    String  filament_vendor;
    String  filament_id;           // Bambu's tray_info_idx, e.g. "GFL99"

    // Operational temperature range — the safe window for this chemistry
    // (`nozzle_temperature_range_low/high`). Same regardless of printer
    // or nozzle, so it lives at the top.
    int32_t nozzle_temp_min = -1;  // °C; -1 = unset
    int32_t nozzle_temp_max = -1;  // °C
    float   density         = 0.f; // g/cm³; 0 = unset

    // Per-(printer, nozzle, extruder) overrides. Stored serialized as a
    // JSON array string (mirrors SpoolRecord::ext_json) to keep the
    // struct POD-ish. Empty string == no variants known.
    String  variants_json;

    // Cloud-sync metadata (only relevant for user filaments).
    String   cloud_setting_id;     // PFUS<hash> of the FIRST synced variant
    // Each variant is a separate cloud preset under the hood. This list
    // tracks all of them so DELETE+POST on update can clean up siblings.
    // Stored as a JSON array of strings; "" = none.
    String   cloud_variant_ids_json;
    uint32_t cloud_synced_at = 0;  // epoch s; last successful push/pull
    uint32_t updated_at      = 0;  // epoch s; mtime, drives "out of sync"

    void toJson(JsonDocument& doc) const;
    bool fromJson(const JsonDocument& doc);

    // Variant accessors.
    std::vector<FilamentVariant> variants() const;
    void setVariants(const std::vector<FilamentVariant>& vs);

    // Pick the variant best matching (printer_model, nozzle_diameter).
    // Match priority:
    //   1. exact printer_model + exact nozzle_diameter
    //   2. exact printer_model + wildcard nozzle (0)
    //   3. wildcard model + exact nozzle
    //   4. wildcard model + wildcard nozzle
    // Returns false if no variant matches at all.
    bool resolveVariant(const String& printer_model, float nozzle_diameter,
                        FilamentVariant& out) const;

    // JSONL line round-trip (mirrors SpoolRecord's pattern).
    String toLine() const;
    bool   fromLine(const String& line);
};
