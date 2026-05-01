#include "store.h"
#include "config.h"
#include "serial_mirror.h"

void SpoolStore::begin(fs::FS& fs, const String& path) {
    _fs        = &fs;
    _path      = path;
    _status    = Status::Ready;
    _lastError = "";
    _rebuildIndexes();
    Serial.printf("[Store] Loaded %u spool records from %s\n",
                  (unsigned)_idIndex.size(), _path.c_str());
}

void SpoolStore::beginUnavailable(const String& reason) {
    _fs        = nullptr;
    _path      = "";
    _status    = Status::NoBackend;
    _lastError = reason;
    _idIndex.clear();
    _tagIndex.clear();
    Serial.printf("[Store] Unavailable: %s\n", reason.c_str());
}

void SpoolStore::_rebuildIndexes() {
    _idIndex.clear();
    _tagIndex.clear();
    if (!_fs) return;

    File f = _fs->open(_path.c_str(), "r");
    if (!f) return;   // empty store — first boot, fine

    size_t offset = 0;
    while (f.available()) {
        size_t lineStart = f.position();
        String line = f.readStringUntil('\n');
        if (line.length() == 0) break;
        if (line.startsWith("x ")) {
            offset = f.position();
            continue;  // tombstoned
        }
        SpoolRecord rec;
        if (rec.fromLine(line)) {
            _idIndex[rec.id]         = lineStart;
            if (rec.tag_id.length()) _tagIndex[rec.tag_id] = rec.id;
        }
        offset = f.position();
    }
    f.close();
    (void)offset;
}

bool SpoolStore::_readAt(size_t offset, SpoolRecord& out) const {
    if (!_fs) return false;
    File f = _fs->open(_path.c_str(), "r");
    if (!f) return false;
    if (!f.seek(offset)) { f.close(); return false; }
    String line = f.readStringUntil('\n');
    f.close();
    if (line.startsWith("x ")) return false;
    return out.fromLine(line);
}

bool SpoolStore::_append(const SpoolRecord& rec, size_t& outOffset) {
    if (!_fs) return false;
    // SD's open(FILE_APPEND) needs the file to exist; LittleFS open("a", true)
    // creates on first write. Try open("a") first, fall back to open("w") if
    // the file doesn't exist yet (creates it). Both backends agree on these
    // mode strings via the fs::FS base class.
    File f = _fs->open(_path.c_str(), "a");
    if (!f) f = _fs->open(_path.c_str(), "w");
    if (!f) return false;
    outOffset = f.size();
    String line = rec.toLine();
    line += '\n';
    size_t n = f.print(line);
    f.close();
    return n == line.length();
}

bool SpoolStore::_tombstone(size_t offset) {
    if (!_fs) return false;
    File f = _fs->open(_path.c_str(), "r+");
    if (!f) return false;
    if (!f.seek(offset)) { f.close(); return false; }
    f.print("x ");
    f.close();
    return true;
}

bool SpoolStore::findByTagId(const String& tag_id, SpoolRecord& out) {
    if (!ready()) return false;
    auto it = _tagIndex.find(tag_id);
    if (it == _tagIndex.end()) return false;
    return findById(it->second, out);
}

bool SpoolStore::findById(const String& id, SpoolRecord& out) {
    if (!ready()) return false;
    auto it = _idIndex.find(id);
    if (it == _idIndex.end()) return false;
    return _readAt(it->second, out);
}

bool SpoolStore::upsert(const SpoolRecord& rec) {
    if (!ready()) return false;
    if (rec.id.isEmpty()) return false;

    // Tombstone the existing record if present.
    auto it = _idIndex.find(rec.id);
    if (it != _idIndex.end()) {
        _tombstone(it->second);
    }

    size_t offset;
    if (!_append(rec, offset)) return false;

    _idIndex[rec.id] = offset;
    if (rec.tag_id.length()) _tagIndex[rec.tag_id] = rec.id;
    return true;
}

bool SpoolStore::remove(const String& id) {
    if (!ready()) return false;
    auto it = _idIndex.find(id);
    if (it == _idIndex.end()) return false;
    _tombstone(it->second);

    // Also remove from tag index.
    SpoolRecord rec;
    if (_readAt(it->second, rec) && rec.tag_id.length()) {
        _tagIndex.erase(rec.tag_id);
    }
    _idIndex.erase(it);
    return true;
}

std::vector<SpoolRecord> SpoolStore::list(size_t offset, size_t limit, const String& material_filter) {
    std::vector<SpoolRecord> out;
    if (!ready()) return out;
    size_t skipped = 0, returned = 0;
    for (auto& kv : _idIndex) {
        SpoolRecord rec;
        if (!_readAt(kv.second, rec)) continue;
        if (material_filter.length() && rec.material_type != material_filter) continue;
        if (skipped < offset) { skipped++; continue; }
        out.push_back(std::move(rec));
        if (++returned >= limit) break;
    }
    return out;
}

bool SpoolStore::pack() {
    if (!ready()) return false;
    File in = _fs->open(_path.c_str(), "r");
    if (!in) return true;  // nothing to pack
    String tmpPath = _path + ".tmp";
    File out = _fs->open(tmpPath.c_str(), "w");
    if (!out) { in.close(); return false; }

    while (in.available()) {
        String line = in.readStringUntil('\n');
        if (line.startsWith("x ") || line.length() == 0) continue;
        out.println(line);
    }
    in.close();
    out.close();

    _fs->remove(_path.c_str());
    _fs->rename(tmpPath.c_str(), _path.c_str());
    _rebuildIndexes();
    Serial.printf("[Store] Packed, %u records remain\n", (unsigned)_idIndex.size());
    return true;
}

bool SpoolStore::migrateTo(fs::FS& dst_fs, const String& dst_path) {
    if (!ready()) {
        _lastError = "store not ready — nothing to migrate";
        return false;
    }
    // Pack first so we don't carry tombstones across — keeps the migrated
    // file tight and the index trivial to rebuild on the destination.
    pack();

    String dst_tmp = dst_path + ".tmp";
    File in = _fs->open(_path.c_str(), "r");
    if (!in) {
        // Source empty — create an empty destination file. Treat as success.
        File touched = dst_fs.open(dst_path.c_str(), "w");
        if (touched) touched.close();
        _fs   = &dst_fs;
        _path = dst_path;
        _rebuildIndexes();
        return true;
    }
    File out = dst_fs.open(dst_tmp.c_str(), "w");
    if (!out) {
        in.close();
        _lastError = "failed to open destination " + dst_tmp;
        return false;
    }
    uint8_t buf[512];
    while (in.available()) {
        size_t n = in.read(buf, sizeof(buf));
        if (n == 0) break;
        if (out.write(buf, n) != n) {
            in.close();
            out.close();
            dst_fs.remove(dst_tmp.c_str());
            _lastError = "write failed during migration";
            return false;
        }
    }
    in.close();
    out.close();
    // Atomically swing dst_path to the freshly-written copy.
    dst_fs.remove(dst_path.c_str());
    if (!dst_fs.rename(dst_tmp.c_str(), dst_path.c_str())) {
        _lastError = "rename failed on destination";
        return false;
    }
    Serial.printf("[Store] Migrated to %s (%u records)\n",
                  dst_path.c_str(), (unsigned)_idIndex.size());
    // Re-bind the live store to the new backend; subsequent writes go there.
    _fs   = &dst_fs;
    _path = dst_path;
    _rebuildIndexes();
    return true;
}
