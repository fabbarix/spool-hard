#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Shape ported from yanshay/SpoolEase:core/src/spool_record.rs. We're not
// byte-compatible with the Rust console's on-disk CSV — this version uses
// JSON Lines on LittleFS — but field names match so a migration tool could
// be written later.
struct SpoolRecord {
    String id;
    String tag_id;

    String material_type;       // PLA, PETG, TPU…
    String material_subtype;    // basic, matte, translucent…
    String color_name;
    String color_code;          // "RRGGBB"
    String brand;

    int32_t weight_advertised = -1;  // new spool weight g, -1 = unset
    int32_t weight_core       = -1;  // empty-spool weight g
    int32_t weight_new        = -1;  // weight when added
    int32_t weight_current    = -1;  // last measured

    // User-flagged "this spool is empty / used up — keep around as a record
    // but stop offering it for new prints". The web UI hides empty spools
    // from the list by default (toggle to show), and the LCD wizard's
    // template-pick flow treats them as eligible references for shape only.
    bool is_empty = false;

    float   consumed_since_add    = 0.f;
    float   consumed_since_weight = 0.f;

    // Print settings, used by the Bambu AMS auto-assignment feature. `-1`
    // means "unset — fall back to a material-default lookup at push time".
    int32_t nozzle_temp_min = -1;
    int32_t nozzle_temp_max = -1;

    // Filament density in g/cm³ (e.g. PLA ≈ 1.24, PETG ≈ 1.27). The gcode
    // analyzer consumes this when converting extruded mm → grams for the
    // AMS slot this spool is mapped to. 0 = unset → the analyzer falls
    // back to its hardcoded per-material-family table. Pre-filled from
    // the filaments library picker (`filament_density` property, resolved
    // via inheritance in the browser).
    float   density = 0.f;

    // Slicer filament identifier — Bambu's `tray_info_idx` (e.g. "GFL99" for
    // PLA Basic). Sent verbatim in the ams_filament_setting command so the
    // printer's slicer profile matches. Empty string means "don't set" and
    // the printer keeps whatever tray_info_idx it had.
    String slicer_filament;

    // Bambu Cloud preset id this spool was created from. Conventions:
    //   "PFUS<hash>" — synced to Bambu Cloud (server-issued)
    //   "PFUL<hash>" — local-only user filament (we generate)
    //   ""           — no preset linked (legacy spool created before the
    //                  Filaments tab landed, or via direct edit)
    // The id resolves through FilamentsStore to the full preset record,
    // letting the spool inherit material/temps/density without pinning a
    // copy. Per-spool overrides (above) still win over the preset's values
    // at push time.
    String setting_id;

    // Freeform user note, mirrors yanshay/SpoolEase's SpoolRecord.note.
    String note;

    String data_origin;         // SpoolHardV1, SpoolHardV2, BambuLab, OpenPrintTag…
    String tag_type;            // NTAG215, Mifare1K, …

    // Extended fields (K-values, origin data, raw tag payload) live in `ext`
    // as a JSON object so we can evolve the schema without a migration.
    // Current schema:
    //   ext.k_values = [
    //     { "printer": "<serial>", "nozzle": 0.4, "extruder": 0, "k": 0.040, "cali_idx": -1 },
    //     …
    //   ]
    String ext_json;            // serialized JSON object (or empty "")

    void toJson(JsonDocument& doc) const;
    bool fromJson(const JsonDocument& doc);

    // Serialize to one-line JSON for JSONL storage.
    String toLine() const;
    bool fromLine(const String& line);

    // Insert or update a K-value entry keyed by (printer, nozzle, extruder).
    // Returns true if the entry changed (and the caller should persist).
    bool upsertKValue(const String& printer_serial, float nozzle_diameter,
                      int extruder_idx, float k, int cali_idx);
};
