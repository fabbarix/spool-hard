#pragma once
#include <Arduino.h>
#include <map>
#include <vector>
#include "spool_record.h"

// On-flash spool database backed by LittleFS.
//
// Format: JSON Lines (one record per line) at SPOOLS_DB_PATH. Deleted
// records are marked with a leading "x " prefix; `pack()` rewrites the file
// without them. An in-RAM index (tag_id → id, id → byte-offset) is built at
// boot from one pass over the file.
//
// Atomic writes: `upsert()` appends a new line and updates the index, then
// rewrites the index file on-flash. Crashes mid-write leave the previous
// record readable; the stale version will be garbage-collected on next pack.
class SpoolStore {
public:
    void begin();

    bool findByTagId(const String& tag_id, SpoolRecord& out);
    bool findById(const String& id, SpoolRecord& out);
    bool upsert(const SpoolRecord& rec);
    bool remove(const String& id);

    // Returns up to `limit` records starting at `offset`.
    std::vector<SpoolRecord> list(size_t offset, size_t limit,
                                  const String& material_filter = "");

    size_t count() const { return _idIndex.size(); }

    // Rewrite the file skipping deleted/tombstoned records. Rebuilds indexes.
    bool pack();

private:
    // id → byte offset of start of line in the file
    std::map<String, size_t> _idIndex;
    // tag_id → id
    std::map<String, String> _tagIndex;

    bool _readAt(size_t offset, SpoolRecord& out) const;
    bool _append(const SpoolRecord& rec, size_t& outOffset);
    bool _tombstone(size_t offset);
    void _rebuildIndexes();
};
