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

    size_t count() const { return _count; }
    bool   loaded() const { return _loaded; }

private:
    // Memory layout: raw JSONL text in ONE PSRAM buffer + a small PSRAM
    // index (offset, len, FNV-1a of setting_id / filament_type). Rows are
    // parsed on demand by findById()/list() instead of keeping ~100
    // FilamentRecords (13 Arduino Strings each => hundreds of tiny
    // internal-DRAM allocations) resident.
    struct RowIdx {
        uint32_t off;        // byte offset of the line in _buf
        uint16_t len;        // line length (no newline)
        uint32_t id_hash;
        uint32_t type_hash;
    };
    void freeBuffers();
    bool parseRow(const RowIdx& r, FilamentRecord& out) const;

    char*   _buf   = nullptr;
    RowIdx* _idx   = nullptr;
    size_t  _count = 0;
    bool _loaded = false;
};
