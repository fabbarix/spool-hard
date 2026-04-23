#pragma once
#include <Arduino.h>
#include <vector>
#include "filament_record.h"

// Read-only stock-filament library from the BambuStudio-derived
// filaments.jsonl on SD. ~100 records / ~32KB total — small enough to
// load entirely into RAM at boot. The frontend pulls the same file via
// /api/filaments and parses it client-side, so the firmware doesn't
// need to expose a per-id GET (the picker UI does the lookup itself).
//
// Stock rows are immutable: there's no upsert/remove. Replacement
// happens when the user uploads a new filaments.jsonl through the web
// UI; reload() re-reads the file from disk.
class StockFilamentsStore {
public:
    void begin();    // load on boot

    // Parse the on-disk file fresh — called after a filaments.jsonl
    // upload completes so the new content takes effect without
    // requiring a reboot.
    void reload();

    bool findById(const String& setting_id, FilamentRecord& out) const;

    // Returns up to `limit` rows starting at `offset`. Optional
    // material filter (PLA / PETG / ...) compares against
    // FilamentRecord::filament_type.
    std::vector<FilamentRecord> list(size_t offset, size_t limit,
                                     const String& material_filter = "") const;

    size_t count() const { return _rows.size(); }
    bool   loaded() const { return _loaded; }

private:
    std::vector<FilamentRecord> _rows;
    bool _loaded = false;
};
