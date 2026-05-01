#pragma once
#include <Arduino.h>
#include <FS.h>
#include <functional>
#include <map>
#include <vector>
#include "spool_record.h"

// Spool database — JSON Lines format, one record per line. Deleted records
// are marked with a leading "x " prefix; `pack()` rewrites the file without
// them. An in-RAM index (tag_id → id, id → byte-offset) is built at boot
// from one pass over the file.
//
// Storage backend is configurable: LittleFS (internal flash) or SD card.
// SD reduces internal-flash wear during long prints (the store is rewritten
// on every 5% progress commit). Pass the chosen `fs::FS&` + path into
// begin(); the rest of the API is backend-agnostic.
//
// State machine — three legal modes:
//   * Ready    : begin() succeeded, reads/writes go to disk.
//   * NoBackend: begin() never called, OR called with the SD-missing
//                error path. All reads return empty, writes return false.
//                lastError() carries a user-facing message.
class SpoolStore {
public:
    enum class Status { NoBackend, Ready };

    // Initialise with a filesystem + JSONL path (e.g. "/spools.jsonl").
    // After a successful begin(), status() returns Ready.
    void begin(fs::FS& fs, const String& path);
    // Mark the store unavailable with a user-facing reason — used when the
    // configured backend (typically SD) isn't mounted at boot. UI surfaces
    // lastError() so the user can either insert the SD or override to
    // internal flash.
    void beginUnavailable(const String& reason);

    Status status()              const { return _status; }
    bool   ready()               const { return _status == Status::Ready; }
    const String& lastError()    const { return _lastError; }
    const String& path()         const { return _path; }
    bool          backendIsSd()  const { return _backend_is_sd; }

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

    // Migrate the underlying file to a different backend + path. Atomic-ish:
    // copies to a `.tmp` on the destination, fsyncs, then renames over.
    // On success, the store is rebound to the new backend (subsequent
    // upserts go to the new location); the old file is left intact for
    // the caller to delete after verifying. Returns false with lastError()
    // populated on any IO failure — the original backend stays active.
    bool migrateTo(fs::FS& dst_fs, const String& dst_path);

    // Caller-driven: signal whether the SD-card backend is currently
    // active. Used purely for UI surfacing (so /api/storage/status can
    // tell the user where bytes land without re-querying NVS).
    void markBackendIsSd(bool b) { _backend_is_sd = b; }

    // Observer for any mutation (upsert / remove / pack). Fires AFTER
    // the index is updated and the on-disk file is consistent. Single
    // callback (set-replaces-set) — we only have one observer (the WS
    // pusher) and a vector of callbacks would be overkill. Set to {}
    // (default-constructed) to clear.
    void onChange(std::function<void()> cb) { _onChange = std::move(cb); }

private:
    fs::FS* _fs   = nullptr;
    String  _path;
    Status  _status = Status::NoBackend;
    String  _lastError;
    bool    _backend_is_sd = false;
    std::function<void()> _onChange;   // fired after upsert/remove/pack

    // id → byte offset of start of line in the file
    std::map<String, size_t> _idIndex;
    // tag_id → id
    std::map<String, String> _tagIndex;

    bool _readAt(size_t offset, SpoolRecord& out) const;
    bool _append(const SpoolRecord& rec, size_t& outOffset);
    bool _tombstone(size_t offset);
    void _rebuildIndexes();
};
