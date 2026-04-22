#include "store.h"
#include "config.h"
#include <LittleFS.h>

void SpoolStore::begin() {
    _rebuildIndexes();
    Serial.printf("[Store] Loaded %u spool records\n", (unsigned)_idIndex.size());
}

void SpoolStore::_rebuildIndexes() {
    _idIndex.clear();
    _tagIndex.clear();

    File f = LittleFS.open(SPOOLS_DB_PATH + 7, "r", true);  // strip "/userfs" prefix
    if (!f) {
        // Try the same path on the plain mount (LittleFS.open resolves against
        // its own mount root, not the absolute mount path).
        f = LittleFS.open("/spools.jsonl", "r", true);
    }
    if (!f) return;

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
    File f = LittleFS.open("/spools.jsonl", "r");
    if (!f) return false;
    if (!f.seek(offset)) { f.close(); return false; }
    String line = f.readStringUntil('\n');
    f.close();
    if (line.startsWith("x ")) return false;
    return out.fromLine(line);
}

bool SpoolStore::_append(const SpoolRecord& rec, size_t& outOffset) {
    File f = LittleFS.open("/spools.jsonl", "a", true);
    if (!f) return false;
    outOffset = f.size();
    String line = rec.toLine();
    line += '\n';
    size_t n = f.print(line);
    f.close();
    return n == line.length();
}

bool SpoolStore::_tombstone(size_t offset) {
    File f = LittleFS.open("/spools.jsonl", "r+");
    if (!f) return false;
    if (!f.seek(offset)) { f.close(); return false; }
    f.print("x ");
    f.close();
    return true;
}

bool SpoolStore::findByTagId(const String& tag_id, SpoolRecord& out) {
    auto it = _tagIndex.find(tag_id);
    if (it == _tagIndex.end()) return false;
    return findById(it->second, out);
}

bool SpoolStore::findById(const String& id, SpoolRecord& out) {
    auto it = _idIndex.find(id);
    if (it == _idIndex.end()) return false;
    return _readAt(it->second, out);
}

bool SpoolStore::upsert(const SpoolRecord& rec) {
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
    File in = LittleFS.open("/spools.jsonl", "r");
    if (!in) return true;  // nothing to pack
    File out = LittleFS.open("/spools.jsonl.tmp", "w", true);
    if (!out) { in.close(); return false; }

    while (in.available()) {
        String line = in.readStringUntil('\n');
        if (line.startsWith("x ") || line.length() == 0) continue;
        out.println(line);
    }
    in.close();
    out.close();

    LittleFS.remove("/spools.jsonl");
    LittleFS.rename("/spools.jsonl.tmp", "/spools.jsonl");
    _rebuildIndexes();
    Serial.printf("[Store] Packed, %u records remain\n", (unsigned)_idIndex.size());
    return true;
}
