#include "user_filaments_store.h"
#include "config.h"
#include "sdcard.h"
#include <SD.h>
#include <esp_random.h>

void UserFilamentsStore::begin() {
    _mounted = g_sd.isMounted();
    if (!_mounted) {
        Serial.println("[UserFilaments] SD not mounted; user filaments unavailable");
        return;
    }
    _rebuildIndex();
    Serial.printf("[UserFilaments] Loaded %u user filament records\n",
                  (unsigned)_idIndex.size());
}

void UserFilamentsStore::_rebuildIndex() {
    _idIndex.clear();
    if (!_mounted) return;

    File f = SD.open(USER_FILAMENTS_PATH, FILE_READ);
    if (!f) return;
    while (f.available()) {
        size_t lineStart = f.position();
        String line = f.readStringUntil('\n');
        if (line.length() == 0) break;
        if (line.startsWith("x ")) continue;  // tombstone
        FilamentRecord rec;
        if (rec.fromLine(line)) {
            _idIndex[rec.setting_id] = lineStart;
        }
    }
    f.close();
}

bool UserFilamentsStore::_readAt(size_t offset, FilamentRecord& out) const {
    if (!_mounted) return false;
    File f = SD.open(USER_FILAMENTS_PATH, FILE_READ);
    if (!f) return false;
    if (!f.seek(offset)) { f.close(); return false; }
    String line = f.readStringUntil('\n');
    f.close();
    if (line.startsWith("x ")) return false;
    return out.fromLine(line);
}

bool UserFilamentsStore::_append(const FilamentRecord& rec, size_t& outOffset) {
    if (!_mounted) return false;
    File f = SD.open(USER_FILAMENTS_PATH, FILE_APPEND);
    if (!f) {
        // First write — create the file.
        f = SD.open(USER_FILAMENTS_PATH, FILE_WRITE);
        if (!f) return false;
    }
    outOffset = f.size();
    String line = rec.toLine();
    line += '\n';
    size_t n = f.print(line);
    f.close();
    return n == line.length();
}

bool UserFilamentsStore::_tombstone(size_t offset) {
    if (!_mounted) return false;
    // SD.open with "r+" mode: in-place overwrite. Arduino's SD library
    // exposes this via FILE_WRITE on an already-existing file at a seekable
    // position. We just need to write 2 bytes ("x ") at the line start.
    File f = SD.open(USER_FILAMENTS_PATH, "r+");
    if (!f) return false;
    if (!f.seek(offset)) { f.close(); return false; }
    f.print("x ");
    f.close();
    return true;
}

bool UserFilamentsStore::findById(const String& setting_id, FilamentRecord& out) {
    auto it = _idIndex.find(setting_id);
    if (it == _idIndex.end()) return false;
    return _readAt(it->second, out);
}

bool UserFilamentsStore::upsert(const FilamentRecord& rec) {
    if (!_mounted || rec.setting_id.isEmpty()) return false;

    auto it = _idIndex.find(rec.setting_id);
    if (it != _idIndex.end()) _tombstone(it->second);

    size_t offset;
    if (!_append(rec, offset)) return false;
    _idIndex[rec.setting_id] = offset;
    return true;
}

bool UserFilamentsStore::remove(const String& setting_id) {
    auto it = _idIndex.find(setting_id);
    if (it == _idIndex.end()) return false;
    if (!_tombstone(it->second)) return false;
    _idIndex.erase(it);
    return true;
}

std::vector<FilamentRecord> UserFilamentsStore::list(
        size_t offset, size_t limit, const String& material_filter) {
    std::vector<FilamentRecord> out;
    size_t skipped = 0, returned = 0;
    for (auto& kv : _idIndex) {
        FilamentRecord rec;
        if (!_readAt(kv.second, rec)) continue;
        if (material_filter.length() && rec.filament_type != material_filter) continue;
        if (skipped < offset) { skipped++; continue; }
        out.push_back(std::move(rec));
        if (++returned >= limit) break;
    }
    return out;
}

bool UserFilamentsStore::pack() {
    if (!_mounted) return false;
    File in = SD.open(USER_FILAMENTS_PATH, FILE_READ);
    if (!in) return true;  // nothing to pack
    const char* tmpPath = USER_FILAMENTS_PATH ".tmp";
    File out = SD.open(tmpPath, FILE_WRITE);
    if (!out) { in.close(); return false; }
    while (in.available()) {
        String line = in.readStringUntil('\n');
        if (line.startsWith("x ") || line.length() == 0) continue;
        out.println(line);
    }
    in.close();
    out.close();
    SD.remove(USER_FILAMENTS_PATH);
    SD.rename(tmpPath, USER_FILAMENTS_PATH);
    _rebuildIndex();
    Serial.printf("[UserFilaments] Packed, %u records remain\n",
                  (unsigned)_idIndex.size());
    return true;
}

String UserFilamentsStore::newLocalId() {
    // Mimic Bambu's PFUS<14-hex> shape but flag as local with the L
    // suffix — see filament_record.h conventions.
    char buf[24];
    uint32_t hi = esp_random();
    uint32_t lo = esp_random();
    snprintf(buf, sizeof(buf), "PFUL%08x%06x",
             (unsigned)hi, (unsigned)(lo & 0xffffff));
    return String(buf);
}
