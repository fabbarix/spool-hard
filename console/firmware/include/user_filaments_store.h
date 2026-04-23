#pragma once
#include <Arduino.h>
#include <map>
#include <vector>
#include "filament_record.h"

// User-managed filament presets, persisted as JSONL on SD at
// USER_FILAMENTS_PATH. Mirrors SpoolStore's pattern: tombstone-on-delete
// (`x ` line prefix), in-RAM index, atomic append, occasional `pack()` to
// reclaim deleted-line space.
//
// Lives on SD (not LittleFS) so it sits next to the stock filaments.db —
// users who swap SD cards keep the two filament sources together. If the
// SD isn't mounted at boot, `begin()` is a no-op + every read returns
// empty / every write returns false (caller surfaces the error).
class UserFilamentsStore {
public:
    void begin();

    bool findById(const String& setting_id, FilamentRecord& out);
    bool upsert(const FilamentRecord& rec);
    bool remove(const String& setting_id);

    // Returns up to `limit` records starting at `offset`. Optional
    // material filter (PLA/PETG/...) compares case-sensitively against
    // FilamentRecord::filament_type.
    std::vector<FilamentRecord> list(size_t offset, size_t limit,
                                     const String& material_filter = "");

    size_t count() const { return _idIndex.size(); }

    bool pack();

    // Generate a stable PFUL<hash> id for a new local-only preset.
    static String newLocalId();

private:
    std::map<String, size_t> _idIndex;   // setting_id → byte offset
    bool _mounted = false;

    bool _readAt(size_t offset, FilamentRecord& out) const;
    bool _append(const FilamentRecord& rec, size_t& outOffset);
    bool _tombstone(size_t offset);
    void _rebuildIndex();
};
